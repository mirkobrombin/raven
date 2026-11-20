#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

static VkPhysicalDevice physical[16];
static VkSurfaceFormatKHR formats[32];
static VkQueueFamilyProperties families[32];
static VkDevice device;
static VkSwapchainKHR idle_swap;
static VkFence idle_fence;
static volatile LONG idle_stop;

/* The driver may say at any time that the swap chain of a shown window no longer fits its surface exactly; it still takes frames. */
static int usable(VkResult result, uint32_t shown)
{
	return result == VK_SUCCESS || (shown && result == VK_SUBOPTIMAL_KHR);
}

/* Takes images from a swap chain of its own, with no use of a queue, while the first thread presents. Returns how many it took. */
static DWORD WINAPI acquire_alone(void *unused)
{
	(void)unused;
	DWORD taken = 0;
	while (!idle_stop) {
		uint32_t index = 0;
		const VkResult result = vkAcquireNextImageKHR(device, idle_swap, 0, VK_NULL_HANDLE, idle_fence, &index);
		if (result == VK_NOT_READY) continue;
		if (result != VK_SUCCESS || vkWaitForFences(device, 1, &idle_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
			vkResetFences(device, 1, &idle_fence) != VK_SUCCESS)
			return 0;
		++taken;
	}
	return taken;
}

/* A swap chain over a window nobody has shown, as DXGI makes for composition: presenting to it returns at once, as on Windows, where
 * the frames of a hidden window go nowhere. Once the window is shown the surface is lost, so the program makes it again over what
 * the screen shows.
 * With "mixed" on the command line, which needs a desktop, each present also carries a swap chain over a window that is shown. */
unsigned int raven_entry(void)
{
	uint32_t targets = 1;
	for (const char *at = GetCommandLineA(); *at; ++at)
		if (at[0] == 'm' && at[1] == 'i' && at[2] == 'x' && at[3] == 'e' && at[4] == 'd' && !at[5])
			targets = 2;
	HWND windows[2] = {CreateWindowExW(0, L"STATIC", L"hidden", WS_OVERLAPPEDWINDOW, 0, 0, 96, 72, NULL, NULL, NULL, NULL), NULL};
	if (targets == 2)
		windows[1] = CreateWindowExW(0, L"STATIC", L"shown", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 240, NULL, NULL, NULL, NULL);
	if (!windows[0] || !windows[targets - 1]) return 1;
	const char *extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Hidden presentation", 1, 0, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &application, 0, 0, 2, extensions};
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&info, 0, &instance) != VK_SUCCESS) return 2;
	PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
	VkSurfaceKHR surfaces[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	for (uint32_t target = 0; target < targets; ++target) {
		VkWin32SurfaceCreateInfoKHR window = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, 0, windows[target]};
		if (!create || create(instance, &window, 0, &surfaces[target]) != VK_SUCCESS) return 3;
	}
	uint32_t count = 16;
	if (vkEnumeratePhysicalDevices(instance, &count, physical) != VK_SUCCESS || !count) return 4;
	count = 32;
	vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families);
	uint32_t family = 0;
	for (; family < count; ++family) {
		uint32_t presents = 0;
		for (uint32_t target = 0; target < targets; ++target) {
			VkBool32 supported = 0;
			if (vkGetPhysicalDeviceSurfaceSupportKHR(physical[0], family, surfaces[target], &supported) == VK_SUCCESS && supported) ++presents;
		}
		if (presents == targets && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			break;
	}
	if (family == count) return 5;
	VkSurfaceCapabilitiesKHR caps[2];
	VkSurfaceFormatKHR chosen[2];
	for (uint32_t target = 0; target < targets; ++target) {
		if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical[0], surfaces[target], &caps[target]) != VK_SUCCESS) return 6;
		/* The count alone, then all of them. */
		uint32_t all = 0;
		if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical[0], surfaces[target], &all, 0) != VK_SUCCESS || !all || all > 32) return 7;
		count = 32;
		if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical[0], surfaces[target], &count, formats) != VK_SUCCESS || count != all) return 7;
		chosen[target] = formats[0];
	}
	RECT client;
	GetClientRect(windows[0], &client);
	if (caps[0].currentExtent.width != (uint32_t)client.right || caps[0].currentExtent.height != (uint32_t)client.bottom) return 6;
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, family, 1, &priority};
	const char *swap_extension = "VK_KHR_swapchain";
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &queue_info, 0, 0, 1, &swap_extension, 0};
	if (vkCreateDevice(physical[0], &device_info, 0, &device) != VK_SUCCESS) return 8;
	VkSwapchainKHR swaps[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkImage images[2][16];
	uint32_t image_counts[2] = {0, 0};
	for (uint32_t target = 0; target < targets; ++target) {
		/* More images than the two the bridge keeps at the least. An array too short for them takes what it holds. */
		VkSwapchainCreateInfoKHR swap_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, surfaces[target], caps[target].minImageCount + 1,
			chosen[target].format, chosen[target].colorSpace, caps[target].currentExtent, 1,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, caps[target].currentTransform,
			VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, 0};
		if (vkCreateSwapchainKHR(device, &swap_info, 0, &swaps[target]) != VK_SUCCESS) return 9;
		count = 1;
		if (vkGetSwapchainImagesKHR(device, swaps[target], &count, images[target]) != VK_INCOMPLETE || count != 1) return 10;
		image_counts[target] = 16;
		if (vkGetSwapchainImagesKHR(device, swaps[target], &image_counts[target], images[target]) != VK_SUCCESS ||
			image_counts[target] < swap_info.minImageCount)
			return 10;
	}
	VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(device, &fence_info, 0, &fence) != VK_SUCCESS) return 13;
	/* An image acquired before the program has asked for a queue: the fence is signaled all the same. */
	uint32_t early = 0;
	if (vkAcquireNextImageKHR(device, swaps[0], UINT64_MAX, VK_NULL_HANDLE, fence, &early) != VK_SUCCESS || early >= image_counts[0] ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
		return 25;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, family, 0, &queue);
	VkPresentInfoKHR give_back = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 0, 0, 1, &swaps[0], &early, 0};
	if (vkQueuePresentKHR(queue, &give_back) != VK_SUCCESS) return 25;
	/* The images the program holds are not handed out again: with all of them held, an acquire that may not wait finds none, and
	 * one that waits a millisecond times out. Each comes back once presented. */
	uint32_t held[16];
	for (uint32_t image = 0; image < image_counts[0]; ++image) {
		if (vkAcquireNextImageKHR(device, swaps[0], UINT64_MAX, VK_NULL_HANDLE, fence, &held[image]) != VK_SUCCESS ||
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
			return 26;
		for (uint32_t before = 0; before < image; ++before)
			if (held[before] == held[image]) return 26;
	}
	uint32_t none = 0;
	if (vkAcquireNextImageKHR(device, swaps[0], 0, VK_NULL_HANDLE, fence, &none) != VK_NOT_READY ||
		vkAcquireNextImageKHR(device, swaps[0], 1000000, VK_NULL_HANDLE, fence, &none) != VK_TIMEOUT)
		return 27;
	for (uint32_t image = 0; image < image_counts[0]; ++image) {
		VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 0, 0, 1, &swaps[0], &held[image], 0};
		if (vkQueuePresentKHR(queue, &present) != VK_SUCCESS) return 27;
	}
	/* The acquire of Vulkan 1.1, with its information in a structure. */
	PFN_vkAcquireNextImage2KHR acquire2 = (PFN_vkAcquireNextImage2KHR)vkGetDeviceProcAddr(device, "vkAcquireNextImage2KHR");
	VkAcquireNextImageInfoKHR acquire_info = {VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR, 0, swaps[0], UINT64_MAX, VK_NULL_HANDLE, fence, 1};
	uint32_t second = 0;
	if (!acquire2 || acquire2(device, &acquire_info, &second) != VK_SUCCESS || second >= image_counts[0] ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
		return 28;
	give_back.pImageIndices = &second;
	if (vkQueuePresentKHR(queue, &give_back) != VK_SUCCESS) return 28;
	/* An image made over the swap chain and bound to one of its images, as a program aliases a swap chain image. */
	VkImageSwapchainCreateInfoKHR over_swap = {VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR, 0, swaps[0]};
	VkImageCreateInfo alias_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &over_swap, 0, VK_IMAGE_TYPE_2D, chosen[0].format,
		{caps[0].currentExtent.width, caps[0].currentExtent.height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
	VkImage alias = VK_NULL_HANDLE;
	if (vkCreateImage(device, &alias_info, 0, &alias) != VK_SUCCESS) return 29;
	VkBindImageMemorySwapchainInfoKHR bind_swap = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR, 0, swaps[0], 0};
	VkBindImageMemoryInfo bind = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, &bind_swap, alias, VK_NULL_HANDLE, 0};
	if (vkBindImageMemory2(device, 1, &bind) != VK_SUCCESS) return 29;
	vkDestroyImage(device, alias, 0);
	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
	VkCommandPool pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(device, &pool_info, 0, &pool) != VK_SUCCESS) return 11;
	VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer command = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocate, &command) != VK_SUCCESS) return 12;
	VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore acquired[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}, finished = VK_NULL_HANDLE;
	if (vkCreateSemaphore(device, &semaphore_info, 0, &acquired[0]) != VK_SUCCESS ||
		vkCreateSemaphore(device, &semaphore_info, 0, &acquired[1]) != VK_SUCCESS ||
		vkCreateSemaphore(device, &semaphore_info, 0, &finished) != VK_SUCCESS)
		return 13;
	/* More frames than the swap chain has images: a present to the hidden window that waited for the screen would stop here. */
	const DWORD started = GetTickCount();
	for (uint32_t frame = 0; frame < image_counts[0] + 3; ++frame) {
		/* Even frames: a semaphore tells that an image is acquired, and one that the images are drawn. Odd frames: a fence tells
		 * the first, and the present waits for no semaphore. */
		const uint32_t fenced = frame & 1;
		uint32_t indices[2] = {0, 0};
		for (uint32_t target = 0; target < targets; ++target) {
			if (!usable(vkAcquireNextImageKHR(device, swaps[target], UINT64_MAX, fenced ? VK_NULL_HANDLE : acquired[target],
					fenced ? fence : VK_NULL_HANDLE, &indices[target]), target) || indices[target] >= image_counts[target])
				return 14;
			if (fenced && (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS))
				return 14;
		}
		if (vkResetCommandBuffer(command, 0) != VK_SUCCESS) return 15;
		VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return 16;
		for (uint32_t target = 0; target < targets; ++target) {
			VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[target][indices[target]],
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &barrier);
			VkClearColorValue color = {{0.25f, 0.5f, 0.75f, 1}};
			vkCmdClearColorImage(command, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = 0;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &barrier);
		}
		if (vkEndCommandBuffer(command) != VK_SUCCESS) return 17;
		VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
		VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, fenced ? 0 : targets, acquired, stages, 1, &command, fenced ? 0 : 1, &finished};
		if (vkQueueSubmit(queue, 1, &submit, 0) != VK_SUCCESS || (fenced && vkQueueWaitIdle(queue) != VK_SUCCESS)) return 18;
		VkResult presented[2] = {VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN};
		VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, fenced ? 0 : 1, &finished, targets, swaps, indices, presented};
		if (!usable(vkQueuePresentKHR(queue, &present), targets - 1) || presented[0] != VK_SUCCESS ||
			!usable(presented[targets - 1], targets - 1) || vkQueueWaitIdle(queue) != VK_SUCCESS)
			return 19;
	}
	if (GetTickCount() - started > 5000) return 20;
	/* A second thread acquires over another hidden window while this one presents. An acquire and a present of two swap chains
	 * are not something a program keeps apart, so Raven must not let them meet on the queue: the validation layer tells when
	 * two threads use it at once. */
	HWND idle = CreateWindowExW(0, L"STATIC", L"idle", WS_OVERLAPPEDWINDOW, 0, 0, 96, 72, NULL, NULL, NULL, NULL);
	VkWin32SurfaceCreateInfoKHR idle_window = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, 0, idle};
	VkSurfaceKHR idle_surface = VK_NULL_HANDLE;
	if (!idle || create(instance, &idle_window, 0, &idle_surface) != VK_SUCCESS) return 22;
	VkSwapchainCreateInfoKHR idle_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, idle_surface, caps[0].minImageCount,
		chosen[0].format, chosen[0].colorSpace, caps[0].currentExtent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE,
		0, 0, caps[0].currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, 0};
	if (vkCreateSwapchainKHR(device, &idle_info, 0, &idle_swap) != VK_SUCCESS || vkCreateFence(device, &fence_info, 0, &idle_fence) != VK_SUCCESS)
		return 22;
	HANDLE thread = CreateThread(NULL, 0, acquire_alone, NULL, 0, NULL);
	if (!thread) return 22;
	for (uint32_t frame = 0; frame < 1000; ++frame) {
		uint32_t index = 0;
		if (vkAcquireNextImageKHR(device, swaps[0], UINT64_MAX, VK_NULL_HANDLE, fence, &index) != VK_SUCCESS ||
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
			return 23;
		VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[0][index], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		if (vkResetCommandBuffer(command, 0) != VK_SUCCESS || vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return 23;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &barrier);
		VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &command, 0, 0};
		if (vkEndCommandBuffer(command) != VK_SUCCESS || vkQueueSubmit(queue, 1, &submit, 0) != VK_SUCCESS || vkQueueWaitIdle(queue) != VK_SUCCESS)
			return 23;
		VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 0, 0, 1, &swaps[0], &index, 0};
		if (vkQueuePresentKHR(queue, &present) != VK_SUCCESS) return 23;
	}
	idle_stop = 1;
	DWORD taken = 0;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &taken) || !taken) return 24;
	CloseHandle(thread);
	ShowWindow(windows[0], SW_SHOWNOACTIVATE);
	uint32_t lost = 0;
	if (vkAcquireNextImageKHR(device, swaps[0], 0, acquired[0], 0, &lost) != VK_ERROR_SURFACE_LOST_KHR) return 21;
	vkDestroyFence(device, idle_fence, 0);
	vkDestroyFence(device, fence, 0);
	vkDestroySemaphore(device, finished, 0);
	vkDestroySemaphore(device, acquired[1], 0);
	vkDestroySemaphore(device, acquired[0], 0);
	vkDestroyCommandPool(device, pool, 0);
	vkDestroySwapchainKHR(device, idle_swap, 0);
	for (uint32_t target = 0; target < targets; ++target) vkDestroySwapchainKHR(device, swaps[target], 0);
	vkDestroyDevice(device, 0);
	vkDestroySurfaceKHR(instance, idle_surface, 0);
	for (uint32_t target = 0; target < targets; ++target) vkDestroySurfaceKHR(instance, surfaces[target], 0);
	vkDestroyInstance(instance, 0);
	DestroyWindow(idle);
	for (uint32_t target = 0; target < targets; ++target) DestroyWindow(windows[target]);
	return 0;
}
