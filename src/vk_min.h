// The few Vulkan declarations the DLSS 5 bridge needs (dlss5_bridge.inl), in namespace vkm so
// they cannot clash with a real <vulkan/vulkan.h>. Names, values and layouts follow the
// Khronos headers; tests/vk_min_check.cpp checks every one of them against vulkan_core.h and
// vulkan_win32.h when build.ps1 is given their folder in VULKAN_HEADERS_INCLUDE.

#pragma once

#include <windows.h>

#include <stdint.h>

namespace vkm {

#define VKM_DISPATCHABLE(name) typedef struct name##_T* name;
VKM_DISPATCHABLE(VkInstance)
VKM_DISPATCHABLE(VkPhysicalDevice)
VKM_DISPATCHABLE(VkDevice)
VKM_DISPATCHABLE(VkQueue)
VKM_DISPATCHABLE(VkCommandBuffer)
#undef VKM_DISPATCHABLE

// Non-dispatchable handles are 64-bit integers on 32-bit platforms.
typedef uint64_t VkImage;
typedef uint64_t VkBuffer;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef uint64_t VkCommandPool;

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef int32_t VkResult;
typedef int32_t VkStructureType;
typedef int32_t VkFormat;
typedef int32_t VkImageLayout;
typedef int32_t VkImageType;
typedef int32_t VkImageTiling;
typedef int32_t VkSharingMode;
typedef int32_t VkSemaphoreType;
typedef int32_t VkCommandBufferLevel;
typedef VkFlags VkImageCreateFlags;
typedef VkFlags VkImageUsageFlags;
typedef VkFlags VkSampleCountFlagBits;
typedef VkFlags VkImageAspectFlags;
typedef VkFlags VkAccessFlags;
typedef VkFlags VkPipelineStageFlags;
typedef VkFlags VkDependencyFlags;
typedef VkFlags VkMemoryPropertyFlags;
typedef VkFlags VkMemoryHeapFlags;
typedef VkFlags VkExternalMemoryHandleTypeFlags;
typedef VkFlags VkExternalSemaphoreHandleTypeFlagBits;
typedef VkFlags VkSemaphoreImportFlags;
typedef VkFlags VkCommandPoolCreateFlags;
typedef VkFlags VkCommandBufferUsageFlags;
typedef VkFlags VkFenceCreateFlags;
typedef VkFlags VkSemaphoreCreateFlags;

constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_TIMEOUT = 2;

constexpr VkStructureType VK_STRUCTURE_TYPE_SUBMIT_INFO = 4;
constexpr VkStructureType VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5;
constexpr VkStructureType VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14;
constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39;
constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40;
constexpr VkStructureType VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45;
constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001;
constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004;
constexpr VkStructureType VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO = 1000072001;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR = 1000073000;
constexpr VkStructureType VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR = 1000073002;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR = 1000078000;
constexpr VkStructureType VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO = 1000127001;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO = 1000207002;
constexpr VkStructureType VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO = 1000207003;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO = 1000207005;

constexpr VkFormat VK_FORMAT_R8G8B8A8_UNORM = 37;
constexpr VkFormat VK_FORMAT_B8G8R8A8_UNORM = 44;
constexpr VkFormat VK_FORMAT_R16G16_SFLOAT = 83;
constexpr VkFormat VK_FORMAT_R32_SFLOAT = 100;

constexpr VkImageLayout VK_IMAGE_LAYOUT_UNDEFINED = 0;
constexpr VkImageLayout VK_IMAGE_LAYOUT_GENERAL = 1;
constexpr VkImageLayout VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6;
constexpr VkImageLayout VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7;

constexpr VkImageType VK_IMAGE_TYPE_2D = 1;
constexpr VkImageTiling VK_IMAGE_TILING_OPTIMAL = 0;
constexpr VkSharingMode VK_SHARING_MODE_EXCLUSIVE = 0;
constexpr VkSampleCountFlagBits VK_SAMPLE_COUNT_1_BIT = 0x1;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x1;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_SAMPLED_BIT = 0x4;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_STORAGE_BIT = 0x8;
constexpr VkImageAspectFlags VK_IMAGE_ASPECT_COLOR_BIT = 0x1;
constexpr VkAccessFlags VK_ACCESS_TRANSFER_READ_BIT = 0x800;
constexpr VkAccessFlags VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000;
constexpr VkAccessFlags VK_ACCESS_MEMORY_READ_BIT = 0x8000;
constexpr VkAccessFlags VK_ACCESS_MEMORY_WRITE_BIT = 0x10000;
constexpr VkPipelineStageFlags VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000;
constexpr VkPipelineStageFlags VK_PIPELINE_STAGE_ALL_COMMANDS_BIT = 0x10000;
constexpr VkMemoryPropertyFlags VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1;
constexpr VkExternalMemoryHandleTypeFlags VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT = 0x40;
constexpr VkExternalSemaphoreHandleTypeFlagBits VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT = 0x8;
constexpr VkSemaphoreType VK_SEMAPHORE_TYPE_TIMELINE = 1;
constexpr VkCommandPoolCreateFlags VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2;
constexpr VkCommandBufferLevel VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;
constexpr VkCommandBufferUsageFlags VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1;
constexpr VkFenceCreateFlags VK_FENCE_CREATE_SIGNALED_BIT = 0x1;
constexpr uint32_t VKM_QUEUE_FAMILY_IGNORED = ~0u;
constexpr uint32_t VKM_QUEUE_FAMILY_EXTERNAL = ~1u;
constexpr uint32_t VKM_MAX_MEMORY_TYPES = 32;
constexpr uint32_t VKM_MAX_MEMORY_HEAPS = 16;
constexpr uint32_t VKM_UUID_SIZE = 16;
constexpr uint32_t VKM_LUID_SIZE = 8;

struct VkExtent3D { uint32_t width, height, depth; };
struct VkOffset3D { int32_t x, y, z; };

struct VkImageCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkImageCreateFlags flags;
    VkImageType imageType;
    VkFormat format;
    VkExtent3D extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    VkSampleCountFlagBits samples;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkSharingMode sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    VkImageLayout initialLayout;
};

