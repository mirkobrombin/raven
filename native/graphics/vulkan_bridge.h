#pragma once

#include "cpu/native_cpu.h"
#include "desktop/presentation.h"
#include "pe/module_set.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace raven {

// Names of the memory, buffer, command and synchronisation entry points, in byte order.
std::span<const std::string_view> vulkan_resource_names();
std::span<const std::string_view> vulkan_enumerate_names();
std::span<const std::string_view> vulkan_query_names();
// Entry points written by hand on top of the generated ones: Windows surfaces over host windows and updates through descriptor templates.
std::span<const std::string_view> vulkan_hand_names();
// Commands marshaled from the Vulkan registry; empty when the registry was not available at build time.
std::span<const std::string_view> vulkan_generated_names();
bool vulkan_generated_is_device(std::string_view name);

class VulkanBridge {
public:
	VulkanBridge(NativeProcess &process, const ModuleSet &modules) : process(process), modules(modules) {}
	~VulkanBridge();
	VulkanBridge(const VulkanBridge &) = delete;
	VulkanBridge &operator=(const VulkanBridge &) = delete;
	static std::span<const std::string_view> exports();
	bool dispatch(std::string_view name);
	// The host display and surface over a local window. Foreign windows present through shared pixels instead.
	struct SurfaceSource {
		std::function<std::optional<HostSurface>(uint64_t window)> create;
		std::function<void(uint64_t window)> release;
		// The client area of the window: what a Windows surface reports as its extent.
		std::function<std::optional<std::pair<uint32_t, uint32_t>>(uint64_t window)> size;
		// The window system whose surface extension serves the Windows one.
		HostPlatform platform = HostPlatform::wayland;
	};
	void set_surface_source(SurfaceSource value) { surface_source = std::move(value); }

