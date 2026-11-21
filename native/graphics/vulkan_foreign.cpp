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
	// The images the program holds: acquired and not presented yet.
	std::vector<bool> held;
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
	// Each flag needs what the bridge does not offer for these surfaces: VK_KHR_swapchain_mutable_format, protected surface capabilities,
	// or a device group of several physical devices.
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
	target->held.assign(target->images.size(),false);
	// No host swapchain stands behind the token: the generated command that destroys it passes a null handle, which the driver ignores,
	// and so does an image a program makes over the swapchain (VkImageSwapchainCreateInfoKHR), which is then an ordinary image. One it
	// binds over a swapchain image takes that image's memory (own_image_memory).
	token=next_handle++;
	generic.emplace(token,Generic{0,static_cast<uint8_t>(H::VkSwapchainKHR),device_token});
	foreign_swapchains.emplace(token,std::move(target));
	return VK_SUCCESS;
}

static_assert(std::atomic<uint32_t>::is_always_lock_free);

VkResult VulkanBridge::detached_present(VkQueue queue,const std::function<VkResult()> &work)
{
	auto &in_flight=presents[queue];
	++in_flight;
	return detached([&] {
		const auto status=work();
		--in_flight;
		return status;
	});
}

std::optional<VkDeviceMemory> VulkanBridge::own_image_memory(uint64_t swapchain,uint32_t index) const
{
	const auto found=foreign_swapchains.find(swapchain);
	if (found==foreign_swapchains.end()) return std::nullopt;
	if (index>=found->second->image_memory.size()) throw std::runtime_error("Vulkan image binds over a swapchain image that does not exist");
	return found->second->image_memory[index];
}

VkResult VulkanBridge::acquire_own_image(Marshal &marshal,uint64_t device_token,uint64_t swapchain,uint64_t timeout,uint64_t semaphore_token,
	uint64_t fence_token,uint64_t index_address)
{
	marshal.context(H::VkDevice,device_token);
	marshal.check_writable(index_address,4);
	// Held for the wait below, while another thread may destroy the swapchain.
	const auto owned=foreign_swapchains.at(swapchain);
	auto &target=*owned;
	const auto semaphore=reinterpret_cast<VkSemaphore>(marshal.host(H::VkSemaphore,semaphore_token));
	const auto fence=reinterpret_cast<VkFence>(marshal.host(H::VkFence,fence_token));
	// Only a queue signals a semaphore or a fence, so an empty batch does it: on the queue the swapchain's frames go to, and before
	// the first present on the device's first queue, which the device was made with whether or not the program has asked for it.
	auto queue=target.queue;
	if (!queue) {
		const auto &device=devices.at(device_token);
		vkGetDeviceQueue(device.handle,device.family,0,&queue);
	}
	// An image the program holds is not handed out again: the next free one after the last handed out is.
	const auto free_image=[&target]() -> std::optional<uint32_t> {
		const auto count=uint32_t(target.images.size());
		for (uint32_t step=0;step<count;++step)
			if (const uint32_t index=(target.next+step)%count; !target.held[index]) return index;
		return std::nullopt;
	};
	// Nor does the batch meet a present in flight on its queue. With every image held, the acquire waits for another thread of the
	// program to present one, up to its timeout; without a way to wait, presents are not in flight and nothing frees an image.
	auto &in_flight=presents[queue];
	const auto ready=[&] { return !in_flight.load(std::memory_order_acquire) && free_image(); };
	if (!ready()) {
		const bool held=!free_image();
		if (held && !timeout) return VK_NOT_READY;
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (held && timeout<(uint64_t(1)<<62)) wake=std::chrono::steady_clock::now()+std::chrono::nanoseconds(timeout);
		if (!park) return VK_TIMEOUT;
		const TablesRelease release(*this);
		if (!park(ready,wake)) return VK_TIMEOUT;
	}
	const uint32_t index=*free_image();
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.signalSemaphoreCount=semaphore ? 1 : 0; submit.pSignalSemaphores=&semaphore;
	// On the program's own thread, as its submits are, with no present in flight on the queue: nothing else uses it at this moment.
	const auto status=vkQueueSubmit(queue,1,&submit,fence);
	if (status!=VK_SUCCESS) return status;
	target.held[index]=true;
	target.next=(index+1)%uint32_t(target.images.size());
	store<uint32_t>(index_address,index);
	return VK_SUCCESS;
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
	// The other surface queries of VK_KHR_get_surface_capabilities2, which the bridge does not offer: a program that calls them anyway
	// on a surface the bridge keeps learns that it is lost.
	if (name=="vkGetPhysicalDeviceSurfaceCapabilities2KHR" || name=="vkGetPhysicalDeviceSurfaceFormats2KHR") {
		const auto a=process.api_arguments(name=="vkGetPhysicalDeviceSurfaceFormats2KHR" ? 4 : 3);
		// VkPhysicalDeviceSurfaceInfo2KHR: sType, pNext, then the surface.
		if (!own_surface(load<uint64_t>(a[1]+16))) return false;
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
	// A presented image is the program's no more: the next acquire may hand it out.
	for (const auto &frame:frames) frame.target->held[frame.index]=false;
	if (frames.empty()) return std::nullopt;
	const auto queue=queues.at(queue_token);
	// The driver's part of the present keeps the chain, with its arrays of one entry per swapchain cut to the driver's swapchains.
	// VkPresentIdKHR and VkDeviceGroupPresentInfoKHR are the structures a present may carry from what the bridge offers.
	VkPresentIdKHR ids{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
	VkDeviceGroupPresentInfoKHR group{VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR};
	std::vector<uint64_t> id_values;
	std::vector<uint32_t> masks;
	const void *chain=nullptr;
	for (auto *element=static_cast<const VkBaseInStructure *>(info.pNext);element;element=element->pNext) {
		if (element->sType==VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
			const auto &source=*reinterpret_cast<const VkPresentIdKHR *>(element);
			if (source.pPresentIds)
				for (const auto place:host_places) id_values.push_back(source.pPresentIds[place]);
			ids.swapchainCount=uint32_t(host_places.size()); ids.pPresentIds=source.pPresentIds ? id_values.data() : nullptr;
			ids.pNext=chain; chain=&ids;
		} else if (element->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR) {
			const auto &source=*reinterpret_cast<const VkDeviceGroupPresentInfoKHR *>(element);
			if (source.swapchainCount)
				for (const auto place:host_places) masks.push_back(source.pDeviceMasks[place]);
			group.swapchainCount=source.swapchainCount ? uint32_t(masks.size()) : 0; group.pDeviceMasks=masks.empty() ? nullptr : masks.data();
			group.mode=source.mode;
			group.pNext=chain; chain=&group;
		} else throw std::runtime_error("unsupported structure in the chain of a Vulkan present");
	}
	// One batch consumes the present's waits once, including waits shared with host swapchains in the same present.
	// The host present follows the completed batch without waiting on those binary semaphores a second time.
	bool presented=false;
	const auto status=detached_present(queue.handle,[&]() -> VkResult {
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
		std::vector<VkResult> results(host_swapchains.size());
		const VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,chain,0,nullptr,uint32_t(host_swapchains.size()),
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
