#include <windows.h>
#include <vulkan/vulkan.h>

// A renderer's submitting thread and the thread that waits for its frames, as Direct3D over Vulkan runs them: one thread resets its pool,
// records, submits and signals a timeline semaphore each round, the other waits for every value and reads the counter. What each call
// costs, and whether the runtime answers it beside the turn, is what RAVEN_CYCLES reports. Each round the submitting thread also makes
// a fence, a call the turn answers, while the other thread waits for that round: a wait that kept the turn's calls out would hold the
// round back until it timed out. The main thread then waits for a fence of a last submission and reads the buffer back.

#define ROUNDS 300
#define RANGE 256

static VkDevice device;
static VkQueue queue;
static VkBuffer buffer;
static uint32_t family;
static VkSemaphore timeline;

static DWORD WINAPI submit_rounds(void *parameter)
{
	(void)parameter;
	const VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
	VkCommandPool pool;
	if (vkCreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS)
		return 10;
	const VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer commands;
	if (vkAllocateCommandBuffers(device, &allocate, &commands) != VK_SUCCESS)
		return 11;
	const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
	for (uint64_t round = 1; round <= ROUNDS; ++round) {
		// The previous round's work must be done before its pool is reset.
		const VkSemaphoreWaitInfo previous = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &timeline, &(uint64_t){round - 1}};
		if (vkWaitSemaphores(device, &previous, 5000000000ull) != VK_SUCCESS)
			return 12;
		if (vkResetCommandPool(device, pool, 0) != VK_SUCCESS || vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS)
			return 13;
		vkCmdFillBuffer(commands, buffer, 0, RANGE, (uint32_t)round);
		if (vkEndCommandBuffer(commands) != VK_SUCCESS)
			return 14;
		const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
		VkFence fence;
		if (vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS)
			return 17;
		vkDestroyFence(device, fence, NULL);
		const VkTimelineSemaphoreSubmitInfo values = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &round};
		const VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &values, 0, NULL, NULL, 1, &commands, 1, &timeline};
		if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
			return 15;
	}
	const VkSemaphoreWaitInfo last = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &timeline, &(uint64_t){ROUNDS}};
	if (vkWaitSemaphores(device, &last, 5000000000ull) != VK_SUCCESS)
		return 16;
	vkDestroyCommandPool(device, pool, NULL);
	return 0;
}

static DWORD WINAPI wait_rounds(void *parameter)
{
	(void)parameter;
	for (uint64_t round = 1; round <= ROUNDS; ++round) {
		const VkSemaphoreWaitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &timeline, &round};
		if (vkWaitSemaphores(device, &wait, 5000000000ull) != VK_SUCCESS)
			return 20;
		uint64_t value = 0;
		if (vkGetSemaphoreCounterValue(device, timeline, &value) != VK_SUCCESS || value < round)
			return 21;
	}
	return 0;
}

unsigned int raven_entry(void)
{
	const VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "queue", 1, "raven", 1, VK_API_VERSION_1_2};
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
	VkPhysicalDeviceVulkan12Features features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	features.timelineSemaphore = VK_TRUE;
	const VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features, 0, 1, &queue_info, 0, NULL, 0, NULL, NULL};
	if (vkCreateDevice(physical, &device_info, NULL, &device) != VK_SUCCESS)
		return 4;
	vkGetDeviceQueue(device, family, 0, &queue);
	const VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, RANGE, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
	const VkSemaphoreTypeCreateInfo timeline_type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
	const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline_type, 0};
	if (vkCreateSemaphore(device, &semaphore_info, NULL, &timeline) != VK_SUCCESS)
		return 8;
	HANDLE threads[2] = {CreateThread(NULL, 0, submit_rounds, NULL, 0, NULL), CreateThread(NULL, 0, wait_rounds, NULL, 0, NULL)};
	if (!threads[0] || !threads[1] || WaitForMultipleObjects(2, threads, TRUE, 60000) != WAIT_OBJECT_0)
		return 9;
	for (int index = 0; index < 2; ++index) {
		DWORD code = 0;
		if (!GetExitCodeThread(threads[index], &code) || code)
			return 100 + (int)code;
	}
	// The last round's fill is what the buffer holds, seen through the guest's mapping after a fence of an empty submission.
	const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
	VkFence fence;
	if (vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS || vkQueueSubmit(queue, 0, NULL, fence) != VK_SUCCESS ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull) != VK_SUCCESS || vkGetFenceStatus(device, fence) != VK_SUCCESS ||
		vkResetFences(device, 1, &fence) != VK_SUCCESS || vkGetFenceStatus(device, fence) != VK_NOT_READY)
		return 22;
	uint32_t *words;
	if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words) != VK_SUCCESS)
		return 18;
	for (int word = 0; word < RANGE / 4; ++word)
		if (words[word] != ROUNDS)
			return 19;
	vkUnmapMemory(device, memory);
	vkDestroyFence(device, fence, NULL);
	vkDestroySemaphore(device, timeline, NULL);
	vkDestroyBuffer(device, buffer, NULL);
	vkFreeMemory(device, memory, NULL);
	vkDestroyDevice(device, NULL);
	vkDestroyInstance(instance, NULL);
	return 0;
}
