// DLSS 5 through dsf-dlss5-helper (dlss5/include/dsf_dlss5_protocol.h). [DLSS5] Helper=1 (with
// Export=1) turns it on.
//
// Each frame, at the pre-HUD point (dlss5_feed.inl):
//   1. two small D3D9 passes write the inputs the helper wants: depth as R32F z/w from the
//      scene depth (an INTZ texture that replaces the game's back-buffer-sized D24S8 surface)
//      and camera motion vectors as RG16F, reprojected through this and the previous frame's
//      view-projection;
//   2. colour, depth and motion go to the helper's shared textures, fence_in = n, FRAME n;
//   3. FRAME_ACK n (the helper sends it once its GPU work is done, so fence_out >= n), then
//      the output is drawn over the HUD target before the HUD.
//
// Two transports move the images (HelperTransport):
//   - Vulkan, under DXVK: DXVK's D3D9 interop (ID3D9VkInteropDevice) gives the VkImages of the
//     game's and our textures and the queue; the helper creates the shared textures and fences
//     on D3D12 and DSF Enhanced imports them as Vulkan external memory and timeline semaphores.
//     Copies stay on the GPU.
//   - CPU, anywhere else (native D3D9, dgVoodoo2): the images are read back and uploaded into
//     D3D11 textures DSF Enhanced creates and shares (CLIENT_CREATES). Several ms a frame.
//
// The helper is started and configured on a worker thread. The render thread only sends FRAME
// and reads FRAME_ACK, with timeouts; any failure drops DLSS 5 for that frame (the game's own
// image stays), a failed frame is followed by a new CONFIGURE, and repeated failures or a dead
// helper turn it off for the session. Nothing ever waits on a fence the helper might not signal.
//
// Motion vectors are camera-only: every pixel is treated as static world, except pixels
// nearer than CockpitDepth metres (the car interior moves with the camera: zero motion).

using D3D11CreateDeviceFn = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDXGIFactory1Fn = HRESULT (WINAPI*)(REFIID, void**);
using D3DCompileFn = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT,
    ID3DBlob**, ID3DBlob**);

enum D5Transport : int
{
    D5_TRANSPORT_AUTO = 0,
    D5_TRANSPORT_CPU = 1,
    D5_TRANSPORT_VULKAN = 2,
};

bool g_d5_helper_bypass = false;
int g_d5_helper_transport = D5_TRANSPORT_AUTO;
char g_d5_helper_path[MAX_PATH] = {};
char g_d5_helper_runtime[MAX_PATH] = {};
char g_d5_helper_log[MAX_PATH] = {};
int g_d5_style = 0;
float g_d5_intensity = 1.0f;
float g_d5_tone_strength = 0.3f;
float g_d5_structure_strength = 0.7f;
float g_d5_skin_strength = 0.5f;
float g_d5_cockpit_depth = 1.5f;
bool g_d5_frame_time_log = false;
bool g_d5_crash_dump = false;

constexpr DWORD kD5HelloTimeoutMs = 20000;      // the helper loads and checks the runtime, starts NGX
constexpr DWORD kD5ConfigureTimeoutMs = 20000;  // creates the feature
constexpr DWORD kD5FrameTimeoutMs = 3000;       // the helper gives up on fence_in after about 2 s
constexpr int kD5MaxConfigureAttempts = 3;

void LoadDlss5BridgeConfig(const char* ini_path)
{
    g_d5_helper_bypass = GetPrivateProfileIntA("DLSS5", "HelperBypass", 0, ini_path) != 0;
    g_d5_helper_transport = ClampInt(GetPrivateProfileIntA("DLSS5", "HelperTransport", 0, ini_path), 0, 2);
    GetPrivateProfileStringA("DLSS5", "HelperPath", "", g_d5_helper_path, MAX_PATH, ini_path);
    if (g_d5_helper_path[0] == '\0') {
        BuildGamePath(g_d5_helper_path, MAX_PATH, "dlss5\\dsf-dlss5-helper.exe");
    }
    GetPrivateProfileStringA("DLSS5", "HelperRuntime", "", g_d5_helper_runtime, MAX_PATH, ini_path);
    if (g_d5_helper_runtime[0] == '\0') {
        BuildGamePath(g_d5_helper_runtime, MAX_PATH, "dlss5");
    }
    GetPrivateProfileStringA("DLSS5", "HelperLog", "", g_d5_helper_log, MAX_PATH, ini_path);
    if (g_d5_helper_log[0] == '\0') {
        BuildGamePath(g_d5_helper_log, MAX_PATH, "dsf-dlss5-helper.log");
    }
    g_d5_style = ClampInt(GetPrivateProfileIntA("DLSS5", "Style", 0, ini_path), 0, 2);
    g_d5_intensity = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "Intensity", 1.0f, ini_path), 1.0f));
    g_d5_tone_strength = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "ToneStrength", 0.3f, ini_path), 1.0f));
    g_d5_structure_strength = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "StructureStrength", 0.7f, ini_path), 1.0f));
    g_d5_skin_strength = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "SkinStrength", 0.5f, ini_path), 1.0f));
    g_d5_cockpit_depth = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "CockpitDepth", 1.5f, ini_path), 10.0f));
    g_d5_frame_time_log = GetPrivateProfileIntA("DLSS5", "FrameTimeLog", 0, ini_path) != 0;
    g_d5_crash_dump = GetPrivateProfileIntA("DLSS5", "CrashDump", 0, ini_path) != 0;
}

template <typename T>
void D5SafeRelease(T*& object)
{
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

double D5NowMs()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

// ---------------------------------------------------------------------------------------
// Frame times (FrameTimeLog=1): present-to-present interval and the bridge's render-thread
// cost, logged every 600 frames.

double g_d5_last_present_ms = 0.0;
float g_d5_frame_intervals[600] = {};
int g_d5_frame_interval_count = 0;
double g_d5_window_bridge_ms = 0.0;
int g_d5_window_bridge_frames = 0;
double g_d5_window_gpu_ms = 0.0;
int g_d5_window_gpu_frames = 0;
const char* g_d5_transport_name = "none";

void D5FrameTimeTick()
{
    if (!g_d5_frame_time_log) {
        return;
    }
    const double now = D5NowMs();
    if (g_d5_last_present_ms != 0.0) {
        g_d5_frame_intervals[g_d5_frame_interval_count++] = static_cast<float>(now - g_d5_last_present_ms);
    }
    g_d5_last_present_ms = now;
    if (g_d5_frame_interval_count < static_cast<int>(sizeof(g_d5_frame_intervals) / sizeof(g_d5_frame_intervals[0]))) {
        return;
    }
    const int count = g_d5_frame_interval_count;
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        sum += g_d5_frame_intervals[i];
    }
    std::sort(g_d5_frame_intervals, g_d5_frame_intervals + count);
    Log("DLSS5 frametime: %d frames mean %.2f ms median %.2f p95 %.2f max %.2f | DLSS 5 on %d of them, bridge %.2f ms, evaluate %.2f ms, transport %s",
        count, sum / count, static_cast<double>(g_d5_frame_intervals[count / 2]), static_cast<double>(g_d5_frame_intervals[count * 95 / 100]),
        static_cast<double>(g_d5_frame_intervals[count - 1]), g_d5_window_bridge_frames,
        g_d5_window_bridge_frames > 0 ? g_d5_window_bridge_ms / g_d5_window_bridge_frames : 0.0,
        g_d5_window_gpu_frames > 0 ? g_d5_window_gpu_ms / g_d5_window_gpu_frames : 0.0, g_d5_transport_name);
    g_d5_frame_interval_count = 0;
    g_d5_window_bridge_ms = 0.0;
    g_d5_window_bridge_frames = 0;
    g_d5_window_gpu_ms = 0.0;
    g_d5_window_gpu_frames = 0;
}

// ---------------------------------------------------------------------------------------
// INTZ scene depth. The game's back-buffer-sized D24S8 depth surfaces are created as the top
// level of an INTZ texture instead, so the depth can be sampled. The surface keeps its texture
// alive; the map only remembers which texture a surface belongs to.

constexpr D3DFORMAT kD5FormatIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

bool D5CreateIntzDepth(IDirect3DDevice9* device, UINT w, UINT h, IDirect3DSurface9** out)
{
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(g_d5_create_texture(device, w, h, 1, D3DUSAGE_DEPTHSTENCIL, kD5FormatIntz, D3DPOOL_DEFAULT, &texture, nullptr)) ||
        texture == nullptr) {
        return false;
    }
    IDirect3DSurface9* surface = nullptr;
    if (FAILED(texture->GetSurfaceLevel(0, &surface)) || surface == nullptr) {
        texture->Release();
        return false;
    }
    texture->Release();   // the surface holds the texture
    *out = surface;
    return true;
}

// The INTZ texture a depth surface belongs to (a reference the caller releases), or null.
IDirect3DTexture9* D5IntzTextureOf(const void* surface_object)
{
    if (surface_object == nullptr) {
        return nullptr;
    }
    auto* surface = static_cast<IDirect3DSurface9*>(const_cast<void*>(surface_object));
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(surface->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&texture))) || texture == nullptr) {
        return nullptr;
    }
    D3DSURFACE_DESC desc = {};
    if (FAILED(texture->GetLevelDesc(0, &desc)) || desc.Format != kD5FormatIntz) {
        texture->Release();
        return nullptr;
    }
    return texture;
}

// ---------------------------------------------------------------------------------------
// Bridge state. The worker thread owns the pipe while the state is STARTING or CONFIGURING;
// the render thread owns it (FRAME/FRAME_ACK) while RUNNING.

