#include "shared_gpu_memory.h"
#include "cpu/native_cpu.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void shared_file_mapping(raven::NativeProcess &process)
{
	const int file = memfd_create("gpu mapping fixture", MFD_CLOEXEC);
	check(file >= 0 && ftruncate(file, 16384) == 0, "cannot create shared file fixture");
	auto *reserve = static_cast<uint8_t *>(mmap(nullptr, 12288, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	check(reserve != MAP_FAILED, "cannot reserve shared file fixture");
	auto *host = static_cast<uint8_t *>(mmap(reserve + 4096, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, 8192));
	check(host == reserve + 4096, "cannot map shared file fixture");
	host[128] = 41;
	const auto mapping = raven::map_gpu_memory(process, host + 128, 3968);
	check(mapping.has_value() && mapping->address - mapping->base == 128, "shared file backing or offset was not found");
	check(process.read_memory(mapping->address, 1) == std::vector<uint8_t>{41}, "shared file mapping read a different offset");
	process.write_memory(mapping->address, std::vector<uint8_t>{42});
	check(host[128] == 42, "guest write did not reach the original shared file mapping");
	process.unmap_memory(mapping->base);
	check(!raven::map_gpu_memory(process, host + 4090, 16) && !raven::map_gpu_memory(process, host, 0) &&
		!raven::map_gpu_memory(process, host, SIZE_MAX), "invalid shared file range was accepted");
	void *private_view = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 8192);
	check(private_view != MAP_FAILED && !raven::map_gpu_memory(process, private_view, 4096), "private file mapping was shared");
	munmap(private_view, 4096);
	check(ftruncate(file, 8192) == 0 && !raven::map_gpu_memory(process, host, 4096), "truncated backing file was accepted");
	close(file);
	const int decoy = memfd_create("gpu mapping fixture", MFD_CLOEXEC);
	check(decoy >= 0 && ftruncate(decoy, 16384) == 0 && !raven::map_gpu_memory(process, host, 4096),
		"a different file with the same name was accepted");
	close(decoy);
	munmap(reserve, 12288);
}

// Whether the driver's mapping at host is one of a DRM device node, as amdgpu's are, and not of a file or an anonymous inode.
static bool device_node_mapping(const void *host)
{
	const auto address = reinterpret_cast<uintptr_t>(host);
	std::ifstream maps("/proc/self/maps");
	std::string line;
	while (std::getline(maps, line)) {
		uintptr_t first = 0, last = 0;
		if (std::sscanf(line.c_str(), "%" SCNxPTR "-%" SCNxPTR, &first, &last) != 2 || address < first || address >= last)
			continue;
		const auto path = line.find('/');
		return path != std::string::npos && line.compare(path, 9, "/dev/dri/") == 0;
	}
	return false;
}

int main(int argc, char **argv)
{
	try {
		if (argc != 2)
			throw std::runtime_error("bootstrap path required");
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Shared GPU memory fixture", 1, nullptr, 0, VK_API_VERSION_1_1};
		VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &application};
		VkInstance instance;
		check(vkCreateInstance(&instance_info, nullptr, &instance) == VK_SUCCESS, "cannot create Vulkan instance");
		uint32_t count = 1;
		VkPhysicalDevice physical;
		// The first device serves; with more than one installed the enumeration reports the rest as incomplete.
		const auto enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
		check((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) && count, "cannot select Vulkan device");
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physical, &properties);
		std::cout << "Actual GPU: " << properties.deviceName << '\n';
		float priority = 1;
		VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &priority};
		VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queue_info};
		VkDevice device;
		check(vkCreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS, "cannot create Vulkan device");
		VkQueue queue;
		vkGetDeviceQueue(device, 0, 0, &queue);
		VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 4096,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
		VkBuffer buffer;
		check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer) == VK_SUCCESS, "cannot create GPU buffer");
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(device, buffer, &requirements);
		VkPhysicalDeviceMemoryProperties types;
		vkGetPhysicalDeviceMemoryProperties(physical, &types);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < types.memoryTypeCount; ++i)
			if ((requirements.memoryTypeBits & (1u << i)) && (types.memoryTypes[i].propertyFlags &
				(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
				(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
		check(type != UINT32_MAX, "GPU has no coherent host-visible buffer memory");
		const VkDeviceSize offset = (4096 + requirements.alignment - 1) / requirements.alignment * requirements.alignment;
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, offset + requirements.size, type};
		VkDeviceMemory memory;
		check(vkAllocateMemory(device, &allocation, nullptr, &memory) == VK_SUCCESS &&
			vkBindBufferMemory(device, buffer, memory, offset) == VK_SUCCESS, "cannot bind standard GPU allocation");
		void *host;
		check(vkMapMemory(device, memory, offset + 128, 4096 - 128, 0, &host) == VK_SUCCESS, "cannot map non-page-aligned GPU memory");
		VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
		VkCommandPool pool;
		check(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS, "cannot create command pool");
		VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
		VkCommandBuffer command;
		check(vkAllocateCommandBuffers(device, &command_info, &command) == VK_SUCCESS, "cannot allocate command buffer");
		bool direct = false;
		for (const auto mode : {raven::NativeMode::x64, raven::NativeMode::x86}) {
			const std::array<raven::LoadedImage *, 0> images{};
			raven::NativeProcess process(images, argv[1], std::chrono::seconds(2), mode);
			shared_file_mapping(process);
			void *anonymous = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			check(anonymous != MAP_FAILED && !raven::map_gpu_memory(process, anonymous, 4096), "unbacked memory did not retain the fallback path");
			munmap(anonymous, 4096);
			const auto mapping = raven::map_gpu_memory(process, host, 4096 - 128);
			if (!mapping) {
				check(!std::getenv("RAVEN_REQUIRE_GPU_MAPPING"), "required direct GPU mapping is unavailable");
				check(!device_node_mapping(host), "GPU memory mapped through a DRM device node was not shared directly");
				continue;
			}
			direct = true;
			check((mapping->address - mapping->base) % 4096 == 128, "GPU map pointer lost its non-page offset");
			const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
			const std::vector<uint8_t> x64{0x8b, 0x01, 0x83, 0xc0, 0x01, 0x89, 0x01, 0xc3};
			const std::vector<uint8_t> x86{0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x01, 0x83, 0xc0, 0x01, 0x89, 0x01, 0xc2, 0x04, 0x00};
			process.write_memory(code, mode == raven::NativeMode::x64 ? x64 : x86);
			process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
			for (uint32_t phase = 0; phase < 2; ++phase) {
				check(vkResetCommandBuffer(command, 0) == VK_SUCCESS, "cannot reset GPU command buffer");
				VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
				check(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS, "cannot begin GPU commands");
				if (!phase) {
					vkCmdFillBuffer(command, buffer, 0, 4096, 0x12345678);
				} else {
					VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
					vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
					VkBufferCopy copy{128, 256, 4};
					vkCmdCopyBuffer(command, buffer, buffer, 1, &copy);
				}
				VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
				vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
				check(vkEndCommandBuffer(command) == VK_SUCCESS, "cannot end GPU commands");
				VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command};
				check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS, "GPU transfer did not finish");
				if (!phase) {
					const std::array<uint64_t, 1> arguments{mapping->address};
					const auto result = process.call(code, arguments, std::chrono::seconds(2));
					check(result.stop == raven::NativeStop::returned && result.value == 0x12345679, "guest CPU did not read and update actual GPU bytes");
				} else {
					const auto bytes = process.read_memory(mapping->address + 128, 4);
					check(bytes == std::vector<uint8_t>({0x79, 0x56, 0x34, 0x12}), "GPU did not consume guest CPU writes through direct mapping");
				}
			}
			process.unmap_memory(code);
			process.unmap_memory(mapping->base);
		}
		vkDestroyCommandPool(device, pool, nullptr);
		vkUnmapMemory(device, memory);
		vkDestroyBuffer(device, buffer, nullptr);
		vkFreeMemory(device, memory, nullptr);
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
		std::cout << (direct ? "Real GPU and x64/x86 guest CPU shared writes and non-page-aligned mapping passed\n" : "Shared GPU mapping unavailable; fallback selection passed\n");
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
