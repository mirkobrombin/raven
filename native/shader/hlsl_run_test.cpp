// Runs compiled compute shaders on a Vulkan device and checks what they write: each test file names its entry point, the number of
// values its RWStructuredBuffer<uint> u0 receives, and the values expected, as "// run: entry target count" followed by
// "// expect: ..." lines of decimal unsigned integers or floats (written with a decimal point and compared to 1e-4 relative error).

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
			if (properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
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
		if (compiled.failed)
			failure = compiled.messages;
		else {
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