enum D5BridgeState : LONG
{
    D5_BRIDGE_OFF = 0,
    D5_BRIDGE_STARTING = 1,
    D5_BRIDGE_READY = 2,         // HELLO done, needs CONFIGURE
    D5_BRIDGE_CONFIGURING = 3,   // the worker is configuring
    D5_BRIDGE_RUNNING = 4,
    D5_BRIDGE_FAILED = 5,
};

volatile LONG g_d5_bridge_state = D5_BRIDGE_OFF;
HANDLE g_d5_worker_event = nullptr;
HANDLE g_d5_pipe = INVALID_HANDLE_VALUE;
HANDLE g_d5_helper_process = nullptr;
int g_d5_transport = D5_TRANSPORT_CPU;
LUID g_d5_adapter_luid = {};
UINT g_d5_want_width = 0;
UINT g_d5_want_height = 0;
UINT g_d5_bridge_width = 0;
UINT g_d5_bridge_height = 0;
std::uint64_t g_d5_frame_number = 0;
int g_d5_configure_attempts = 0;
LONG g_d5_bridge_frames = 0;
double g_d5_bridge_ms_sum = 0.0;
double g_d5_bridge_gpu_ms_sum = 0.0;
DXGI_FORMAT g_d5_slot_formats[DSF_DLSS5_SLOT_COUNT] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN};

void D5RemoveExceptionHandler();

void D5BridgeFail(const char* what)
{
    Log("DLSS5 helper: %s; DLSS 5 off for this session (the game's own image is shown)", what);
    InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_FAILED);
    if (g_d5_worker_event != nullptr) {
        SetEvent(g_d5_worker_event);   // the worker sends SHUTDOWN and ends
    }
    D5RemoveExceptionHandler();
}

// A frame failed but the connection still works: CONFIGURE again, a few times at most.
void D5BridgeRetry(const char* what)
{
    if (++g_d5_configure_attempts >= kD5MaxConfigureAttempts) {
        D5BridgeFail(what);
        return;
    }
    Log("DLSS5 helper: %s; frame shown without DLSS 5, configuring again (attempt %d of %d)", what, g_d5_configure_attempts + 1,
        kD5MaxConfigureAttempts);
    InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_READY);
}

// ---------------------------------------------------------------------------------------
// CPU transport: D3D11 textures DSF Enhanced creates and shares (CLIENT_CREATES).

ID3D11Device* g_d5_d11 = nullptr;
ID3D11Device5* g_d5_d11_5 = nullptr;
ID3D11DeviceContext4* g_d5_ctx = nullptr;
ID3D11Texture2D* g_d5_slot_tex[DSF_DLSS5_SLOT_COUNT] = {};
ID3D11Texture2D* g_d5_output_staging = nullptr;
ID3D11Fence* g_d5_fence_in = nullptr;
ID3D11Fence* g_d5_fence_out = nullptr;

bool D5CreateD3D11(IDXGIAdapter1* adapter)
{
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    auto create_device = d3d11 != nullptr ? reinterpret_cast<D3D11CreateDeviceFn>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    if (create_device == nullptr) {
        return false;
    }
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g_d5_d11, nullptr, &context);
    if (FAILED(hr) || g_d5_d11 == nullptr) {
        Log("DLSS5 helper: D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    g_d5_d11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_d5_d11_5));
    context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_d5_ctx));
    context->Release();
    return g_d5_d11_5 != nullptr && g_d5_ctx != nullptr;
}

ID3D11Texture2D* D5CreateSharedTexture(UINT w, UINT h, DXGI_FORMAT format, UINT bind, std::uint64_t* handle_value)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_d5_d11->CreateTexture2D(&desc, nullptr, &texture)) || texture == nullptr) {
        return nullptr;
    }
    IDXGIResource1* resource = nullptr;
    HANDLE handle = nullptr;
    if (FAILED(texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource))) ||
        FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
        D5SafeRelease(resource);
        texture->Release();
        return nullptr;
    }
    resource->Release();
    *handle_value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    return texture;
}

void D5CpuRelease()
{
    for (auto*& texture : g_d5_slot_tex) {
        D5SafeRelease(texture);
    }
    D5SafeRelease(g_d5_output_staging);
    D5SafeRelease(g_d5_fence_in);
    D5SafeRelease(g_d5_fence_out);
}

// ---------------------------------------------------------------------------------------
// Vulkan transport: DXVK's device, queue and images, the helper's D3D12 textures and fences
// imported into it.

struct D5VkFunctions
{
    vkm::PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    vkm::PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    vkm::PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    vkm::PFN_vkCreateImage CreateImage;
    vkm::PFN_vkDestroyImage DestroyImage;
    vkm::PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    vkm::PFN_vkAllocateMemory AllocateMemory;
    vkm::PFN_vkFreeMemory FreeMemory;
    vkm::PFN_vkBindImageMemory BindImageMemory;
    vkm::PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR;
    vkm::PFN_vkCreateSemaphore CreateSemaphore;
    vkm::PFN_vkDestroySemaphore DestroySemaphore;
    vkm::PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR;
    vkm::PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue;
    vkm::PFN_vkCreateFence CreateFence;
    vkm::PFN_vkDestroyFence DestroyFence;
    vkm::PFN_vkWaitForFences WaitForFences;
    vkm::PFN_vkResetFences ResetFences;
    vkm::PFN_vkCreateCommandPool CreateCommandPool;
    vkm::PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    vkm::PFN_vkBeginCommandBuffer BeginCommandBuffer;
    vkm::PFN_vkEndCommandBuffer EndCommandBuffer;
    vkm::PFN_vkResetCommandBuffer ResetCommandBuffer;
    vkm::PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    vkm::PFN_vkCmdCopyImage CmdCopyImage;
    vkm::PFN_vkQueueSubmit QueueSubmit;
};

constexpr int kD5VkRing = 3;   // command buffers in flight per kind (inputs, output)

ID3D9VkInteropDevice* g_d5_vk_interop = nullptr;
D5VkFunctions g_vk = {};
vkm::VkInstance g_d5_vk_instance = nullptr;
vkm::VkPhysicalDevice g_d5_vk_physical = nullptr;
vkm::VkDevice g_d5_vk_device = nullptr;
vkm::VkQueue g_d5_vk_queue = nullptr;
uint32_t g_d5_vk_family = 0;
vkm::VkCommandPool g_d5_vk_pool = 0;
vkm::VkCommandBuffer g_d5_vk_cmd[2][kD5VkRing] = {};
vkm::VkFence g_d5_vk_cmd_fence[2][kD5VkRing] = {};
int g_d5_vk_ring = 0;
vkm::VkImage g_d5_vk_slot_image[DSF_DLSS5_SLOT_COUNT] = {};
vkm::VkDeviceMemory g_d5_vk_slot_memory[DSF_DLSS5_SLOT_COUNT] = {};
vkm::VkSemaphore g_d5_vk_fence_in = 0;
vkm::VkSemaphore g_d5_vk_fence_out = 0;
bool g_d5_vk_slot_touched[DSF_DLSS5_SLOT_COUNT] = {};

template <typename Fn>
bool D5VkLoad(Fn* target, const char* name, bool device_level)
{
    *target = device_level ? reinterpret_cast<Fn>(g_vk.GetDeviceProcAddr(g_d5_vk_device, name)) : nullptr;
    if (*target == nullptr) {
        Log("DLSS5 helper: Vulkan function %s missing", name);
    }
    return *target != nullptr;
}