struct VkExternalMemoryImageCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkExternalMemoryHandleTypeFlags handleTypes;
};

struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
};

struct VkMemoryAllocateInfo {
    VkStructureType sType;
    const void* pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
};

struct VkMemoryDedicatedAllocateInfo {
    VkStructureType sType;
    const void* pNext;
    VkImage image;
    VkBuffer buffer;
};

struct VkImportMemoryWin32HandleInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkExternalMemoryHandleTypeFlags handleType;
    HANDLE handle;
    LPCWSTR name;
};

struct VkMemoryWin32HandlePropertiesKHR {
    VkStructureType sType;
    void* pNext;
    uint32_t memoryTypeBits;
};

struct VkMemoryType { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; };
struct VkMemoryHeap { VkDeviceSize size; VkMemoryHeapFlags flags; };

struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[VKM_MAX_MEMORY_TYPES];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[VKM_MAX_MEMORY_HEAPS];
};

struct VkPhysicalDeviceIDProperties {
    VkStructureType sType;
    void* pNext;
    uint8_t deviceUUID[VKM_UUID_SIZE];
    uint8_t driverUUID[VKM_UUID_SIZE];
    uint8_t deviceLUID[VKM_LUID_SIZE];
    uint32_t deviceNodeMask;
    VkBool32 deviceLUIDValid;
};

struct VkSemaphoreCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkSemaphoreCreateFlags flags;
};

struct VkSemaphoreTypeCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkSemaphoreType semaphoreType;
    uint64_t initialValue;
};

struct VkImportSemaphoreWin32HandleInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkSemaphore semaphore;
    VkSemaphoreImportFlags flags;
    VkExternalSemaphoreHandleTypeFlagBits handleType;
    HANDLE handle;
    LPCWSTR name;
};

struct VkFenceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFenceCreateFlags flags;
};

struct VkCommandPoolCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkCommandPoolCreateFlags flags;
    uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
    VkStructureType sType;
    const void* pNext;
    VkCommandPool commandPool;
    VkCommandBufferLevel level;
    uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
    VkStructureType sType;
    const void* pNext;
    VkCommandBufferUsageFlags flags;
    const void* pInheritanceInfo;
};

