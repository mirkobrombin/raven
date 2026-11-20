#include "vulkan_marshal.h"
#include "desktop/foreign_surface.h"
#include "desktop/shared_pixels.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace raven {

// The formats of a surface the bridge keeps itself: the byte order of the pixels a foreign window's owner shows, so a present
// copies texels as they are.
static constexpr VkSurfaceFormatKHR own_formats[]={{VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	{VK_FORMAT_B8G8R8A8_SRGB,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
// A present to such a surface never waits for a screen, whatever the mode.
static constexpr VkPresentModeKHR own_modes[]={VK_PRESENT_MODE_IMMEDIATE_KHR,VK_PRESENT_MODE_FIFO_KHR};

static constexpr uint32_t own_min_images=2,own_max_images=8;

static bool own_format(VkFormat format)
{
	return std::any_of(std::begin(own_formats),std::end(own_formats),[format](const auto &item) { return item.format==format; });
}

// A swapchain the bridge keeps itself, over a window that is hidden or that another process owns: its images are ordinary images of
// the device, and a present copies the image to the pixels the window's owner shows (foreign) or goes nowhere (hidden).
struct VulkanBridge::ForeignSwapchain {
	uint64_t device_token=0,window=0;
	VkDevice device=VK_NULL_HANDLE;
	VkFormat format=VK_FORMAT_UNDEFINED;
	uint32_t width=0,height=0,family=UINT32_MAX,next=0;
	// The queue of the last present.
	VkQueue queue=VK_NULL_HANDLE;
	std::vector<VkDeviceMemory> image_memory;
	VkBuffer buffer=VK_NULL_HANDLE;
	VkDeviceMemory memory=VK_NULL_HANDLE;
	void *mapped=nullptr;
	VkCommandPool pool=VK_NULL_HANDLE;
	VkCommandBuffer command=VK_NULL_HANDLE;
	std::vector<VkImage> images;
	std::shared_ptr<ForeignSurface> surface;
	~ForeignSwapchain()
	{
		if (pool) vkDestroyCommandPool(device,pool,nullptr);
		if (mapped) vkUnmapMemory(device,memory);
		if (buffer) vkDestroyBuffer(device,buffer,nullptr);
		if (memory) vkFreeMemory(device,memory,nullptr);
		for (const auto image:images) vkDestroyImage(device,image,nullptr);
		for (const auto block:image_memory) vkFreeMemory(device,block,nullptr);
	}
};

VkSurfaceCapabilitiesKHR VulkanBridge::own_surface_capabilities()
{
	VkSurfaceCapabilitiesKHR result{};
	result.minImageCount=own_min_images; result.maxImageCount=own_max_images; result.maxImageArrayLayers=1;
	result.supportedTransforms=result.currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	result.supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	result.supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT;
	return result;
}

bool VulkanBridge::queue_family_draws(uint64_t physical_token,uint32_t family) const
{
	const auto found=physical.find(physical_token);
	if (found==physical.end()) throw std::runtime_error("invalid Vulkan physical device handle");
	uint32_t count=0;
	vkGetPhysicalDeviceQueueFamilyProperties(found->second.handle,&count,nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(found->second.handle,&count,families.data());
	return family<count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

VkResult VulkanBridge::create_own_swapchain(uint64_t device_token,const VkSwapchainCreateInfoKHR &info,uint64_t window,bool foreign,uint64_t &token)
{
	if (!own_format(info.imageFormat)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
	// shortcut: no mutable-format, protected or split-instance swapchains, add them when a program asks for one.
	if (info.flags) return VK_ERROR_INITIALIZATION_FAILED;
	if (foreign) {
		const auto size=foreign_window_size(window);
		if (!size) return VK_ERROR_SURFACE_LOST_KHR;
		if (size->first!=info.imageExtent.width || size->second!=info.imageExtent.height) return VK_ERROR_OUT_OF_DATE_KHR;
	}
	const uint64_t bytes=uint64_t(info.imageExtent.width)*info.imageExtent.height*4;
	if (!bytes || bytes>256ULL*1024*1024 || info.imageArrayLayers!=1) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	auto target=std::make_shared<ForeignSwapchain>();
	target->device_token=device_token; target->window=window; target->device=devices.at(device_token).handle;
	target->format=info.imageFormat;
	target->width=info.imageExtent.width; target->height=info.imageExtent.height;
	VkPhysicalDeviceMemoryProperties memory{};
	vkGetPhysicalDeviceMemoryProperties(physical.at(devices.at(device_token).physical).handle,&memory);
	const auto memory_type=[&](const VkMemoryRequirements &requirements,VkMemoryPropertyFlags flags) {
		uint32_t type=0;
		for (;type<memory.memoryTypeCount;++type)
			if ((requirements.memoryTypeBits & (1U<<type)) && (memory.memoryTypes[type].propertyFlags & flags)==flags) break;
		return type;
	};
	VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image.imageType=VK_IMAGE_TYPE_2D; image.format=info.imageFormat; image.extent={target->width,target->height,1};
	image.mipLevels=1; image.arrayLayers=1; image.samples=VK_SAMPLE_COUNT_1_BIT; image.tiling=VK_IMAGE_TILING_OPTIMAL;
	image.usage=info.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; image.sharingMode=info.imageSharingMode;
	image.queueFamilyIndexCount=info.queueFamilyIndexCount; image.pQueueFamilyIndices=info.pQueueFamilyIndices;
	for (uint32_t index=0;index<std::clamp(info.minImageCount,own_min_images,own_max_images);++index) {
		target->images.push_back(VK_NULL_HANDLE);
		auto result=vkCreateImage(target->device,&image,nullptr,&target->images.back());
		if (result!=VK_SUCCESS) { target->images.pop_back(); return result; }
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(target->device,target->images.back(),&requirements);
		const auto type=memory_type(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type==memory.memoryTypeCount) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,type};
		target->image_memory.push_back(VK_NULL_HANDLE);
		result=vkAllocateMemory(target->device,&allocate,nullptr,&target->image_memory.back());
		if (result!=VK_SUCCESS) { target->image_memory.pop_back(); return result; }
		result=vkBindImageMemory(target->device,target->images.back(),target->image_memory.back(),0);
		if (result!=VK_SUCCESS) return result;
	}
	if (foreign) {
		target->surface=ForeignSurface::open_for_drawing(window,target->width,target->height);
		if (!target->surface) return VK_ERROR_OUT_OF_HOST_MEMORY;
		VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer.size=bytes; buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
		auto result=vkCreateBuffer(target->device,&buffer,nullptr,&target->buffer);
		if (result!=VK_SUCCESS) return result;
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(target->device,target->buffer,&requirements);
		// The host reads every texel of every frame: cached memory where the device has it, since the other kinds read slowly.
		const auto readable=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		auto type=memory_type(requirements,readable | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
		if (type==memory.memoryTypeCount) type=memory_type(requirements,readable);
		if (type==memory.memoryTypeCount) return VK_ERROR_MEMORY_MAP_FAILED;
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,type};
		result=vkAllocateMemory(target->device,&allocate,nullptr,&target->memory);
		if (result!=VK_SUCCESS) return result;
		result=vkBindBufferMemory(target->device,target->buffer,target->memory,0);
		if (result!=VK_SUCCESS) return result;
		result=vkMapMemory(target->device,target->memory,0,VK_WHOLE_SIZE,0,&target->mapped);
		if (result!=VK_SUCCESS) return result;
	}
	// No host swapchain stands behind the token: the generated command that destroys it passes a null handle, which the driver ignores.
	// shortcut: an image a program makes or binds over the swapchain (VkImageSwapchainCreateInfoKHR, VkBindImageMemorySwapchainInfoKHR)
	// gets the null handle too, refuse those when a program uses them.
	token=next_handle++;
	generic.emplace(token,Generic{0,static_cast<uint8_t>(H::VkSwapchainKHR),device_token});
	foreign_swapchains.emplace(token,std::move(target));
	return VK_SUCCESS;
}

static_assert(std::atomic<uint32_t>::is_always_lock_free);

VkResult VulkanBridge::detached_present(const std::function<VkResult()> &work)
{
	++presents;
	return detached([&] {
		const auto status=work();
		--presents;
		return status;
	});
}

VkResult VulkanBridge::acquire_own_image(Marshal &marshal,const std::vector<uint64_t> &a)
{
	if (park) park([this] { return !presents; });
	marshal.context(H::VkDevice,a[0]);
	marshal.check_writable(a[5],4);
	auto &target=*foreign_swapchains.at(a[1]);
	const auto semaphore=reinterpret_cast<VkSemaphore>(marshal.host(H::VkSemaphore,a[3]));
	const auto fence=reinterpret_cast<VkFence>(marshal.host(H::VkFence,a[4]));
	// Only a queue signals a semaphore or a fence, so an empty batch does it: on the queue the swapchain's frames go to, and
	// before the first present on any queue of the device.
	auto queue=target.queue;
	if (!queue) {
		const auto any=std::find_if(queues.begin(),queues.end(),[&](const auto &item) { return item.second.device==a[0]; });
		// shortcut: fails before the program has a queue of the device, get one for it when a program acquires that early.
		if (any==queues.end()) return VK_ERROR_INITIALIZATION_FAILED;
		queue=any->second.handle;
	}
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.signalSemaphoreCount=semaphore ? 1 : 0; submit.pSignalSemaphores=&semaphore;
	// On the program's own thread, as its submits are, with no present in flight: nothing else uses the queue at this moment.
	const auto status=vkQueueSubmit(queue,1,&submit,fence);
	// shortcut: images go round in order and none is ever busy, track them when a program holds several at once.
	if (status==VK_SUCCESS) store<uint32_t>(a[5],target.next++%uint32_t(target.images.size()));
	return status;
}

// The surface queries and the image list the driver cannot answer for a surface or a swapchain it never saw.
bool VulkanBridge::dispatch_own(std::string_view name)
{
	const auto list=[&](Marshal &marshal,uint64_t count,uint64_t array,const void *items,uint32_t total,size_t size) {
		marshal.check_writable(count,4);
		if (!array) { store<uint32_t>(count,total); return VK_SUCCESS; }
		const auto copied=std::min(load<uint32_t>(count),total);
		marshal.write(array,items,copied*size);
		store<uint32_t>(count,copied);
		return copied<total ? VK_INCOMPLETE : VK_SUCCESS;
	};
	if (name=="vkGetPhysicalDeviceSurfaceSupportKHR") {
		const auto a=process.api_arguments(4);
		if (!own_surface(a[2])) return false;
		Marshal marshal(*this,process);
		marshal.check_writable(a[3],4);
		store<VkBool32>(a[3],queue_family_draws(a[0],uint32_t(a[1])) ? VK_TRUE : VK_FALSE);
		process.return_api(0);
		return true;
	}
	if (name=="vkGetPhysicalDeviceSurfaceFormatsKHR" || name=="vkGetPhysicalDeviceSurfacePresentModesKHR") {
		const auto a=process.api_arguments(4);
		if (!own_surface(a[1])) return false;
		Marshal marshal(*this,process);
		marshal.context(H::VkPhysicalDevice,a[0]);
		const bool formats=name=="vkGetPhysicalDeviceSurfaceFormatsKHR";
		process.return_api(static_cast<uint32_t>(formats ? list(marshal,a[2],a[3],own_formats,uint32_t(std::size(own_formats)),sizeof own_formats[0]) :
			list(marshal,a[2],a[3],own_modes,uint32_t(std::size(own_modes)),sizeof own_modes[0])));
		return true;
	}
	if (name=="vkGetSwapchainImagesKHR") {
		const auto a=process.api_arguments(4);
		const auto found=foreign_swapchains.find(a[1]);
		if (found==foreign_swapchains.end()) return false;
		Marshal marshal(*this,process);
		marshal.context(H::VkDevice,a[0]);
		std::vector<uint64_t> tokens;
		for (const auto image:found->second->images) tokens.push_back(marshal.swapchain_image_token(H::VkImage,reinterpret_cast<uint64_t>(image)));
		process.return_api(static_cast<uint32_t>(list(marshal,a[2],a[3],tokens.data(),uint32_t(tokens.size()),sizeof tokens[0])));
		return true;
	}
	// shortcut: the extended queries and acquire are refused for these surfaces and swapchains, answer them when a program uses them.
	// VkPhysicalDeviceSurfaceInfo2KHR and VkAcquireNextImageInfoKHR: sType, pNext, then the surface or the swapchain.
	if (name=="vkGetPhysicalDeviceSurfaceCapabilities2KHR" || name=="vkGetPhysicalDeviceSurfaceFormats2KHR" || name=="vkAcquireNextImage2KHR") {
		const auto a=process.api_arguments(name=="vkGetPhysicalDeviceSurfaceFormats2KHR" ? 4 : 3);
		const auto handle=load<uint64_t>(a[1]+16);
		if (!own_surface(handle) && !foreign_swapchains.contains(handle)) return false;
		process.return_api(static_cast<uint32_t>(VK_ERROR_SURFACE_LOST_KHR));
		return true;
	}
	return false;
}

void VulkanBridge::release_foreign_swapchains(uint64_t device)
{
	std::erase_if(foreign_swapchains,[&](const auto &item) { return item.second->device_token==device; });
}

std::optional<VkResult> VulkanBridge::present_foreign(uint64_t queue_token,const VkPresentInfoKHR &info,const std::vector<uint64_t> &tokens)
{
	struct Frame { std::shared_ptr<ForeignSwapchain> target; uint32_t index; };
	std::vector<Frame> frames;
	// What is left for the driver: the swapchains it made, with the place of each in the program's present.
	std::vector<VkSwapchainKHR> host_swapchains;
	std::vector<uint32_t> host_indices,host_places;
	for (uint32_t i=0;i<info.swapchainCount;++i) {
		const auto own=foreign_swapchains.find(tokens[i]);
		if (own==foreign_swapchains.end()) {
			host_swapchains.push_back(info.pSwapchains[i]); host_indices.push_back(info.pImageIndices[i]); host_places.push_back(i);
		} else if (info.pImageIndices[i]>=own->second->images.size()) return VK_ERROR_OUT_OF_DATE_KHR;
		else frames.push_back({own->second,info.pImageIndices[i]});
	}
	if (frames.empty()) return std::nullopt;
	const auto queue=queues.at(queue_token);
	// One batch consumes the present's waits once, including waits shared with host swapchains in the same present.
	// The host present follows the completed batch without waiting on those binary semaphores a second time.
	bool presented=false;
	const auto status=detached_present([&]() -> VkResult {
		std::vector<VkCommandBuffer> commands;
		for (const auto &frame:frames) {
			auto &target=*frame.target;
			if (target.device_token!=queue.device) return VK_ERROR_DEVICE_LOST;
			target.queue=queue.handle;
			// A hidden window shows nothing: its frame is only waited for.
			if (!target.surface) continue;
			if (!target.pool || target.family!=queue.family) {
				if (target.pool) vkDestroyCommandPool(target.device,target.pool,nullptr);
				target.pool=VK_NULL_HANDLE;
				VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,queue.family};
				auto result=vkCreateCommandPool(target.device,&pool,nullptr,&target.pool);
				if (result!=VK_SUCCESS) return result;
				VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,target.pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
				result=vkAllocateCommandBuffers(target.device,&allocate,&target.command);
				if (result!=VK_SUCCESS) return result;
				target.family=queue.family;
			}
			auto result=vkResetCommandBuffer(target.command,0);
			if (result!=VK_SUCCESS) return result;
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
			result=vkBeginCommandBuffer(target.command,&begin);
			if (result!=VK_SUCCESS) return result;
			VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
				target.images[frame.index],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
			vkCmdPipelineBarrier(target.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
			VkBufferImageCopy copy{};
			copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={target.width,target.height,1};
			vkCmdCopyImageToBuffer(target.command,target.images[frame.index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.buffer,1,&copy);
			image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; image.dstAccessMask=0;
			image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; image.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vkCmdPipelineBarrier(target.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&image);
			VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
			vkCmdPipelineBarrier(target.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
			result=vkEndCommandBuffer(target.command);
			if (result!=VK_SUCCESS) return result;
			commands.push_back(target.command);
		}
		std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,info.waitSemaphoreCount,info.pWaitSemaphores,stages.data(),
			uint32_t(commands.size()),commands.data(),0,nullptr};
		auto result=vkQueueSubmit(queue.handle,1,&submit,VK_NULL_HANDLE);
		if (result!=VK_SUCCESS) return result;
		result=commands.empty() && host_swapchains.empty() ? VK_SUCCESS : vkQueueWaitIdle(queue.handle);
		if (result!=VK_SUCCESS) return result;
		for (const auto &frame:frames) {
			auto &target=*frame.target;
			if (!target.surface) continue;
			const auto *source=static_cast<const uint8_t *>(target.mapped);
			auto *destination=target.surface->pixels->data();
			const size_t bytes=size_t(target.width)*target.height*4;
			std::memcpy(destination,source,bytes);
			target.surface->publish_frame();
		}
		presented=true;
		if (info.pResults) std::fill_n(info.pResults,info.swapchainCount,VK_SUCCESS);
		if (host_swapchains.empty()) return VK_SUCCESS;
		// shortcut: the driver's part of a mixed present drops the pNext chain (present id, device group), filter its arrays when
		// an offered extension's chain changes what a present does.
		std::vector<VkResult> results(host_swapchains.size());
		const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,nullptr,0,nullptr,uint32_t(host_swapchains.size()),
			host_swapchains.data(),host_indices.data(),results.data()};
		result=vkQueuePresentKHR(queue.handle,&present);
		if (info.pResults)
			for (size_t i=0;i<results.size();++i) info.pResults[host_places[i]]=results[i];
		return result;
	});
	if (!presented && info.pResults) std::fill_n(info.pResults,info.swapchainCount,status);
	return status;
}

}