// Render thread, once: the interop device, DXVK's Vulkan handles and our command buffers.
bool D5VkInit(IDirect3DDevice9* device)
{
    if (FAILED(device->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&g_d5_vk_interop))) ||
        g_d5_vk_interop == nullptr) {
        return false;
    }
    g_d5_vk_interop->GetVulkanHandles(&g_d5_vk_instance, &g_d5_vk_physical, &g_d5_vk_device);
    uint32_t queue_index = 0;
    g_d5_vk_interop->GetSubmissionQueue(&g_d5_vk_queue, &queue_index, &g_d5_vk_family);

    HMODULE loader = GetModuleHandleA("vulkan-1.dll");
    auto get_instance_proc = loader != nullptr ?
        reinterpret_cast<vkm::PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
    if (get_instance_proc == nullptr || g_d5_vk_device == nullptr || g_d5_vk_queue == nullptr) {
        Log("DLSS5 helper: DXVK interop found but no Vulkan loader or device");
        return false;
    }
    g_vk.GetDeviceProcAddr = reinterpret_cast<vkm::PFN_vkGetDeviceProcAddr>(get_instance_proc(g_d5_vk_instance, "vkGetDeviceProcAddr"));
    g_vk.GetPhysicalDeviceMemoryProperties = reinterpret_cast<vkm::PFN_vkGetPhysicalDeviceMemoryProperties>(
        get_instance_proc(g_d5_vk_instance, "vkGetPhysicalDeviceMemoryProperties"));
    g_vk.GetPhysicalDeviceProperties2 = reinterpret_cast<vkm::PFN_vkGetPhysicalDeviceProperties2>(
        get_instance_proc(g_d5_vk_instance, "vkGetPhysicalDeviceProperties2"));
    if (g_vk.GetDeviceProcAddr == nullptr || g_vk.GetPhysicalDeviceMemoryProperties == nullptr || g_vk.GetPhysicalDeviceProperties2 == nullptr) {
        return false;
    }
    bool ok = D5VkLoad(&g_vk.CreateImage, "vkCreateImage", true) && D5VkLoad(&g_vk.DestroyImage, "vkDestroyImage", true) &&
        D5VkLoad(&g_vk.GetImageMemoryRequirements, "vkGetImageMemoryRequirements", true) &&
        D5VkLoad(&g_vk.AllocateMemory, "vkAllocateMemory", true) && D5VkLoad(&g_vk.FreeMemory, "vkFreeMemory", true) &&
        D5VkLoad(&g_vk.BindImageMemory, "vkBindImageMemory", true) &&
        D5VkLoad(&g_vk.GetMemoryWin32HandlePropertiesKHR, "vkGetMemoryWin32HandlePropertiesKHR", true) &&
        D5VkLoad(&g_vk.CreateSemaphore, "vkCreateSemaphore", true) && D5VkLoad(&g_vk.DestroySemaphore, "vkDestroySemaphore", true) &&
        D5VkLoad(&g_vk.ImportSemaphoreWin32HandleKHR, "vkImportSemaphoreWin32HandleKHR", true) &&
        D5VkLoad(&g_vk.GetSemaphoreCounterValue, "vkGetSemaphoreCounterValue", true) &&
        D5VkLoad(&g_vk.CreateFence, "vkCreateFence", true) && D5VkLoad(&g_vk.DestroyFence, "vkDestroyFence", true) &&
        D5VkLoad(&g_vk.WaitForFences, "vkWaitForFences", true) && D5VkLoad(&g_vk.ResetFences, "vkResetFences", true) &&
        D5VkLoad(&g_vk.CreateCommandPool, "vkCreateCommandPool", true) &&
        D5VkLoad(&g_vk.AllocateCommandBuffers, "vkAllocateCommandBuffers", true) &&
        D5VkLoad(&g_vk.BeginCommandBuffer, "vkBeginCommandBuffer", true) &&
        D5VkLoad(&g_vk.EndCommandBuffer, "vkEndCommandBuffer", true) &&
        D5VkLoad(&g_vk.ResetCommandBuffer, "vkResetCommandBuffer", true) &&
        D5VkLoad(&g_vk.CmdPipelineBarrier, "vkCmdPipelineBarrier", true) && D5VkLoad(&g_vk.CmdCopyImage, "vkCmdCopyImage", true) &&
        D5VkLoad(&g_vk.QueueSubmit, "vkQueueSubmit", true);
    if (!ok) {
        return false;
    }

    // The adapter DXVK renders on: its LUID goes into HELLO.
    alignas(8) std::uint8_t properties[4096] = {};
    vkm::VkPhysicalDeviceIDProperties id = {};
    id.sType = vkm::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    auto* head = reinterpret_cast<vkm::VkPhysicalDeviceProperties2Head*>(properties);
    head->sType = vkm::VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    head->pNext = &id;
    g_vk.GetPhysicalDeviceProperties2(g_d5_vk_physical, properties);
    if (id.deviceLUIDValid) {
        std::memcpy(&g_d5_adapter_luid, id.deviceLUID, sizeof(g_d5_adapter_luid));
    }

    vkm::VkCommandPoolCreateInfo pool_info = {vkm::VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        vkm::VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_d5_vk_family};
    if (g_vk.CreateCommandPool(g_d5_vk_device, &pool_info, nullptr, &g_d5_vk_pool) != vkm::VK_SUCCESS) {
        return false;
    }
    vkm::VkCommandBufferAllocateInfo allocate = {vkm::VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_d5_vk_pool,
        vkm::VK_COMMAND_BUFFER_LEVEL_PRIMARY, kD5VkRing};
    const vkm::VkFenceCreateInfo fence_info = {vkm::VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, vkm::VK_FENCE_CREATE_SIGNALED_BIT};
    for (int kind = 0; kind < 2; ++kind) {
        if (g_vk.AllocateCommandBuffers(g_d5_vk_device, &allocate, g_d5_vk_cmd[kind]) != vkm::VK_SUCCESS) {
            return false;
        }
        for (int i = 0; i < kD5VkRing; ++i) {
            if (g_vk.CreateFence(g_d5_vk_device, &fence_info, nullptr, &g_d5_vk_cmd_fence[kind][i]) != vkm::VK_SUCCESS) {
                return false;
            }
        }
    }
    D3DDEVICE_CREATION_PARAMETERS creation = {};
    device->GetCreationParameters(&creation);
    Log("DLSS5 helper: device behaviour flags 0x%08lX (multithreaded %d)", static_cast<unsigned long>(creation.BehaviorFlags),
        (creation.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0 ? 1 : 0);
    Log("DLSS5 helper: Vulkan transport through DXVK interop: queue family %u, adapter LUID %08lX:%08lX%s", g_d5_vk_family,
        static_cast<unsigned long>(g_d5_adapter_luid.HighPart), static_cast<unsigned long>(g_d5_adapter_luid.LowPart),
        id.deviceLUIDValid ? "" : " (not reported)");
    return true;
}

// Waits for every submission of ours, so imported images can be released. Bounded.
bool D5VkWaitIdle()
{
    vkm::VkFence fences[2 * kD5VkRing] = {};
    for (int kind = 0; kind < 2; ++kind) {
        for (int i = 0; i < kD5VkRing; ++i) {
            fences[kind * kD5VkRing + i] = g_d5_vk_cmd_fence[kind][i];
        }
    }
    if (g_d5_vk_device != nullptr && fences[0] != 0 &&
        g_vk.WaitForFences(g_d5_vk_device, 2 * kD5VkRing, fences, 1, 2000ull * 1000 * 1000) != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: our Vulkan copies did not finish within 2 s");
        return false;
    }
    return true;
}

void D5VkReleaseImports()
{
    if (g_d5_vk_device == nullptr) {
        return;
    }
    // Still in use by the GPU: leak them rather than free memory a pending copy touches.
    const bool idle = D5VkWaitIdle();
    if (!idle) {
        Log("DLSS5 helper: the previous shared textures and fences stay allocated (GPU still busy)");
    }
    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) {
        if (!idle) {
            g_d5_vk_slot_image[slot] = 0;
            g_d5_vk_slot_memory[slot] = 0;
            g_d5_vk_slot_touched[slot] = false;
            continue;
        }
        if (g_d5_vk_slot_image[slot] != 0) {
            g_vk.DestroyImage(g_d5_vk_device, g_d5_vk_slot_image[slot], nullptr);
        }
        if (g_d5_vk_slot_memory[slot] != 0) {
            g_vk.FreeMemory(g_d5_vk_device, g_d5_vk_slot_memory[slot], nullptr);
        }
        g_d5_vk_slot_image[slot] = 0;
        g_d5_vk_slot_memory[slot] = 0;
        g_d5_vk_slot_touched[slot] = false;
    }
    if (g_d5_vk_fence_in != 0 && idle) {
        g_vk.DestroySemaphore(g_d5_vk_device, g_d5_vk_fence_in, nullptr);
    }
    if (g_d5_vk_fence_out != 0 && idle) {
        g_vk.DestroySemaphore(g_d5_vk_device, g_d5_vk_fence_out, nullptr);
    }
    g_d5_vk_fence_in = 0;
    g_d5_vk_fence_out = 0;
}

vkm::VkFormat D5VkFormatOf(uint32_t dxgi)
{
    switch (dxgi) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return vkm::VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return vkm::VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R32_FLOAT: return vkm::VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R16G16_FLOAT: return vkm::VK_FORMAT_R16G16_SFLOAT;
    default: return 0;
    }
}

// Imports one of the helper's D3D12 textures (an NT handle valid in this process).
bool D5VkImportImage(int slot, HANDLE handle, uint32_t dxgi_format, UINT w, UINT h)
{
    const vkm::VkFormat format = D5VkFormatOf(dxgi_format);
    if (format == 0) {
        Log("DLSS5 helper: slot %d has format %u, which the Vulkan transport does not map", slot, dxgi_format);
        return false;
    }
    vkm::VkExternalMemoryImageCreateInfo external = {vkm::VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
        vkm::VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT};
    vkm::VkImageCreateInfo info = {};
    info.sType = vkm::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &external;
    info.imageType = vkm::VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = vkm::VK_SAMPLE_COUNT_1_BIT;
    info.tiling = vkm::VK_IMAGE_TILING_OPTIMAL;
    info.usage = vkm::VK_IMAGE_USAGE_TRANSFER_SRC_BIT | vkm::VK_IMAGE_USAGE_TRANSFER_DST_BIT | vkm::VK_IMAGE_USAGE_SAMPLED_BIT |
        (slot == DSF_DLSS5_SLOT_OUTPUT ? vkm::VK_IMAGE_USAGE_STORAGE_BIT : 0);
    info.sharingMode = vkm::VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = vkm::VK_IMAGE_LAYOUT_UNDEFINED;
    vkm::VkResult result = g_vk.CreateImage(g_d5_vk_device, &info, nullptr, &g_d5_vk_slot_image[slot]);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: vkCreateImage for slot %d failed (%d)", slot, result);
        return false;
    }
    vkm::VkMemoryRequirements requirements = {};
    g_vk.GetImageMemoryRequirements(g_d5_vk_device, g_d5_vk_slot_image[slot], &requirements);
    vkm::VkMemoryWin32HandlePropertiesKHR handle_properties = {vkm::VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR, nullptr, 0};
    g_vk.GetMemoryWin32HandlePropertiesKHR(g_d5_vk_device, vkm::VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &handle_properties);
    uint32_t type_bits = requirements.memoryTypeBits & (handle_properties.memoryTypeBits != 0 ? handle_properties.memoryTypeBits : ~0u);
    vkm::VkPhysicalDeviceMemoryProperties memory = {};
    g_vk.GetPhysicalDeviceMemoryProperties(g_d5_vk_physical, &memory);
    uint32_t type_index = ~0u;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (type_index == ~0u || (memory.memoryTypes[i].propertyFlags & vkm::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)) {
            type_index = i;
            if ((memory.memoryTypes[i].propertyFlags & vkm::VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                break;
            }
        }
    }
    if (type_index == ~0u) {
        Log("DLSS5 helper: no memory type for slot %d (bits 0x%08X)", slot, type_bits);
        return false;
    }
    vkm::VkMemoryDedicatedAllocateInfo dedicated = {vkm::VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr,
        g_d5_vk_slot_image[slot], 0};
    vkm::VkImportMemoryWin32HandleInfoKHR import = {vkm::VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated,
        vkm::VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, nullptr};
    vkm::VkMemoryAllocateInfo allocate = {vkm::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, requirements.size, type_index};
    result = g_vk.AllocateMemory(g_d5_vk_device, &allocate, nullptr, &g_d5_vk_slot_memory[slot]);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: importing slot %d failed (%d)", slot, result);
        return false;
    }
    result = g_vk.BindImageMemory(g_d5_vk_device, g_d5_vk_slot_image[slot], g_d5_vk_slot_memory[slot], 0);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: binding slot %d failed (%d)", slot, result);
        return false;
    }
    return true;
}

