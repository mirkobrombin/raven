#include "vulkan_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace raven {

#ifndef RAVEN_VULKAN_GENERATED
// Without the registry at build time no command is marshaled beyond the hand-written ones.
std::span<const std::string_view> vulkan_generated_names() { return {}; }
bool vulkan_generated_is_device(std::string_view) { return false; }
bool VulkanBridge::dispatch_generated(std::string_view) { return false; }
void VulkanBridge::destroy_generic_objects(uint64_t) {}
std::span<const std::string_view> vulkan_hand_names() { return {}; }
bool VulkanBridge::dispatch_hand(std::string_view) { return false; }
void VulkanBridge::remember_template(uint64_t, const VkDescriptorUpdateTemplateCreateInfo *) {}
bool VulkanBridge::swapchain_stale(uint64_t) const { return false; }
void VulkanBridge::release_foreign_swapchains(uint64_t) {}
#endif

// The export table of the guest provider is binary searched, so the names stay in byte order.
static constexpr std::array<std::string_view, 14> names{
	"vkCreateDevice",
	"vkCreateInstance",
	"vkDestroyDevice",
	"vkDestroyInstance",
	"vkDeviceWaitIdle",
	"vkEnumerateInstanceVersion",
	"vkEnumeratePhysicalDevices",
	"vkGetDeviceProcAddr",
	"vkGetDeviceQueue",
	"vkGetInstanceProcAddr",
	"vkGetPhysicalDeviceFeatures",
	"vkGetPhysicalDeviceMemoryProperties",
	"vkGetPhysicalDeviceProperties",
	"vkGetPhysicalDeviceQueueFamilyProperties"
};
// Entry points reached through a device rather than an instance.
static constexpr std::array<std::string_view, 4> device_names{"vkDestroyDevice", "vkGetDeviceQueue", "vkDeviceWaitIdle", "vkGetDeviceProcAddr"};
static bool is_device_function(std::string_view symbol)
{
	const auto resources = vulkan_resource_names(), enumerate = vulkan_enumerate_names();
	return std::find(device_names.begin(), device_names.end(), symbol) != device_names.end() ||
		std::find(resources.begin(), resources.end(), symbol) != resources.end() ||
		(std::find(enumerate.begin(), enumerate.end(), symbol) != enumerate.end() && symbol.find("Device") != std::string_view::npos) ||
		vulkan_generated_is_device(symbol);
}

static_assert(sizeof(VkInstanceCreateInfo) == 64 && sizeof(VkApplicationInfo) == 48);
static_assert(sizeof(VkDeviceCreateInfo) == 72 && sizeof(VkDeviceQueueCreateInfo) == 40 && sizeof(VkQueueFamilyProperties) == 24);
static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == 520 && sizeof(VkPhysicalDeviceFeatures) == 220);
static_assert(sizeof(VkPhysicalDeviceProperties) == 824 && offsetof(VkPhysicalDeviceProperties, limits) == 296);

std::span<const std::string_view> VulkanBridge::exports()
{
	static const auto merged = [] {
		std::vector<std::string_view> all(names.begin(), names.end());
		for (const auto name : vulkan_resource_names())
			all.push_back(name);
		for (const auto name : vulkan_enumerate_names())
			all.push_back(name);
		for (const auto name : vulkan_query_names())
			all.push_back(name);
		for (const auto name : vulkan_generated_names())
			all.push_back(name);
		for (const auto name : vulkan_hand_names())
			all.push_back(name);
		std::sort(all.begin(), all.end());
		return all;
	}();
	return merged;
}

template<typename H> static H as_handle(uint64_t value) { return reinterpret_cast<H>(value); }

