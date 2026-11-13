#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

static VkPhysicalDevice physical[16];
static VkSurfaceFormatKHR formats[32];
static VkQueueFamilyProperties families[32];

/* A swap chain over a window nobody has shown, as DXGI makes for composition: presenting to it returns at once, as on Windows, where
 * the frames of a hidden window go nowhere. Once the window is shown the surface is lost, so the program makes it again over what
 * the screen shows. */
unsigned int raven_entry(void)
{
	HWND hidden = CreateWindowExW(0, L"STATIC", L"hidden", WS_OVERLAPPEDWINDOW, 0, 0, 96, 72, NULL, NULL, NULL, NULL);
	if (!hidden) return 1;
	const char *extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
	VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "Hidden presentation", 1, 0, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &application, 0, 0, 2, extensions};
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&info, 0, &instance) != VK_SUCCESS) return 2;
	PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR");
	VkWin32SurfaceCreateInfoKHR window = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, 0, hidden};
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	if (!create || create(instance, &window, 0, &surface) != VK_SUCCESS) return 3;
	uint32_t count = 16;
	if (vkEnumeratePhysicalDevices(instance, &count, physical) != VK_SUCCESS || !count) return 4;
	count = 32;
	vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families);
	uint32_t family = 0;
	for (; family < count; ++family) {
		VkBool32 supported = 0;
		if (vkGetPhysicalDeviceSurfaceSupportKHR(physical[0], family, surface, &supported) == VK_SUCCESS && supported &&
			(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			break;
	}
	if (family == count) return 5;
	RECT client;
	GetClientRect(hidden, &client);
	VkSurfaceCapabilitiesKHR caps;
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical[0], surface, &caps) != VK_SUCCESS ||
		caps.currentExtent.width != (uint32_t)client.right || caps.currentExtent.height != (uint32_t)client.bottom)
		return 6;
	count = 32;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical[0], surface, &count, formats) != VK_SUCCESS || !count) return 7;
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, family, 1, &priority};
	const char *swap_extension = "VK_KHR_swapchain";
	VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &queue_info, 0, 0, 1, &swap_extension, 0};
	VkDevice device = VK_NULL_HANDLE;
	if (vkCreateDevice(physical[0], &device_info, 0, &device) != VK_SUCCESS) return 8;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, family, 0, &queue);
	VkSwapchainCreateInfoKHR swap_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, surface, caps.minImageCount,
		formats[0].format, formats[0].colorSpace, caps.currentExtent, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VK_SHARING_MODE_EXCLUSIVE, 0, 0, caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, 0};
	VkSwapchainKHR swap = VK_NULL_HANDLE;
	if (vkCreateSwapchainKHR(device, &swap_info, 0, &swap) != VK_SUCCESS) return 9;
	VkImage images[16];
	count = 16;
	if (vkGetSwapchainImagesKHR(device, swap, &count, images) != VK_SUCCESS || !count) return 10;
	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
	VkCommandPool pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(device, &pool_info, 0, &pool) != VK_SUCCESS) return 11;
	VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer command = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocate, &command) != VK_SUCCESS) return 12;
	VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore acquired = VK_NULL_HANDLE, finished = VK_NULL_HANDLE;
	if (vkCreateSemaphore(device, &semaphore_info, 0, &acquired) != VK_SUCCESS ||
		vkCreateSemaphore(device, &semaphore_info, 0, &finished) != VK_SUCCESS)
		return 13;
	/* More frames than the swap chain has images: a present that waited for the screen would stop here. */
	const DWORD started = GetTickCount();
	for (uint32_t frame = 0; frame < count + 3; ++frame) {
		uint32_t index = 0;
		if (vkAcquireNextImageKHR(device, swap, UINT64_MAX, acquired, 0, &index) != VK_SUCCESS) return 14;
		if (vkResetCommandBuffer(command, 0) != VK_SUCCESS) return 15;
		VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return 16;
		VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[index],
			{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &barrier);
		VkClearColorValue color = {{0.25f, 0.5f, 0.75f, 1}};
		vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = 0;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &barrier);
		if (vkEndCommandBuffer(command) != VK_SUCCESS) return 17;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 1, &acquired, &stage, 1, &command, 1, &finished};
		if (vkQueueSubmit(queue, 1, &submit, 0) != VK_SUCCESS) return 18;
		VkResult presented = VK_ERROR_UNKNOWN;
		VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &finished, 1, &swap, &index, &presented};
		if (vkQueuePresentKHR(queue, &present) != VK_SUCCESS || presented != VK_SUCCESS || vkQueueWaitIdle(queue) != VK_SUCCESS) return 19;
	}
	if (GetTickCount() - started > 5000) return 20;
	ShowWindow(hidden, SW_SHOWNOACTIVATE);
	uint32_t lost = 0;
	if (vkAcquireNextImageKHR(device, swap, 0, acquired, 0, &lost) != VK_ERROR_SURFACE_LOST_KHR) return 21;
	vkDestroySemaphore(device, finished, 0);
	vkDestroySemaphore(device, acquired, 0);
	vkDestroyCommandPool(device, pool, 0);
	vkDestroySwapchainKHR(device, swap, 0);
	vkDestroyDevice(device, 0);
	vkDestroySurfaceKHR(instance, surface, 0);
	vkDestroyInstance(instance, 0);
	DestroyWindow(hidden);
	return 0;
}