bool D5VkImportSemaphore(vkm::VkSemaphore* semaphore, HANDLE handle)
{
    vkm::VkSemaphoreTypeCreateInfo type = {vkm::VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, vkm::VK_SEMAPHORE_TYPE_TIMELINE, 0};
    vkm::VkSemaphoreCreateInfo info = {vkm::VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type, 0};
    if (g_vk.CreateSemaphore(g_d5_vk_device, &info, nullptr, semaphore) != vkm::VK_SUCCESS) {
        return false;
    }
    vkm::VkImportSemaphoreWin32HandleInfoKHR import = {vkm::VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR, nullptr, *semaphore,
        0, vkm::VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, handle, nullptr};
    const vkm::VkResult result = g_vk.ImportSemaphoreWin32HandleKHR(g_d5_vk_device, &import);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: importing a fence failed (%d)", result);
        return false;
    }
    return true;
}

vkm::VkImageMemoryBarrier D5Barrier(vkm::VkImage image, vkm::VkAccessFlags src_access, vkm::VkAccessFlags dst_access,
    vkm::VkImageLayout old_layout, vkm::VkImageLayout new_layout, uint32_t src_family, uint32_t dst_family)
{
    vkm::VkImageMemoryBarrier barrier = {};
    barrier.sType = vkm::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = src_family;
    barrier.dstQueueFamilyIndex = dst_family;
    barrier.image = image;
    barrier.subresourceRange = {vkm::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

// ---------------------------------------------------------------------------------------
// Helper process, pipe, HELLO and CONFIGURE (worker thread).

bool D5StartHelper()
{
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, PROCESS_DUP_HANDLE | SYNCHRONIZE, TRUE, 0);
    char command[3 * MAX_PATH + 128] = {};
    std::snprintf(command, sizeof(command), "\"%s\" --game-pid %lu --game-process %llu --runtime \"%s\" --log \"%s\"",
        g_d5_helper_path, GetCurrentProcessId(), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(self)),
        g_d5_helper_runtime, g_d5_helper_log);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buffer(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self), nullptr, nullptr);
    STARTUPINFOEXA si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessA(nullptr, command, nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(self);
    if (!started) {
        Log("DLSS5 helper: could not start %s (error %lu)", command, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    g_d5_helper_process = pi.hProcess;
    Log("DLSS5 helper: started pid %lu: %s", pi.dwProcessId, command);

    wchar_t pipe_name[128] = {};
    swprintf_s(pipe_name, DSF_DLSS5_PIPE_FMT, GetCurrentProcessId());
    for (int attempt = 0; attempt < 300 && g_d5_pipe == INVALID_HANDLE_VALUE; ++attempt) {
        // Identification level only: the server may identify us but not act as us.
        g_d5_pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (g_d5_pipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                Log("DLSS5 helper: exited (code %lu) before opening its pipe", code);
                return false;
            }
            Sleep(50);
        }
    }
    if (g_d5_pipe == INVALID_HANDLE_VALUE) {
        Log("DLSS5 helper: could not connect to the pipe");
        return false;
    }
    // The pipe must belong to the helper we started, not to a process that took the name first.
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(g_d5_pipe, &server_pid) || server_pid != pi.dwProcessId) {
        Log("DLSS5 helper: the pipe's server is pid %lu, not the helper (pid %lu); not using it", server_pid, pi.dwProcessId);
        CloseHandle(g_d5_pipe);
        g_d5_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    DsfDlss5Hello hello = {GetCurrentProcessId(),
        static_cast<uint32_t>(g_d5_transport == D5_TRANSPORT_VULKAN ? DSF_DLSS5_API_VULKAN : DSF_DLSS5_API_D3D11),
        g_d5_adapter_luid.LowPart, g_d5_adapter_luid.HighPart};
    DsfDlss5HelloAck ack = {};
    DsfDlss5Error error = {};
    if (!dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_HELLO, hello, kD5HelloTimeoutMs) ||
        !dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_HELLO_ACK, &ack, kD5HelloTimeoutMs, &error)) {
        error.message[sizeof(error.message) - 1] = '\0';
        Log("DLSS5 helper: HELLO failed (error %lu, helper status %d: %s)", GetLastError(), error.status, error.message);
        return false;
    }
    ack.message[sizeof(ack.message) - 1] = '\0';
    ack.runtime_sha256[sizeof(ack.runtime_sha256) - 1] = '\0';
    Log("DLSS5 helper: HELLO_ACK status=%d helper_pid=%lu ngx=0x%08lX runtime %u.%u.%u.%u sha256=%s: %s", ack.status,
        static_cast<unsigned long>(ack.helper_pid), static_cast<unsigned long>(ack.ngx_result), ack.runtime_version[0],
        ack.runtime_version[1], ack.runtime_version[2], ack.runtime_version[3], ack.runtime_sha256, ack.message);
    return ack.status == DSF_DLSS5_OK;
}

// The adapter for HELLO on the CPU transport: the first NVIDIA one (DLSS 5 runs there).
bool D5FindNvidiaAdapter(IDXGIAdapter1** out)
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    auto create_factory = dxgi != nullptr ? reinterpret_cast<CreateDXGIFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (create_factory == nullptr || FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE) {
            g_d5_adapter_luid = desc.AdapterLuid;
            break;
        }
        D5SafeRelease(adapter);
    }
    factory->Release();
    *out = adapter;
    return adapter != nullptr;
}

enum D5ConfigureResult
{
    D5_CONFIGURE_OK,
    D5_CONFIGURE_FAILED,      // the stream is still in step: CONFIGURE again
    D5_CONFIGURE_PIPE_LOST,   // timeout or broken pipe: the stream is in an unknown state
};

D5ConfigureResult D5Configure(UINT w, UINT h)
{
    if (g_d5_transport == D5_TRANSPORT_VULKAN) {
        D5VkReleaseImports();
    } else {
        D5CpuRelease();
    }
    DsfDlss5Configure configure = {};
    configure.width = w;
    configure.height = h;
    configure.preset = 2;
    std::uint64_t handles[DSF_DLSS5_SLOT_COUNT] = {};
    for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
        configure.formats[slot] = g_d5_slot_formats[slot];
    }
    if (g_d5_transport == D5_TRANSPORT_CPU) {
        configure.flags = DSF_DLSS5_CONFIG_CLIENT_CREATES;
        for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
            const UINT bind = slot == DSF_DLSS5_SLOT_OUTPUT ? D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_SHADER_RESOURCE;
            g_d5_slot_tex[slot] = D5CreateSharedTexture(w, h, g_d5_slot_formats[slot], bind, &handles[slot]);
            if (g_d5_slot_tex[slot] == nullptr) {
                Log("DLSS5 helper: shared texture %d (%ux%u format %d) failed", slot, w, h, static_cast<int>(g_d5_slot_formats[slot]));
                for (std::uint64_t handle : handles) {
                    if (handle != 0) {
                        CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)));
                    }
                }
                return D5_CONFIGURE_FAILED;
            }
            configure.textures[slot] = handles[slot];
        }
    }
    DsfDlss5ConfigureAck ack = {};
    DsfDlss5Error error = {};
    const bool sent = dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_CONFIGURE, configure, kD5ConfigureTimeoutMs) &&
        dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_CONFIGURE_ACK, &ack, kD5ConfigureTimeoutMs, &error);
    for (std::uint64_t handle : handles) {
        if (handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)));
        }
    }
    if (!sent) {
        error.message[sizeof(error.message) - 1] = '\0';
        Log("DLSS5 helper: CONFIGURE failed on the pipe (error %lu, helper status %d: %s)", GetLastError(), error.status, error.message);
        return D5_CONFIGURE_PIPE_LOST;
    }
    ack.message[sizeof(ack.message) - 1] = '\0';
    Log("DLSS5 helper: CONFIGURE %ux%u -> status=%d ngx=0x%08lX formats %u/%u/%u/%u: %s", w, h, ack.status,
        static_cast<unsigned long>(ack.ngx_result), ack.formats[0], ack.formats[1], ack.formats[2], ack.formats[3], ack.message);
    const HANDLE fence_in = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.fence_in));
    const HANDLE fence_out = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.fence_out));
    bool ok = ack.status == DSF_DLSS5_OK;
    if (ok && g_d5_transport == D5_TRANSPORT_VULKAN) {
        for (int slot = 0; ok && slot < DSF_DLSS5_SLOT_MASK; ++slot) {
            ok = D5VkImportImage(slot, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.textures[slot])), ack.formats[slot], w, h);
        }
        ok = ok && D5VkImportSemaphore(&g_d5_vk_fence_in, fence_in) && D5VkImportSemaphore(&g_d5_vk_fence_out, fence_out);
    } else if (ok) {
        ok = SUCCEEDED(g_d5_d11_5->OpenSharedFence(fence_in, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_d5_fence_in))) &&
            SUCCEEDED(g_d5_d11_5->OpenSharedFence(fence_out, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_d5_fence_out)));
        if (ok) {
            D3D11_TEXTURE2D_DESC staging = {};
            g_d5_slot_tex[DSF_DLSS5_SLOT_OUTPUT]->GetDesc(&staging);
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.MiscFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ok = SUCCEEDED(g_d5_d11->CreateTexture2D(&staging, nullptr, &g_d5_output_staging));
        }
    }
    // The game owns every handle it received; imports keep their own references.
    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) {
        if (ack.textures[slot] != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.textures[slot])));
        }
    }
    if (fence_in != nullptr) {
        CloseHandle(fence_in);
    }
    if (fence_out != nullptr) {
        CloseHandle(fence_out);
    }
    if (!ok) {
        return D5_CONFIGURE_FAILED;
    }
    g_d5_bridge_width = w;
    g_d5_bridge_height = h;
    g_d5_frame_number = 0;   // n starts at 1 after every CONFIGURE
    return D5_CONFIGURE_OK;
}