void VulkanBridge::destroy_object(const VkDevice &device, const Object &object)
{
	switch (object.kind) {
	case Kind::shader: vkDestroyShaderModule(device, as_handle<VkShaderModule>(object.handle), nullptr); break;
	case Kind::set_layout: vkDestroyDescriptorSetLayout(device, as_handle<VkDescriptorSetLayout>(object.handle), nullptr); break;
	case Kind::pipeline_layout: vkDestroyPipelineLayout(device, as_handle<VkPipelineLayout>(object.handle), nullptr); break;
	case Kind::pipeline: vkDestroyPipeline(device, as_handle<VkPipeline>(object.handle), nullptr); break;
	case Kind::pool: vkDestroyDescriptorPool(device, as_handle<VkDescriptorPool>(object.handle), nullptr); break;
	case Kind::set: break;
	case Kind::image: if (!object.borrowed) vkDestroyImage(device, as_handle<VkImage>(object.handle), nullptr); break;
	case Kind::image_view: vkDestroyImageView(device, as_handle<VkImageView>(object.handle), nullptr); break;
	case Kind::semaphore: vkDestroySemaphore(device, as_handle<VkSemaphore>(object.handle), nullptr); break;
	case Kind::render_pass: vkDestroyRenderPass(device, as_handle<VkRenderPass>(object.handle), nullptr); break;
	case Kind::framebuffer: vkDestroyFramebuffer(device, as_handle<VkFramebuffer>(object.handle), nullptr); break;
	case Kind::sampler: vkDestroySampler(device, as_handle<VkSampler>(object.handle), nullptr); break;
	}
}

// Host threads for driver calls that block: as many as there are calls in flight, kept for the ones that follow.
struct VulkanBridge::Workers {
	std::mutex lock;
	std::condition_variable signal;
	std::deque<std::function<void()>> jobs;
	std::vector<std::thread> threads;
	size_t idle = 0;
	bool stopping = false;

	void run(std::function<void()> job)
	{
		std::lock_guard hold(lock);
		jobs.push_back(std::move(job));
		if (jobs.size() > idle)
			threads.emplace_back([this] { loop(); });
		signal.notify_one();
	}

	void loop()
	{
		std::unique_lock hold(lock);
		for (;;) {
			++idle;
			signal.wait(hold, [this] { return stopping || !jobs.empty(); });
			--idle;
			if (stopping)
				return;
			auto job = std::move(jobs.front());
			jobs.pop_front();
			hold.unlock();
			job();
			hold.lock();
		}
	}

	~Workers()
	{
		{
			std::lock_guard hold(lock);
			stopping = true;
		}
		signal.notify_all();
		for (auto &thread : threads)
			thread.join();
	}
};

VkResult VulkanBridge::detached(const std::function<VkResult()> &work, std::atomic<bool> *cancel)
{
	if (!park)
		return work();
	if (!workers)
		workers = std::make_shared<Workers>();
	struct Call {
		VkResult status = VK_SUCCESS;
		std::atomic<bool> done{false};
	};
	const auto call = std::make_shared<Call>();
	workers->run([call, &work, notify = wake] {
		call->status = work();
		call->done.store(true, std::memory_order_release);
		if (notify)
			notify();
	});
	try {
		park([&call] { return call->done.load(std::memory_order_acquire); });
	} catch (...) {
		if (cancel)
			cancel->store(true);
		// The driver reads what this frame decoded, so the frame stays until the call is back.
		while (!call->done.load(std::memory_order_acquire))
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		throw;
	}
	return call->status;
}

VkResult VulkanBridge::detached_wait(const std::function<VkResult(uint64_t)> &wait, uint64_t timeout)
{
	if (!park || timeout == 0)
		return wait(timeout);
	constexpr uint64_t slice = 50'000'000;
	std::atomic<bool> cancelled{false};
	const auto start = std::chrono::steady_clock::now();
	return detached([&] {
		for (;;) {
			const auto spent = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
			const uint64_t left = spent >= timeout ? 0 : timeout - spent;
			const VkResult status = wait(std::min(left, slice));
			if (status != VK_TIMEOUT || left <= slice || cancelled.load())
				return status;
		}
	}, &cancelled);
}

