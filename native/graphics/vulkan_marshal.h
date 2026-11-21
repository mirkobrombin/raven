#pragma once

#include "vulkan_bridge.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace raven {

// The Vulkan handle types, in the order of the registry; the generated command code names them.
enum class H : uint8_t {
#include "vulkan_handle_types.inc"
};

// One guest call under translation: guest memory is read into host-owned copies that live as long as the call, guest handle tokens
// become driver handles, and results travel back the same way.
class Marshal {
public:
	Marshal(VulkanBridge &bridge, NativeProcess &process) : bridge(bridge), process(process) {}
	VulkanBridge &bridge;
	NativeProcess &process;
	uint64_t device_token = 0;
	VkDevice host_device = VK_NULL_HANDLE;
	VkInstance host_instance = VK_NULL_HANDLE;
	VkObjectType capture_object = VK_OBJECT_TYPE_UNKNOWN;
	size_t capture_descriptor_size(VkObjectType type) const;

	// Records the device or instance the first handle argument belongs to; function lookup and ownership follow from it.
	void context(H type, uint64_t token);
	uint64_t host(H type, uint64_t token) const;
	// The token of a handle the driver returned: the existing one, or a new one owned by the current device.
	uint64_t token(H type, uint64_t host);
	uint64_t new_token(H type, uint64_t host);
	// The token of an image a swapchain owns: stable across queries, and never destroyed on the guest's behalf.
	uint64_t swapchain_image_token(H type, uint64_t host);
	// Host copies of the image descriptors a write refers to: the sampler counts for samplers and combined image samplers, the view for every type but a sampler.
	VkDescriptorImageInfo *descriptor_image_infos(uint64_t guest, size_t count, VkDescriptorType type);
	// A write names its descriptors in exactly one of three arrays by type; the pointers of the others are ignored by the driver and
	// carry whatever the program left in them, so they are never followed.
	static bool image_descriptor(VkDescriptorType type) { return type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; }
	static bool buffer_descriptor(VkDescriptorType type) { return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; }
	static bool texel_descriptor(VkDescriptorType type) { return type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; }
	void forget(uint64_t token);

	template<typename T> T *make(size_t count)
	{
		arena.emplace_back(std::max<size_t>(count, 1) * sizeof(T) + 16, 0);
		auto address = (reinterpret_cast<uintptr_t>(arena.back().data()) + 15) & ~uintptr_t(15);
		return reinterpret_cast<T *>(address);
	}
	template<typename T> T load(uint64_t address) const
	{
		T value{};
		read_into(address, &value, sizeof(T));
		return value;
	}
	template<typename T> void store(uint64_t address, const T &value) const { write(address, &value, sizeof(T)); }
	void read_into(uint64_t address, void *target, size_t size) const;
	void write(uint64_t address, const void *source, size_t size) const;
	void check_writable(uint64_t address, size_t size) const;
	std::string read_string(uint64_t address) const;
	const char *string(const std::string &value);
	const char *const *string_array(uint64_t address, size_t count);
	void *copy_bytes(uint64_t address, size_t size);
	template<typename T> const T *host_array(H type, uint64_t address, size_t count)
	{
		auto *result = make<T>(count);
		for (size_t index = 0; index < count; ++index)
			result[index] = reinterpret_cast<T>(host(type, load<uint64_t>(address + index * 8)));
		return result;
	}
	void require_null(uint64_t pointer) const;
	float float_argument(const std::array<uint64_t, 4> &registers, const std::vector<uint64_t> &words, size_t index) const;
	double double_argument(const std::array<uint64_t, 4> &registers, const std::vector<uint64_t> &words, size_t index) const;
	template<typename F> F proc(const char *name)
	{
		// Device-specific entry points stay with their device, including when the driver reuses a destroyed device's handle.
		auto *const procedures = host_device ? &bridge.devices.at(device_token).procedures : nullptr;
		if (procedures) {
			const std::lock_guard lock(bridge.procedures_lock);
			if (const auto found = procedures->find(name); found != procedures->end())
				return reinterpret_cast<F>(found->second);
		}
		const auto lookup = [&](const char *spelling) {
			return host_device ? vkGetDeviceProcAddr(host_device, spelling) : vkGetInstanceProcAddr(host_instance, spelling);
		};
		PFN_vkVoidFunction function = lookup(name);
		// A command promoted to core is still provided under its extension name by a driver that only has the extension.
		for (const char *suffix : {"KHR", "EXT"})
			if (!function)
				function = lookup((std::string(name) + suffix).c_str());
		if (!function)
			throw std::runtime_error(std::string("the driver does not provide ") + name);
		if (procedures) {
			const std::lock_guard lock(bridge.procedures_lock);
			procedures->emplace(name, function);
		}
		return reinterpret_cast<F>(function);
	}

	// vkAllocateMemory: kept beside the memory bookkeeping, with a create info that may carry a pNext chain.
	uint64_t allocate_memory(const std::vector<uint64_t> &arguments);
	// The hand-written object kind that stands for a handle type, when there is one.
	static std::optional<VulkanBridge::Kind> kind_of(H type);

private:
	// Growing the list moves the owning vectors, preserving their buffers and the pointers passed to the driver.
	std::vector<std::vector<uint8_t>> arena;
};

}