DWORD WINAPI D5BridgeWorker(LPVOID)
{
    if (g_d5_transport == D5_TRANSPORT_CPU) {
        IDXGIAdapter1* adapter = nullptr;
        const bool device = D5FindNvidiaAdapter(&adapter) && D5CreateD3D11(adapter);
        D5SafeRelease(adapter);
        if (!device) {
            D5BridgeFail("no NVIDIA adapter or no D3D11.4 device for the CPU transport");
            return 0;
        }
    }
    if (!D5StartHelper()) {
        D5BridgeFail("the helper did not start");
        return 0;
    }
    InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_READY);
    for (;;) {
        WaitForSingleObject(g_d5_worker_event, INFINITE);
        const LONG state = g_d5_bridge_state;
        if (state == D5_BRIDGE_FAILED) {
            break;
        }
        if (state != D5_BRIDGE_CONFIGURING) {
            continue;
        }
        const D5ConfigureResult result = D5Configure(g_d5_want_width, g_d5_want_height);
        if (result == D5_CONFIGURE_OK) {
            InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_RUNNING);
        } else if (result == D5_CONFIGURE_PIPE_LOST) {
            D5BridgeFail("CONFIGURE got no answer");
            break;
        } else if (++g_d5_configure_attempts >= kD5MaxConfigureAttempts) {
            D5BridgeFail("CONFIGURE failed");
            break;
        } else {
            InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_READY);
        }
    }
    if (g_d5_pipe != INVALID_HANDLE_VALUE) {
        dsf_dlss5::PipeSendEmpty(g_d5_pipe, DSF_DLSS5_MSG_SHUTDOWN, 500);
    }
    return 0;
}

// CrashDump=1 (with Helper=1): the first hardware fault of the session (access violation,
// illegal instruction, in-page error and the like) is logged with its module, offset and the
// bridge step, and written to dsf_dlss5_crash_<pid>.dmp beside Driver.exe. The handler sits
// first in the chain, because DriverNGHook's own handler ends the process. It does no heap or
// loader work itself: it hands the exception to a thread started beforehand and waits for it.
// Stack overflows, C++ exceptions and debugger notifications are left alone. A fault that a
// handler further down would have caught can still be dumped; that is why it is opt-in.
// Logging only: every exception then goes on to the game's own handlers.
volatile LONG g_d5_veh_dumped = 0;
volatile LONG g_d5_step = 0;   // where D5BridgeFrame is (0: outside it)
PVOID g_d5_veh = nullptr;
HANDLE g_d5_dump_request = nullptr;
HANDLE g_d5_dump_done = nullptr;
EXCEPTION_POINTERS* g_d5_dump_pointers = nullptr;
DWORD g_d5_dump_thread_id = 0;
LONG g_d5_dump_step = 0;

using MiniDumpWriteDumpFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_d5_minidump_write = nullptr;

DWORD WINAPI D5DumpThread(LPVOID)
{
    WaitForSingleObject(g_d5_dump_request, INFINITE);
    const EXCEPTION_RECORD* record = g_d5_dump_pointers->ExceptionRecord;
    char where[160] = {};
    D5ModuleName(record->ExceptionAddress, where, sizeof(where));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        Log("DLSS5: exception 0x%08lX (access violation, %s 0x%08lX) at %s thread %lu step %ld", record->ExceptionCode,
            record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute",
            static_cast<unsigned long>(record->ExceptionInformation[1]), where, g_d5_dump_thread_id, g_d5_dump_step);
    } else {
        Log("DLSS5: exception 0x%08lX at %s thread %lu step %ld", record->ExceptionCode, where, g_d5_dump_thread_id, g_d5_dump_step);
    }
    char name[64] = {};
    std::snprintf(name, sizeof(name), "dsf_dlss5_crash_%lu.dmp", GetCurrentProcessId());
    char path[MAX_PATH] = {};
    BuildGamePath(path, MAX_PATH, name);
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception = {g_d5_dump_thread_id, g_d5_dump_pointers, FALSE};
        const BOOL ok = g_d5_minidump_write(GetCurrentProcess(), GetCurrentProcessId(), file,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithDataSegs), &exception,
            nullptr, nullptr);
        CloseHandle(file);
        Log("DLSS5: %s %s", ok ? "wrote" : "could not write", path);
    }
    SetEvent(g_d5_dump_done);
    return 0;
}

LONG CALLBACK D5OnFault(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;   // stack overflow, C++, debugger and the rest
    }
    if (InterlockedCompareExchange(&g_d5_veh_dumped, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_d5_dump_pointers = info;
    g_d5_dump_thread_id = GetCurrentThreadId();
    g_d5_dump_step = g_d5_step;
    SetEvent(g_d5_dump_request);
    WaitForSingleObject(g_d5_dump_done, 15000);
    return EXCEPTION_CONTINUE_SEARCH;
}

void D5InstallExceptionHandler()
{
    if (!g_d5_crash_dump || g_d5_veh != nullptr) {
        return;
    }
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    g_d5_minidump_write = dbghelp != nullptr ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    g_d5_dump_request = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_d5_dump_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    const HANDLE thread = g_d5_minidump_write != nullptr && g_d5_dump_request != nullptr && g_d5_dump_done != nullptr ?
        CreateThread(nullptr, 0, D5DumpThread, nullptr, 0, nullptr) : nullptr;
    if (thread == nullptr) {
        Log("DLSS5: CrashDump=1 but dbghelp or the dump thread is not available");
        return;
    }
    CloseHandle(thread);
    g_d5_veh = AddVectoredExceptionHandler(1, D5OnFault);
    Log("DLSS5: crash dumps on (first hardware fault -> dsf_dlss5_crash_<pid>.dmp)");
}

void D5RemoveExceptionHandler()
{
    PVOID handler = InterlockedExchangePointer(&g_d5_veh, nullptr);
    if (handler != nullptr) {
        RemoveVectoredExceptionHandler(handler);
    }
}

void D5BridgeStart(IDirect3DDevice9* device)
{
    if (!g_d5_helper || InterlockedCompareExchange(&g_d5_bridge_state, D5_BRIDGE_STARTING, D5_BRIDGE_OFF) != D5_BRIDGE_OFF) {
        return;
    }
    D5InstallExceptionHandler();
    g_d5_transport = D5_TRANSPORT_CPU;
    if (g_d5_helper_transport != D5_TRANSPORT_CPU) {
        if (D5VkInit(device)) {
            g_d5_transport = D5_TRANSPORT_VULKAN;
        } else if (g_d5_helper_transport == D5_TRANSPORT_VULKAN) {
            D5BridgeFail("HelperTransport=2 needs DXVK's Vulkan interop, which this d3d9.dll does not have");
            return;
        }
    }
    g_d5_transport_name = g_d5_transport == D5_TRANSPORT_VULKAN ? "vulkan" : "cpu";
    Log("DLSS5 helper: transport %s", g_d5_transport_name);
    g_d5_worker_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    const HANDLE thread = CreateThread(nullptr, 0, D5BridgeWorker, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

// ---------------------------------------------------------------------------------------
// D3D9 side: the input passes and the composite.

struct D5Vertex
{
    float x, y, z, rhw, u, v;
};

IDirect3DPixelShader9* g_d5_ps_depth = nullptr;
IDirect3DPixelShader9* g_d5_ps_motion = nullptr;
IDirect3DPixelShader9* g_d5_ps_copy = nullptr;
IDirect3DPixelShader9* g_d5_ps_copy_swap = nullptr;   // RGBA bytes copied raw into an A8R8G8B8 texture
IDirect3DTexture9* g_d5_tex_depth = nullptr;          // R32F
IDirect3DTexture9* g_d5_tex_motion = nullptr;         // G16R16F
IDirect3DSurface9* g_d5_rt_depth = nullptr;
IDirect3DSurface9* g_d5_rt_motion = nullptr;
IDirect3DSurface9* g_d5_sys_color = nullptr;
IDirect3DSurface9* g_d5_sys_depth = nullptr;
IDirect3DSurface9* g_d5_sys_motion = nullptr;
IDirect3DTexture9* g_d5_composite = nullptr;          // the helper's output, A8R8G8B8
IDirect3DStateBlock9* g_d5_state = nullptr;
UINT g_d5_d9_width = 0;
UINT g_d5_d9_height = 0;
D3DFORMAT g_d5_d9_color_format = D3DFMT_UNKNOWN;
bool g_d5_shaders_failed = false;

const char kD5ShaderSource[] = R"(
sampler2D tex0 : register(s0);
float4 reproject[4] : register(c0);   // rows of VP_prev * inverse(VP_cur), built in double on the CPU
float4 worldW : register(c4);         // row 3 of inverse(VP_cur): the world point's homogeneous w
float4 size : register(c8);           // width, height, cockpit depth (m), 0

float4 DepthPS(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(tex0, uv).rrrr;
}

float4 MotionPS(float2 uv : TEXCOORD0) : COLOR0
{
    // No world position in between: one at 2 km from the origin has about 0.1 mm of fp32
    // resolution, which is pixels of noise close to the camera.
    float z = tex2D(tex0, uv).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1);
    float4 prev = float4(dot(reproject[0], ndc), dot(reproject[1], ndc), dot(reproject[2], ndc), dot(reproject[3], ndc));
    float w = dot(worldW, ndc);   // > 0 in front of the camera
    float2 prevUv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    float2 motion = (prevUv - uv) * size.xy;
    // prev.w / w is last frame's view depth. Nearer than the cockpit depth: the car interior,
    // which moves with the camera. Behind the camera last frame: no usable history either.
    if (prev.w < size.z * w) motion = 0;
    return float4(motion, 0, 1);
}

float4 CopyPS(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(tex0, uv);
}

float4 CopySwapPS(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(tex0, uv).bgra;
}
)";

bool D5CompileShaders(IDirect3DDevice9* device)
{
    if (g_d5_ps_copy_swap != nullptr) {
        return true;
    }
    if (g_d5_shaders_failed) {
        return false;
    }
    g_d5_shaders_failed = true;
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = compiler != nullptr ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr) {
        Log("DLSS5 helper: d3dcompiler_47.dll not found");
        return false;
    }
    const char* entries[4] = {"DepthPS", "MotionPS", "CopyPS", "CopySwapPS"};
    IDirect3DPixelShader9** targets[4] = {&g_d5_ps_depth, &g_d5_ps_motion, &g_d5_ps_copy, &g_d5_ps_copy_swap};
    for (int i = 0; i < 4; ++i) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = compile(kD5ShaderSource, sizeof(kD5ShaderSource) - 1, "dsf_dlss5", nullptr, nullptr, entries[i], "ps_2_0", 0, 0,
            &code, &errors);
        if (FAILED(hr) || code == nullptr) {
            Log("DLSS5 helper: %s did not compile: %s", entries[i],
                errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            D5SafeRelease(errors);
            return false;
        }
        D5SafeRelease(errors);
        const HRESULT created = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), targets[i]);
        code->Release();
        if (FAILED(created)) {
            Log("DLSS5 helper: CreatePixelShader(%s) failed 0x%08lX", entries[i], static_cast<unsigned long>(created));
            return false;
        }
    }
    g_d5_shaders_failed = false;
    return true;
}