	friend class Marshal;
	// Brings the guest's view of mapped memory and the driver's in line before and after work the driver does.
	void synchronize_to_device(uint64_t device) { shadow_to_device(device); }
	void synchronize_from_device(uint64_t device) { shadow_from_device(device, cached_memory); }
	// Reading a timeline semaphore makes the work behind a new value visible; the same value again brings nothing the last read did not.
	void synchronize_after_counter(uint64_t device, uint64_t semaphore, uint64_t value)
	{
		const auto [seen, fresh] = counters.try_emplace(semaphore, value);
		if (!fresh && seen->second == value)
			return;
		seen->second = value;
		shadow_from_device(device, cached_memory);
	}
	// How a guest thread waits while the others run: `park` returns once the condition holds, and `wake` may be called from any host
	// thread to have the scheduler look at the conditions again.
	void set_blocking(std::function<void(std::function<bool()>)> park_thread, std::function<void()> wake_scheduler)
	{
		park = std::move(park_thread);
		wake = std::move(wake_scheduler);
	}
	// Runs a driver call that can take long, such as a present, on a host thread of its own; the guest thread that made it waits for it
	// while the process's other threads go on, as they do on Windows. When the guest thread is made to leave meanwhile, `cancel` is set
	// and the call is waited for, since it reads what the thread's frame decoded.
	VkResult detached(const std::function<VkResult()> &work, std::atomic<bool> *cancel = nullptr);
	// A wait the driver does with `timeout` in nanoseconds, detached the same way unless it only polls.
	VkResult detached_wait(const std::function<VkResult(uint64_t slice)> &wait, uint64_t timeout);

private:
	NativeProcess &process;
	const ModuleSet &modules;
	std::function<void(std::function<bool()>)> park;
	std::function<void()> wake;
	struct Workers;
	std::shared_ptr<Workers> workers;
	std::map<uint64_t, VkInstance> instances;
	struct Physical { VkPhysicalDevice handle; uint64_t instance; };
	std::map<uint64_t, Physical> physical;
	struct Device {
		VkDevice handle; uint64_t physical;
		std::map<std::string, PFN_vkVoidFunction, std::less<>> procedures;
	};
	std::map<uint64_t, Device> devices;
	// The maps a handle is translated through are hashed: every recorded command looks its handles up.
	struct Queue { VkQueue handle; uint64_t device; uint32_t family, index; };
	std::unordered_map<uint64_t, Queue> queues;
	// Device memory with the guest-visible shadow that stands in for a host mapping, and the other resources a guest device owns.
	struct Memory {
		VkDeviceMemory handle; uint64_t device; VkDeviceSize size;
		void *host = nullptr; uint64_t shadow = 0; VkDeviceSize offset = 0, mapped = 0;
		uint64_t mapping_base = 0;
		bool coherent = false;
		// Host-cached memory reads back cheaply; the other host-visible kinds are write-combined, where every byte read is slow.
		bool cached = false;
		std::vector<uint8_t> snapshot;
		// The guest's shadow is a memfd the host maps as well, so comparing it with the driver's mapping never goes through the debugger
		// interface: `view` is the host's window on the same bytes as the guest address `shadow`.
		int shadow_file = -1;
		uint8_t *view = nullptr;
		size_t view_size = 0;
	};
	std::unordered_map<uint64_t, Memory> memories;
	// The last value read from each timeline semaphore.
	std::unordered_map<uint64_t, uint64_t> counters;
	struct Buffer { VkBuffer handle; uint64_t device; };
	std::unordered_map<uint64_t, Buffer> buffers;
	struct Pool { VkCommandPool handle; uint64_t device; };
	std::unordered_map<uint64_t, Pool> pools;
	struct CommandBuffer { VkCommandBuffer handle; uint64_t device, pool; };
	std::unordered_map<uint64_t, CommandBuffer> command_buffers;
	struct Fence { VkFence handle; uint64_t device; };
	std::unordered_map<uint64_t, Fence> fences;
	// Shader modules, descriptor and pipeline objects: one host handle each, owned by a device token.
	enum class Kind { shader, set_layout, pipeline_layout, pipeline, pool, set, image, image_view, semaphore, render_pass, framebuffer, sampler };
	struct Object { uint64_t handle; uint64_t device; Kind kind; uint64_t parent = 0; bool borrowed = false; };
	std::unordered_map<uint64_t, Object> objects;
	bool dispatch_enumerate(std::string_view name);
	bool dispatch_hand(std::string_view name);
	SurfaceSource surface_source;
	// Surface tokens and the guest windows behind them.
	std::map<uint64_t, uint64_t> surface_windows;
	// The instance token each of those surfaces was created from.
	std::map<uint64_t, uint64_t> surface_instances;
	std::set<uint64_t> foreign_surfaces;
	struct ForeignSwapchain;
	std::map<uint64_t, std::shared_ptr<ForeignSwapchain>> foreign_swapchains;
	VkResult prepare_foreign_swapchain(uint64_t token,uint64_t device,VkSwapchainKHR swapchain,
		const VkSwapchainCreateInfoKHR &info,uint64_t window);
	std::optional<VkResult> present_foreign(uint64_t queue,const VkPresentInfoKHR &info);
	void release_foreign_swapchains(uint64_t device);
	// A swapchain's surface token and the extent it was created with; a surface whose window changed size makes it out of date.
	struct Swapchain { uint64_t surface; uint32_t width, height; };
	std::map<uint64_t, Swapchain> swapchains;
	bool swapchain_stale(uint64_t swapchain) const;
	// What each descriptor update template writes, for updates that pass handles in raw data.
	std::map<uint64_t, std::vector<VkDescriptorUpdateTemplateEntry>> templates;
public:
	void remember_template(uint64_t token, const VkDescriptorUpdateTemplateCreateInfo *info);
private:
	bool dispatch_queries(std::string_view name);
	// A pNext chain of plain structures mirrored into host memory: the guest address of each element and a host copy of it.
	struct ChainEntry { uint64_t guest; std::vector<uint8_t> host; };
	std::vector<ChainEntry> mirror_chain(uint64_t first, bool strict) const;
	// Links the host copies and returns the first one, or null for an empty chain.
	static void *link_chain(std::vector<ChainEntry> &chain);
	// Writes what the host filled in back to the guest structures, leaving their sType and pNext alone.
	void copy_back_chain(const std::vector<ChainEntry> &chain) const;
	// Host names for the extension names a guest enables; false when one is not offered to guests.
	bool host_extension_names(const std::vector<std::string> &guest, bool device, std::vector<std::string> &host) const;
	void destroy_object(const VkDevice &device, const Object &object);
	// Objects created by generated commands that no hand-written map owns.
	struct Generic { uint64_t host; uint8_t type; uint64_t device; };
	std::unordered_map<uint64_t, Generic> generic;
	bool dispatch_generated(std::string_view name);
	void destroy_generic_objects(uint64_t device_token);
	uint64_t next_handle = 0x5600000000000001ULL;
	void destroy_device(uint64_t token);
	// Copies between a guest shadow and the host mapping behind it, moving only the bytes that changed on one side.
	void shadow_to_device(uint64_t device);
	// Maps a shadow the guest and the host share, or releases one.
	uint64_t make_shadow(Memory &memory, size_t size);
	void drop_shadow(Memory &memory);
	// Which mapped memory a read back covers. Waiting on the device reads back host-cached memory only: write-combined memory is where a
	// program writes for the device, and reading all of it at every wait costs uncached reads of each mapping. A program that reads such
	// memory invalidates the ranges it reads, which reads back the memory they name whatever its kind.
	enum ReadBack { cached_memory, named_memory };
	void shadow_from_device(uint64_t device, ReadBack scope, const std::vector<uint64_t> &named = {});
	bool dispatch_resource(std::string_view name);
	template<typename T> T load(uint64_t address) const;
	template<typename T> void store(uint64_t address, const T &value) const;
	void check_writable(uint64_t address, size_t size) const;
	std::string string(uint64_t address);
};

template<typename T> T VulkanBridge::load(uint64_t address) const
{
	T result{};
	const auto bytes = process.read_memory(address, sizeof(T));
	std::memcpy(&result, bytes.data(), sizeof(T));
	return result;
}

template<typename T> void VulkanBridge::store(uint64_t address, const T &value) const
{
	process.write_memory(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), sizeof(T)));
}

}
