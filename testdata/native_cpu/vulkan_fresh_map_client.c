#include <vulkan/vulkan.h>

/* The first mapping of new host-coherent memory shows zeros, whatever bytes the driver's allocation held: its contents are undefined until
   something writes them, and reading them through a write-combined mapping is slow. Memory the device wrote before its first mapping still
   shows what the device wrote. A zero the program writes into new memory reaches the device. The exit code names the first check that
   failed. */

static VkPhysicalDevice physical[16];
static VkQueueFamilyProperties families[16];
static VkDevice device;
static uint32_t memory_type = UINT32_MAX;

enum { size = 4096 };

static VkBuffer new_buffer(VkDeviceMemory *memory)
{
	VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
	VkBuffer buffer = VK_NULL_HANDLE;
	VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, size, memory_type};
	if (vkCreateBuffer(device, &buffer_info, 0, &buffer) != VK_SUCCESS || vkAllocateMemory(device, &allocation, 0, memory) != VK_SUCCESS ||
		vkBindBufferMemory(device, buffer, *memory, 0) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return buffer;
}

unsigned int raven_entry(void)
{
	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Vulkan fresh map fixture", 1, 0, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &application};
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t count = 16;
	if (vkCreateInstance(&instance_info, 0, &instance) != VK_SUCCESS ||
		vkEnumeratePhysicalDevices(instance, &count, physical) != VK_SUCCESS || !count)
		return 1;
	count = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
		if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
			family = i;
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, family, 1, &priority};
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &queue_info};
	if (family == UINT32_MAX || vkCreateDevice(physical[0], &device_info, 0, &device) != VK_SUCCESS)
		return 2;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, family, 0, &queue);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(physical[0], &properties);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < properties.memoryTypeCount && memory_type == UINT32_MAX; ++i)
		if ((properties.memoryTypes[i].propertyFlags & wanted) == wanted)
			memory_type = i;
	if (memory_type == UINT32_MAX)
		return 3;

	/* Memory the driver held other bytes in: written through a mapping, then freed, so a new allocation of the same size can reuse it. */
	VkDeviceMemory used = VK_NULL_HANDLE;
	unsigned int *words = 0;
	VkBuffer used_buffer = new_buffer(&used);
	if (!used_buffer || vkMapMemory(device, used, 0, size, 0, (void **)&words) != VK_SUCCESS)
		return 4;
	for (int i = 0; i < size / 4; ++i)
		words[i] = 0xeeeeeeeeu;
	vkUnmapMemory(device, used);
	vkDestroyBuffer(device, used_buffer, 0);
	vkFreeMemory(device, used, 0);

	VkDeviceMemory fresh = VK_NULL_HANDLE, filled = VK_NULL_HANDLE;
	VkBuffer fresh_buffer = new_buffer(&fresh), filled_buffer = new_buffer(&filled);
	if (!fresh_buffer || !filled_buffer || vkMapMemory(device, fresh, 0, size, 0, (void **)&words) != VK_SUCCESS)
		return 5;
	for (int i = 0; i < size / 4; ++i)
		if (words[i])
			return 6;
	/* Word 0 stays the zero the mapping showed; word 1 is written. */
	words[1] = 7;

	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, family};
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo command_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	if (vkCreateCommandPool(device, &pool_info, 0, &pool) != VK_SUCCESS)
		return 7;
	command_info.commandPool = pool;
	if (vkAllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS || vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
		return 8;
	/* The device fills the second buffer before anything maps it, then copies the first two words of the new one over it. */
	vkCmdFillBuffer(command, filled_buffer, 0, size, 0x5a5a5a5au);
	VkMemoryBarrier filled_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &filled_barrier, 0, 0, 0, 0);
	VkBufferCopy copy = {0, 0, 8};
	vkCmdCopyBuffer(command, fresh_buffer, filled_buffer, 1, &copy);
	VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, 0, 0, 0);
	if (vkEndCommandBuffer(command) != VK_SUCCESS)
		return 9;
	VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence = VK_NULL_HANDLE;
	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &command};
	if (vkCreateFence(device, &fence_info, 0, &fence) != VK_SUCCESS || vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS)
		return 10;
	unsigned int *read = 0;
	if (vkMapMemory(device, filled, 0, size, 0, (void **)&read) != VK_SUCCESS)
		return 11;
	if (read[0] != 0 || read[1] != 7)
		return 12;
	for (int i = 2; i < size / 4; ++i)
		if (read[i] != 0x5a5a5a5au)
			return 13;

	vkDestroyFence(device, fence, 0);
	vkDestroyCommandPool(device, pool, 0);
	vkUnmapMemory(device, filled);
	vkUnmapMemory(device, fresh);
	vkDestroyBuffer(device, filled_buffer, 0);
	vkDestroyBuffer(device, fresh_buffer, 0);
	vkFreeMemory(device, filled, 0);
	vkFreeMemory(device, fresh, 0);
	vkDestroyDevice(device, 0);
	vkDestroyInstance(instance, 0);
	return 0;
}