void D5ReleaseD3D9()
{
    D5SafeRelease(g_d5_rt_depth);
    D5SafeRelease(g_d5_rt_motion);
    D5SafeRelease(g_d5_tex_depth);
    D5SafeRelease(g_d5_tex_motion);
    D5SafeRelease(g_d5_sys_color);
    D5SafeRelease(g_d5_sys_depth);
    D5SafeRelease(g_d5_sys_motion);
    D5SafeRelease(g_d5_composite);
    D5SafeRelease(g_d5_state);
    g_d5_d9_width = 0;
    g_d5_d9_height = 0;
}

bool D5EnsureD3D9(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT color_format)
{
    if (g_d5_d9_width == w && g_d5_d9_height == h && g_d5_d9_color_format == color_format && g_d5_composite != nullptr) {
        return true;
    }
    D5ReleaseD3D9();
    const bool cpu = g_d5_transport == D5_TRANSPORT_CPU;
    bool ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_d5_tex_depth, nullptr)) &&
        SUCCEEDED(g_d5_tex_depth->GetSurfaceLevel(0, &g_d5_rt_depth)) &&
        SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &g_d5_tex_motion, nullptr)) &&
        SUCCEEDED(g_d5_tex_motion->GetSurfaceLevel(0, &g_d5_rt_motion)) &&
        SUCCEEDED(device->CreateTexture(w, h, 1, cpu ? D3DUSAGE_DYNAMIC : 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_d5_composite, nullptr)) &&
        SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &g_d5_state));
    if (ok && cpu) {
        // GetRenderTargetData needs the target's own format (A8R8G8B8 or X8R8G8B8, both BGRA bytes).
        ok = SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, color_format, D3DPOOL_SYSTEMMEM, &g_d5_sys_color, nullptr)) &&
            SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &g_d5_sys_depth, nullptr)) &&
            SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &g_d5_sys_motion, nullptr));
    }
    if (!ok) {
        Log("DLSS5 helper: D3D9 resources %ux%u could not be created", w, h);
        D5ReleaseD3D9();
        return false;
    }
    g_d5_d9_width = w;
    g_d5_d9_height = h;
    g_d5_d9_color_format = color_format;
    return true;
}

void D5DrawQuad(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DPixelShader9* shader, IDirect3DBaseTexture9* texture)
{
    device->SetRenderTarget(0, target);
    for (DWORD i = 1; i < 4; ++i) {
        device->SetRenderTarget(i, nullptr);
    }
    const D3DVIEWPORT9 viewport = {0, 0, g_d5_d9_width, g_d5_d9_height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(shader);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, texture);
    // Past DSF Enhanced's own sampler overrides (anisotropic, trilinear): these quads need exact texels.
    auto set_sampler = [device](D3DSAMPLERSTATETYPE type, DWORD value) {
        if (g_real_set_sampler_state != nullptr) {
            g_real_set_sampler_state(device, 0, type, value);
        } else {
            device->SetSamplerState(0, type, value);
        }
    };
    set_sampler(D3DSAMP_MINFILTER, D3DTEXF_POINT);
    set_sampler(D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    set_sampler(D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    set_sampler(D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    set_sampler(D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    set_sampler(D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    const float w = static_cast<float>(g_d5_d9_width);
    const float h = static_cast<float>(g_d5_d9_height);
    // Pretransformed corners, shifted by half a pixel so texels map to pixels (D3D9 rules).
    const D5Vertex quad[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(D5Vertex));
}

bool D5Invert4x4(const double* m, double* out)
{
    // Gauss-Jordan on a row-major 4x4.
    double a[4][8] = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
        }
        a[r][4 + r] = 1.0;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-12) {
            return false;
        }
        for (int c = 0; c < 8; ++c) {
            std::swap(a[col][c], a[pivot][c]);
        }
        const double inv = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != col) {
                const double f = a[r][col];
                for (int c = 0; c < 8; ++c) {
                    a[r][c] -= f * a[col][c];
                }
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r][4 + c];
        }
    }
    return true;
}

// Row-major 4x4 product a * b.
void D5Multiply4x4(const double* a, const double* b, double* out)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

// Depth and motion into our render targets; the device's state comes back as it was.
bool D5RenderInputs(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DTexture9* depth, UINT w, UINT h)
{
    // VP_prev * inverse(VP_cur) in double: NDC of this frame -> clip space of the last.
    double current[16] = {}, previous[16] = {}, inv_vp[16] = {}, reproject[16] = {};
    for (int i = 0; i < 16; ++i) {
        current[i] = g_d5_vp_current[i];
        previous[i] = g_d5_vp_previous[i];
    }
    const bool have_motion = g_d5_vp_valid && g_d5_vp_previous_valid && D5Invert4x4(current, inv_vp);
    D5Multiply4x4(previous, inv_vp, reproject);
    IDirect3DSurface9* saved_ds = nullptr;
    device->GetDepthStencilSurface(&saved_ds);
    g_d5_state->Capture();
    device->SetDepthStencilSurface(nullptr);
    D5DrawQuad(device, g_d5_rt_depth, g_d5_ps_depth, depth);
    float constants[9 * 4] = {};
    for (int i = 0; i < 16; ++i) {
        constants[i] = static_cast<float>(reproject[i]);
    }
    for (int i = 0; i < 4; ++i) {
        constants[16 + i] = static_cast<float>(inv_vp[12 + i]);
    }
    constants[32] = static_cast<float>(w);
    constants[33] = static_cast<float>(h);
    constants[34] = g_d5_cockpit_depth;
    device->SetPixelShaderConstantF(0, constants, 9);
    D5DrawQuad(device, g_d5_rt_motion, g_d5_ps_motion, depth);
    if (!have_motion) {
        device->ColorFill(g_d5_rt_motion, nullptr, 0);   // no previous matrix: zero motion
    }
    if (g_d5_trace_frame != nullptr && g_d5_trace_dump && g_d5_trace_pair_step == 1) {
        D5DumpSurface(device, g_d5_rt_depth, "dlss5-depth");
        D5DumpSurface(device, g_d5_rt_motion, "dlss5-motion");
    }
    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(saved_ds);
    g_d5_state->Apply();
    D5SafeRelease(saved_ds);
    return have_motion;
}

void D5Composite(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DPixelShader9* shader)
{
    IDirect3DSurface9* saved_ds = nullptr;
    device->GetDepthStencilSurface(&saved_ds);
    g_d5_state->Capture();
    D5DrawQuad(device, target, shader, g_d5_composite);
    device->SetDepthStencilSurface(saved_ds);
    g_d5_state->Apply();
    D5SafeRelease(saved_ds);
}

