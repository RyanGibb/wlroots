#include <vulkan/vulkan.h>
#include <wlr/util/log.h>
#include "render/vulkan.h"

int vulkan_find_mem_type(struct wlr_vk_device *dev,
		VkMemoryPropertyFlags flags, uint32_t req_bits) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phdev, &props);

	for (unsigned i = 0u; i < props.memoryTypeCount; ++i) {
		if (req_bits & (1 << i)) {
			if ((props.memoryTypes[i].propertyFlags & flags) == flags) {
				return i;
			}
		}
	}

	return -1;
}

const char *vulkan_strerror(VkResult err) {
#define ERR_STR(r) case VK_ ##r: return #r
	switch (err) {
	ERR_STR(SUCCESS);
	ERR_STR(NOT_READY);
	ERR_STR(TIMEOUT);
	ERR_STR(EVENT_SET);
	ERR_STR(EVENT_RESET);
	ERR_STR(INCOMPLETE);
	ERR_STR(ERROR_OUT_OF_HOST_MEMORY);
	ERR_STR(ERROR_OUT_OF_DEVICE_MEMORY);
	ERR_STR(ERROR_INITIALIZATION_FAILED);
	ERR_STR(ERROR_DEVICE_LOST);
	ERR_STR(ERROR_MEMORY_MAP_FAILED);
	ERR_STR(ERROR_LAYER_NOT_PRESENT);
	ERR_STR(ERROR_EXTENSION_NOT_PRESENT);
	ERR_STR(ERROR_FEATURE_NOT_PRESENT);
	ERR_STR(ERROR_INCOMPATIBLE_DRIVER);
	ERR_STR(ERROR_TOO_MANY_OBJECTS);
	ERR_STR(ERROR_FORMAT_NOT_SUPPORTED);
	ERR_STR(ERROR_FRAGMENTED_POOL);
	ERR_STR(ERROR_UNKNOWN);
	ERR_STR(ERROR_OUT_OF_POOL_MEMORY);
	ERR_STR(ERROR_INVALID_EXTERNAL_HANDLE);
	ERR_STR(ERROR_FRAGMENTATION);
	ERR_STR(ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
	ERR_STR(PIPELINE_COMPILE_REQUIRED);
	ERR_STR(ERROR_SURFACE_LOST_KHR);
	ERR_STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
	ERR_STR(SUBOPTIMAL_KHR);
	ERR_STR(ERROR_OUT_OF_DATE_KHR);
	ERR_STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
	ERR_STR(ERROR_VALIDATION_FAILED_EXT);
	ERR_STR(ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
	default:
		return "<unknown>";
	}
#undef ERR_STR
}

void vulkan_change_layout_queue(VkCommandBuffer cb, VkImage img,
		VkImageLayout ol, VkPipelineStageFlags srcs, VkAccessFlags srca,
		VkImageLayout nl, VkPipelineStageFlags dsts, VkAccessFlags dsta,
		uint32_t src_family, uint32_t dst_family) {
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = ol,
		.newLayout = nl,
		.image = img,
		.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.subresourceRange.layerCount = 1,
		.subresourceRange.levelCount = 1,
		.srcAccessMask = srca,
		.dstAccessMask = dsta,
		.srcQueueFamilyIndex = src_family,
		.dstQueueFamilyIndex = dst_family,
	};

	vkCmdPipelineBarrier(cb, srcs, dsts, 0, 0, NULL, 0, NULL, 1, &barrier);
}

void vulkan_change_layout(VkCommandBuffer cb, VkImage img,
		VkImageLayout ol, VkPipelineStageFlags srcs, VkAccessFlags srca,
		VkImageLayout nl, VkPipelineStageFlags dsts, VkAccessFlags dsta) {
	vulkan_change_layout_queue(cb, img, ol, srcs, srca, nl, dsts, dsta,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
}

bool vulkan_has_extension(size_t count, const char **exts, const char *find) {
	for (unsigned i = 0; i < count; ++i) {
		if (strcmp(exts[i], find) == 0) {
			return true;
		}
	}

	return false;
}
