// Compile-time check of src/vk_min.h against the Khronos headers: every constant, structure
// size and field offset the DLSS 5 bridge relies on. build.ps1 compiles it (x86) when
// VULKAN_HEADERS_INCLUDE names a Vulkan-Headers include folder; it needs no Vulkan runtime.

#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdio>

#include "../src/vk_min.h"

#define SAME_VALUE(name) static_assert(static_cast<long long>(vkm::name) == static_cast<long long>(name), #name)
#define SAME_STRUCT(type) static_assert(sizeof(vkm::type) == sizeof(::type) && alignof(vkm::type) == alignof(::type), #type)
#define SAME_FIELD(type, field) static_assert(offsetof(vkm::type, field) == offsetof(::type, field), #type "." #field)

SAME_VALUE(VK_SUCCESS);
SAME_VALUE(VK_TIMEOUT);
SAME_VALUE(VK_STRUCTURE_TYPE_SUBMIT_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
SAME_VALUE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
SAME_VALUE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
SAME_VALUE(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR);
SAME_VALUE(VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR);
SAME_VALUE(VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR);
SAME_VALUE(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
SAME_VALUE(VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
SAME_VALUE(VK_FORMAT_R8G8B8A8_UNORM);
SAME_VALUE(VK_FORMAT_B8G8R8A8_UNORM);
SAME_VALUE(VK_FORMAT_R16G16_SFLOAT);
SAME_VALUE(VK_FORMAT_R16G16B16A16_SFLOAT);
SAME_VALUE(VK_FORMAT_R32_SFLOAT);
SAME_VALUE(VK_IMAGE_LAYOUT_UNDEFINED);
SAME_VALUE(VK_IMAGE_LAYOUT_GENERAL);
SAME_VALUE(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
SAME_VALUE(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
SAME_VALUE(VK_IMAGE_TYPE_2D);
SAME_VALUE(VK_IMAGE_TILING_OPTIMAL);
SAME_VALUE(VK_SHARING_MODE_EXCLUSIVE);
SAME_VALUE(VK_SAMPLE_COUNT_1_BIT);
SAME_VALUE(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
SAME_VALUE(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
SAME_VALUE(VK_IMAGE_USAGE_SAMPLED_BIT);
SAME_VALUE(VK_IMAGE_USAGE_STORAGE_BIT);
SAME_VALUE(VK_IMAGE_ASPECT_COLOR_BIT);
SAME_VALUE(VK_ACCESS_TRANSFER_READ_BIT);
SAME_VALUE(VK_ACCESS_TRANSFER_WRITE_BIT);
SAME_VALUE(VK_ACCESS_MEMORY_READ_BIT);
SAME_VALUE(VK_ACCESS_MEMORY_WRITE_BIT);
SAME_VALUE(VK_PIPELINE_STAGE_TRANSFER_BIT);
SAME_VALUE(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
SAME_VALUE(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
SAME_VALUE(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT);
SAME_VALUE(VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
SAME_VALUE(VK_SEMAPHORE_TYPE_TIMELINE);
SAME_VALUE(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
SAME_VALUE(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
SAME_VALUE(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
SAME_VALUE(VK_FENCE_CREATE_SIGNALED_BIT);
static_assert(vkm::VKM_QUEUE_FAMILY_IGNORED == VK_QUEUE_FAMILY_IGNORED, "VK_QUEUE_FAMILY_IGNORED");
static_assert(vkm::VKM_QUEUE_FAMILY_EXTERNAL == VK_QUEUE_FAMILY_EXTERNAL, "VK_QUEUE_FAMILY_EXTERNAL");
static_assert(vkm::VKM_MAX_MEMORY_TYPES == VK_MAX_MEMORY_TYPES, "VK_MAX_MEMORY_TYPES");
static_assert(vkm::VKM_MAX_MEMORY_HEAPS == VK_MAX_MEMORY_HEAPS, "VK_MAX_MEMORY_HEAPS");
static_assert(vkm::VKM_UUID_SIZE == VK_UUID_SIZE, "VK_UUID_SIZE");
static_assert(vkm::VKM_LUID_SIZE == VK_LUID_SIZE, "VK_LUID_SIZE");

static_assert(sizeof(vkm::VkImage) == sizeof(::VkImage), "non-dispatchable handle size");
static_assert(sizeof(vkm::VkSemaphore) == sizeof(::VkSemaphore), "non-dispatchable handle size");

SAME_STRUCT(VkExtent3D);
SAME_STRUCT(VkOffset3D);
SAME_STRUCT(VkImageCreateInfo);
SAME_FIELD(VkImageCreateInfo, format);
SAME_FIELD(VkImageCreateInfo, extent);
SAME_FIELD(VkImageCreateInfo, usage);
SAME_FIELD(VkImageCreateInfo, pQueueFamilyIndices);
SAME_FIELD(VkImageCreateInfo, initialLayout);
SAME_STRUCT(VkExternalMemoryImageCreateInfo);
SAME_STRUCT(VkMemoryRequirements);
SAME_FIELD(VkMemoryRequirements, memoryTypeBits);
SAME_STRUCT(VkMemoryAllocateInfo);
SAME_FIELD(VkMemoryAllocateInfo, allocationSize);
SAME_FIELD(VkMemoryAllocateInfo, memoryTypeIndex);
SAME_STRUCT(VkMemoryDedicatedAllocateInfo);
SAME_FIELD(VkMemoryDedicatedAllocateInfo, image);
SAME_FIELD(VkMemoryDedicatedAllocateInfo, buffer);
SAME_STRUCT(VkImportMemoryWin32HandleInfoKHR);
SAME_FIELD(VkImportMemoryWin32HandleInfoKHR, handle);
SAME_FIELD(VkImportMemoryWin32HandleInfoKHR, name);
SAME_STRUCT(VkMemoryWin32HandlePropertiesKHR);
SAME_STRUCT(VkMemoryType);
SAME_STRUCT(VkMemoryHeap);
SAME_STRUCT(VkPhysicalDeviceMemoryProperties);
SAME_FIELD(VkPhysicalDeviceMemoryProperties, memoryHeapCount);
SAME_FIELD(VkPhysicalDeviceMemoryProperties, memoryHeaps);
SAME_STRUCT(VkPhysicalDeviceIDProperties);
SAME_FIELD(VkPhysicalDeviceIDProperties, deviceLUID);
SAME_FIELD(VkPhysicalDeviceIDProperties, deviceLUIDValid);
SAME_STRUCT(VkSemaphoreCreateInfo);
SAME_STRUCT(VkSemaphoreTypeCreateInfo);
SAME_FIELD(VkSemaphoreTypeCreateInfo, initialValue);
SAME_STRUCT(VkImportSemaphoreWin32HandleInfoKHR);
SAME_FIELD(VkImportSemaphoreWin32HandleInfoKHR, semaphore);
SAME_FIELD(VkImportSemaphoreWin32HandleInfoKHR, flags);
SAME_FIELD(VkImportSemaphoreWin32HandleInfoKHR, handleType);
SAME_FIELD(VkImportSemaphoreWin32HandleInfoKHR, handle);
SAME_STRUCT(VkFenceCreateInfo);
SAME_STRUCT(VkCommandPoolCreateInfo);
SAME_STRUCT(VkCommandBufferAllocateInfo);
SAME_FIELD(VkCommandBufferAllocateInfo, commandPool);
SAME_FIELD(VkCommandBufferAllocateInfo, commandBufferCount);
SAME_STRUCT(VkCommandBufferBeginInfo);
SAME_STRUCT(VkImageSubresourceRange);
SAME_STRUCT(VkImageSubresourceLayers);
SAME_STRUCT(VkImageMemoryBarrier);
SAME_FIELD(VkImageMemoryBarrier, image);
SAME_FIELD(VkImageMemoryBarrier, subresourceRange);
SAME_STRUCT(VkImageCopy);
SAME_FIELD(VkImageCopy, dstOffset);
SAME_FIELD(VkImageCopy, extent);
SAME_STRUCT(VkSubmitInfo);
SAME_FIELD(VkSubmitInfo, pSignalSemaphores);
SAME_STRUCT(VkTimelineSemaphoreSubmitInfo);
SAME_FIELD(VkTimelineSemaphoreSubmitInfo, pSignalSemaphoreValues);
SAME_STRUCT(VkSemaphoreSignalInfo);
SAME_FIELD(VkSemaphoreSignalInfo, value);
static_assert(offsetof(vkm::VkPhysicalDeviceProperties2Head, pNext) == offsetof(::VkPhysicalDeviceProperties2, pNext), "properties2 pNext");
static_assert(sizeof(::VkPhysicalDeviceProperties2) <= 4096, "the bridge passes a 4 KiB buffer");

int main()
{
    std::printf("PASS: vk_min.h matches the Vulkan headers\n");
    return 0;
}