DsfDlss5Frame D5MakeFrame(std::uint64_t n)
{
    DsfDlss5Frame frame = {};
    frame.frame = n;
    // RESET on the first frame after CONFIGURE and whenever last frame's VP is not the frame just
    // before (menus, pauses, loading): the helper's history would be from another view.
    const bool reset = n == 1 || !g_d5_vp_previous_valid;
    frame.flags = DSF_DLSS5_FRAME_AUTO_MASK | DSF_DLSS5_FRAME_GPU_TIME | (reset ? DSF_DLSS5_FRAME_RESET : 0) |
        (g_d5_helper_bypass ? DSF_DLSS5_FRAME_BYPASS : 0);
    frame.style = static_cast<std::uint32_t>(g_d5_style);
    frame.mv_scale[0] = 1.0f;
    frame.mv_scale[1] = 1.0f;
    frame.intensity = g_d5_intensity;
    frame.tone_strength = g_d5_tone_strength;
    frame.structure_strength = g_d5_structure_strength;
    frame.skin_strength = g_d5_skin_strength;
    return frame;
}

// FRAME n and its FRAME_ACK. Returns false when the connection is gone (the session is over);
// *ack_ok says whether the helper evaluated the frame (fence_out >= n either way).
bool D5ExchangeFrame(std::uint64_t n, bool* ack_ok, DsfDlss5FrameAck* ack)
{
    *ack_ok = false;
    const DsfDlss5Frame frame = D5MakeFrame(n);
    DsfDlss5Error error = {};
    if (!dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_FRAME, frame, kD5FrameTimeoutMs) ||
        !dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_FRAME_ACK, ack, kD5FrameTimeoutMs, &error)) {
        const DWORD last = GetLastError();
        error.message[sizeof(error.message) - 1] = '\0';
        char what[256] = {};
        std::snprintf(what, sizeof(what), "FRAME %llu got no answer (%s, helper status %d: %s)", static_cast<unsigned long long>(n),
            last == WAIT_TIMEOUT ? "timed out" : "pipe closed", error.status, error.message);
        D5BridgeFail(what);
        return false;
    }
    if (ack->frame != n || ack->status != DSF_DLSS5_OK) {
        char what[160] = {};
        std::snprintf(what, sizeof(what), "FRAME %llu answered with status %d (ngx 0x%08lX)", static_cast<unsigned long long>(n), ack->status,
            static_cast<unsigned long>(ack->ngx_result));
        D5BridgeRetry(what);
        return true;
    }
    *ack_ok = true;
    return true;
}

// CPU transport frame. Returns true when the output was composited.
bool D5CpuFrame(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT w, UINT h, float* gpu_ms)
{
    auto readback = [device](IDirect3DSurface9* source, IDirect3DSurface9* staging, ID3D11Texture2D* shared) {
        if (FAILED(device->GetRenderTargetData(source, staging))) {
            return false;
        }
        D3DLOCKED_RECT locked = {};
        if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            return false;
        }
        g_d5_ctx->UpdateSubresource(shared, 0, nullptr, locked.pBits, static_cast<UINT>(locked.Pitch), 0);
        staging->UnlockRect();
        return true;
    };
    if (!readback(target, g_d5_sys_color, g_d5_slot_tex[DSF_DLSS5_SLOT_COLOR]) ||
        !readback(g_d5_rt_depth, g_d5_sys_depth, g_d5_slot_tex[DSF_DLSS5_SLOT_DEPTH]) ||
        !readback(g_d5_rt_motion, g_d5_sys_motion, g_d5_slot_tex[DSF_DLSS5_SLOT_MOTION])) {
        D5BridgeFail("readback failed");
        return false;
    }
    const std::uint64_t n = ++g_d5_frame_number;
    g_d5_ctx->Signal(g_d5_fence_in, n);
    g_d5_ctx->Flush();
    bool ack_ok = false;
    DsfDlss5FrameAck ack = {};
    if (!D5ExchangeFrame(n, &ack_ok, &ack) || !ack_ok) {
        return false;
    }
    *gpu_ms = ack.gpu_ms;
    // fence_out >= n already (the ACK comes after the helper's GPU work); the wait orders our copy.
    g_d5_ctx->Wait(g_d5_fence_out, n);
    g_d5_ctx->CopyResource(g_d5_output_staging, g_d5_slot_tex[DSF_DLSS5_SLOT_OUTPUT]);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    D3DLOCKED_RECT locked = {};
    bool copied = false;
    if (SUCCEEDED(g_d5_ctx->Map(g_d5_output_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        if (SUCCEEDED(g_d5_composite->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD))) {
            for (UINT y = 0; y < h; ++y) {
                const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                auto* dst = static_cast<std::uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
                for (UINT x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            g_d5_composite->UnlockRect(0);
            copied = true;
        }
        g_d5_ctx->Unmap(g_d5_output_staging, 0);
    }
    if (copied) {
        D5Composite(device, target, g_d5_ps_copy);
    }
    return copied;
}

struct D5VkTexture
{
    ID3D9VkInteropTexture* interop = nullptr;
    vkm::VkImage image = 0;
    vkm::VkImageLayout layout = 0;
};

bool D5VkTextureOf(IDirect3DResource9* resource, D5VkTexture* out)
{
    if (FAILED(resource->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&out->interop))) ||
        out->interop == nullptr) {
        return false;
    }
    return SUCCEEDED(out->interop->GetVulkanImageInfo(&out->image, &out->layout, nullptr)) && out->image != 0;
}

// Records and submits one of our command buffers on DXVK's queue (which must be locked).
bool D5VkSubmit(int kind, const vkm::VkImageMemoryBarrier* before, uint32_t before_count, const vkm::VkImageCopy* copy,
    const vkm::VkImage* copy_src, const vkm::VkImage* copy_dst, uint32_t copy_count, const vkm::VkImageMemoryBarrier* after,
    uint32_t after_count, vkm::VkSemaphore wait, std::uint64_t wait_value, vkm::VkSemaphore signal, std::uint64_t signal_value)
{
    const int slot = g_d5_vk_ring;
    vkm::VkCommandBuffer cmd = g_d5_vk_cmd[kind][slot];
    vkm::VkFence fence = g_d5_vk_cmd_fence[kind][slot];
    // The ring is three frames deep; a frame's copies finish long before that.
    if (g_vk.WaitForFences(g_d5_vk_device, 1, &fence, 1, 1000ull * 1000 * 1000) != vkm::VK_SUCCESS) {
        return false;
    }
    g_vk.ResetFences(g_d5_vk_device, 1, &fence);
    g_vk.ResetCommandBuffer(cmd, 0);
    const vkm::VkCommandBufferBeginInfo begin = {vkm::VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        vkm::VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    g_vk.BeginCommandBuffer(cmd, &begin);
    g_vk.CmdPipelineBarrier(cmd, vkm::VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, vkm::VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        before_count, before);
    for (uint32_t i = 0; i < copy_count; ++i) {
        g_vk.CmdCopyImage(cmd, copy_src[i], vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_dst[i], vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy[i]);
    }
    g_vk.CmdPipelineBarrier(cmd, vkm::VK_PIPELINE_STAGE_TRANSFER_BIT, vkm::VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        after_count, after);
    g_vk.EndCommandBuffer(cmd);

    const vkm::VkPipelineStageFlags wait_stage = vkm::VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkm::VkTimelineSemaphoreSubmitInfo timeline = {vkm::VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr, wait != 0 ? 1u : 0u,
        &wait_value, signal != 0 ? 1u : 0u, &signal_value};
    vkm::VkSubmitInfo submit = {};
    submit.sType = vkm::VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = wait != 0 ? 1u : 0u;
    submit.pWaitSemaphores = &wait;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = signal != 0 ? 1u : 0u;
    submit.pSignalSemaphores = &signal;
    const vkm::VkResult result = g_vk.QueueSubmit(g_d5_vk_queue, 1, &submit, fence);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: vkQueueSubmit failed (%d)", result);
        return false;
    }
    return true;
}

