#include "vulkan_marshal.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace raven {

size_t Marshal::capture_descriptor_size(VkObjectType type) const
{
	VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &descriptor};
	const auto physical = bridge.devices.at(device_token).physical;
	vkGetPhysicalDeviceProperties2(bridge.physical.at(physical).handle, &properties);
	switch (type) {
	case VK_OBJECT_TYPE_BUFFER: return descriptor.bufferCaptureReplayDescriptorDataSize;
	case VK_OBJECT_TYPE_IMAGE: return descriptor.imageCaptureReplayDescriptorDataSize;
	case VK_OBJECT_TYPE_IMAGE_VIEW: return descriptor.imageViewCaptureReplayDescriptorDataSize;
	case VK_OBJECT_TYPE_SAMPLER: return descriptor.samplerCaptureReplayDescriptorDataSize;
	case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return descriptor.accelerationStructureCaptureReplayDescriptorDataSize;
	default: throw std::runtime_error("invalid Vulkan capture descriptor object type");
	}
}

void Marshal::context(H type, uint64_t token)
{
	switch (type) {
	case H::VkDevice: {
		const auto found = bridge.devices.find(token);
		if (found == bridge.devices.end())
			throw std::runtime_error("invalid Vulkan device handle");
		device_token = token;
		host_device = found->second.handle;
		break;
	}
	case H::VkQueue: {
		const auto found = bridge.queues.find(token);
		if (found == bridge.queues.end())
			throw std::runtime_error("invalid Vulkan queue handle");
		context(H::VkDevice, found->second.device);
		break;
	}
	case H::VkCommandBuffer: {
		const auto found = bridge.command_buffers.find(token);
		if (found == bridge.command_buffers.end())
			throw std::runtime_error("invalid Vulkan command buffer handle");
		context(H::VkDevice, found->second.device);
		break;
	}
	case H::VkInstance: {
		const auto found = bridge.instances.find(token);
		if (found == bridge.instances.end())
			throw std::runtime_error("invalid Vulkan instance handle");
		host_instance = found->second;
		break;
	}
	case H::VkPhysicalDevice: {
		const auto found = bridge.physical.find(token);
		if (found == bridge.physical.end())
			throw std::runtime_error("invalid Vulkan physical device handle");
		context(H::VkInstance, found->second.instance);
		break;
	}
	default:
		break;
	}
}

std::optional<VulkanBridge::Kind> Marshal::kind_of(H type)
{
	using Kind = VulkanBridge::Kind;
	switch (type) {
	case H::VkShaderModule: return Kind::shader;
	case H::VkDescriptorSetLayout: return Kind::set_layout;
	case H::VkPipelineLayout: return Kind::pipeline_layout;
	case H::VkPipeline: return Kind::pipeline;
	case H::VkDescriptorPool: return Kind::pool;
	case H::VkDescriptorSet: return Kind::set;
	case H::VkImage: return Kind::image;
	case H::VkImageView: return Kind::image_view;
	case H::VkSemaphore: return Kind::semaphore;
	case H::VkRenderPass: return Kind::render_pass;
	case H::VkFramebuffer: return Kind::framebuffer;
	case H::VkSampler: return Kind::sampler;
	default: return std::nullopt;
	}
}

