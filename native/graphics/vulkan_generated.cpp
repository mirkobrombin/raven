// Commands marshaled from the Vulkan registry; see scripts/generate-vulkan-marshal.py.
#include "vulkan_marshal.h"
#include "desktop/foreign_surface.h"
#include <vulkan/vulkan_wayland.h>
#ifdef RAVEN_XCB
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#endif

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace raven {

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "vulkan_marshal_generated.inc"

static const GeneratedCommand *generated_command(std::string_view name)
{
	const auto found = std::lower_bound(std::begin(generated_commands), std::end(generated_commands), name,
		[](const auto &command, std::string_view wanted) { return std::string_view(command.name) < wanted; });
	return found != std::end(generated_commands) && name == found->name ? found : nullptr;
}

uint64_t Marshal::allocate_memory(const std::vector<uint64_t> &a)
{
	context(H::VkDevice, a[0]);
	require_null(a[2]);
	auto *info = decode_array_VkMemoryAllocateInfo(*this, a[1], 1);
	if (!info->allocationSize || info->allocationSize > (256ULL << 20))
		throw std::runtime_error("unsupported Vulkan memory allocation size");
	check_writable(a[3], 8);
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VulkanBridge::Memory record{VK_NULL_HANDLE, a[0], info->allocationSize};
	VkPhysicalDeviceMemoryProperties properties{};
	const auto physical = bridge.devices.at(a[0]).physical;
	vkGetPhysicalDeviceMemoryProperties(bridge.physical.at(physical).handle, &properties);
	if (info->memoryTypeIndex >= properties.memoryTypeCount)
		throw std::runtime_error("invalid Vulkan memory type index");
	record.coherent = (properties.memoryTypes[info->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	// Importing host memory would require matching external-memory flags on every bound resource.
	const auto status = vkAllocateMemory(host_device, info, nullptr, &memory);
	if (status == VK_SUCCESS) {
		const auto token_value = bridge.next_handle++;
		record.handle = memory;
		bridge.memories.emplace(token_value, record);
		store<uint64_t>(a[3], token_value);
	}
	return static_cast<uint32_t>(status);
}

std::span<const std::string_view> vulkan_generated_names()
{
	static const auto names = [] {
		std::vector<std::string_view> list{"vkAllocateMemory"};
		for (const auto &command : generated_commands)
			list.emplace_back(command.name);
		std::sort(list.begin(), list.end());
		return list;
	}();
	return names;
}

void VulkanBridge::destroy_generic_objects(uint64_t device_token)
{
	const auto found = devices.find(device_token);
	if (found == devices.end())
		return;
	// Newest first: an object is destroyed before what it was built from.
	std::vector<uint64_t> owned;
	for (const auto &[token, entry] : generic)
		if (entry.device == device_token)
			owned.push_back(token);
	std::sort(owned.rbegin(), owned.rend());
	Marshal marshal(*this, process);
	marshal.context(H::VkDevice, device_token);
	for (const auto token : owned) {
		const auto entry = generic.at(token);
		try { destroy_generated_handle(marshal, static_cast<H>(entry.type), entry.host); } catch (const std::runtime_error &) {}
		generic.erase(token);
	}
}

std::span<const std::string_view> vulkan_hand_names()
{
	static constexpr std::array<std::string_view, 9> names{"vkCmdPushDescriptorSetWithTemplate", "vkCmdPushDescriptorSetWithTemplateKHR",
		"vkCreateWin32SurfaceKHR", "vkGetMemoryWin32HandleKHR", "vkGetPhysicalDeviceWin32PresentationSupportKHR", "vkGetSemaphoreWin32HandleKHR",
		"vkImportSemaphoreWin32HandleKHR",
		"vkUpdateDescriptorSetWithTemplate", "vkUpdateDescriptorSetWithTemplateKHR"};
	// The swapchain commands below wrap the generated ones; they are dispatched here first and export under their own names.
	return names;
}

// A Windows surface over a guest HWND becomes a Wayland or XCB surface over the host window that shows it.
bool VulkanBridge::dispatch_hand(std::string_view name)
{
	if (name == "vkCreateWin32SurfaceKHR") {
		const auto a = process.api_arguments(4);
		Marshal marshal(*this, process);
		marshal.context(H::VkInstance, a[0]);
		marshal.require_null(a[2]);
		marshal.check_writable(a[3], 8);
		// VkWin32SurfaceCreateInfoKHR: sType, pNext, flags, hinstance, hwnd.
		if (load<uint32_t>(a[1]) != VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR)
			throw std::runtime_error("vkCreateWin32SurfaceKHR needs a Win32 surface create info");
		const auto window = load<uint64_t>(a[1] + 32);
		uint32_t status = static_cast<uint32_t>(VK_ERROR_INITIALIZATION_FAILED);
		const auto host = surface_source.create ? surface_source.create(window) : std::nullopt;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		bool foreign=false;
		if (host) {
			VkResult created = VK_ERROR_INITIALIZATION_FAILED;
			if (host->platform == HostPlatform::wayland) {
				VkWaylandSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0,
					static_cast<struct wl_display *>(host->display), static_cast<struct wl_surface *>(host->surface)};
				created = marshal.proc<PFN_vkCreateWaylandSurfaceKHR>("vkCreateWaylandSurfaceKHR")(marshal.host_instance, &info, nullptr, &surface);
			}
#ifdef RAVEN_XCB
			else if (host->platform == HostPlatform::xcb) {
				VkXcbSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, nullptr, 0,
					static_cast<xcb_connection_t *>(host->display), host->window};
				created = marshal.proc<PFN_vkCreateXcbSurfaceKHR>("vkCreateXcbSurfaceKHR")(marshal.host_instance, &info, nullptr, &surface);
			}
#endif
			status = static_cast<uint32_t>(created);
			const bool shared = std::any_of(surface_windows.begin(), surface_windows.end(), [window](const auto &entry) { return entry.second == window; });
			if (created != VK_SUCCESS && surface_source.release && !shared) {
				surface_source.release(window);
			}
		} else if (foreign_window_size(window)) {
			const auto create=reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(marshal.host_instance,"vkCreateHeadlessSurfaceEXT"));
			if (create) {
				VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
				status=static_cast<uint32_t>(create(marshal.host_instance,&info,nullptr,&surface));
				foreign=status==VK_SUCCESS;
			}
		}
		if (status==VK_SUCCESS) {
			const auto token=marshal.new_token(H::VkSurfaceKHR,reinterpret_cast<uint64_t>(surface));
			surface_windows.emplace(token,window); surface_instances.emplace(token,a[0]);
			if (foreign) foreign_surfaces.insert(token);
			store<uint64_t>(a[3],token);
		}
		process.return_api(status);
		return true;
	}
	// Memory and semaphores are not exchanged as Windows handles: the host devices offer file descriptors, which no handle stands for
	// yet. Translation layers ask for the handles of every shared resource whether or not the extensions were enabled, so they get a
	// clean failure.
	if (name == "vkGetMemoryWin32HandleKHR" || name == "vkGetSemaphoreWin32HandleKHR" || name == "vkImportSemaphoreWin32HandleKHR") {
		const bool import = name == "vkImportSemaphoreWin32HandleKHR";
		const auto a = process.api_arguments(import ? 2 : 3);
		Marshal marshal(*this, process);
		marshal.context(H::VkDevice, a[0]);
		if (!import)
			marshal.check_writable(a[2], 8);
		process.return_api(static_cast<uint32_t>(import ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_ERROR_OUT_OF_HOST_MEMORY));
		return true;
	}
	if (name == "vkGetPhysicalDeviceWin32PresentationSupportKHR") {
		auto a = process.api_arguments(2);
		a[1] = static_cast<uint32_t>(a[1]);
		const auto found = physical.find(a[0]);
		if (found == physical.end())
			throw std::runtime_error("invalid Vulkan physical device handle");
		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(found->second.handle, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(found->second.handle, &count, families.data());
		// Presentation goes through the host window the surface is created over; a family can present when it can draw.
		process.return_api(a[1] < count && (families[a[1]].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? VK_TRUE : VK_FALSE);
		return true;
	}
	// A Windows surface reports the client area of its window as its only extent and a swapchain of another size is out of date.
	// The window's size stands in for the extent of the host surface, which a Wayland surface does not have.
	if (name == "vkGetPhysicalDeviceSurfaceCapabilitiesKHR" || name == "vkCreateSwapchainKHR" || name == "vkAcquireNextImageKHR" ||
		name == "vkQueuePresentKHR") {
		const auto arguments = process.api_arguments(name == "vkAcquireNextImageKHR" ? 6 : name == "vkCreateSwapchainKHR" ? 4 : 3);
		std::array<uint64_t, 4> floating{};
		try { floating = process.api_float_arguments(); } catch (const std::runtime_error &) {}
		Marshal marshal(*this, process);
		const auto window_size = [&](uint64_t surface) -> std::optional<std::pair<uint32_t, uint32_t>> {
			const auto window = surface_windows.find(surface);
			if (window == surface_windows.end())
				return std::nullopt;
			if (foreign_surfaces.contains(surface)) return foreign_window_size(window->second);
			return surface_source.size ? surface_source.size(window->second) : std::nullopt;
		};
		uint64_t result = 0;
		if (name == "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") {
			result = cmd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(*this, marshal, arguments, floating);
			if (static_cast<int32_t>(result) == VK_SUCCESS)
				if (const auto size = window_size(arguments[1])) {
					// VkSurfaceCapabilitiesKHR: minImageCount, maxImageCount, then currentExtent, minImageExtent and maxImageExtent.
					for (const uint64_t offset : {8, 16, 24}) {
						store<uint32_t>(arguments[2] + offset, size->first);
						store<uint32_t>(arguments[2] + offset + 4, size->second);
					}
				}
		} else if (name == "vkCreateSwapchainKHR") {
			const auto surface=load<uint64_t>(arguments[1]+24);
			if (foreign_surfaces.contains(surface)) {
				marshal.context(H::VkDevice,arguments[0]);
				marshal.require_null(arguments[2]); marshal.check_writable(arguments[3],8);
				auto *info=decode_array_VkSwapchainCreateInfoKHR(marshal,arguments[1],1);
				info->imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				VkSwapchainKHR swapchain=VK_NULL_HANDLE;
				auto status=vkCreateSwapchainKHR(marshal.host_device,info,nullptr,&swapchain);
				if (status==VK_SUCCESS) {
					const auto token=marshal.new_token(H::VkSwapchainKHR,reinterpret_cast<uint64_t>(swapchain));
					status=prepare_foreign_swapchain(token,arguments[0],swapchain,*info,surface_windows.at(surface));
					if (status==VK_SUCCESS) store<uint64_t>(arguments[3],token);
					else { vkDestroySwapchainKHR(marshal.host_device,swapchain,nullptr); marshal.forget(token); }
				}
				result=static_cast<uint32_t>(status);
			} else result = cmd_vkCreateSwapchainKHR(*this, marshal, arguments, floating);
			if (static_cast<int32_t>(result) == VK_SUCCESS) {
				// VkSwapchainCreateInfoKHR: surface at 24 and imageExtent at 44.
				const auto token = load<uint64_t>(arguments[3]);
				swapchains[token] = {load<uint64_t>(arguments[1] + 24), load<uint32_t>(arguments[1] + 44), load<uint32_t>(arguments[1] + 48)};
			}
		} else if (name == "vkAcquireNextImageKHR") {
			const auto swap=swapchains.find(arguments[1]);
			const bool lost=swap!=swapchains.end() && foreign_surfaces.contains(swap->second.surface) && !window_size(swap->second.surface);
			result = lost ? static_cast<uint32_t>(VK_ERROR_SURFACE_LOST_KHR) : swapchain_stale(arguments[1]) ? static_cast<uint32_t>(VK_ERROR_OUT_OF_DATE_KHR)
				: cmd_vkAcquireNextImageKHR(*this, marshal, arguments, floating);
		} else {
			marshal.context(H::VkQueue,arguments[0]);
			if (load<uint32_t>(arguments[1]+32)>4096 || load<uint32_t>(arguments[1]+16)>4096)
				throw std::runtime_error("Vulkan present array count exceeds limit");
			auto *info=decode_array_VkPresentInfoKHR(marshal,arguments[1],1);
			const auto results=load<uint64_t>(arguments[1]+56);
			if (results) marshal.check_writable(results,size_t(info->swapchainCount)*4);
			const auto foreign=present_foreign(arguments[0],*info);
			const auto host_queue=queues.at(arguments[0]).handle;
			result=static_cast<uint32_t>(foreign ? *foreign : detached([&] {
				return vkQueuePresentKHR(host_queue,info);
			}));
			if (static_cast<int32_t>(result) == VK_SUCCESS) {
				// VkPresentInfoKHR: swapchainCount at 32 and pSwapchains at 40.
				const auto count = load<uint32_t>(arguments[1] + 32);
				const auto list = load<uint64_t>(arguments[1] + 40);
				for (uint32_t index = 0; index < count; ++index) {
					const auto token=load<uint64_t>(list+uint64_t(index)*8);
					const auto swap=swapchains.find(token);
					const bool lost=swap!=swapchains.end() && foreign_surfaces.contains(swap->second.surface) && !window_size(swap->second.surface);
					if (lost || swapchain_stale(token)) {
						const auto status=lost ? VK_ERROR_SURFACE_LOST_KHR : VK_SUBOPTIMAL_KHR;
						result=static_cast<uint32_t>(status);
						if (info->pResults) info->pResults[index]=status;
					}
				}
			}
			if (results) marshal.write(results,info->pResults,size_t(info->swapchainCount)*4);
		}
		process.return_api(result);
		return true;
	}
	const bool push_template = name == "vkCmdPushDescriptorSetWithTemplate" || name == "vkCmdPushDescriptorSetWithTemplateKHR";
	if (push_template || name == "vkUpdateDescriptorSetWithTemplate" || name == "vkUpdateDescriptorSetWithTemplateKHR") {
		const auto a = process.api_arguments(push_template ? 5 : 4);
		Marshal marshal(*this, process);
		marshal.context(push_template ? H::VkCommandBuffer : H::VkDevice, a[0]);
		const auto template_token = a[push_template ? 1 : 2];
		const auto host_template = marshal.host(H::VkDescriptorUpdateTemplate, template_token);
		const auto entries = templates.find(template_token);
		if (entries == templates.end())
			throw std::runtime_error("descriptor update template was not created through the bridge");
		// The guest data holds guest handle tokens where the entries say descriptors live; the driver gets the same layout with host handles.
		size_t extent = 0;
		const auto element_size = [](VkDescriptorType type) -> size_t {
			switch (type) {
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 8;
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return sizeof(VkDescriptorBufferInfo);
			case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return sizeof(VkDescriptorImageInfo);
			default: throw std::runtime_error("unsupported descriptor type in a template update");
			}
		};
		for (const auto &entry : entries->second)
			if (entry.descriptorCount)
				extent = std::max(extent, entry.offset + (entry.descriptorCount - 1) * entry.stride + element_size(entry.descriptorType));
		std::vector<uint8_t> data(extent, 0);
		for (const auto &entry : entries->second)
			for (uint32_t index = 0; index < entry.descriptorCount; ++index) {
				const auto at = entry.offset + size_t(index) * entry.stride;
				const auto guest = a[push_template ? 4 : 3] + at;
				if (entry.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || entry.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
					const uint64_t view = marshal.host(H::VkBufferView, marshal.load<uint64_t>(guest));
					std::memcpy(data.data() + at, &view, 8);
				} else if (entry.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || entry.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
					entry.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || entry.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
					auto info = marshal.load<VkDescriptorBufferInfo>(guest);
					info.buffer = reinterpret_cast<VkBuffer>(marshal.host(H::VkBuffer, reinterpret_cast<uint64_t>(info.buffer)));
					std::memcpy(data.data() + at, &info, sizeof info);
				} else {
					auto info = marshal.load<VkDescriptorImageInfo>(guest);
					// Only the members the descriptor type reads are handles; the others are not initialised by programs and stay null.
					const bool uses_sampler = entry.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || entry.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					const bool uses_view = entry.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER;
					info.sampler = uses_sampler ? reinterpret_cast<VkSampler>(marshal.host(H::VkSampler, reinterpret_cast<uint64_t>(info.sampler))) : VK_NULL_HANDLE;
					info.imageView = uses_view ? reinterpret_cast<VkImageView>(marshal.host(H::VkImageView, reinterpret_cast<uint64_t>(info.imageView))) : VK_NULL_HANDLE;
					std::memcpy(data.data() + at, &info, sizeof info);
				}
			}
		if (push_template)
			marshal.proc<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplate")(
				reinterpret_cast<VkCommandBuffer>(marshal.host(H::VkCommandBuffer, a[0])),
				reinterpret_cast<VkDescriptorUpdateTemplate>(host_template),
				reinterpret_cast<VkPipelineLayout>(marshal.host(H::VkPipelineLayout, a[2])), uint32_t(a[3]), data.data());
		else
			marshal.proc<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplate")(marshal.host_device,
				reinterpret_cast<VkDescriptorSet>(marshal.host(H::VkDescriptorSet, a[1])),
				reinterpret_cast<VkDescriptorUpdateTemplate>(host_template), data.data());
		process.return_api(0);
		return true;
	}
	return false;
}

