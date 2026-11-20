#include "vulkan_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace raven {

// Names are kept in byte order because the guest provider binary searches its export table.
static constexpr std::array<std::string_view, 4> names{
	"vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties", "vkEnumerateInstanceExtensionProperties",
	"vkEnumerateInstanceLayerProperties"
};

std::span<const std::string_view> vulkan_enumerate_names() { return names; }

namespace {

// Extensions offered to guests: the ones whose commands the bridge implements, under the names Windows programs ask for.
// The Windows surface extension is served by the surface extension of the host's window system.
struct Offer {
	const char *name;
	uint32_t version;
	const char *host;
};

constexpr Offer instance_offers[] = {
	{"VK_KHR_surface", 25, "VK_KHR_surface"}, {"VK_KHR_win32_surface", 6, "VK_KHR_wayland_surface"},
#ifdef RAVEN_VULKAN_GENERATED
	{"VK_KHR_get_physical_device_properties2", 2, "VK_KHR_get_physical_device_properties2"},
#endif
};
// Besides the swapchain, the extensions offered here only add feature and property structures, so offering them needs no commands.
constexpr Offer device_offers[] = {
	{"VK_KHR_swapchain", 70, "VK_KHR_swapchain"},
#ifdef RAVEN_VULKAN_GENERATED
	{"VK_KHR_maintenance1", 0, "VK_KHR_maintenance1"},
	{"VK_KHR_create_renderpass2", 0, "VK_KHR_create_renderpass2"},
	{"VK_KHR_imageless_framebuffer", 0, "VK_KHR_imageless_framebuffer"},
	{"VK_KHR_descriptor_update_template", 0, "VK_KHR_descriptor_update_template"},
#endif
	{"VK_EXT_depth_clip_enable", 1, "VK_EXT_depth_clip_enable"},
	{"VK_EXT_robustness2", 1, "VK_EXT_robustness2"},
	{"VK_EXT_custom_border_color", 12, "VK_EXT_custom_border_color"},
	{"VK_EXT_memory_priority", 1, "VK_EXT_memory_priority"},
	{"VK_EXT_non_seamless_cube_map", 1, "VK_EXT_non_seamless_cube_map"},
	{"VK_KHR_load_store_op_none", 1, "VK_KHR_load_store_op_none"},
	{"VK_KHR_maintenance5", 1, "VK_KHR_maintenance5"},
	{"VK_KHR_maintenance6", 1, "VK_KHR_maintenance6"},
	// Version 0 reports the version the driver implements. These add features, properties and commands that the generated marshaling
	// covers; programs that do not ask for them never see a difference.
	{"VK_EXT_vertex_attribute_divisor", 0, "VK_EXT_vertex_attribute_divisor"},
	{"VK_KHR_vertex_attribute_divisor", 0, "VK_KHR_vertex_attribute_divisor"},
	{"VK_EXT_extended_dynamic_state", 0, "VK_EXT_extended_dynamic_state"},
	{"VK_EXT_extended_dynamic_state2", 0, "VK_EXT_extended_dynamic_state2"},
	{"VK_EXT_extended_dynamic_state3", 0, "VK_EXT_extended_dynamic_state3"},
	{"VK_EXT_vertex_input_dynamic_state", 0, "VK_EXT_vertex_input_dynamic_state"},
	{"VK_EXT_line_rasterization", 0, "VK_EXT_line_rasterization"},
	{"VK_KHR_line_rasterization", 0, "VK_KHR_line_rasterization"},
	{"VK_EXT_transform_feedback", 0, "VK_EXT_transform_feedback"},
	{"VK_EXT_conservative_rasterization", 0, "VK_EXT_conservative_rasterization"},
	{"VK_EXT_conditional_rendering", 0, "VK_EXT_conditional_rendering"},
	{"VK_EXT_depth_bias_control", 0, "VK_EXT_depth_bias_control"},
	{"VK_EXT_fragment_shader_interlock", 0, "VK_EXT_fragment_shader_interlock"},
	{"VK_EXT_shader_stencil_export", 0, "VK_EXT_shader_stencil_export"},
	{"VK_EXT_shader_viewport_index_layer", 0, "VK_EXT_shader_viewport_index_layer"},
	{"VK_EXT_shader_image_atomic_int64", 0, "VK_EXT_shader_image_atomic_int64"},
	{"VK_EXT_image_view_min_lod", 0, "VK_EXT_image_view_min_lod"},
	{"VK_EXT_image_sliced_view_of_3d", 0, "VK_EXT_image_sliced_view_of_3d"},
	{"VK_EXT_mutable_descriptor_type", 0, "VK_EXT_mutable_descriptor_type"},
	{"VK_EXT_descriptor_buffer", 0, "VK_EXT_descriptor_buffer"},
	{"VK_VALVE_mutable_descriptor_type", 0, "VK_VALVE_mutable_descriptor_type"},
	{"VK_KHR_push_descriptor", 0, "VK_KHR_push_descriptor"},
	{"VK_EXT_pageable_device_local_memory", 0, "VK_EXT_pageable_device_local_memory"},
	{"VK_EXT_memory_budget", 0, "VK_EXT_memory_budget"},
	{"VK_KHR_pipeline_library", 0, "VK_KHR_pipeline_library"},
	{"VK_EXT_graphics_pipeline_library", 0, "VK_EXT_graphics_pipeline_library"},
	{"VK_KHR_present_id", 0, "VK_KHR_present_id"},
	{"VK_KHR_calibrated_timestamps", 0, "VK_KHR_calibrated_timestamps"},
	{"VK_KHR_fragment_shading_rate", 0, "VK_KHR_fragment_shading_rate"},
	{"VK_KHR_cooperative_matrix", 0, "VK_KHR_cooperative_matrix"},
	{"VK_EXT_dynamic_rendering_unused_attachments", 0, "VK_EXT_dynamic_rendering_unused_attachments"},
	{"VK_KHR_dynamic_rendering_local_read", 0, "VK_KHR_dynamic_rendering_local_read"},
	{"VK_KHR_maintenance7", 0, "VK_KHR_maintenance7"},
	{"VK_KHR_maintenance8", 0, "VK_KHR_maintenance8"},
	{"VK_KHR_maintenance9", 0, "VK_KHR_maintenance9"},
	// Ray tracing: the device commands only (acceleration structures are built on the device, never with host addresses).
	{"VK_KHR_deferred_host_operations", 0, "VK_KHR_deferred_host_operations"},
	{"VK_KHR_acceleration_structure", 0, "VK_KHR_acceleration_structure"},
	{"VK_KHR_ray_tracing_pipeline", 0, "VK_KHR_ray_tracing_pipeline"},
	{"VK_KHR_ray_query", 0, "VK_KHR_ray_query"},
	{"VK_KHR_ray_tracing_maintenance1", 0, "VK_KHR_ray_tracing_maintenance1"},
};

std::vector<VkExtensionProperties> host_instance_extensions()
{
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> result(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, result.data());
	result.resize(count);
	return result;
}

std::vector<VkExtensionProperties> host_device_extensions(VkPhysicalDevice device)
{
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> result(count);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, result.data());
	result.resize(count);
	return result;
}