VulkanBridge::~VulkanBridge()
{
	workers.reset();
	while (!devices.empty())
		destroy_device(devices.begin()->first);
	// Surfaces a program left behind belong to the instance that created them, if that instance still exists here.
	for (const auto &[token, window] : surface_windows) {
		if (surface_source.release) surface_source.release(window);
		const auto found = generic.find(token);
		const auto owner = surface_instances.find(token);
		if (found == generic.end() || owner == surface_instances.end() || !instances.contains(owner->second))
			continue;
		const auto instance = instances.at(owner->second);
		if (const auto destroy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR")))
			destroy(instance, reinterpret_cast<VkSurfaceKHR>(found->second.host), nullptr);
	}
	for (const auto &[token, instance] : instances)
		vkDestroyInstance(instance, nullptr);
}

void VulkanBridge::destroy_device(uint64_t token)
{
	const auto found = devices.find(token);
	if (found == devices.end())
		return;
	std::erase_if(queues, [&](const auto &item) { return item.second.device == token; });
	// Resources a guest left behind go before their device, in dependency order.
	vkDeviceWaitIdle(found->second.handle);
	release_foreign_swapchains(token);
	std::erase_if(command_buffers, [&](const auto &item) { return item.second.device == token; });
	destroy_generic_objects(token);
	// Pipelines first, then what they were built from; descriptor sets go with their pool.
	for (const auto kind : {Kind::pipeline, Kind::framebuffer, Kind::render_pass, Kind::pool, Kind::pipeline_layout, Kind::set_layout, Kind::shader, Kind::image_view, Kind::sampler, Kind::image, Kind::semaphore})
		for (auto item = objects.begin(); item != objects.end();)
			if (item->second.device == token && item->second.kind == kind) {
				destroy_object(found->second.handle, item->second);
				item = objects.erase(item);
			} else ++item;
	std::erase_if(objects, [&](const auto &item) { return item.second.device == token; });
	for (auto item = pools.begin(); item != pools.end();)
		if (item->second.device == token) { vkDestroyCommandPool(found->second.handle, item->second.handle, nullptr); item = pools.erase(item); } else ++item;
	for (auto item = fences.begin(); item != fences.end();)
		if (item->second.device == token) { vkDestroyFence(found->second.handle, item->second.handle, nullptr); item = fences.erase(item); } else ++item;
	for (auto item = buffers.begin(); item != buffers.end();)
		if (item->second.device == token) { vkDestroyBuffer(found->second.handle, item->second.handle, nullptr); item = buffers.erase(item); } else ++item;
	for (auto item = memories.begin(); item != memories.end();) {
		if (item->second.device == token) {
			if (item->second.host) {
				vkUnmapMemory(found->second.handle, item->second.handle);
				drop_shadow(item->second);
			}
			vkFreeMemory(found->second.handle, item->second.handle, nullptr);
			item = memories.erase(item);
		} else ++item;
	}
	vkDestroyDevice(found->second.handle, nullptr);
	devices.erase(found);
}

std::string VulkanBridge::string(uint64_t address)
{
	std::string result;
	for (size_t index = 0; index < 4096; ++index) {
		if (address > UINT64_MAX - index)
			break;
		const auto byte = process.read_memory(address + index, 1)[0];
		if (!byte)
			return result;
		result.push_back(static_cast<char>(byte));
	}
	throw std::runtime_error("invalid Vulkan guest string");
}

bool VulkanBridge::dispatch(std::string_view name)
{
	if (dispatch_resource(name) || dispatch_enumerate(name) || dispatch_queries(name) || dispatch_hand(name) || dispatch_generated(name))
		return true;
	if (std::find(names.begin(), names.end(), name) == names.end())
		return false;
	const auto words = [&]() -> size_t {
		if (name == "vkEnumerateInstanceVersion" || name == "vkDeviceWaitIdle")
			return 1;
		if (name == "vkGetDeviceQueue" || name == "vkCreateDevice")
			return 4;
		if (name == "vkCreateInstance" || name == "vkEnumeratePhysicalDevices" || name == "vkGetPhysicalDeviceQueueFamilyProperties")
			return 3;
		return 2;
	}();
	const auto arguments = process.api_arguments(words);
	const auto read = [&]<typename T>(uint64_t address) {
		T result{};
		const auto bytes = process.read_memory(address, sizeof(T));
		std::memcpy(&result, bytes.data(), sizeof(T));
		return result;
	};
	const auto write = [&]<typename T>(uint64_t address, const T &value) {
		process.write_memory(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), sizeof(T)));
	};
	const auto writable = [&](uint64_t address, size_t size) {
		process.write_memory(address, process.read_memory(address, size));
	};
	uint64_t result = 0;
	if (name == "vkEnumerateInstanceVersion") {
		writable(arguments[0], 4);
		uint32_t version = 0;
		result = static_cast<uint32_t>(vkEnumerateInstanceVersion(&version));
		write(arguments[0], version);
	} else if (name == "vkGetInstanceProcAddr") {
		if (arguments[0] && !instances.contains(arguments[0]))
			throw std::runtime_error("invalid Vulkan instance handle");
		const auto symbol = string(arguments[1]);
		const bool global = symbol == "vkCreateInstance" || symbol == "vkEnumerateInstanceVersion" || symbol == "vkGetInstanceProcAddr" ||
			symbol == "vkEnumerateInstanceExtensionProperties" || symbol == "vkEnumerateInstanceLayerProperties";
		const auto exported = VulkanBridge::exports();
		const auto instance = arguments[0] ? instances.at(arguments[0]) : VK_NULL_HANDLE;
		const bool xcb = surface_source.platform == HostPlatform::xcb;
		const char *host_name = symbol == "vkCreateWin32SurfaceKHR" ? (xcb ? "vkCreateXcbSurfaceKHR" : "vkCreateWaylandSurfaceKHR") :
			symbol == "vkGetPhysicalDeviceWin32PresentationSupportKHR" ?
			(xcb ? "vkGetPhysicalDeviceXcbPresentationSupportKHR" : "vkGetPhysicalDeviceWaylandPresentationSupportKHR") : symbol.c_str();
		if ((arguments[0] || global) && std::find(exported.begin(), exported.end(), symbol) != exported.end() &&
			vkGetInstanceProcAddr(instance, host_name))
			result = modules.resolve({"vulkan-1.dll", symbol, 0, false, 0});
		if (std::getenv("RAVEN_TRACE_API") && !result)
			std::fprintf(stderr, "vulkan: unavailable instance function %s\n", symbol.c_str());
	} else if (name == "vkGetDeviceProcAddr") {
		if (!devices.contains(arguments[0]))
			throw std::runtime_error("invalid Vulkan device handle");
		const auto symbol = string(arguments[1]);
		const auto &module = *modules.all().at("vulkan-1.dll");
		const bool exported = std::any_of(module.exports.begin(), module.exports.end(), [&](const auto &entry) {
			return std::find(entry.names.begin(), entry.names.end(), symbol) != entry.names.end();
		});
		// The Windows handle commands have no host counterpart; they are offered wherever the library exports them.
		const bool win32_handles = symbol == "vkGetMemoryWin32HandleKHR" || symbol == "vkGetSemaphoreWin32HandleKHR" ||
			symbol == "vkImportSemaphoreWin32HandleKHR";
		if (exported && ((is_device_function(symbol) && vkGetDeviceProcAddr(devices.at(arguments[0]).handle, symbol.c_str())) || win32_handles))
			result = modules.resolve({"vulkan-1.dll", symbol, 0, false, 0});
		if (std::getenv("RAVEN_TRACE_API") && !result)
			std::fprintf(stderr, "vulkan: unavailable device function %s\n", symbol.c_str());
	} else if (name == "vkCreateInstance") {
		auto info = read.template operator()<VkInstanceCreateInfo>(arguments[0]);
		if (arguments[1] || info.pNext || info.sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
			info.flags || info.enabledLayerCount || info.enabledExtensionCount > 32)
			throw std::runtime_error("Vulkan allocator, pNext, flags or layers are not implemented");
		std::vector<std::string> guest_extensions, host_extensions;
		for (uint32_t index = 0; index < info.enabledExtensionCount; ++index)
			guest_extensions.push_back(string(read.template operator()<uint64_t>(reinterpret_cast<uint64_t>(info.ppEnabledExtensionNames) + uint64_t(index) * 8)));
		if (!host_extension_names(guest_extensions, false, host_extensions))
			throw std::runtime_error("Vulkan instance extension is not offered");
		std::vector<const char *> host_pointers;
		for (const auto &extension : host_extensions)
			host_pointers.push_back(extension.c_str());
		VkApplicationInfo application{};
		std::string application_name, engine_name;
		if (info.pApplicationInfo) {
			application = read.template operator()<VkApplicationInfo>(reinterpret_cast<uint64_t>(info.pApplicationInfo));
			if (application.pNext || application.sType != VK_STRUCTURE_TYPE_APPLICATION_INFO)
				throw std::runtime_error("unsupported Vulkan application info");
			if (application.pApplicationName)
				application_name = string(reinterpret_cast<uint64_t>(application.pApplicationName));
			if (application.pEngineName)
				engine_name = string(reinterpret_cast<uint64_t>(application.pEngineName));
			application.pApplicationName = application.pApplicationName ? application_name.c_str() : nullptr;
			application.pEngineName = application.pEngineName ? engine_name.c_str() : nullptr;
			info.pApplicationInfo = &application;
		}
		info.ppEnabledLayerNames = nullptr;
		info.enabledExtensionCount = uint32_t(host_pointers.size());
		info.ppEnabledExtensionNames = host_pointers.empty() ? nullptr : host_pointers.data();
		writable(arguments[2], 8);
		VkInstance instance = VK_NULL_HANDLE;
		const auto status = vkCreateInstance(&info, nullptr, &instance);
		result = static_cast<uint32_t>(status);
		if (status == VK_SUCCESS) {
			const auto token = next_handle++;
			try {
				instances.emplace(token, instance);
				write(arguments[2], token);
			} catch (...) {
				instances.erase(token);
				vkDestroyInstance(instance, nullptr);
				throw;
			}
		}
	} else if (name == "vkDestroyInstance") {
		if (arguments[1])
			throw std::runtime_error("Vulkan guest allocation callbacks are not implemented");
		if (arguments[0]) {
			const auto found = instances.find(arguments[0]);
			if (found == instances.end())
				throw std::runtime_error("invalid Vulkan instance handle");
			// A device that outlives its instance would be undefined behaviour in the host driver.
			std::vector<uint64_t> owned;
			for (const auto &[token, device] : devices)
				if (physical.contains(device.physical) && physical.at(device.physical).instance == arguments[0])
					owned.push_back(token);
			for (const auto token : owned)
				destroy_device(token);
			vkDestroyInstance(found->second, nullptr);
			instances.erase(found);
			std::erase_if(physical, [&](const auto &item) { return item.second.instance == arguments[0]; });
		}
	} else if (name == "vkEnumeratePhysicalDevices") {
		const auto found = instances.find(arguments[0]);
		if (found == instances.end())
			throw std::runtime_error("invalid Vulkan instance handle");
		writable(arguments[1], 4);
		uint32_t count = 0;
		if (arguments[2]) {
			count = read.template operator()<uint32_t>(arguments[1]);
			if (count > 4096)
				throw std::runtime_error("Vulkan physical device capacity exceeds limit");
			writable(arguments[2], size_t(count) * 8);
		}
		std::vector<VkPhysicalDevice> devices(count);
		VkPhysicalDevice zero_capacity = VK_NULL_HANDLE;
		const auto status = vkEnumeratePhysicalDevices(found->second, &count,
			arguments[2] ? (devices.empty() ? &zero_capacity : devices.data()) : nullptr);
		result = static_cast<uint32_t>(status);
		if (status == VK_SUCCESS || status == VK_INCOMPLETE) {
			if (arguments[2]) {
				for (size_t index = 0; index < count; ++index) {
					const auto existing = std::find_if(physical.begin(), physical.end(), [&](const auto &item) {
						return item.second.instance == arguments[0] && item.second.handle == devices.at(index);
					});
					uint64_t token = existing == physical.end() ? next_handle++ : existing->first;
					if (existing == physical.end())
						physical.emplace(token, Physical{devices.at(index), arguments[0]});
					write(arguments[2] + index * 8, token);
				}
			}
			write(arguments[1], count);
		}
	} else if (name == "vkGetPhysicalDeviceMemoryProperties" || name == "vkGetPhysicalDeviceFeatures") {
		const auto found = physical.find(arguments[0]);
		if (found == physical.end())
			throw std::runtime_error("invalid Vulkan physical device handle");
		if (name == "vkGetPhysicalDeviceMemoryProperties") {
			writable(arguments[1], sizeof(VkPhysicalDeviceMemoryProperties));
			VkPhysicalDeviceMemoryProperties memory;
			std::memset(&memory, 0, sizeof(memory));
			vkGetPhysicalDeviceMemoryProperties(found->second.handle, &memory);
			write(arguments[1], memory);
		} else {
			writable(arguments[1], sizeof(VkPhysicalDeviceFeatures));
			VkPhysicalDeviceFeatures features;
			std::memset(&features, 0, sizeof(features));
			vkGetPhysicalDeviceFeatures(found->second.handle, &features);
			write(arguments[1], features);
		}
	} else if (name == "vkGetPhysicalDeviceQueueFamilyProperties") {
		const auto found = physical.find(arguments[0]);
		if (found == physical.end())
			throw std::runtime_error("invalid Vulkan physical device handle");
		writable(arguments[1], 4);
		uint32_t count = 0;
		if (arguments[2]) {
			count = read.template operator()<uint32_t>(arguments[1]);
			if (count > 4096)
				throw std::runtime_error("Vulkan queue family capacity exceeds limit");
			writable(arguments[2], size_t(count) * sizeof(VkQueueFamilyProperties));
		}
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(found->second.handle, &count, arguments[2] ? families.data() : nullptr);
		if (arguments[2])
			for (size_t index = 0; index < count && index < families.size(); ++index)
				write(arguments[2] + index * sizeof(VkQueueFamilyProperties), families[index]);
		write(arguments[1], count);
	} else if (name == "vkCreateDevice") {
		const auto found = physical.find(arguments[0]);
		if (found == physical.end())
			throw std::runtime_error("invalid Vulkan physical device handle");
		if (arguments[2])
			throw std::runtime_error("Vulkan guest allocation callbacks are not implemented");
		auto info = read.template operator()<VkDeviceCreateInfo>(arguments[1]);
		if (info.sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || info.flags || info.enabledLayerCount ||
			info.enabledExtensionCount > 96 || !info.queueCreateInfoCount || info.queueCreateInfoCount > 16)
			throw std::runtime_error("Vulkan device flags or layers are not implemented");
		// Feature structures chained to the create info are plain blocks of bytes that travel to the driver unchanged.
		auto device_chain = mirror_chain(reinterpret_cast<uint64_t>(info.pNext), true);
		info.pNext = link_chain(device_chain);
		std::vector<std::string> guest_extensions, host_extensions;
		for (uint32_t index = 0; index < info.enabledExtensionCount; ++index)
			guest_extensions.push_back(string(read.template operator()<uint64_t>(reinterpret_cast<uint64_t>(info.ppEnabledExtensionNames) + uint64_t(index) * 8)));
		if (!host_extension_names(guest_extensions, true, host_extensions))
			throw std::runtime_error("Vulkan device extension is not offered");
		std::vector<const char *> host_pointers;
		for (const auto &extension : host_extensions)
			host_pointers.push_back(extension.c_str());
		std::vector<VkDeviceQueueCreateInfo> queue_infos(info.queueCreateInfoCount);
		std::vector<std::vector<float>> priorities(info.queueCreateInfoCount);
		std::vector<decltype(device_chain)> queue_chains;
		queue_chains.reserve(info.queueCreateInfoCount);
		for (uint32_t index = 0; index < info.queueCreateInfoCount; ++index) {
			queue_infos[index] = read.template operator()<VkDeviceQueueCreateInfo>(
				reinterpret_cast<uint64_t>(info.pQueueCreateInfos) + uint64_t(index) * sizeof(VkDeviceQueueCreateInfo));
			auto &queue = queue_infos[index];
			if (queue.sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || !queue.queueCount || queue.queueCount > 64)
				throw std::runtime_error("unsupported Vulkan queue create info");
			// A chain on the queue's create info, such as a global priority request, travels like the device's own.
			if (queue.pNext) {
				queue_chains.push_back(mirror_chain(reinterpret_cast<uint64_t>(queue.pNext), true));
				queue.pNext = link_chain(queue_chains.back());
			}
			priorities[index].resize(queue.queueCount);
			const auto bytes = process.read_memory(reinterpret_cast<uint64_t>(queue.pQueuePriorities), size_t(queue.queueCount) * 4);
			std::memcpy(priorities[index].data(), bytes.data(), bytes.size());
			queue.pQueuePriorities = priorities[index].data();
		}
		VkPhysicalDeviceFeatures features{};
		if (info.pEnabledFeatures) {
			features = read.template operator()<VkPhysicalDeviceFeatures>(reinterpret_cast<uint64_t>(info.pEnabledFeatures));
			info.pEnabledFeatures = &features;
		}
		info.pQueueCreateInfos = queue_infos.data();
		info.ppEnabledLayerNames = nullptr;
		info.enabledExtensionCount = uint32_t(host_pointers.size());
		info.ppEnabledExtensionNames = host_pointers.empty() ? nullptr : host_pointers.data();
		writable(arguments[3], 8);
		VkDevice device = VK_NULL_HANDLE;
		const auto status = vkCreateDevice(found->second.handle, &info, nullptr, &device);
		result = static_cast<uint32_t>(status);
		if (status == VK_SUCCESS) {
			const auto token = next_handle++;
			Device record{device, arguments[0], {}};
			try {
				devices.emplace(token, record);
				write(arguments[3], token);
			} catch (...) {
				devices.erase(token);
				vkDestroyDevice(device, nullptr);
				throw;
			}
		}
	} else if (name == "vkDestroyDevice") {
		if (arguments[1])
			throw std::runtime_error("Vulkan guest allocation callbacks are not implemented");
		if (arguments[0]) {
			if (!devices.contains(arguments[0]))
				throw std::runtime_error("invalid Vulkan device handle");
			destroy_device(arguments[0]);
		}
	} else if (name == "vkGetDeviceQueue") {
		const auto found = devices.find(arguments[0]);
		if (found == devices.end())
			throw std::runtime_error("invalid Vulkan device handle");
		writable(arguments[3], 8);
		VkQueue handle = VK_NULL_HANDLE;
		vkGetDeviceQueue(found->second.handle, uint32_t(arguments[1]), uint32_t(arguments[2]), &handle);
		if (!handle)
			throw std::runtime_error("Vulkan queue was not requested at device creation");
		const auto existing = std::find_if(queues.begin(), queues.end(), [&](const auto &item) {
			return item.second.device == arguments[0] && item.second.family == uint32_t(arguments[1]) && item.second.index == uint32_t(arguments[2]);
		});
		uint64_t token = existing == queues.end() ? next_handle++ : existing->first;
		if (existing == queues.end())
			queues.emplace(token, Queue{handle, arguments[0], uint32_t(arguments[1]), uint32_t(arguments[2])});
		write(arguments[3], token);
	} else if (name == "vkDeviceWaitIdle") {
		const auto found = devices.find(arguments[0]);
		if (found == devices.end())
			throw std::runtime_error("invalid Vulkan device handle");
		const auto status = vkDeviceWaitIdle(found->second.handle);
		result = static_cast<uint32_t>(status);
		if (status == VK_SUCCESS)
			shadow_from_device(arguments[0], cached_memory);
	} else if (name == "vkGetPhysicalDeviceProperties") {
		const auto found = physical.find(arguments[0]);
		if (found == physical.end())
			throw std::runtime_error("invalid Vulkan physical device handle");
		writable(arguments[1], sizeof(VkPhysicalDeviceProperties));
		VkPhysicalDeviceProperties properties;
		std::memset(&properties, 0, sizeof(properties));
		vkGetPhysicalDeviceProperties(found->second.handle, &properties);
		write(arguments[1], properties);
	}
	process.return_api(result);
	return true;
}

}