// Vulkan transport frame (DXVK). Returns true when the output was composited.
bool D5VkFrame(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT w, UINT h, float* gpu_ms)
{
    IDirect3DSurface9* composite_surface = nullptr;
    g_d5_composite->GetSurfaceLevel(0, &composite_surface);
    D5VkTexture color, depth, motion, composite;
    const bool found = D5VkTextureOf(target, &color) && D5VkTextureOf(g_d5_rt_depth, &depth) &&
        D5VkTextureOf(g_d5_rt_motion, &motion) && composite_surface != nullptr && D5VkTextureOf(composite_surface, &composite);
    D5SafeRelease(composite_surface);
    auto release = [&]() {
        D5SafeRelease(color.interop);
        D5SafeRelease(depth.interop);
        D5SafeRelease(motion.interop);
        D5SafeRelease(composite.interop);
    };
    if (!found) {
        release();
        D5BridgeFail("DXVK did not give the Vulkan images of the frame's textures");
        return false;
    }

    // The game also calls the device from its main thread (it locks dynamic vertex and index
    // buffers there). DXVK's interop calls don't take the device lock that D3D9 calls take, so
    // hold it while they run: without it a Flush raced a buffer Lock and crashed in DXVK.
    g_d5_vk_interop->LockDevice();
    const vkm::VkImageSubresourceRange range = {vkm::VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const D5VkTexture* sources[3] = {&color, &depth, &motion};
    for (const D5VkTexture* source : sources) {
        g_d5_vk_interop->TransitionTextureLayout(source->interop, &range, source->layout, vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    g_d5_vk_interop->TransitionTextureLayout(composite.interop, &range, composite.layout, vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    g_d5_step = 3;
    g_d5_vk_interop->FlushRenderingCommands();

    // Inputs: the game's colour and our depth and motion into the helper's textures, fence_in = n.
    const std::uint64_t n = ++g_d5_frame_number;
    const int slots[3] = {DSF_DLSS5_SLOT_COLOR, DSF_DLSS5_SLOT_DEPTH, DSF_DLSS5_SLOT_MOTION};
    vkm::VkImageMemoryBarrier before[6] = {};
    vkm::VkImageMemoryBarrier after[3] = {};
    vkm::VkImageCopy copies[3] = {};
    vkm::VkImage copy_src[3] = {};
    vkm::VkImage copy_dst[3] = {};
    for (int i = 0; i < 3; ++i) {
        const vkm::VkImage shared = g_d5_vk_slot_image[slots[i]];
        // DXVK's writes to the source (its transition already ran on the queue) before our reads.
        before[i] = D5Barrier(sources[i]->image, vkm::VK_ACCESS_MEMORY_WRITE_BIT, vkm::VK_ACCESS_TRANSFER_READ_BIT,
            vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkm::VKM_QUEUE_FAMILY_IGNORED,
            vkm::VKM_QUEUE_FAMILY_IGNORED);
        // The shared texture comes from D3D12: acquire it, its old contents are not needed.
        before[3 + i] = D5Barrier(shared, 0, vkm::VK_ACCESS_TRANSFER_WRITE_BIT, vkm::VK_IMAGE_LAYOUT_UNDEFINED,
            vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vkm::VKM_QUEUE_FAMILY_EXTERNAL, g_d5_vk_family);
        after[i] = D5Barrier(shared, vkm::VK_ACCESS_TRANSFER_WRITE_BIT, 0, vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            vkm::VK_IMAGE_LAYOUT_GENERAL, g_d5_vk_family, vkm::VKM_QUEUE_FAMILY_EXTERNAL);
        copies[i].srcSubresource = {vkm::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copies[i].dstSubresource = {vkm::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copies[i].extent = {w, h, 1};
        copy_src[i] = sources[i]->image;
        copy_dst[i] = shared;
    }
    g_d5_step = 4;
    g_d5_vk_interop->LockSubmissionQueue();
    g_d5_step = 5;
    const bool submitted = D5VkSubmit(0, before, 6, copies, copy_src, copy_dst, 3, after, 3, 0, 0, g_d5_vk_fence_in, n);
    g_d5_vk_interop->ReleaseSubmissionQueue();
    g_d5_vk_interop->UnlockDevice();   // not held while the helper works

    bool ack_ok = false;
    DsfDlss5FrameAck ack = {};
    bool alive = submitted;
    if (!submitted) {
        D5BridgeFail("submitting the input copies failed");
    } else {
        g_d5_step = 6;
        alive = D5ExchangeFrame(n, &ack_ok, &ack);
    }

    // Output: only after an OK ACK (fence_out >= n has been reached) do we queue the wait.
    bool composited = false;
    if (alive && ack_ok) {
        *gpu_ms = ack.gpu_ms;
        const vkm::VkImage output = g_d5_vk_slot_image[DSF_DLSS5_SLOT_OUTPUT];
        vkm::VkImageMemoryBarrier out_before[2] = {
            D5Barrier(output, 0, vkm::VK_ACCESS_TRANSFER_READ_BIT, vkm::VK_IMAGE_LAYOUT_GENERAL, vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                vkm::VKM_QUEUE_FAMILY_EXTERNAL, g_d5_vk_family),
            D5Barrier(composite.image, vkm::VK_ACCESS_MEMORY_WRITE_BIT | vkm::VK_ACCESS_MEMORY_READ_BIT, vkm::VK_ACCESS_TRANSFER_WRITE_BIT,
                vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vkm::VKM_QUEUE_FAMILY_IGNORED,
                vkm::VKM_QUEUE_FAMILY_IGNORED),
        };
        vkm::VkImageMemoryBarrier out_after[2] = {
            D5Barrier(output, vkm::VK_ACCESS_TRANSFER_READ_BIT, 0, vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkm::VK_IMAGE_LAYOUT_GENERAL,
                g_d5_vk_family, vkm::VKM_QUEUE_FAMILY_EXTERNAL),
            D5Barrier(composite.image, vkm::VK_ACCESS_TRANSFER_WRITE_BIT, vkm::VK_ACCESS_MEMORY_READ_BIT,
                vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vkm::VKM_QUEUE_FAMILY_IGNORED,
                vkm::VKM_QUEUE_FAMILY_IGNORED),
        };
        vkm::VkImageCopy copy = {};
        copy.srcSubresource = {vkm::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {vkm::VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {w, h, 1};
        g_d5_step = 7;
        g_d5_vk_interop->LockSubmissionQueue();
        g_d5_step = 8;
        composited = D5VkSubmit(1, out_before, 2, &copy, &output, &composite.image, 1, out_after, 2, g_d5_vk_fence_out, n, 0, 0);
        g_d5_vk_interop->ReleaseSubmissionQueue();
        if (!composited) {
            D5BridgeFail("submitting the output copy failed");
        }
    }
    g_d5_vk_ring = (g_d5_vk_ring + 1) % kD5VkRing;

    g_d5_step = 9;
    g_d5_vk_interop->LockDevice();
    // Back to the layouts DXVK expects, then draw the output over the HUD target.
    for (const D5VkTexture* source : sources) {
        g_d5_vk_interop->TransitionTextureLayout(source->interop, &range, vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source->layout);
    }
    g_d5_vk_interop->TransitionTextureLayout(composite.interop, &range, vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, composite.layout);
    g_d5_vk_interop->UnlockDevice();
    release();
    if (composited) {
        D5Composite(device, target, g_d5_ps_copy_swap);
    }
    return composited;
}

// Runs at the pre-HUD point with the HUD target bound and the composite drawn into it.
void D5BridgeFrame(IDirect3DDevice9* device)
{
    const LONG state = g_d5_bridge_state;
    if (!g_d5_helper || state == D5_BRIDGE_FAILED) {
        return;
    }
    if (state == D5_BRIDGE_OFF) {
        D5BridgeStart(device);
        return;
    }

    auto* target = static_cast<IDirect3DSurface9*>(const_cast<void*>(g_d5.rt0));
    D3DSURFACE_DESC desc = {};
    if (target == nullptr || FAILED(target->GetDesc(&desc)) ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) {
        return;
    }
    if (state == D5_BRIDGE_READY) {
        // The worker configures for this size; frames go without DLSS 5 meanwhile.
        g_d5_want_width = desc.Width;
        g_d5_want_height = desc.Height;
        InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_CONFIGURING);
        SetEvent(g_d5_worker_event);
        return;
    }
    if (state != D5_BRIDGE_RUNNING) {
        return;
    }
    if (g_d5_bridge_width != desc.Width || g_d5_bridge_height != desc.Height) {
        InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_READY);
        return;
    }
    IDirect3DTexture9* depth = D5IntzTextureOf(g_d5_scene_ds);
    if (depth == nullptr) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("DLSS5 helper: the scene depth is not an INTZ texture; waiting");
        }
        return;
    }
    g_d5_internal = true;
    if (!D5CompileShaders(device) || !D5EnsureD3D9(device, desc.Width, desc.Height, desc.Format)) {
        g_d5_internal = false;
        depth->Release();
        D5BridgeFail("D3D9 shaders or resources failed");
        return;
    }

    const double start = D5NowMs();
    g_d5_step = 1;
    const bool have_motion = D5RenderInputs(device, target, depth, desc.Width, desc.Height);
    depth->Release();
    g_d5_step = 2;
    float gpu_ms = -1.0f;
    const bool composited = g_d5_transport == D5_TRANSPORT_VULKAN ? D5VkFrame(device, target, desc.Width, desc.Height, &gpu_ms) :
        D5CpuFrame(device, target, desc.Width, desc.Height, &gpu_ms);
    g_d5_internal = false;
    g_d5_step = 0;
    const double ms = D5NowMs() - start;
    if (!composited) {
        return;
    }
    g_d5_configure_attempts = 0;
    g_d5_window_bridge_ms += ms;
    ++g_d5_window_bridge_frames;
    if (gpu_ms >= 0.0f) {
        g_d5_window_gpu_ms += gpu_ms;
        ++g_d5_window_gpu_frames;
    }
    g_d5_bridge_ms_sum += ms;
    g_d5_bridge_gpu_ms_sum += gpu_ms;
    const LONG frames = InterlockedIncrement(&g_d5_bridge_frames);
    if (frames <= 3 || frames % 600 == 0) {
        Log("DLSS5 helper: frame %llu composited, motion=%d, bridge %.2f ms (mean %.2f) evaluate %.2f ms, transport %s%s",
            static_cast<unsigned long long>(g_d5_frame_number), have_motion ? 1 : 0, ms, g_d5_bridge_ms_sum / frames,
            static_cast<double>(gpu_ms), g_d5_transport_name, g_d5_helper_bypass ? " (bypass)" : "");
    }
}

// Before IDirect3DDevice9::Reset: our default-pool resources must go.
void Dlss5OnReset()
{
    D5ReleaseD3D9();
    D5SafeRelease(g_d5_ps_depth);
    D5SafeRelease(g_d5_ps_motion);
    D5SafeRelease(g_d5_ps_copy);
    D5SafeRelease(g_d5_ps_copy_swap);
    g_d5_shaders_failed = false;
    // Reset releases every default-pool resource: forget the targets and textures the hooks saw,
    // so nothing calls into a freed surface before the game binds new ones.
    g_d5.rt0 = nullptr;
    g_d5.ds = nullptr;
    for (auto*& texture : g_d5.bound) {
        texture = nullptr;
    }
    g_d5_scene_rt = nullptr;
    g_d5_scene_ds = nullptr;
    g_d5_frame_best_rt = nullptr;
    g_d5_frame_best_ds = nullptr;
    g_d5_frame_best_draws = 0;
    g_d5_counting_rt = nullptr;
    g_d5_counting_frame = -1;
    g_d5_counting_full = false;
}

// DLL_PROCESS_DETACH.
void Dlss5Shutdown()
{
    D5RemoveExceptionHandler();
}