bool has(const std::vector<VkExtensionProperties> &list, const char *name)
{
	return std::any_of(list.begin(), list.end(), [&](const auto &item) { return !std::strcmp(item.extensionName, name); });
}

const char *surface_extension(HostPlatform platform)
{
	return platform == HostPlatform::xcb ? "VK_KHR_xcb_surface" : "VK_KHR_wayland_surface";
}

// The host extension behind an offer; the Windows surface one depends on the window system.
const char *host_name(const Offer &offer, HostPlatform platform)
{
	return std::strcmp(offer.name, "VK_KHR_win32_surface") ? offer.host : surface_extension(platform);
}

std::vector<VkExtensionProperties> offered(const std::vector<VkExtensionProperties> &host, bool device, HostPlatform platform)
{
	std::vector<VkExtensionProperties> result;
	const auto add = [&](const Offer &offer) {
		if (!has(host, host_name(offer, platform)))
			return;
		VkExtensionProperties entry{};
		std::strncpy(entry.extensionName, offer.name, sizeof entry.extensionName - 1);
		entry.specVersion = offer.version;
		if (!offer.version)
			for (const auto &item : host)
				if (!std::strcmp(item.extensionName, host_name(offer, platform)))
					entry.specVersion = item.specVersion;
		result.push_back(entry);
	};
	if (device)
		for (const auto &offer : device_offers)
			add(offer);
	else
		for (const auto &offer : instance_offers)
			add(offer);
	return result;
}

}

