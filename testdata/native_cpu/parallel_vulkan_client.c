#include <windows.h>
#include <vulkan/vulkan.h>

// Threads that record Vulkan command buffers at once, each from a command pool of its own, as a renderer's worker threads do: what each
// recording call costs, and whether the runtime answers it beside the turn, is what RAVEN_CYCLES reports. Each thread fills a range of one
// buffer; the main thread submits the last recording of every thread and reads the buffer back.

#define THREADS 4
#define ROUNDS 300
#define RANGE 256

static VkDevice device;
static VkBuffer buffer;
static uint32_t family;
static VkCommandPool pools[THREADS];
static VkCommandBuffer commands[THREADS];
static HANDLE start;
static volatile LONG ready;

static DWORD WINAPI record(void *parameter)
{
	const int index = (int)(INT_PTR)parameter;
	const VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
	if (vkCreateCommandPool(device, &pool_info, NULL, &pools[index]) != VK_SUCCESS)
		return 10;
	const VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pools[index], VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	if (vkAllocateCommandBuffers(device, &allocate, &commands[index]) != VK_SUCCESS)
		return 11;
	InterlockedIncrement(&ready);
	if (WaitForSingleObject(start, 5000) != WAIT_OBJECT_0)
		return 12;
	const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
	const VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	for (unsigned round = 0; round < ROUNDS; ++round) {
		if (vkResetCommandBuffer(commands[index], 0) != VK_SUCCESS || vkBeginCommandBuffer(commands[index], &begin) != VK_SUCCESS)
			return 13;
		vkCmdFillBuffer(commands[index], buffer, (VkDeviceSize)index * RANGE, RANGE, 0x1000u * (index + 1) + round);
		vkCmdPipelineBarrier(commands[index], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
		if (vkEndCommandBuffer(commands[index]) != VK_SUCCESS)
			return 14;
	}
	return 0;
}

unsigned int raven_entry(void)
{
	const VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "parallel", 1, "raven", 1, VK_API_VERSION_1_1};
	const VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &application, 0, NULL, 0, NULL};
	VkInstance instance;
	if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS)
		return 1;
	uint32_t count = 1;
	VkPhysicalDevice physical;
	if (vkEnumeratePhysicalDevices(instance, &count, &physical) < 0 || !count)
		return 2;
	VkQueueFamilyProperties families[16];
	uint32_t family_count = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
	for (family = 0; family < family_count && !(families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)); ++family)
		;
	if (family == family_count)
		return 3;
	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority};
	const VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info, 0, NULL, 0, NULL, NULL};
	if (vkCreateDevice(physical, &device_info, NULL, &device) != VK_SUCCESS)
		return 4;
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);
	const VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, THREADS * RANGE, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
	if (vkCreateBuffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS)
		return 5;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	VkPhysicalDeviceMemoryProperties memory_properties;
	vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = 0;
	while (type < memory_properties.memoryTypeCount &&
		(!(requirements.memoryTypeBits & (1u << type)) || (memory_properties.memoryTypes[type].propertyFlags & wanted) != wanted))
		++type;
	if (type == memory_properties.memoryTypeCount)
		return 6;
	const VkMemoryAllocateInfo memory_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, requirements.size, type};
	VkDeviceMemory memory;
	if (vkAllocateMemory(device, &memory_info, NULL, &memory) != VK_SUCCESS || vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
		return 7;
	start = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE threads[THREADS];
	for (int index = 0; index < THREADS; ++index)
		if (!(threads[index] = CreateThread(NULL, 0, record, (void *)(INT_PTR)index, 0, NULL)))
			return 8;
	while (ready < THREADS) {
		DWORD code = STILL_ACTIVE;
		for (int index = 0; index < THREADS; ++index)
			if (GetExitCodeThread(threads[index], &code) && code != STILL_ACTIVE)
				return 100 + index * 20 + (int)code;
		Sleep(1);
	}
	SetEvent(start);
	if (WaitForMultipleObjects(THREADS, threads, TRUE, 60000) != WAIT_OBJECT_0)
		return 9;
	for (int index = 0; index < THREADS; ++index) {
		DWORD code = 0;
		if (!GetExitCodeThread(threads[index], &code) || code)
			return 100 + index * 20 + (int)code;
	}
	const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
	VkFence fence;
	const VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, THREADS, commands, 0, NULL};
	if (vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS || vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull) != VK_SUCCESS)
		return 15;
	uint32_t *words;
	if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words) != VK_SUCCESS)
		return 16;
	// Each range holds what its thread filled it with in its last round.
	for (int index = 0; index < THREADS; ++index)
		for (int word = 0; word < RANGE / 4; ++word)
			if (words[index * RANGE / 4 + word] != 0x1000u * (index + 1) + ROUNDS - 1)
				return 17;
	vkUnmapMemory(device, memory);
	vkDestroyFence(device, fence, NULL);
	for (int index = 0; index < THREADS; ++index)
		vkDestroyCommandPool(device, pools[index], NULL);
	vkDestroyBuffer(device, buffer, NULL);
	vkFreeMemory(device, memory, NULL);
	vkDestroyDevice(device, NULL);
	vkDestroyInstance(instance, NULL);
	return 0;
}