uint64_t Marshal::host(H type, uint64_t token) const
{
	if (!token)
		return 0;
	const auto fail = [&]() -> uint64_t { throw std::runtime_error("invalid Vulkan handle for type " + std::to_string(static_cast<unsigned>(type))); };
	switch (type) {
	case H::VkInstance: { const auto f = bridge.instances.find(token); return f == bridge.instances.end() ? fail() : reinterpret_cast<uint64_t>(f->second); }
	case H::VkPhysicalDevice: { const auto f = bridge.physical.find(token); return f == bridge.physical.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkDevice: { const auto f = bridge.devices.find(token); return f == bridge.devices.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkQueue: { const auto f = bridge.queues.find(token); return f == bridge.queues.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkCommandBuffer: { const auto f = bridge.command_buffers.find(token); return f == bridge.command_buffers.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkCommandPool: { const auto f = bridge.pools.find(token); return f == bridge.pools.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkBuffer: { const auto f = bridge.buffers.find(token); return f == bridge.buffers.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkDeviceMemory: { const auto f = bridge.memories.find(token); return f == bridge.memories.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	case H::VkFence: { const auto f = bridge.fences.find(token); return f == bridge.fences.end() ? fail() : reinterpret_cast<uint64_t>(f->second.handle); }
	default: break;
	}
	if (kind_of(type)) {
		const auto f = bridge.objects.find(token);
		if (f == bridge.objects.end() || f->second.kind != *kind_of(type))
			return fail();
		return f->second.handle;
	}
	const auto f = bridge.generic.find(token);
	if (f == bridge.generic.end() || f->second.type != static_cast<uint8_t>(type))
		return fail();
	return f->second.host;
}

uint64_t Marshal::new_token(H type, uint64_t host_handle)
{
	if (!host_handle)
		return 0;
	const auto token_value = bridge.next_handle++;
	if (type == H::VkQueue) {
		bridge.queues.emplace(token_value, VulkanBridge::Queue{reinterpret_cast<VkQueue>(host_handle), device_token, 0, 0});
	} else if (type == H::VkBuffer) {
		bridge.buffers.emplace(token_value, VulkanBridge::Buffer{reinterpret_cast<VkBuffer>(host_handle), device_token});
	} else if (type == H::VkCommandPool) {
		bridge.pools.emplace(token_value, VulkanBridge::Pool{reinterpret_cast<VkCommandPool>(host_handle), device_token});
	} else if (type == H::VkFence) {
		bridge.fences.emplace(token_value, VulkanBridge::Fence{reinterpret_cast<VkFence>(host_handle), device_token});
	} else if (const auto kind = kind_of(type)) {
		bridge.objects.emplace(token_value, VulkanBridge::Object{host_handle, device_token, *kind});
	} else {
		bridge.generic.emplace(token_value, VulkanBridge::Generic{host_handle, static_cast<uint8_t>(type), device_token});
	}
	return token_value;
}

uint64_t Marshal::swapchain_image_token(H, uint64_t host_handle)
{
	if (!host_handle)
		return 0;
	for (const auto &[key, object] : bridge.objects)
		if (object.handle == host_handle && object.kind == VulkanBridge::Kind::image && object.borrowed)
			return key;
	const auto token_value = bridge.next_handle++;
	bridge.objects.emplace(token_value, VulkanBridge::Object{host_handle, device_token, VulkanBridge::Kind::image, 0, true});
	return token_value;
}

VkDescriptorImageInfo *Marshal::descriptor_image_infos(uint64_t guest, size_t count, VkDescriptorType type)
{
	auto *result = make<VkDescriptorImageInfo>(count);
	const bool uses_sampler = type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	const bool uses_view = type != VK_DESCRIPTOR_TYPE_SAMPLER;
	for (size_t index = 0; index < count; ++index) {
		auto info = load<VkDescriptorImageInfo>(guest + index * sizeof(VkDescriptorImageInfo));
		try {
			info.sampler = uses_sampler ? reinterpret_cast<VkSampler>(host(H::VkSampler, reinterpret_cast<uint64_t>(info.sampler))) : VK_NULL_HANDLE;
			info.imageView = uses_view ? reinterpret_cast<VkImageView>(host(H::VkImageView, reinterpret_cast<uint64_t>(info.imageView))) : VK_NULL_HANDLE;
		} catch (const std::runtime_error &error) {
			throw std::runtime_error(std::string(error.what()) + " in image descriptor " + std::to_string(index) + " of type " + std::to_string(int(type)) +
				" (sampler " + std::to_string(reinterpret_cast<uint64_t>(info.sampler)) + ", view " + std::to_string(reinterpret_cast<uint64_t>(info.imageView)) + ")");
		}
		result[index] = info;
	}
	return result;
}

uint64_t Marshal::token(H type, uint64_t host_handle)
{
	if (!host_handle)
		return 0;
	if (type == H::VkQueue) {
		for (const auto &[key, queue] : bridge.queues)
			if (reinterpret_cast<uint64_t>(queue.handle) == host_handle)
				return key;
	} else if (type == H::VkBuffer || type == H::VkCommandPool || type == H::VkFence || type == H::VkDeviceMemory) {
		throw std::runtime_error("a handle of this type is never returned by the driver");
	} else if (const auto kind = kind_of(type)) {
		for (const auto &[key, object] : bridge.objects)
			if (object.handle == host_handle && object.kind == *kind)
				return key;
	} else {
		for (const auto &[key, entry] : bridge.generic)
			if (entry.host == host_handle && entry.type == static_cast<uint8_t>(type))
				return key;
	}
	return new_token(type, host_handle);
}

void Marshal::forget(uint64_t token_value)
{
	if (const auto surface = bridge.surface_windows.find(token_value); surface != bridge.surface_windows.end()) {
		const auto window = surface->second;
		bridge.surface_windows.erase(surface);
		// A program may make several surfaces over one window; they all render to the window's one host surface, which goes with the last.
		const bool shared = std::any_of(bridge.surface_windows.begin(), bridge.surface_windows.end(),
			[window](const auto &entry) { return entry.second == window; });
		if (bridge.surface_source.release && !shared)
			bridge.surface_source.release(window);
		bridge.surface_instances.erase(token_value);
		bridge.foreign_surfaces.erase(token_value);
	}
	bridge.templates.erase(token_value);
	bridge.foreign_swapchains.erase(token_value);
	bridge.swapchains.erase(token_value);
	bridge.generic.erase(token_value);
	bridge.objects.erase(token_value);
	bridge.buffers.erase(token_value);
	bridge.fences.erase(token_value);
	bridge.pools.erase(token_value);
}

void Marshal::read_into(uint64_t address, void *target, size_t size) const
{
	const auto bytes = process.read_memory(address, size);
	std::memcpy(target, bytes.data(), size);
}

void Marshal::write(uint64_t address, const void *source, size_t size) const
{
	process.write_memory(address, std::span<const uint8_t>(static_cast<const uint8_t *>(source), size));
}

void Marshal::check_writable(uint64_t address, size_t size) const
{
	process.write_memory(address, process.read_memory(address, size));
}

std::string Marshal::read_string(uint64_t address) const
{
	std::string result;
	for (size_t index = 0; index < 4096; ++index) {
		const auto byte = process.read_memory(address + index, 1)[0];
		if (!byte)
			return result;
		result.push_back(static_cast<char>(byte));
	}
	throw std::runtime_error("invalid Vulkan guest string");
}

const char *Marshal::string(const std::string &value)
{
	auto *copy = make<char>(value.size() + 1);
	std::memcpy(copy, value.c_str(), value.size() + 1);
	return copy;
}

const char *const *Marshal::string_array(uint64_t address, size_t count)
{
	if (count > 4096)
		throw std::runtime_error("Vulkan string array is too long");
	auto *result = make<const char *>(count);
	for (size_t index = 0; index < count; ++index)
		result[index] = string(read_string(load<uint64_t>(address + index * 8)));
	return result;
}

void *Marshal::copy_bytes(uint64_t address, size_t size)
{
	if (size > (64U << 20))
		throw std::runtime_error("Vulkan data block is too large");
	auto *copy = make<uint8_t>(size);
	if (size)
		read_into(address, copy, size);
	return copy;
}

void Marshal::require_null(uint64_t pointer) const
{
	if (pointer)
		throw std::runtime_error("Vulkan guest allocation callbacks are not implemented");
}

float Marshal::float_argument(const std::array<uint64_t, 4> &registers, const std::vector<uint64_t> &words, size_t index) const
{
	const uint32_t bits = static_cast<uint32_t>(index < 4 ? registers[index] : words[index]);
	float value;
	std::memcpy(&value, &bits, 4);
	return value;
}

double Marshal::double_argument(const std::array<uint64_t, 4> &registers, const std::vector<uint64_t> &words, size_t index) const
{
	const uint64_t bits = index < 4 ? registers[index] : words[index];
	double value;
	std::memcpy(&value, &bits, 8);
	return value;
}

}