bool VulkanBridge::host_extension_names(const std::vector<std::string> &guest, bool device, std::vector<std::string> &host) const
{
	for (const auto &name : guest) {
		bool found = false;
		for (const auto &offer : device ? std::span<const Offer>(device_offers) : std::span<const Offer>(instance_offers))
			if (name == offer.name) {
				found = true;
				const char *extension = host_name(offer, surface_source.platform);
				if (std::find(host.begin(), host.end(), extension) == host.end())
					host.push_back(extension);
				// A surface type needs the base surface extension next to it.
				if (!device && std::strcmp(extension, "VK_KHR_surface") && std::find(host.begin(), host.end(), "VK_KHR_surface") == host.end())
					host.push_back("VK_KHR_surface");
			}
		if (!found)
			return false;
	}
	return true;
}

bool VulkanBridge::dispatch_enumerate(std::string_view name)
{
	if (std::find(names.begin(), names.end(), name) == names.end())
		return false;
	const bool device = name.find("Device") != std::string_view::npos;
	const bool extensions = name.find("Extension") != std::string_view::npos;
	const auto a = process.api_arguments(device ? (extensions ? 4 : 3) : (extensions ? 3 : 2));
	const auto read = [&]<typename T>(uint64_t address) {
		T result{};
		const auto bytes = process.read_memory(address, sizeof(T));
		std::memcpy(&result, bytes.data(), sizeof(T));
		return result;
	};
	const auto write = [&]<typename T>(uint64_t address, const T &value) {
		process.write_memory(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), sizeof(T)));
	};
	uint64_t result = 0;
	const size_t count_slot = device ? (extensions ? 2 : 1) : (extensions ? 1 : 0);
	const size_t array_slot = count_slot + 1;
	std::vector<VkExtensionProperties> list;
	if (extensions) {
		const uint64_t layer_name = a[device ? 1 : 0];
		if (device && !physical.contains(a[0]))
			throw std::runtime_error("invalid Vulkan physical device handle");
		if (layer_name) {
			// Only the implicit layer-free configuration exists: asking for a layer's extensions names a layer that is not there.
			result = static_cast<uint32_t>(VK_ERROR_LAYER_NOT_PRESENT);
			process.return_api(result);
			return true;
		}
		list = offered(device ? host_device_extensions(physical.at(a[0]).handle) : host_instance_extensions(), device, surface_source.platform);
	} else if (device && !physical.contains(a[0])) {
		throw std::runtime_error("invalid Vulkan physical device handle");
	}
	// Both layers and extensions follow the usual count-then-fill convention.
	const auto size = extensions ? sizeof(VkExtensionProperties) : sizeof(VkLayerProperties);
	process.write_memory(a[count_slot], process.read_memory(a[count_slot], 4));
	const uint32_t total = extensions ? uint32_t(list.size()) : 0;
	if (!a[array_slot]) {
		write(a[count_slot], total);
	} else {
		const auto capacity = read.template operator()<uint32_t>(a[count_slot]);
		if (capacity > 4096)
			throw std::runtime_error("Vulkan extension capacity exceeds limit");
		process.write_memory(a[array_slot], process.read_memory(a[array_slot], size_t(capacity) * size));
		const auto copied = std::min(capacity, total);
		for (uint32_t index = 0; index < copied; ++index)
			write(a[array_slot] + uint64_t(index) * size, list[index]);
		write(a[count_slot], copied);
		if (copied < total)
			result = static_cast<uint32_t>(VK_INCOMPLETE);
	}
	process.return_api(result);
	return true;
}

}
