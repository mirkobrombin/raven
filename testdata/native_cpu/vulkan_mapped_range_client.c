#include <vulkan/vulkan.h>

/* A mapping whose ends are not on non-coherent atoms, and an interior range of whole atoms the program flushes and invalidates by
   itself: it writes the first atom of the range, flushes it, has the device copy it to the second and reads that back after
   invalidating it. The atom is the device's own, and memory that is not host-coherent is preferred, as a program that reads back
   through host-cached memory chooses it. The exit code names the first check that failed. */

static VkPhysicalDevice physical[16];
static VkQueueFamilyProperties families[16];

unsigned int raven_entry(void)
{
	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Vulkan mapped range fixture", 1, 0, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &application};
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t count = 16;
	if (vkCreateInstance(&instance_info, 0, &instance) != VK_SUCCESS ||
		vkEnumeratePhysicalDevices(instance, &count, physical) != VK_SUCCESS || !count)
		return 1;
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(physical[0], &properties);
	const VkDeviceSize atom = properties.limits.nonCoherentAtomSize < 64 ? 64 : properties.limits.nonCoherentAtomSize;
	count = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
		if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
			family = i;
	if (family == UINT32_MAX)
		return 2;
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, family, 1, &priority};
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &queue_info};
	VkDevice device = VK_NULL_HANDLE;
	if (vkCreateDevice(physical[0], &device_info, 0, &device) != VK_SUCCESS)
		return 3;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, family, 0, &queue);

	VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 4 * atom,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(device, &buffer_info, 0, &buffer) != VK_SUCCESS)
		return 4;
	VkMemoryRequirements requirements;
	VkPhysicalDeviceMemoryProperties memory_properties;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	vkGetPhysicalDeviceMemoryProperties(physical[0], &memory_properties);
	uint32_t memory_type = UINT32_MAX;
	for (int coherent = 0; coherent < 2 && memory_type == UINT32_MAX; ++coherent)
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount && memory_type == UINT32_MAX; ++i) {
			const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
			if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
				!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == !coherent)
				memory_type = i;
		}
	if (memory_type == UINT32_MAX)
		return 5;
	/* The allocation ends past the mapping, which ends a quarter atom into one, and starts a quarter atom into the first. */
	const VkDeviceSize size = requirements.size > 4 * atom ? requirements.size : 4 * atom;
	VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, size, memory_type};
	VkDeviceMemory memory = VK_NULL_HANDLE;
	unsigned char *data = 0;
	const VkDeviceSize start = atom / 4;
	if (vkAllocateMemory(device, &allocation, 0, &memory) != VK_SUCCESS || vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
		vkMapMemory(device, memory, start, 3 * atom, 0, (void **)&data) != VK_SUCCESS || !data)
		return 6;
	unsigned char *first = data + (atom - start), *second = data + (2 * atom - start);
	for (VkDeviceSize i = 0; i < atom; ++i) {
		first[i] = (unsigned char)(i * 7 + 3);
		second[i] = 0;
	}
	VkMappedMemoryRange written = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 0, memory, atom, atom};
	if (vkFlushMappedMemoryRanges(device, 1, &written) != VK_SUCCESS)
		return 7;

	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, family};
	VkCommandPool pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(device, &pool_info, 0, &pool) != VK_SUCCESS)
		return 8;
	VkCommandBufferAllocateInfo command_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	if (vkAllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS || vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
		return 9;
	VkBufferCopy copy = {atom, 2 * atom, atom};
	vkCmdCopyBuffer(command, buffer, buffer, 1, &copy);
	VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, 0, 0, 0);
	if (vkEndCommandBuffer(command) != VK_SUCCESS)
		return 10;
	VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence = VK_NULL_HANDLE;
	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &command};
	if (vkCreateFence(device, &fence_info, 0, &fence) != VK_SUCCESS || vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS)
		return 11;
	VkMappedMemoryRange copied = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 0, memory, 2 * atom, atom};
	if (vkInvalidateMappedMemoryRanges(device, 1, &copied) != VK_SUCCESS)
		return 12;
	for (VkDeviceSize i = 0; i < atom; ++i)
		if (second[i] != (unsigned char)(i * 7 + 3))
			return 13;

	vkDestroyFence(device, fence, 0);
	vkDestroyCommandPool(device, pool, 0);
	vkUnmapMemory(device, memory);
	vkDestroyBuffer(device, buffer, 0);
	vkFreeMemory(device, memory, 0);
	vkDestroyDevice(device, 0);
	vkDestroyInstance(instance, 0);
	return 0;
}