struct VkImageSubresourceRange {
    VkImageAspectFlags aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkImageSubresourceLayers {
    VkImageAspectFlags aspectMask;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkImageMemoryBarrier {
    VkStructureType sType;
    const void* pNext;
    VkAccessFlags srcAccessMask;
    VkAccessFlags dstAccessMask;
    VkImageLayout oldLayout;
    VkImageLayout newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
};

struct VkImageCopy {
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D srcOffset;
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffset;
    VkExtent3D extent;
};

struct VkSubmitInfo {
    VkStructureType sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    const VkPipelineStageFlags* pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore* pSignalSemaphores;
};

struct VkTimelineSemaphoreSubmitInfo {
    VkStructureType sType;
    const void* pNext;
    uint32_t waitSemaphoreValueCount;
    const uint64_t* pWaitSemaphoreValues;
    uint32_t signalSemaphoreValueCount;
    const uint64_t* pSignalSemaphoreValues;
};

struct VkSemaphoreSignalInfo {
    VkStructureType sType;
    const void* pNext;
    VkSemaphore semaphore;
    uint64_t value;
};

// Just the head of VkPhysicalDeviceProperties2: callers pass a buffer large enough for the
// whole structure (the properties member is about 800 bytes) and read only the pNext chain.
struct VkPhysicalDeviceProperties2Head {
    VkStructureType sType;
    void* pNext;
};

#define VKM_API __stdcall
typedef void (VKM_API* PFN_vkVoidFunction)();
typedef PFN_vkVoidFunction (VKM_API* PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction (VKM_API* PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef void (VKM_API* PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
typedef void (VKM_API* PFN_vkGetPhysicalDeviceProperties2)(VkPhysicalDevice, void*);
typedef VkResult (VKM_API* PFN_vkCreateImage)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*);
typedef void (VKM_API* PFN_vkDestroyImage)(VkDevice, VkImage, const void*);
typedef void (VKM_API* PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*);
typedef VkResult (VKM_API* PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*);
typedef void (VKM_API* PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);
typedef VkResult (VKM_API* PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
typedef VkResult (VKM_API* PFN_vkGetMemoryWin32HandlePropertiesKHR)(VkDevice, VkExternalMemoryHandleTypeFlags, HANDLE,
    VkMemoryWin32HandlePropertiesKHR*);
typedef VkResult (VKM_API* PFN_vkCreateSemaphore)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*);
typedef void (VKM_API* PFN_vkDestroySemaphore)(VkDevice, VkSemaphore, const void*);
typedef VkResult (VKM_API* PFN_vkImportSemaphoreWin32HandleKHR)(VkDevice, const VkImportSemaphoreWin32HandleInfoKHR*);
typedef VkResult (VKM_API* PFN_vkGetSemaphoreCounterValue)(VkDevice, VkSemaphore, uint64_t*);
typedef VkResult (VKM_API* PFN_vkSignalSemaphore)(VkDevice, const VkSemaphoreSignalInfo*);
typedef VkResult (VKM_API* PFN_vkCreateFence)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*);
typedef void (VKM_API* PFN_vkDestroyFence)(VkDevice, VkFence, const void*);
typedef VkResult (VKM_API* PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
typedef VkResult (VKM_API* PFN_vkResetFences)(VkDevice, uint32_t, const VkFence*);
typedef VkResult (VKM_API* PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
typedef void (VKM_API* PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);
typedef VkResult (VKM_API* PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef VkResult (VKM_API* PFN_vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
typedef VkResult (VKM_API* PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef VkResult (VKM_API* PFN_vkResetCommandBuffer)(VkCommandBuffer, VkFlags);
typedef void (VKM_API* PFN_vkCmdPipelineBarrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const void*, uint32_t, const void*, uint32_t, const VkImageMemoryBarrier*);
typedef void (VKM_API* PFN_vkCmdCopyImage)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*);
typedef VkResult (VKM_API* PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);

} // namespace vkm

// DXVK's D3D9 interop interfaces (src/d3d9/d3d9_interfaces.h in DXVK): the same IIDs and
// method order, declared with the vkm types.
MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
ID3D9VkInteropTexture : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(vkm::VkImage* pHandle, vkm::VkImageLayout* pLayout,
        vkm::VkImageCreateInfo* pInfo) = 0;
};

MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDevice : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(vkm::VkInstance* pInstance, vkm::VkPhysicalDevice* pPhysDev,
        vkm::VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(vkm::VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture,
        const vkm::VkImageSubresourceRange* pSubresources, vkm::VkImageLayout OldLayout, vkm::VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
};
