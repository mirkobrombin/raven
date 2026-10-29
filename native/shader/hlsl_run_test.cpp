// Runs compiled compute shaders on a Vulkan device and checks what they write: each test file names its entry point, the number of
// values its RWStructuredBuffer<uint> u0 receives, and the values expected, as "// run: entry target count" followed by
// "// expect: ..." lines of decimal unsigned integers or floats (written with a decimal point and compared to 1e-4 relative error).
// A pixel shader target draws a row of count / 4 pixels into a float RGBA target instead, and the values are those pixels; its input
// TEXCOORD0 holds the pixel's position in normalized device coordinates, after SV_Position on shader model 4. Every texture it
// samples is the same four texels wide row, texel i holding (i + 1, 10 (i + 1), 100 (i + 1), 1), read through a nearest sampler.

#include "hlsl_compile.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace raven::shader;

std::string read_file(const std::filesystem::path &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

struct Expected {
	uint32_t bits = 0;
	bool floating = false;
};

struct Case {
	std::string entry, target;
	uint32_t count = 0;
	std::vector<Expected> values;
};

Case case_of(const std::string &source)
{
	Case item;
	std::istringstream lines(source);
	std::string line;
	while (std::getline(lines, line)) {
		std::istringstream words(line);
		std::string slashes, kind;
		words >> slashes >> kind;
		if (slashes != "//")
			continue;
		if (kind == "run:")
			words >> item.entry >> item.target >> item.count;
		else if (kind == "expect:")
			for (std::string word; words >> word;) {
				Expected value;
				if (word.find_first_of(".en") != std::string::npos) {
					value.floating = true;
					value.bits = std::bit_cast<uint32_t>(word == "nan" ? NAN : word == "inf" ? INFINITY : word == "-inf" ? -INFINITY : std::stof(word));
				} else
					value.bits = word[0] == '-' ? uint32_t(std::stol(word)) : uint32_t(std::stoul(word));
				item.values.push_back(value);
			}
	}
	return item;
}

// The binding vkd3d-shader gave the storage buffer: the first Binding decoration of the module.
uint32_t binding_of(const std::vector<uint32_t> &words)
{
	for (size_t at = 5; at < words.size();) {
		const uint32_t count = words[at] >> 16, opcode = words[at] & 0xffff;
		if (!count)
			break;
		if (opcode == 71 && count >= 4 && words[at + 2] == 33)
			return words[at + 3];
		at += count;
	}
	return 0;
}

struct Binding {
	uint32_t binding = 0;
	bool sampler = false;
};

// The images and samplers vkd3d-shader declared in a module, with their bindings.
std::vector<Binding> bindings_of(const std::vector<uint32_t> &words)
{
	std::vector<uint32_t> binding_ids, binding_values;
	std::vector<std::pair<uint32_t, uint32_t>> pointers, variables;
	std::vector<std::pair<uint32_t, bool>> kinds;
	for (size_t at = 5; at < words.size();) {
		const uint32_t count = words[at] >> 16, opcode = words[at] & 0xffff;
		if (!count)
			break;
		if (opcode == 71 && count >= 4 && words[at + 2] == 33) {
			binding_ids.push_back(words[at + 1]);
			binding_values.push_back(words[at + 3]);
		} else if (opcode == 25 || opcode == 26)
			kinds.emplace_back(words[at + 1], opcode == 26);
		else if (opcode == 32)
			pointers.emplace_back(words[at + 1], words[at + 3]);
		else if (opcode == 59)
			variables.emplace_back(words[at + 2], words[at + 1]);
		at += count;
	}
	std::vector<Binding> found;
	for (size_t i = 0; i < binding_ids.size(); ++i)
		for (const auto &[id, pointer] : variables)
			if (id == binding_ids[i])
				for (const auto &[pointer_id, pointee] : pointers)
					if (pointer_id == pointer)
						for (const auto &[kind_id, sampler] : kinds)
							if (kind_id == pointee)
								found.push_back({binding_values[i], sampler});
	return found;
}

class Device {
public:
	bool open(std::string &problem)
	{
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		application.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		instance_info.pApplicationInfo = &application;
		if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
			problem = "no Vulkan instance";
			return false;
		}
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		if (!count) {
			problem = "no Vulkan device";
			return false;
		}
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance, &count, devices.data());
		physical = devices[0];
		uint32_t families = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
		std::vector<VkQueueFamilyProperties> properties(families);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, properties.data());
		for (uint32_t i = 0; i < families; ++i)
			if ((properties[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) {
				family = i;
				break;
			}
		const float priority = 1;
		VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queue_info.queueFamilyIndex = family;
		queue_info.queueCount = 1;
		queue_info.pQueuePriorities = &priority;
		VkPhysicalDeviceFeatures features{};
		vkGetPhysicalDeviceFeatures(physical, &features);
		VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		device_info.queueCreateInfoCount = 1;
		device_info.pQueueCreateInfos = &queue_info;
		device_info.pEnabledFeatures = &features;
		if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) {
			problem = "no Vulkan logical device";
			return false;
		}
		vkGetDeviceQueue(device, family, 0, &queue);
		return true;
	}

	// Runs one dispatch of a single workgroup and returns the buffer's contents.
	bool run(const std::vector<uint32_t> &spirv, uint32_t count, std::vector<uint32_t> &result, std::string &problem)
	{
		const VkDeviceSize size = VkDeviceSize(count) * 4;
		VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer_info.size = size;
		buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		VkBuffer buffer;
		vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
		VkMemoryRequirements needs;
		vkGetBufferMemoryRequirements(device, buffer, &needs);
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
		uint32_t type = 0;
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
			if ((needs.memoryTypeBits & (1u << i)) &&
				(memory_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
					(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
				type = i;
				break;
			}
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = needs.size;
		allocate.memoryTypeIndex = type;
		VkDeviceMemory memory;
		vkAllocateMemory(device, &allocate, nullptr, &memory);
		vkBindBufferMemory(device, buffer, memory, 0);
		void *mapped = nullptr;
		vkMapMemory(device, memory, 0, size, 0, &mapped);
		std::memset(mapped, 0xcd, size_t(size));

		VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		module_info.codeSize = spirv.size() * 4;
		module_info.pCode = spirv.data();
		VkShaderModule module;
		if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS) {
			problem = "the SPIR-V module was refused";
			return false;
		}
		VkDescriptorSetLayoutBinding binding{};
		binding.binding = binding_of(spirv);
		binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		set_info.bindingCount = 1;
		set_info.pBindings = &binding;
		VkDescriptorSetLayout set_layout;
		vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
		VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &set_layout;
		VkPipelineLayout layout;
		vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
		VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pipeline_info.stage.module = module;
		pipeline_info.stage.pName = "main";
		pipeline_info.layout = layout;
		VkPipeline pipeline;
		if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
			problem = "the compute pipeline was refused";
			return false;
		}
		VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
		VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pool_info.maxSets = 1;
		pool_info.poolSizeCount = 1;
		pool_info.pPoolSizes = &pool_size;
		VkDescriptorPool pool;
		vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
		VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		set_allocate.descriptorPool = pool;
		set_allocate.descriptorSetCount = 1;
		set_allocate.pSetLayouts = &set_layout;
		VkDescriptorSet set;
		vkAllocateDescriptorSets(device, &set_allocate, &set);
		VkDescriptorBufferInfo described{buffer, 0, size};
		VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		write.dstSet = set;
		write.dstBinding = binding.binding;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo = &described;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

		VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		command_pool_info.queueFamilyIndex = family;
		VkCommandPool command_pool;
		vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool);
		VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		command_info.commandPool = command_pool;
		command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		command_info.commandBufferCount = 1;
		VkCommandBuffer commands;
		vkAllocateCommandBuffers(device, &command_info, &commands);
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(commands, &begin);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
		vkCmdDispatch(commands, 1, 1, 1);
		vkEndCommandBuffer(commands);
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(queue);
		result.assign(static_cast<uint32_t *>(mapped), static_cast<uint32_t *>(mapped) + count);

		vkDestroyCommandPool(device, command_pool, nullptr);
		vkDestroyDescriptorPool(device, pool, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, layout, nullptr);
		vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		vkDestroyShaderModule(device, module, nullptr);
		vkUnmapMemory(device, memory);
		vkDestroyBuffer(device, buffer, nullptr);
		vkFreeMemory(device, memory, nullptr);
		return true;
	}

	uint32_t host_memory(uint32_t bits) const
	{
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
		const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
			if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted)
				return i;
		return 0;
	}

	uint32_t device_memory(uint32_t bits) const
	{
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
			if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				return i;
		return host_memory(bits);
	}

	VkShaderModule module_of(const std::vector<uint32_t> &spirv)
	{
		VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		module_info.codeSize = spirv.size() * 4;
		module_info.pCode = spirv.data();
		VkShaderModule module = VK_NULL_HANDLE;
		vkCreateShaderModule(device, &module_info, nullptr, &module);
		return module;
	}

	// Draws a triangle over a row of pixels with the two shaders and returns the pixels as four floats each.
	bool draw(const std::vector<uint32_t> &vertex_spirv, const std::vector<uint32_t> &pixel_spirv, uint32_t width, std::vector<uint32_t> &result,
		std::string &problem)
	{
		const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
		VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = format;
		image_info.extent = {width, 1, 1};
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VkImage image;
		vkCreateImage(device, &image_info, nullptr, &image);
		VkMemoryRequirements image_needs;
		vkGetImageMemoryRequirements(device, image, &image_needs);
		VkMemoryAllocateInfo image_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		image_allocate.allocationSize = image_needs.size;
		image_allocate.memoryTypeIndex = device_memory(image_needs.memoryTypeBits);
		VkDeviceMemory image_memory;
		vkAllocateMemory(device, &image_allocate, nullptr, &image_memory);
		vkBindImageMemory(device, image, image_memory, 0);
		VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view_info.image = image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = format;
		view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view;
		vkCreateImageView(device, &view_info, nullptr, &view);

		// The vertices, and the buffer the pixels are copied to.
		const float vertices[] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
		const auto host_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buffer, VkDeviceMemory &memory, void *&mapped) {
			VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			buffer_info.size = size;
			buffer_info.usage = usage;
			vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
			VkMemoryRequirements needs;
			vkGetBufferMemoryRequirements(device, buffer, &needs);
			VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.allocationSize = needs.size;
			allocate.memoryTypeIndex = host_memory(needs.memoryTypeBits);
			vkAllocateMemory(device, &allocate, nullptr, &memory);
			vkBindBufferMemory(device, buffer, memory, 0);
			vkMapMemory(device, memory, 0, size, 0, &mapped);
		};
		VkBuffer vertex_buffer, pixel_buffer;
		VkDeviceMemory vertex_memory, pixel_memory;
		void *vertex_mapped = nullptr, *pixel_mapped = nullptr;
		host_buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_memory, vertex_mapped);
		std::memcpy(vertex_mapped, vertices, sizeof(vertices));
		const VkDeviceSize pixel_size = VkDeviceSize(width) * 16;
		host_buffer(pixel_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, pixel_buffer, pixel_memory, pixel_mapped);
		std::memset(pixel_mapped, 0xcd, size_t(pixel_size));

		VkAttachmentDescription attachment{};
		attachment.format = format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &reference;
		VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass_info.attachmentCount = 1;
		pass_info.pAttachments = &attachment;
		pass_info.subpassCount = 1;
		pass_info.pSubpasses = &subpass;
		VkRenderPass pass;
		vkCreateRenderPass(device, &pass_info, nullptr, &pass);
		VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebuffer_info.renderPass = pass;
		framebuffer_info.attachmentCount = 1;
		framebuffer_info.pAttachments = &view;
		framebuffer_info.width = width;
		framebuffer_info.height = 1;
		framebuffer_info.layers = 1;
		VkFramebuffer framebuffer;
		vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer);

		const VkShaderModule vertex_module = module_of(vertex_spirv), pixel_module = module_of(pixel_spirv);
		if (!vertex_module || !pixel_module) {
			problem = "a SPIR-V module was refused";
			return false;
		}
		VkPipelineShaderStageCreateInfo stages[2]{};
		for (auto &stage : stages) {
			stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stage.pName = "main";
		}
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertex_module;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = pixel_module;
		VkVertexInputBindingDescription vertex_binding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
		VkVertexInputAttributeDescription vertex_attribute{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
		VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vertex_input.vertexBindingDescriptionCount = 1;
		vertex_input.pVertexBindingDescriptions = &vertex_binding;
		vertex_input.vertexAttributeDescriptionCount = 1;
		vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
		VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkViewport viewport{0, 0, float(width), 1, 0, 1};
		VkRect2D scissor{{0, 0}, {width, 1}};
		VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		viewport_state.viewportCount = 1;
		viewport_state.pViewports = &viewport;
		viewport_state.scissorCount = 1;
		viewport_state.pScissors = &scissor;
		VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.lineWidth = 1;
		VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blend_attachment{};
		blend_attachment.colorWriteMask = 0xf;
		VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		blend.attachmentCount = 1;
		blend.pAttachments = &blend_attachment;
		// The texture and sampler, when the pixel shader samples one.
		const std::vector<Binding> bindings = bindings_of(pixel_spirv);
		const uint32_t texels = 4;
		VkImage texture = VK_NULL_HANDLE;
		VkDeviceMemory texture_memory = VK_NULL_HANDLE;
		VkImageView texture_view = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkBuffer texel_buffer = VK_NULL_HANDLE;
		VkDeviceMemory texel_memory = VK_NULL_HANDLE;
		VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		if (!bindings.empty()) {
			image_info.extent = {texels, 1, 1};
			image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			vkCreateImage(device, &image_info, nullptr, &texture);
			VkMemoryRequirements needs;
			vkGetImageMemoryRequirements(device, texture, &needs);
			VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.allocationSize = needs.size;
			allocate.memoryTypeIndex = device_memory(needs.memoryTypeBits);
			vkAllocateMemory(device, &allocate, nullptr, &texture_memory);
			vkBindImageMemory(device, texture, texture_memory, 0);
			view_info.image = texture;
			vkCreateImageView(device, &view_info, nullptr, &texture_view);
			VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
			sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			vkCreateSampler(device, &sampler_info, nullptr, &sampler);
			void *texel_mapped = nullptr;
			host_buffer(texels * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, texel_buffer, texel_memory, texel_mapped);
			for (uint32_t i = 0; i < texels; ++i) {
				const float texel[] = {float(i + 1), float(10 * (i + 1)), float(100 * (i + 1)), 1};
				std::memcpy(static_cast<char *>(texel_mapped) + i * 16, texel, 16);
			}
			std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
			for (const auto &binding : bindings) {
				VkDescriptorSetLayoutBinding entry{};
				entry.binding = binding.binding;
				entry.descriptorType = binding.sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				entry.descriptorCount = 1;
				entry.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
				layout_bindings.push_back(entry);
			}
			VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			set_info.bindingCount = uint32_t(layout_bindings.size());
			set_info.pBindings = layout_bindings.data();
			vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
			const VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16}};
			VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pool_info.maxSets = 1;
			pool_info.poolSizeCount = 2;
			pool_info.pPoolSizes = pool_sizes;
			vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
			VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			set_allocate.descriptorPool = pool;
			set_allocate.descriptorSetCount = 1;
			set_allocate.pSetLayouts = &set_layout;
			vkAllocateDescriptorSets(device, &set_allocate, &set);
			const VkDescriptorImageInfo image_described{sampler, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			std::vector<VkWriteDescriptorSet> writes;
			for (const auto &entry : layout_bindings) {
				VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				write.dstSet = set;
				write.dstBinding = entry.binding;
				write.descriptorCount = 1;
				write.descriptorType = entry.descriptorType;
				write.pImageInfo = &image_described;
				writes.push_back(write);
			}
			vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
		}
		VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layout_info.setLayoutCount = set_layout ? 1 : 0;
		layout_info.pSetLayouts = &set_layout;
		VkPipelineLayout layout;
		vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
		VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pipeline_info.stageCount = 2;
		pipeline_info.pStages = stages;
		pipeline_info.pVertexInputState = &vertex_input;
		pipeline_info.pInputAssemblyState = &assembly;
		pipeline_info.pViewportState = &viewport_state;
		pipeline_info.pRasterizationState = &raster;
		pipeline_info.pMultisampleState = &multisample;
		pipeline_info.pColorBlendState = &blend;
		pipeline_info.layout = layout;
		pipeline_info.renderPass = pass;
		VkPipeline pipeline;
		if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
			problem = "the graphics pipeline was refused";
			return false;
		}

		VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		command_pool_info.queueFamilyIndex = family;
		VkCommandPool command_pool;
		vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool);
		VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		command_info.commandPool = command_pool;
		command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		command_info.commandBufferCount = 1;
		VkCommandBuffer commands;
		vkAllocateCommandBuffers(device, &command_info, &commands);
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(commands, &begin);
		if (texture) {
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = texture;
			barrier.subresourceRange = view_info.subresourceRange;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferImageCopy upload{};
			upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			upload.imageExtent = {texels, 1, 1};
			vkCmdCopyBufferToImage(commands, texel_buffer, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		VkClearValue clear{};
		VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass_begin.renderPass = pass;
		pass_begin.framebuffer = framebuffer;
		pass_begin.renderArea = scissor;
		pass_begin.clearValueCount = 1;
		pass_begin.pClearValues = &clear;
		vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		if (set)
			vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commands, 0, 1, &vertex_buffer, &offset);
		vkCmdDraw(commands, 3, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {width, 1, 1};
		vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixel_buffer, 1, &copy);
		vkEndCommandBuffer(commands);
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(queue);
		result.assign(static_cast<uint32_t *>(pixel_mapped), static_cast<uint32_t *>(pixel_mapped) + width * 4);

		vkDestroyCommandPool(device, command_pool, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, layout, nullptr);
		if (set_layout) {
			vkDestroyDescriptorPool(device, pool, nullptr);
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
			vkDestroySampler(device, sampler, nullptr);
			vkDestroyImageView(device, texture_view, nullptr);
			vkDestroyImage(device, texture, nullptr);
			vkFreeMemory(device, texture_memory, nullptr);
			vkUnmapMemory(device, texel_memory);
			vkDestroyBuffer(device, texel_buffer, nullptr);
			vkFreeMemory(device, texel_memory, nullptr);
		}
		vkDestroyShaderModule(device, vertex_module, nullptr);
		vkDestroyShaderModule(device, pixel_module, nullptr);
		vkDestroyFramebuffer(device, framebuffer, nullptr);
		vkDestroyRenderPass(device, pass, nullptr);
		vkDestroyImageView(device, view, nullptr);
		vkDestroyImage(device, image, nullptr);
		vkFreeMemory(device, image_memory, nullptr);
		for (auto [buffer, memory] : {std::pair{vertex_buffer, vertex_memory}, std::pair{pixel_buffer, pixel_memory}}) {
			vkUnmapMemory(device, memory);
			vkDestroyBuffer(device, buffer, nullptr);
			vkFreeMemory(device, memory, nullptr);
		}
		return true;
	}

	~Device()
	{
		if (device)
			vkDestroyDevice(device, nullptr);
		if (instance)
			vkDestroyInstance(instance, nullptr);
	}

private:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
};

bool matches(const Expected &expected, uint32_t got)
{
	if (!expected.floating)
		return expected.bits == got;
	const float want = std::bit_cast<float>(expected.bits), have = std::bit_cast<float>(got);
	if (std::isnan(want))
		return std::isnan(have);
	if (std::isinf(want))
		return want == have;
	return std::fabs(want - have) <= 1e-4f * std::max(1.0f, std::fabs(want));
}

std::string shown(uint32_t bits, bool floating)
{
	std::ostringstream text;
	if (floating)
		text << std::bit_cast<float>(bits);
	else
		text << bits;
	return text.str();
}

}

int main(int argc, char **argv)
{
	if (argc != 3) {
		std::cerr << "usage: hlsl_run_test shader-directory work-directory\n";
		return 2;
	}
	const std::filesystem::path directory = argv[1], work = argv[2];
	std::filesystem::create_directories(work);
	if (std::system("command -v vkd3d-compiler >/dev/null 2>&1") != 0) {
		std::cout << "vkd3d-compiler is missing: nothing to run\n";
		return 0;
	}
	Device device;
	std::string problem;
	if (!device.open(problem)) {
		std::cout << problem << ": nothing to run\n";
		return 0;
	}
	std::vector<std::filesystem::path> files;
	for (const auto &entry : std::filesystem::directory_iterator(directory))
		if (entry.path().extension() == ".hlsl")
			files.push_back(entry.path());
	std::sort(files.begin(), files.end());
	int failures = 0, passed = 0;
	for (const auto &file : files) {
		const std::string source = read_file(file);
		const Case item = case_of(source);
		const std::string label = file.filename().string();
		CompileRequest request;
		request.source = source;
		request.name = label;
		request.entry = item.entry;
		request.target = item.target;
		const auto compiled = compile(request);
		std::string failure;
		std::vector<uint32_t> result;
		const bool pixel = item.target.starts_with("ps_");
		if (compiled.failed)
			failure = compiled.messages;
		else if (pixel) {
			// The vertex shader of the same shader model passes the triangle's corners through.
			const bool legacy = item.target[3] < '4';
			CompileRequest corners;
			// The position goes out as TEXCOORD0 too, a value that differs from pixel to pixel. vkd3d links the stages by register, so
			// TEXCOORD0 takes the register the pixel shader reads it from.
			corners.source = std::string(legacy ? "struct Corner { float4 place : TEXCOORD0; float4 position : SV_Position; };\n"
												: "struct Corner { float4 position : SV_Position; float4 place : TEXCOORD0; };\n") +
				"Corner main(float4 position : POSITION) { Corner corner; corner.position = position; corner.place = position; return corner; }";
			corners.name = "corners";
			corners.entry = "main";
			corners.target = legacy ? (item.target[3] == '3' ? "vs_3_0" : "vs_2_0") : "vs_4_0";
			const auto vertex = compile(corners);
			const std::string kind = legacy ? "d3dbc" : "dxbc-tpf";
			std::vector<uint32_t> spirv[2];
			const CompileResult *programs[2] = {&vertex, &compiled};
			for (int i = 0; i < 2 && failure.empty(); ++i) {
				const auto input = work / (file.stem().string() + (i ? ".ps" : ".vs")), output = work / (file.stem().string() + (i ? ".ps.spv" : ".vs.spv"));
				std::ofstream(input, std::ios::binary).write(reinterpret_cast<const char *>(programs[i]->code.data()),
					std::streamsize(programs[i]->code.size()));
				const std::string command = "vkd3d-compiler -x " + kind + " -o '" + output.string() + "' '" + input.string() + "'";
				if (programs[i]->failed || std::system(command.c_str()) != 0)
					failure = "vkd3d-compiler rejected it";
				else {
					const std::string bytes = read_file(output);
					spirv[i].resize(bytes.size() / 4);
					std::memcpy(spirv[i].data(), bytes.data(), spirv[i].size() * 4);
				}
			}
			if (failure.empty() && device.draw(spirv[0], spirv[1], item.count / 4, result, failure)) {
				if (item.values.size() != item.count)
					failure = "the test expects " + std::to_string(item.values.size()) + " values for " + std::to_string(item.count);
				for (size_t i = 0; i < item.values.size() && i < result.size(); ++i)
					if (!matches(item.values[i], result[i]))
						failure += "value " + std::to_string(i) + ": expected " + shown(item.values[i].bits, item.values[i].floating) + ", got " +
							shown(result[i], item.values[i].floating) + "; ";
			}
		} else {
			const auto dxbc = work / (file.stem().string() + ".dxbc"), spirv = work / (file.stem().string() + ".spv");
			std::ofstream(dxbc, std::ios::binary).write(reinterpret_cast<const char *>(compiled.code.data()), std::streamsize(compiled.code.size()));
			const std::string command = "vkd3d-compiler -x dxbc-tpf --buffer-uav=storage-buffer -o '" + spirv.string() + "' '" + dxbc.string() + "'";
			if (std::system(command.c_str()) != 0)
				failure = "vkd3d-compiler rejected it";
			else {
				const std::string bytes = read_file(spirv);
				std::vector<uint32_t> words(bytes.size() / 4);
				std::memcpy(words.data(), bytes.data(), words.size() * 4);
				if (device.run(words, item.count, result, failure)) {
					if (item.values.size() != item.count)
						failure = "the test expects " + std::to_string(item.values.size()) + " values for " + std::to_string(item.count);
					for (size_t i = 0; i < item.values.size() && i < result.size(); ++i)
						if (!matches(item.values[i], result[i]))
							failure += "value " + std::to_string(i) + ": expected " + shown(item.values[i].bits, item.values[i].floating) + ", got " +
								shown(result[i], item.values[i].floating) + "; ";
				}
			}
		}
		if (failure.empty()) {
			++passed;
			continue;
		}
		++failures;
		std::cout << "FAIL " << label << ": " << failure << "\n";
	}
	std::cout << passed << " passed, " << failures << " failed\n";
	return failures ? 1 : 0;
}