bool VulkanBridge::swapchain_stale(uint64_t swapchain) const
{
	const auto found = swapchains.find(swapchain);
	if (found == swapchains.end())
		return false;
	const auto window = surface_windows.find(found->second.surface);
	if (window == surface_windows.end())
		return false;
	const auto size = foreign_surfaces.contains(found->second.surface) ? foreign_window_size(window->second) :
		surface_source.size ? surface_source.size(window->second) : std::nullopt;
	return size && (size->first != found->second.width || size->second != found->second.height);
}

void VulkanBridge::remember_template(uint64_t token, const VkDescriptorUpdateTemplateCreateInfo *info)
{
	templates[token].assign(info->pDescriptorUpdateEntries, info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
}

bool vulkan_generated_is_device(std::string_view name)
{
	if (name == "vkAllocateMemory" || name == "vkUpdateDescriptorSetWithTemplate" || name == "vkUpdateDescriptorSetWithTemplateKHR" ||
		name == "vkCmdPushDescriptorSetWithTemplate" || name == "vkCmdPushDescriptorSetWithTemplateKHR")
		return true;
	const auto *command = generated_command(name);
	return command && command->device;
}

bool VulkanBridge::dispatch_generated(std::string_view name)
{
	if (name == "vkAllocateMemory") {
		const auto arguments = process.api_arguments(4);
		Marshal marshal(*this, process);
		process.return_api(marshal.allocate_memory(arguments));
		return true;
	}
	const auto *command = generated_command(name);
	if (!command)
		return false;
	const auto arguments = process.api_arguments(command->arguments);
	std::array<uint64_t, 4> floating{};
	try { floating = process.api_float_arguments(); } catch (const std::runtime_error &) {}
	Marshal marshal(*this, process);
	process.return_api(command->function(*this, marshal, arguments, floating));
	return true;
}

}
