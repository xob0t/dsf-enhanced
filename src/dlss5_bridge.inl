// DLSS 5 and DLAA through dsf-dlss5-helper (dlss5/include/dsf_dlss5_protocol.h). [DLSS5] Helper=1
// (with Export=1) turns DLSS 5 on, [DLAA] Enable=1 DLAA (dlaa.inl). Both share the helper, the
// pipe, the depth and the motion vectors; each has its own slots, fences and frame numbers.
//
// DLAA runs at the scene end on the HDR scene target (linear f16): the target goes in as it is
// (NaNs and negatives removed) with the frame's jitter, and DLAA's output is written back into it
// (colour only, the scene keeps its alpha), before DLSS 5 and the game's post chain read it.
//
// Where DLSS 5 sees the frame (Input):
//   0 at the pre-HUD point: the game's final LDR image (graded, film grain, bloom), and the
//     output is drawn over the HUD target before the HUD;
//   1 at the scene end, before the post chain, as RTX Remix does it: the HDR scene target is
//     tone-mapped into an f16 colour input (exposure, ACES approximation, 1/2.2 gamma), and
//     DLSS 5's output is tone-mapped back (Remix's inverse, with its highlight recovery) into
//     the scene target. The game's bloom, tone map, grading and HUD then run on it as usual.
//
// Each frame, at that point (dlss5_feed.inl):
//   1. small D3D9 passes write the inputs the helper wants: the colour (Input=1), depth as R32F
//      z/w from the scene depth (an INTZ texture that replaces the game's back-buffer-sized
//      D24S8 surface) and camera motion vectors as RG16F, reprojected through this and the
//      previous frame's view-projection;
//   2. colour, depth and motion go to the helper's shared textures, fence_in = n, FRAME n;
//   3. FRAME_ACK n (the helper sends it once its GPU work is done, so fence_out >= n), then
//      the output is drawn back.
// Every D3D9 state the bridge changes is put back before the game's next call.
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
// and reads FRAME_ACK, with timeouts; any failure drops that feature for that frame (the game's
// own image stays), a failed frame is followed by a new CONFIGURE, and repeated failures turn
// the feature off for the session, a dead or stalled helper both. Nothing ever waits on a fence
// the helper might not signal. DLAA's jitter and the MLAA skip follow DLAA's output (dlaa.inl).
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
int g_d5_input = 0;                // 0: the pre-HUD LDR frame, 1: the HDR scene target (Remix's way)
float g_d5_hdr_exposure = 0.6f;    // Input=1: scale on the scene's linear values before the ACES curve
bool g_d5_self_test = false;       // the game-side passes without the helper (DLSS 5's output = its input)

constexpr DWORD kD5HelloTimeoutMs = 20000;      // the helper loads and checks the runtime, starts NGX
constexpr DWORD kD5ConfigureTimeoutMs = 20000;  // creates the feature
constexpr DWORD kD5FrameTimeoutMs = 3000;       // the helper gives up on fence_in after about 2 s
constexpr int kD5MaxConfigureAttempts = 3;

// [DLAA] (dlaa.inl)
extern bool g_dlaa_enable;
extern int g_dlaa_preset;
extern bool g_dlaa_jitter_frame;
extern float g_dlaa_jx;
extern float g_dlaa_jy;
bool DlaaSceneUsable();

// An ini path: empty gives the default, a relative one is taken from the game folder (beside
// Driver.exe), not from the current directory.
void ReadGamePathSetting(const char* key, const char* fallback, char* out, const char* ini_path)
{
    char value[MAX_PATH] = {};
    GetPrivateProfileStringA("DLSS5", key, "", value, MAX_PATH, ini_path);
    const bool absolute = (value[0] != '\0' && value[1] == ':') || (value[0] == '\\' && value[1] == '\\');
    if (absolute) {
        std::snprintf(out, MAX_PATH, "%s", value);
    } else {
        BuildGamePath(out, MAX_PATH, value[0] != '\0' ? value : fallback);
    }
}

void LoadDlss5BridgeConfig(const char* ini_path)
{
    g_d5_helper_bypass = GetPrivateProfileIntA("DLSS5", "HelperBypass", 0, ini_path) != 0;
    g_d5_helper_transport = ClampInt(GetPrivateProfileIntA("DLSS5", "HelperTransport", 0, ini_path), 0, 2);
    ReadGamePathSetting("HelperPath", "dlss5\\dsf-dlss5-helper.exe", g_d5_helper_path, ini_path);
    ReadGamePathSetting("HelperRuntime", "dlss5", g_d5_helper_runtime, ini_path);
    ReadGamePathSetting("HelperLog", "dsf-dlss5-helper.log", g_d5_helper_log, ini_path);
    g_d5_style = ClampInt(GetPrivateProfileIntA("DLSS5", "Style", 0, ini_path), 0, 2);
    g_d5_intensity = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "Intensity", 1.0f, ini_path), 1.0f));
    g_d5_tone_strength = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "ToneStrength", 0.3f, ini_path), 1.0f));
    g_d5_structure_strength = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "StructureStrength", 0.7f, ini_path), 1.0f));
    g_d5_skin_strength = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "SkinStrength", 0.5f, ini_path), 1.0f));
    g_d5_cockpit_depth = std::max(0.0f, std::min(ReadProfileFloat("DLSS5", "CockpitDepth", 1.5f, ini_path), 10.0f));
    g_d5_frame_time_log = GetPrivateProfileIntA("DLSS5", "FrameTimeLog", 0, ini_path) != 0;
    g_d5_crash_dump = GetPrivateProfileIntA("DLSS5", "CrashDump", 0, ini_path) != 0;
    g_d5_input = ClampInt(GetPrivateProfileIntA("DLSS5", "Input", 0, ini_path), 0, 1);
    g_d5_hdr_exposure = std::max(1.0f / 64.0f, std::min(ReadProfileFloat("DLSS5", "HdrExposure", 0.6f, ini_path), 64.0f));
    g_d5_self_test = GetPrivateProfileIntA("DLSS5", "SelfTest", 0, ini_path) != 0;
    g_d5_object_motion = GetPrivateProfileIntA("DLSS5", "ObjectMotion", 1, ini_path) != 0;
    g_d5_object_debug = GetPrivateProfileIntA("DLSS5", "ObjectMotionDebug", 0, ini_path) != 0;
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
double g_d5_window_dlaa_ms = 0.0;
int g_d5_window_dlaa_frames = 0;
double g_d5_window_dlaa_gpu_ms = 0.0;
int g_d5_window_dlaa_gpu_frames = 0;
const char* g_d5_transport_name = "none";
double g_d5_window_object_gpu_ms = 0.0;   // the object pass on the GPU (timestamp queries)
int g_d5_window_object_gpu_frames = 0;
long long g_d5_window_object_drawn = 0;   // quads drawn (moving cars on screen)

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
    if (g_dlaa_enable) {
        Log("DLSS5 frametime: DLAA on %d of them, bridge %.2f ms, evaluate %.2f ms", g_d5_window_dlaa_frames,
            g_d5_window_dlaa_frames > 0 ? g_d5_window_dlaa_ms / g_d5_window_dlaa_frames : 0.0,
            g_d5_window_dlaa_gpu_frames > 0 ? g_d5_window_dlaa_gpu_ms / g_d5_window_dlaa_gpu_frames : 0.0);
    }
    if (g_d5_window_object_frames > 0) {
        const double frames = g_d5_window_object_frames;
        Log("DLSS5 frametime: object motion %.1f cars, %.1f moving, %.1f drawn a frame, CPU %.3f ms (tagging, matching), pass GPU %.3f ms (%d frames timed)",
            g_d5_window_object_cars / frames, g_d5_window_object_moving / frames, g_d5_window_object_drawn / frames,
            g_d5_window_object_cpu_ms / frames,
            g_d5_window_object_gpu_frames > 0 ? g_d5_window_object_gpu_ms / g_d5_window_object_gpu_frames : 0.0, g_d5_window_object_gpu_frames);
    }
    g_d5_window_object_cpu_ms = 0.0;
    g_d5_window_object_cars = 0;
    g_d5_window_object_moving = 0;
    g_d5_window_object_drawn = 0;
    g_d5_window_object_frames = 0;
    g_d5_window_object_gpu_ms = 0.0;
    g_d5_window_object_gpu_frames = 0;
    g_d5_frame_interval_count = 0;
    g_d5_window_bridge_ms = 0.0;
    g_d5_window_bridge_frames = 0;
    g_d5_window_gpu_ms = 0.0;
    g_d5_window_gpu_frames = 0;
    g_d5_window_dlaa_ms = 0.0;
    g_d5_window_dlaa_frames = 0;
    g_d5_window_dlaa_gpu_ms = 0.0;
    g_d5_window_dlaa_gpu_frames = 0;
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
// Bridge state. The connection has one state, each feature (DLSS 5, DLAA) its own. The worker
// thread owns the pipe while the bridge is STARTING or a feature is CONFIGURING; the render
// thread owns it (FRAME/FRAME_ACK) otherwise. Only the render thread asks for a CONFIGURE, so
// while it sees no feature configuring, the worker is not on the pipe.

enum D5BridgeState : LONG
{
    D5_BRIDGE_OFF = 0,
    D5_BRIDGE_STARTING = 1,
    D5_BRIDGE_READY = 2,         // HELLO done: the features configure and run
    D5_BRIDGE_FAILED = 3,        // the helper is gone for this session
};

enum D5FeatureState : LONG
{
    D5_FEATURE_OFF = 0,          // not wanted, not offered by the helper, or given up
    D5_FEATURE_IDLE = 1,         // needs CONFIGURE
    D5_FEATURE_CONFIGURING = 2,  // the worker is configuring it
    D5_FEATURE_RUNNING = 3,
};

volatile LONG g_d5_bridge_state = D5_BRIDGE_OFF;
HANDLE g_d5_worker_event = nullptr;
HANDLE g_d5_pipe = INVALID_HANDLE_VALUE;
HANDLE g_d5_helper_process = nullptr;
int g_d5_transport = D5_TRANSPORT_CPU;
LUID g_d5_adapter_luid = {};

// One feature's side of the protocol: its shared textures and fences on either transport, its
// frame numbers, and the D3D9 textures its colour goes out of and its output comes back into.
struct D5Feature
{
    DsfDlss5Feature id = DSF_DLSS5_FEATURE_NR;
    const char* name = "DLSS 5";
    volatile LONG state = D5_FEATURE_OFF;
    UINT want_width = 0;              // what the worker configures for
    UINT want_height = 0;
    UINT width = 0;                   // as configured
    UINT height = 0;
    std::uint64_t frame_number = 0;   // n: from 1 after every CONFIGURE
    int configure_attempts = 0;
    int preset = 0;
    DXGI_FORMAT formats[DSF_DLSS5_SLOT_COUNT] = {};
    LONG composited_frame = -1;       // g_d5_frame_index of the last frame whose output went back
    // CPU transport: D3D11 textures DSF Enhanced creates and shares.
    ID3D11Texture2D* slot_tex[DSF_DLSS5_SLOT_COUNT] = {};
    ID3D11Texture2D* output_staging = nullptr;
    ID3D11Fence* fence_in = nullptr;
    ID3D11Fence* fence_out = nullptr;
    // Vulkan transport: the helper's D3D12 textures and fences imported into DXVK's device.
    vkm::VkImage vk_image[DSF_DLSS5_SLOT_COUNT] = {};
    vkm::VkDeviceMemory vk_memory[DSF_DLSS5_SLOT_COUNT] = {};
    vkm::VkSemaphore vk_fence_in = 0;
    vkm::VkSemaphore vk_fence_out = 0;
    // D3D9, default pool (released before Reset).
    IDirect3DTexture9* composite = nullptr;      // the helper's output: A8R8G8B8, or A16B16G16R16F for HDR runs
    IDirect3DTexture9* tex_color_in = nullptr;   // HDR runs: the colour input, A16B16G16R16F
    IDirect3DSurface9* rt_color_in = nullptr;
    IDirect3DTexture9* tex_hdr_copy = nullptr;   // DLSS 5 Input=1: the scene as it was (the scene target can't be read and written at once)
    IDirect3DSurface9* rt_hdr_copy = nullptr;
    IDirect3DSurface9* sys_color = nullptr;      // CPU transport: the colour's readback
    UINT d9_width = 0;
    UINT d9_height = 0;
    D3DFORMAT d9_color_format = D3DFMT_UNKNOWN;
    LONG frames = 0;
    double ms_sum = 0.0;
    double gpu_ms_sum = 0.0;
};

D5Feature g_d5_features[DSF_DLSS5_FEATURE_COUNT];

void D5RemoveExceptionHandler();

void D5BridgeFail(const char* what)
{
    Log("DLSS5 helper: %s; the helper is off for this session (the game's own image is shown)", what);
    InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_FAILED);
    if (g_d5_worker_event != nullptr) {
        SetEvent(g_d5_worker_event);   // the worker sends SHUTDOWN and ends
    }
    D5RemoveExceptionHandler();
}

// The render thread may send FRAME: connected, and no feature being configured.
bool D5PipeFree()
{
    if (g_d5_bridge_state != D5_BRIDGE_READY) {
        return false;
    }
    for (const D5Feature& f : g_d5_features) {
        if (f.state == D5_FEATURE_CONFIGURING) {
            return false;
        }
    }
    return true;
}

// A feature gives up for the session; the helper goes when no feature is left.
void D5FeatureOff(D5Feature& f, const char* what)
{
    Log("DLSS5 helper: %s; %s off for this session", what, f.name);
    InterlockedExchange(&f.state, D5_FEATURE_OFF);
    for (const D5Feature& other : g_d5_features) {
        if (other.state != D5_FEATURE_OFF) {
            return;
        }
    }
    D5BridgeFail("no feature left");
}

// A frame failed but the connection still works: CONFIGURE again, a few times at most.
void D5FeatureRetry(D5Feature& f, const char* what)
{
    if (++f.configure_attempts >= kD5MaxConfigureAttempts) {
        D5FeatureOff(f, what);
        return;
    }
    Log("DLSS5 helper: %s; frame shown without %s, configuring again (attempt %d of %d)", what, f.name, f.configure_attempts + 1,
        kD5MaxConfigureAttempts);
    InterlockedExchange(&f.state, D5_FEATURE_IDLE);
}

// ---------------------------------------------------------------------------------------
// CPU transport: D3D11 textures DSF Enhanced creates and shares (CLIENT_CREATES).

ID3D11Device* g_d5_d11 = nullptr;
ID3D11Device5* g_d5_d11_5 = nullptr;
ID3D11DeviceContext4* g_d5_ctx = nullptr;

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

void D5CpuRelease(D5Feature& f)
{
    for (auto*& texture : f.slot_tex) {
        D5SafeRelease(texture);
    }
    D5SafeRelease(f.output_staging);
    D5SafeRelease(f.fence_in);
    D5SafeRelease(f.fence_out);
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

// One feature's imports. The worker calls it while the render thread submits nothing of ours
// (no FRAME goes out while a feature is configuring).
void D5VkReleaseImports(D5Feature& f)
{
    if (g_d5_vk_device == nullptr) {
        return;
    }
    // Still in use by the GPU: leak them rather than free memory a pending copy touches.
    const bool idle = D5VkWaitIdle();
    if (!idle) {
        Log("DLSS5 helper: the previous %s shared textures and fences stay allocated (GPU still busy)", f.name);
    }
    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) {
        if (idle && f.vk_image[slot] != 0) {
            g_vk.DestroyImage(g_d5_vk_device, f.vk_image[slot], nullptr);
        }
        if (idle && f.vk_memory[slot] != 0) {
            g_vk.FreeMemory(g_d5_vk_device, f.vk_memory[slot], nullptr);
        }
        f.vk_image[slot] = 0;
        f.vk_memory[slot] = 0;
    }
    if (f.vk_fence_in != 0 && idle) {
        g_vk.DestroySemaphore(g_d5_vk_device, f.vk_fence_in, nullptr);
    }
    if (f.vk_fence_out != 0 && idle) {
        g_vk.DestroySemaphore(g_d5_vk_device, f.vk_fence_out, nullptr);
    }
    f.vk_fence_in = 0;
    f.vk_fence_out = 0;
}

vkm::VkFormat D5VkFormatOf(uint32_t dxgi)
{
    switch (dxgi) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return vkm::VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return vkm::VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R32_FLOAT: return vkm::VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R16G16_FLOAT: return vkm::VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return vkm::VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return 0;
    }
}

// Imports one of the helper's D3D12 textures (an NT handle valid in this process).
bool D5VkImportImage(D5Feature& f, int slot, HANDLE handle, uint32_t dxgi_format, UINT w, UINT h)
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
    vkm::VkResult result = g_vk.CreateImage(g_d5_vk_device, &info, nullptr, &f.vk_image[slot]);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: vkCreateImage for slot %d failed (%d)", slot, result);
        return false;
    }
    vkm::VkMemoryRequirements requirements = {};
    g_vk.GetImageMemoryRequirements(g_d5_vk_device, f.vk_image[slot], &requirements);
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
        f.vk_image[slot], 0};
    vkm::VkImportMemoryWin32HandleInfoKHR import = {vkm::VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated,
        vkm::VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, nullptr};
    vkm::VkMemoryAllocateInfo allocate = {vkm::VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, requirements.size, type_index};
    result = g_vk.AllocateMemory(g_d5_vk_device, &allocate, nullptr, &f.vk_memory[slot]);
    if (result != vkm::VK_SUCCESS) {
        Log("DLSS5 helper: importing slot %d failed (%d)", slot, result);
        return false;
    }
    result = g_vk.BindImageMemory(g_d5_vk_device, f.vk_image[slot], f.vk_memory[slot], 0);
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
    if (ack.status != DSF_DLSS5_OK) {
        return false;
    }
    // Protocol 2: the helper answers OK when either runtime can run. A wanted feature it doesn't
    // offer is off for the session; the others go on.
    Log("DLSS5 helper: features 0x%lX offered, DLAA runtime %u.%u.%u.%u", static_cast<unsigned long>(ack.features),
        ack.dlaa_runtime_version[0], ack.dlaa_runtime_version[1], ack.dlaa_runtime_version[2], ack.dlaa_runtime_version[3]);
    bool any = false;
    for (D5Feature& f : g_d5_features) {
        if (f.state == D5_FEATURE_OFF) {
            continue;
        }
        if ((ack.features & DSF_DLSS5_FEATURE_BIT(f.id)) == 0) {
            Log("DLSS5 helper: %s is not available (%s)", f.name,
                f.id == DSF_DLSS5_FEATURE_DLAA ? "nvngx_dlss.dll missing or refused, or no RTX GPU" :
                                                 "nvngx_dlssnr.dll missing or refused, or no RTX 50 GPU");
            InterlockedExchange(&f.state, D5_FEATURE_OFF);
            continue;
        }
        any = true;
    }
    return any;
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

D5ConfigureResult D5Configure(D5Feature& f, UINT w, UINT h)
{
    if (g_d5_transport == D5_TRANSPORT_VULKAN) {
        D5VkReleaseImports(f);
    } else {
        D5CpuRelease(f);
    }
    DsfDlss5Configure configure = {};
    configure.width = w;
    configure.height = h;
    configure.preset = f.preset;
    configure.feature = static_cast<uint32_t>(f.id);
    std::uint64_t handles[DSF_DLSS5_SLOT_COUNT] = {};
    for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
        configure.formats[slot] = f.formats[slot];
    }
    if (g_d5_transport == D5_TRANSPORT_CPU) {
        configure.flags = DSF_DLSS5_CONFIG_CLIENT_CREATES;
        for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
            const UINT bind = slot == DSF_DLSS5_SLOT_OUTPUT ? D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_SHADER_RESOURCE;
            f.slot_tex[slot] = D5CreateSharedTexture(w, h, f.formats[slot], bind, &handles[slot]);
            if (f.slot_tex[slot] == nullptr) {
                Log("DLSS5 helper: %s shared texture %d (%ux%u format %d) failed", f.name, slot, w, h, static_cast<int>(f.formats[slot]));
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
    Log("DLSS5 helper: CONFIGURE %s %ux%u preset %d -> status=%d ngx=0x%08lX formats %u/%u/%u/%u: %s", f.name, w, h, f.preset, ack.status,
        static_cast<unsigned long>(ack.ngx_result), ack.formats[0], ack.formats[1], ack.formats[2], ack.formats[3], ack.message);
    const HANDLE fence_in = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.fence_in));
    const HANDLE fence_out = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.fence_out));
    bool ok = ack.status == DSF_DLSS5_OK;
    if (ok && g_d5_transport == D5_TRANSPORT_VULKAN) {
        for (int slot = 0; ok && slot < DSF_DLSS5_SLOT_MASK; ++slot) {
            ok = D5VkImportImage(f, slot, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.textures[slot])), ack.formats[slot], w, h);
        }
        ok = ok && D5VkImportSemaphore(&f.vk_fence_in, fence_in) && D5VkImportSemaphore(&f.vk_fence_out, fence_out);
    } else if (ok) {
        ok = SUCCEEDED(g_d5_d11_5->OpenSharedFence(fence_in, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f.fence_in))) &&
            SUCCEEDED(g_d5_d11_5->OpenSharedFence(fence_out, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f.fence_out)));
        if (ok) {
            D3D11_TEXTURE2D_DESC staging = {};
            f.slot_tex[DSF_DLSS5_SLOT_OUTPUT]->GetDesc(&staging);
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.MiscFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ok = SUCCEEDED(g_d5_d11->CreateTexture2D(&staging, nullptr, &f.output_staging));
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
    f.width = w;
    f.height = h;
    f.frame_number = 0;   // n starts at 1 after every CONFIGURE (new fences)
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
    while (g_d5_bridge_state != D5_BRIDGE_FAILED) {
        WaitForSingleObject(g_d5_worker_event, INFINITE);
        // Every feature the render thread asked to configure, one after the other.
        for (D5Feature& f : g_d5_features) {
            if (g_d5_bridge_state == D5_BRIDGE_FAILED) {
                break;
            }
            if (f.state != D5_FEATURE_CONFIGURING) {
                continue;
            }
            const D5ConfigureResult result = D5Configure(f, f.want_width, f.want_height);
            if (result == D5_CONFIGURE_OK) {
                InterlockedExchange(&f.state, D5_FEATURE_RUNNING);
            } else if (result == D5_CONFIGURE_PIPE_LOST) {
                D5BridgeFail("CONFIGURE got no answer");
            } else if (++f.configure_attempts >= kD5MaxConfigureAttempts) {
                D5FeatureOff(f, "CONFIGURE failed");
            } else {
                InterlockedExchange(&f.state, D5_FEATURE_IDLE);
            }
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

// The features this session wants: DLSS 5 ([DLSS5] Helper) and DLAA ([DLAA] Enable).
void D5InitFeatures()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    D5Feature& nr = g_d5_features[DSF_DLSS5_FEATURE_NR];
    nr.id = DSF_DLSS5_FEATURE_NR;
    nr.name = "DLSS 5";
    nr.preset = 2;
    // Input=1: the tone-mapped scene goes in and comes back in f16, as in Remix.
    const bool hdr = g_d5_input == 1;
    const DXGI_FORMAT nr_formats[DSF_DLSS5_SLOT_COUNT] = {hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
        hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN};
    std::memcpy(nr.formats, nr_formats, sizeof(nr_formats));
    nr.state = g_d5_nr ? D5_FEATURE_IDLE : D5_FEATURE_OFF;
    // DLAA: the linear HDR scene in and out (IsHDR), the same depth and motion as DLSS 5.
    D5Feature& dlaa = g_d5_features[DSF_DLSS5_FEATURE_DLAA];
    dlaa.id = DSF_DLSS5_FEATURE_DLAA;
    dlaa.name = "DLAA";
    dlaa.preset = g_dlaa_preset;
    const DXGI_FORMAT dlaa_formats[DSF_DLSS5_SLOT_COUNT] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN};
    std::memcpy(dlaa.formats, dlaa_formats, sizeof(dlaa_formats));
    dlaa.state = g_dlaa_enable ? D5_FEATURE_IDLE : D5_FEATURE_OFF;
}

void D5BridgeStart(IDirect3DDevice9* device)
{
    if (!g_d5_helper || InterlockedCompareExchange(&g_d5_bridge_state, D5_BRIDGE_STARTING, D5_BRIDGE_OFF) != D5_BRIDGE_OFF) {
        return;
    }
    D5InitFeatures();
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
    Log("DLSS5 helper: transport %s, DLSS 5 %s, DLAA %s", g_d5_transport_name,
        !g_d5_nr ? "off" : g_d5_input == 1 ? "on the HDR scene target at the scene end (f16)" : "on the pre-HUD frame (8-bit)",
        !g_dlaa_enable ? "off" : g_d5_nr ? "on the HDR scene target at the scene end, before DLSS 5" : "on the HDR scene target at the scene end");
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
IDirect3DPixelShader9* g_d5_ps_object_motion = nullptr;   // per-car motion, stencil-tested
IDirect3DPixelShader9* g_d5_ps_hdr_tonemap = nullptr;     // Input=1: scene -> colour input
IDirect3DPixelShader9* g_d5_ps_hdr_inverse = nullptr;     // Input=1: output -> scene
IDirect3DPixelShader9* g_d5_ps_hdr_sanitize = nullptr;    // DLAA: scene -> colour input, and output -> scene

// FrameTimeLog=1: timestamp queries around the object pass, read back a couple of frames later.
struct D5PassTimer
{
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* frequency = nullptr;
    IDirect3DQuery9* begin = nullptr;
    IDirect3DQuery9* end = nullptr;
    bool pending = false;
};
D5PassTimer g_d5_object_timers[3];
int g_d5_object_timer_next = 0;

void D5ReleasePassTimers()
{
    for (D5PassTimer& timer : g_d5_object_timers) {
        D5SafeRelease(timer.disjoint);
        D5SafeRelease(timer.frequency);
        D5SafeRelease(timer.begin);
        D5SafeRelease(timer.end);
        timer.pending = false;
    }
}

// Collects the finished timings, then starts a free timer for this frame (nullptr: none free).
D5PassTimer* D5StartPassTimer(IDirect3DDevice9* device)
{
    for (D5PassTimer& timer : g_d5_object_timers) {
        if (!timer.pending) {
            continue;
        }
        BOOL disjoint = TRUE;
        UINT64 frequency = 0, begin = 0, end = 0;
        if (timer.disjoint->GetData(&disjoint, sizeof(disjoint), 0) == S_OK && timer.frequency->GetData(&frequency, sizeof(frequency), 0) == S_OK &&
            timer.begin->GetData(&begin, sizeof(begin), 0) == S_OK && timer.end->GetData(&end, sizeof(end), 0) == S_OK) {
            timer.pending = false;
            if (!disjoint && frequency > 0 && end >= begin) {
                g_d5_window_object_gpu_ms += static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
                ++g_d5_window_object_gpu_frames;
            }
        }
    }
    D5PassTimer& timer = g_d5_object_timers[g_d5_object_timer_next];
    if (timer.pending) {
        return nullptr;
    }
    if (timer.begin == nullptr &&
        (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &timer.disjoint)) ||
         FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &timer.frequency)) ||
         FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &timer.begin)) || FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &timer.end)))) {
        D5ReleasePassTimers();
        return nullptr;
    }
    g_d5_object_timer_next = (g_d5_object_timer_next + 1) % 3;
    timer.disjoint->Issue(D3DISSUE_BEGIN);
    timer.begin->Issue(D3DISSUE_END);
    return &timer;
}

void D5EndPassTimer(D5PassTimer* timer)
{
    if (timer == nullptr) {
        return;
    }
    timer->end->Issue(D3DISSUE_END);
    timer->disjoint->Issue(D3DISSUE_END);
    timer->frequency->Issue(D3DISSUE_END);
    timer->pending = true;
}
IDirect3DTexture9* g_d5_tex_depth = nullptr;          // R32F
IDirect3DTexture9* g_d5_tex_motion = nullptr;         // G16R16F
IDirect3DSurface9* g_d5_rt_depth = nullptr;
IDirect3DSurface9* g_d5_rt_motion = nullptr;
IDirect3DSurface9* g_d5_sys_depth = nullptr;
IDirect3DSurface9* g_d5_sys_motion = nullptr;
IDirect3DStateBlock9* g_d5_state = nullptr;
UINT g_d5_d9_width = 0;
UINT g_d5_d9_height = 0;
bool g_d5_shaders_failed = false;
// Depth and motion are drawn (and on the CPU transport read back) once per frame, for the first
// feature that runs; the second reuses them.
LONG g_d5_inputs_frame = -1;
bool g_d5_inputs_have_motion = false;
LONG g_d5_sys_inputs_frame = -1;

const char kD5ShaderSource[] = R"(
sampler2D tex0 : register(s0);
sampler2D tex1 : register(s1);
float4 reproject[4] : register(c0);   // rows of VP_prev * inverse(VP_cur), built in double on the CPU
float4 worldW : register(c4);         // row 3 of inverse(VP_cur): the world point's homogeneous w
float4 size : register(c8);           // width, height, cockpit depth (m), 0
float4 objectReproject[4] : register(c9);   // one car: VP_prev * W_prev * inverse(W_cur) * inverse(VP_cur), in double on the CPU
float4 debug : register(c13);         // ObjectMotionDebug: x > 0 paints the car's pixels with motion (y, z)
float4 hdrParams : register(c14);     // Input=1: exposure, 1 / exposure
float4 recovery : register(c15);      // highlight recovery edges: exposed HDR max channel 1..8, LDR max channel 0.75..0.99

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

// One moving car's pixels (stencil-tested): through its own previous transform. tex0 is the
// R32F depth copy. Like MotionPS, no world position in between.
float4 ObjectMotionPS(float2 uv : TEXCOORD0) : COLOR0
{
    float z = tex2D(tex0, uv).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1);
    float4 prev = float4(dot(objectReproject[0], ndc), dot(objectReproject[1], ndc), dot(objectReproject[2], ndc), dot(objectReproject[3], ndc));
    float2 prevUv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    float2 motion = (prevUv - uv) * size.xy;
    if (prev.w <= 0) motion = 0;
    if (debug.x > 0) motion = debug.yz;
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

// Input=1, RTX Remix's fast tone map and its inverse (tonemapping_fast_tonemapping and
// tonemapping_inverse_tonemapping in dxvk-remix). The ACES fit is Narkowicz's approximation.
float3 AcesApprox(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// fp16 range, negatives to 0 (min/max also turn a NaN into the other operand on GPUs).
float3 SanitizeHdr(float3 c)
{
    return min(max(c, 0), 65504);
}

// The scene target (linear HDR) -> DLSS 5's colour input: exposure, ACES, 1/2.2 gamma.
float4 HdrTonemapPS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 x = SanitizeHdr(tex2D(tex0, uv).rgb) * hdrParams.x;
    return float4(pow(AcesApprox(x), 1 / 2.2), 1);
}

// DLSS 5's output (tex0) back to HDR, with the scene as it was (tex1): the output's linear
// value times the ratio by which the curve compressed the original, per channel, so an
// unchanged pixel gives the original back. Highlight recovery: a pixel that was very bright
// and sits in the curve's shoulder, where the LDR output cannot carry it, blends back to the
// original. The scene's alpha is kept.
float4 HdrInversePS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 original = tex2D(tex1, uv);
    float3 hdr = SanitizeHdr(original.rgb);
    float3 x = hdr * hdrParams.x;
    float3 t = AcesApprox(x);
    float3 color = pow(saturate(tex2D(tex0, uv).rgb), 2.2) * (x / max(t, 1e-6)) * hdrParams.y;
    float3 ldr = pow(t, 1 / 2.2);
    float mask = smoothstep(recovery.x, recovery.y, max(x.r, max(x.g, x.b))) *
        smoothstep(recovery.z, recovery.w, max(ldr.r, max(ldr.g, ldr.b)));
    return float4(min(max(lerp(color, hdr, mask), 0), 65504), original.a);
}

// DLAA: the scene target into its colour input, and its output back into the scene target (drawn
// with alpha writes off, so the scene keeps its alpha). No NaN or negative may reach DLAA's
// history, and none comes back.
float4 HdrSanitizePS(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(SanitizeHdr(tex2D(tex0, uv).rgb), 1);
}
)";

bool g_d5_object_shader_failed = false;

// ObjectMotionPS on its own: if it doesn't compile, object motion is off and the rest goes on.
void D5CompileObjectShader(IDirect3DDevice9* device, D3DCompileFn compile)
{
    if (!g_d5_object_motion || g_d5_ps_object_motion != nullptr || g_d5_object_shader_failed) {
        return;
    }
    g_d5_object_shader_failed = true;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(kD5ShaderSource, sizeof(kD5ShaderSource) - 1, "dsf_dlss5", nullptr, nullptr, "ObjectMotionPS", "ps_2_0", 0, 0,
        &code, &errors);
    if (FAILED(hr) || code == nullptr) {
        Log("DLSS5 helper: ObjectMotionPS did not compile, object motion off: %s",
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    } else if (FAILED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g_d5_ps_object_motion))) {
        Log("DLSS5 helper: CreatePixelShader(ObjectMotionPS) failed, object motion off");
        g_d5_ps_object_motion = nullptr;
    } else {
        g_d5_object_shader_failed = false;
    }
    D5SafeRelease(errors);
    D5SafeRelease(code);
}

bool D5CompileShaders(IDirect3DDevice9* device)
{
    if (g_d5_ps_copy_swap != nullptr && (g_d5_ps_object_motion != nullptr || !g_d5_object_motion || g_d5_object_shader_failed)) {
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
    // CopySwapPS last: it marks the set as compiled.
    const char* entries[7] = {"DepthPS", "MotionPS", "CopyPS", "HdrTonemapPS", "HdrInversePS", "HdrSanitizePS", "CopySwapPS"};
    IDirect3DPixelShader9** targets[7] = {&g_d5_ps_depth, &g_d5_ps_motion, &g_d5_ps_copy, &g_d5_ps_hdr_tonemap, &g_d5_ps_hdr_inverse,
        &g_d5_ps_hdr_sanitize, &g_d5_ps_copy_swap};
    for (int i = 0; i < 7 && g_d5_ps_copy_swap == nullptr; ++i) {
        D5SafeRelease(*targets[i]);   // from an earlier attempt that stopped half-way
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
    D5CompileObjectShader(device, compile);
    return true;
}

void D5ReleaseFeatureD3D9(D5Feature& f)
{
    D5SafeRelease(f.sys_color);
    D5SafeRelease(f.composite);
    D5SafeRelease(f.rt_hdr_copy);
    D5SafeRelease(f.tex_hdr_copy);
    D5SafeRelease(f.rt_color_in);
    D5SafeRelease(f.tex_color_in);
    f.d9_width = 0;
    f.d9_height = 0;
}

void D5ReleaseD3D9()
{
    D5SafeRelease(g_d5_rt_depth);
    D5SafeRelease(g_d5_rt_motion);
    D5SafeRelease(g_d5_tex_depth);
    D5SafeRelease(g_d5_tex_motion);
    D5SafeRelease(g_d5_sys_depth);
    D5SafeRelease(g_d5_sys_motion);
    D5SafeRelease(g_d5_state);
    g_d5_d9_width = 0;
    g_d5_d9_height = 0;
    g_d5_inputs_frame = -1;
    g_d5_sys_inputs_frame = -1;
    for (D5Feature& f : g_d5_features) {
        D5ReleaseFeatureD3D9(f);
    }
}

// What every feature shares: depth, motion, their CPU readbacks and the state block.
bool D5EnsureD3D9(IDirect3DDevice9* device, UINT w, UINT h)
{
    if (g_d5_d9_width == w && g_d5_d9_height == h && g_d5_state != nullptr) {
        return true;
    }
    D5ReleaseD3D9();
    const bool cpu = g_d5_transport == D5_TRANSPORT_CPU && !g_d5_self_test;
    bool ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_d5_tex_depth, nullptr)) &&
        SUCCEEDED(g_d5_tex_depth->GetSurfaceLevel(0, &g_d5_rt_depth)) &&
        SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &g_d5_tex_motion, nullptr)) &&
        SUCCEEDED(g_d5_tex_motion->GetSurfaceLevel(0, &g_d5_rt_motion)) &&
        SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &g_d5_state));
    if (ok && cpu) {
        ok = SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &g_d5_sys_depth, nullptr)) &&
            SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &g_d5_sys_motion, nullptr));
    }
    if (!ok) {
        Log("DLSS5 helper: D3D9 resources %ux%u could not be created", w, h);
        D5ReleaseD3D9();
        return false;
    }
    g_d5_d9_width = w;
    g_d5_d9_height = h;
    return true;
}

// One feature's own: color_format is the format of the frame it reads (the HUD target for
// DLSS 5 Input=0, else the HDR scene target, whose colour input is A16B16G16R16F too).
bool D5EnsureFeatureD3D9(IDirect3DDevice9* device, D5Feature& f, UINT w, UINT h, D3DFORMAT color_format)
{
    if (f.d9_width == w && f.d9_height == h && f.d9_color_format == color_format && f.composite != nullptr) {
        return true;
    }
    D5ReleaseFeatureD3D9(f);
    const bool hdr = color_format == D3DFMT_A16B16G16R16F;
    const bool cpu = g_d5_transport == D5_TRANSPORT_CPU && !g_d5_self_test;
    // The output: the CPU transport locks it, the Vulkan one copies into it, SelfTest StretchRects into it.
    const DWORD composite_usage = g_d5_self_test ? D3DUSAGE_RENDERTARGET : cpu ? D3DUSAGE_DYNAMIC : 0;
    bool ok = SUCCEEDED(device->CreateTexture(w, h, 1, composite_usage, hdr ? D3DFMT_A16B16G16R16F : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &f.composite, nullptr));
    if (ok && hdr) {
        ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &f.tex_color_in, nullptr)) &&
            SUCCEEDED(f.tex_color_in->GetSurfaceLevel(0, &f.rt_color_in));
    }
    if (ok && hdr && f.id == DSF_DLSS5_FEATURE_NR) {
        ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &f.tex_hdr_copy, nullptr)) &&
            SUCCEEDED(f.tex_hdr_copy->GetSurfaceLevel(0, &f.rt_hdr_copy));
    }
    if (ok && cpu) {
        // GetRenderTargetData needs the target's own format (A8R8G8B8 or X8R8G8B8, both BGRA bytes).
        ok = SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, color_format, D3DPOOL_SYSTEMMEM, &f.sys_color, nullptr));
    }
    if (!ok) {
        Log("DLSS5 helper: %s D3D9 resources %ux%u (format %d) could not be created", f.name, w, h, static_cast<int>(color_format));
        D5ReleaseFeatureD3D9(f);
        return false;
    }
    f.d9_width = w;
    f.d9_height = h;
    f.d9_color_format = color_format;
    return true;
}

void D5DrawQuad(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DPixelShader9* shader, IDirect3DBaseTexture9* texture,
    int stencil_ref = -1, const RECT* scissor = nullptr, IDirect3DBaseTexture9* texture1 = nullptr, DWORD color_write = 0xF)
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
    auto set_sampler = [device](DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) {
        if (g_real_set_sampler_state != nullptr) {
            g_real_set_sampler_state(device, stage, type, value);
        } else {
            device->SetSamplerState(stage, type, value);
        }
    };
    if (texture1 != nullptr) {
        device->SetTexture(1, texture1);
    }
    for (DWORD stage = 0; stage < (texture1 != nullptr ? 2u : 1u); ++stage) {
        set_sampler(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        set_sampler(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        set_sampler(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        set_sampler(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        set_sampler(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        set_sampler(stage, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    if (stencil_ref >= 0) {
        // Only the pixels the scene tagged with this car (stencil bits 1-7).
        device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        device->SetRenderState(D3DRS_STENCILREF, static_cast<DWORD>(stencil_ref));
        device->SetRenderState(D3DRS_STENCILMASK, 0xFE);
        device->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
        device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    } else {
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    }
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor != nullptr ? TRUE : FALSE);
    if (scissor != nullptr) {
        device->SetScissorRect(scissor);
    }
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, color_write);
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

// The screen rectangle of a car: the box 12 m around its origin (cars, buses and trucks fit),
// projected with this frame's VP. False: off screen or behind the camera. *clip stays null
// when the box reaches behind the near plane (the whole screen, as for the cockpit interior).
bool D5ObjectScissor(const double* vp, const float* world, UINT w, UINT h, RECT* rect, const RECT** clip)
{
    const double radius = 12.0;
    double left = 1e30, right = -1e30, top = 1e30, bottom = -1e30;
    int behind = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const double p[3] = {world[3] + (corner & 1 ? radius : -radius), world[7] + (corner & 2 ? radius : -radius),
            world[11] + (corner & 4 ? radius : -radius)};
        const double x = vp[0] * p[0] + vp[1] * p[1] + vp[2] * p[2] + vp[3];
        const double y = vp[4] * p[0] + vp[5] * p[1] + vp[6] * p[2] + vp[7];
        const double cw = vp[12] * p[0] + vp[13] * p[1] + vp[14] * p[2] + vp[15];
        if (cw <= 0.05) {
            ++behind;
            continue;
        }
        const double sx = (x / cw * 0.5 + 0.5) * w;
        const double sy = (0.5 - y / cw * 0.5) * h;
        left = std::min(left, sx);
        right = std::max(right, sx);
        top = std::min(top, sy);
        bottom = std::max(bottom, sy);
    }
    *clip = nullptr;
    if (behind == 8) {
        return false;
    }
    if (behind > 0) {
        return true;   // reaches behind the camera: no useful rectangle
    }
    if (right < 0.0 || bottom < 0.0 || left > w || top > h) {
        return false;
    }
    rect->left = static_cast<LONG>(std::max(0.0, std::floor(left)));
    rect->top = static_cast<LONG>(std::max(0.0, std::floor(top)));
    rect->right = static_cast<LONG>(std::min(static_cast<double>(w), std::ceil(right)));
    rect->bottom = static_cast<LONG>(std::min(static_cast<double>(h), std::ceil(bottom)));
    if (rect->right <= rect->left || rect->bottom <= rect->top) {
        return false;
    }
    *clip = rect;
    return true;
}

// The render targets and depth surface, which a state block does not hold.
struct D5SavedTargets
{
    IDirect3DSurface9* rt[4] = {};
    IDirect3DSurface9* ds = nullptr;
};

void D5SaveTargets(IDirect3DDevice9* device, D5SavedTargets* saved)
{
    for (DWORD i = 0; i < 4; ++i) {
        device->GetRenderTarget(i, &saved->rt[i]);   // D3DERR_NOTFOUND (null) for an unbound slot
    }
    device->GetDepthStencilSurface(&saved->ds);
}

void D5RestoreTargets(IDirect3DDevice9* device, D5SavedTargets* saved)
{
    for (DWORD i = 0; i < 4; ++i) {
        if (i != 0 || saved->rt[0] != nullptr) {   // slot 0 can't be unbound
            device->SetRenderTarget(i, saved->rt[i]);
        }
        D5SafeRelease(saved->rt[i]);
    }
    device->SetDepthStencilSurface(saved->ds);
    D5SafeRelease(saved->ds);
}

// The states the bridge touches, read back to check that it put them all back (the first
// frames, traced frames and SelfTest frames; reads only, the device is not pure).
struct D5StateCheck
{
    void* objects[14] = {};   // render targets 0-3, depth, shaders, declaration, stream 0, indices, textures 0-2
    UINT stream_offset = 0;
    UINT stream_stride = 0;
    DWORD fvf = 0;
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {};
    DWORD render[20] = {};
    DWORD sampler[2][6] = {};
    float ps_constants[16 * 4] = {};
};

void D5ReadStates(IDirect3DDevice9* device, D5StateCheck* c)
{
    static const D3DRENDERSTATETYPE kRender[20] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF,
        D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILPASS, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_SCISSORTESTENABLE,
        D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_FILLMODE};
    static const D3DSAMPLERSTATETYPE kSampler[6] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU,
        D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
    IDirect3DSurface9* surface = nullptr;
    for (DWORD i = 0; i < 4; ++i) {
        surface = nullptr;
        device->GetRenderTarget(i, &surface);
        c->objects[i] = surface;
    }
    surface = nullptr;
    device->GetDepthStencilSurface(&surface);
    c->objects[4] = surface;
    IDirect3DVertexShader9* vs = nullptr;
    device->GetVertexShader(&vs);
    c->objects[5] = vs;
    IDirect3DPixelShader9* ps = nullptr;
    device->GetPixelShader(&ps);
    c->objects[6] = ps;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    device->GetVertexDeclaration(&declaration);
    c->objects[7] = declaration;
    IDirect3DVertexBuffer9* stream = nullptr;
    device->GetStreamSource(0, &stream, &c->stream_offset, &c->stream_stride);
    c->objects[8] = stream;
    IDirect3DIndexBuffer9* indices = nullptr;
    device->GetIndices(&indices);
    c->objects[9] = indices;
    for (DWORD stage = 0; stage < 3; ++stage) {
        IDirect3DBaseTexture9* texture = nullptr;
        device->GetTexture(stage, &texture);
        c->objects[10 + stage] = texture;
    }
    // Only the identities are compared: drop the references Get* took.
    for (void* object : c->objects) {
        if (object != nullptr) {
            static_cast<IUnknown*>(object)->Release();
        }
    }
    device->GetFVF(&c->fvf);
    device->GetViewport(&c->viewport);
    device->GetScissorRect(&c->scissor);
    for (int i = 0; i < 20; ++i) {
        device->GetRenderState(kRender[i], &c->render[i]);
    }
    for (DWORD stage = 0; stage < 2; ++stage) {
        for (int i = 0; i < 6; ++i) {
            device->GetSamplerState(stage, kSampler[i], &c->sampler[stage][i]);
        }
    }
    device->GetPixelShaderConstantF(0, c->ps_constants, 16);
}

void D5CompareStates(const D5StateCheck& before, const D5StateCheck& after, const char* where)
{
    char diff[256] = {};
    size_t used = 0;
    auto note = [&](const char* what, int index) {
        if (used + 24 < sizeof(diff)) {
            used += std::snprintf(diff + used, sizeof(diff) - used, " %s%d", what, index);
        }
    };
    for (int i = 0; i < 14; ++i) {
        if (before.objects[i] != after.objects[i]) {
            note("object", i);
        }
    }
    if (before.stream_offset != after.stream_offset || before.stream_stride != after.stream_stride) {
        note("stream", 0);
    }
    if (before.fvf != after.fvf) {
        note("fvf", 0);
    }
    if (std::memcmp(&before.viewport, &after.viewport, sizeof(before.viewport)) != 0) {
        note("viewport", 0);
    }
    if (std::memcmp(&before.scissor, &after.scissor, sizeof(before.scissor)) != 0) {
        note("scissor", 0);
    }
    for (int i = 0; i < 20; ++i) {
        if (before.render[i] != after.render[i]) {
            note("render", i);
        }
    }
    for (int stage = 0; stage < 2; ++stage) {
        for (int i = 0; i < 6; ++i) {
            if (before.sampler[stage][i] != after.sampler[stage][i]) {
                note(stage == 0 ? "sampler0." : "sampler1.", i);
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (std::memcmp(before.ps_constants + i * 4, after.ps_constants + i * 4, sizeof(float) * 4) != 0) {
            note("psconst", i);
        }
    }
    if (used == 0) {
        Log("DLSS5 helper: device state after the bridge (%s) matches the state before it", where);
    } else {
        Log("DLSS5 helper: device state after the bridge (%s) differs:%s", where, diff);
    }
}

// Depth and motion into our render targets. The caller saves and restores the device's state.
bool D5RenderInputs(IDirect3DDevice9* device, IDirect3DTexture9* depth, UINT w, UINT h)
{
    // VP_prev * inverse(VP_cur) in double: NDC of this frame -> clip space of the last.
    double current[16] = {}, previous[16] = {}, inv_vp[16] = {}, reproject[16] = {};
    for (int i = 0; i < 16; ++i) {
        current[i] = g_d5_vp_current[i];
        previous[i] = g_d5_vp_previous[i];
    }
    const bool have_motion = g_d5_vp_valid && g_d5_vp_previous_valid && D5Invert4x4(current, inv_vp);
    D5Multiply4x4(previous, inv_vp, reproject);
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
    } else if (g_d5_object_motion && (g_d5_moving_objects > 0 || g_d5_object_debug) && g_d5_scene_ds != nullptr) {
        // Moving cars: their pixels, by stencil tag, get their own previous transform.
        if (g_d5_trace_frame != nullptr && g_d5_trace_dump && g_d5_trace_pair_step != 0) {
            D5DumpSurface(device, g_d5_rt_motion, "dlss5-motion-camera");   // before, for comparison
        }
        D5PassTimer* timer = g_d5_frame_time_log ? D5StartPassTimer(device) : nullptr;
        device->SetDepthStencilSurface(static_cast<IDirect3DSurface9*>(const_cast<void*>(g_d5_scene_ds)));
        int drawn = 0;
        for (int k = 1; k <= g_d5_object_count; ++k) {
            const D5Object& object = g_d5_objects[k];
            if (!object.moving && !g_d5_object_debug) {
                continue;
            }
            RECT scissor = {};
            const RECT* clip = nullptr;
            if (!D5ObjectScissor(current, object.world, w, h, &scissor, &clip)) {
                continue;   // off screen or behind the camera
            }
            // VP_prev * D * inverse(VP_cur), D = W_prev * inverse(W_cur) (identity when not moving).
            double delta[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            if (object.moving) {
                std::memcpy(delta, object.delta, sizeof(object.delta));
            }
            double through[16] = {}, matrix[16] = {};
            D5Multiply4x4(previous, delta, through);
            D5Multiply4x4(through, inv_vp, matrix);
            float rows[16] = {};
            for (int i = 0; i < 16; ++i) {
                rows[i] = static_cast<float>(matrix[i]);
            }
            // ObjectMotionDebug: each tagged car in a flat colour of its own (dumped as dlss5-motion)
            const float debug[4] = {g_d5_object_debug ? 1.0f : 0.0f, static_cast<float>((k * 37) % 61 - 30), static_cast<float>((k * 23) % 61 - 30), 0.0f};
            device->SetPixelShaderConstantF(9, rows, 4);
            device->SetPixelShaderConstantF(13, debug, 1);
            D5DrawQuad(device, g_d5_rt_motion, g_d5_ps_object_motion, g_d5_tex_depth, k << 1, clip);
            ++drawn;
        }
        g_d5_window_object_drawn += drawn;
        D5EndPassTimer(timer);
        device->SetDepthStencilSurface(nullptr);
    }
    if (g_d5_trace_frame != nullptr && g_d5_trace_dump && g_d5_trace_pair_step != 0) {
        D5DumpSurface(device, g_d5_rt_depth, "dlss5-depth");
        D5DumpSurface(device, g_d5_rt_motion, "dlss5-motion");
    }
    return have_motion;
}

// Depth and motion for this frame: drawn for the first feature that runs, reused by the other.
bool D5FrameInputs(IDirect3DDevice9* device, IDirect3DTexture9* depth, UINT w, UINT h)
{
    if (g_d5_inputs_frame != g_d5_frame_index) {
        g_d5_inputs_have_motion = D5RenderInputs(device, depth, w, h);
        g_d5_inputs_frame = g_d5_frame_index;
    }
    return g_d5_inputs_have_motion;
}

DsfDlss5Frame D5MakeFrame(const D5Feature& f, std::uint64_t n)
{
    DsfDlss5Frame frame = {};
    frame.frame = n;
    frame.feature = static_cast<std::uint32_t>(f.id);
    // RESET on the first frame after CONFIGURE, whenever last frame's VP is not the frame just
    // before (menus, pauses, loading, camera cuts), and when this feature skipped the frame before:
    // the helper's history would be from another view.
    const bool reset = n == 1 || !g_d5_vp_previous_valid || f.composited_frame != g_d5_frame_index - 1;
    frame.flags = DSF_DLSS5_FRAME_GPU_TIME | (reset ? DSF_DLSS5_FRAME_RESET : 0);
    frame.mv_scale[0] = 1.0f;
    frame.mv_scale[1] = 1.0f;
    if (f.id == DSF_DLSS5_FEATURE_DLAA) {
        // The offset this frame's scene was drawn with; zero on frames it wasn't jittered.
        if (g_dlaa_jitter_frame) {
            frame.jitter[0] = g_dlaa_jx;
            frame.jitter[1] = g_dlaa_jy;
        }
        return frame;
    }
    frame.flags |= DSF_DLSS5_FRAME_AUTO_MASK | (g_d5_helper_bypass ? DSF_DLSS5_FRAME_BYPASS : 0);
    frame.style = static_cast<std::uint32_t>(g_d5_style);
    frame.intensity = g_d5_intensity;
    frame.tone_strength = g_d5_tone_strength;
    frame.structure_strength = g_d5_structure_strength;
    frame.skin_strength = g_d5_skin_strength;
    return frame;
}

// FRAME n of a feature and its FRAME_ACK. Returns false when the connection is gone (the
// session is over); *ack_ok says whether the helper evaluated the frame (fence_out >= n either
// way).
bool D5ExchangeFrame(D5Feature& f, std::uint64_t n, bool* ack_ok, DsfDlss5FrameAck* ack)
{
    *ack_ok = false;
    const DsfDlss5Frame frame = D5MakeFrame(f, n);
    DsfDlss5Error error = {};
    if (!dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_FRAME, frame, kD5FrameTimeoutMs) ||
        !dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_FRAME_ACK, ack, kD5FrameTimeoutMs, &error)) {
        const DWORD last = GetLastError();
        error.message[sizeof(error.message) - 1] = '\0';
        char what[256] = {};
        std::snprintf(what, sizeof(what), "%s FRAME %llu got no answer (%s, helper status %d: %s)", f.name, static_cast<unsigned long long>(n),
            last == WAIT_TIMEOUT ? "timed out" : "pipe closed", error.status, error.message);
        D5BridgeFail(what);
        return false;
    }
    if (ack->frame != n || ack->feature != static_cast<std::uint32_t>(f.id) || ack->status != DSF_DLSS5_OK) {
        char what[192] = {};
        std::snprintf(what, sizeof(what), "%s FRAME %llu answered with status %d (ngx 0x%08lX, frame %llu, feature %lu)", f.name,
            static_cast<unsigned long long>(n), ack->status, static_cast<unsigned long>(ack->ngx_result),
            static_cast<unsigned long long>(ack->frame), static_cast<unsigned long>(ack->feature));
        D5FeatureRetry(f, what);
        return true;
    }
    *ack_ok = true;
    return true;
}

// CPU transport frame: color_source is the frame the feature reads. Returns true when the
// output is in f.composite.
bool D5CpuFrame(IDirect3DDevice9* device, D5Feature& f, IDirect3DSurface9* color_source, UINT w, UINT h, float* gpu_ms)
{
    auto upload = [](IDirect3DSurface9* staging, ID3D11Texture2D* shared) {
        D3DLOCKED_RECT locked = {};
        if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            return false;
        }
        g_d5_ctx->UpdateSubresource(shared, 0, nullptr, locked.pBits, static_cast<UINT>(locked.Pitch), 0);
        staging->UnlockRect();
        return true;
    };
    // Depth and motion come back once per frame, for both features.
    bool ok = true;
    if (g_d5_sys_inputs_frame != g_d5_frame_index) {
        ok = SUCCEEDED(device->GetRenderTargetData(g_d5_rt_depth, g_d5_sys_depth)) &&
            SUCCEEDED(device->GetRenderTargetData(g_d5_rt_motion, g_d5_sys_motion));
        g_d5_sys_inputs_frame = ok ? g_d5_frame_index : -1;
    }
    ok = ok && SUCCEEDED(device->GetRenderTargetData(color_source, f.sys_color)) && upload(f.sys_color, f.slot_tex[DSF_DLSS5_SLOT_COLOR]) &&
        upload(g_d5_sys_depth, f.slot_tex[DSF_DLSS5_SLOT_DEPTH]) && upload(g_d5_sys_motion, f.slot_tex[DSF_DLSS5_SLOT_MOTION]);
    if (!ok) {
        D5BridgeFail("readback failed");
        return false;
    }
    const std::uint64_t n = ++f.frame_number;
    g_d5_ctx->Signal(f.fence_in, n);
    g_d5_ctx->Flush();
    bool ack_ok = false;
    DsfDlss5FrameAck ack = {};
    if (!D5ExchangeFrame(f, n, &ack_ok, &ack) || !ack_ok) {
        return false;
    }
    *gpu_ms = ack.gpu_ms;
    // fence_out >= n already (the ACK comes after the helper's GPU work); the wait orders our copy.
    g_d5_ctx->Wait(f.fence_out, n);
    g_d5_ctx->CopyResource(f.output_staging, f.slot_tex[DSF_DLSS5_SLOT_OUTPUT]);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    D3DLOCKED_RECT locked = {};
    bool copied = false;
    if (SUCCEEDED(g_d5_ctx->Map(f.output_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const bool hdr = f.formats[DSF_DLSS5_SLOT_OUTPUT] == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (SUCCEEDED(f.composite->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD))) {
            for (UINT y = 0; y < h; ++y) {
                const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                auto* dst = static_cast<std::uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
                if (hdr) {
                    std::memcpy(dst, src, static_cast<size_t>(w) * 8);   // RGBA16F in both
                    continue;
                }
                for (UINT x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            f.composite->UnlockRect(0);
            copied = true;
        }
        g_d5_ctx->Unmap(f.output_staging, 0);
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
    // The ring is three submissions deep; a frame's copies finish long before that.
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

// Vulkan transport frame (DXVK): color_source is the frame the feature reads. Returns true when
// the output is in f.composite.
bool D5VkFrame(D5Feature& f, IDirect3DSurface9* color_source, UINT w, UINT h, float* gpu_ms)
{
    IDirect3DSurface9* composite_surface = nullptr;
    f.composite->GetSurfaceLevel(0, &composite_surface);
    D5VkTexture color, depth, motion, composite;
    const bool found = D5VkTextureOf(color_source, &color) && D5VkTextureOf(g_d5_rt_depth, &depth) &&
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

    // Inputs: the colour and our depth and motion into the feature's shared textures, fence_in = n.
    const std::uint64_t n = ++f.frame_number;
    const int slots[3] = {DSF_DLSS5_SLOT_COLOR, DSF_DLSS5_SLOT_DEPTH, DSF_DLSS5_SLOT_MOTION};
    vkm::VkImageMemoryBarrier before[6] = {};
    vkm::VkImageMemoryBarrier after[3] = {};
    vkm::VkImageCopy copies[3] = {};
    vkm::VkImage copy_src[3] = {};
    vkm::VkImage copy_dst[3] = {};
    for (int i = 0; i < 3; ++i) {
        const vkm::VkImage shared = f.vk_image[slots[i]];
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
    const bool submitted = D5VkSubmit(0, before, 6, copies, copy_src, copy_dst, 3, after, 3, 0, 0, f.vk_fence_in, n);
    g_d5_vk_interop->ReleaseSubmissionQueue();
    g_d5_vk_interop->UnlockDevice();   // not held while the helper works

    bool ack_ok = false;
    DsfDlss5FrameAck ack = {};
    bool alive = submitted;
    if (!submitted) {
        D5BridgeFail("submitting the input copies failed");
    } else {
        g_d5_step = 6;
        alive = D5ExchangeFrame(f, n, &ack_ok, &ack);
    }

    // Output: only after an OK ACK (fence_out >= n has been reached) do we queue the wait.
    bool composited = false;
    if (alive && ack_ok) {
        *gpu_ms = ack.gpu_ms;
        const vkm::VkImage output = f.vk_image[DSF_DLSS5_SLOT_OUTPUT];
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
        composited = D5VkSubmit(1, out_before, 2, &copy, &output, &composite.image, 1, out_after, 2, f.vk_fence_out, n, 0, 0);
        g_d5_vk_interop->ReleaseSubmissionQueue();
        if (!composited) {
            D5BridgeFail("submitting the output copy failed");
        }
    }
    g_d5_vk_ring = (g_d5_vk_ring + 1) % kD5VkRing;

    g_d5_step = 9;
    g_d5_vk_interop->LockDevice();
    // Back to the layouts DXVK expects; the caller then draws the output.
    for (const D5VkTexture* source : sources) {
        g_d5_vk_interop->TransitionTextureLayout(source->interop, &range, vkm::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source->layout);
    }
    g_d5_vk_interop->TransitionTextureLayout(composite.interop, &range, vkm::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, composite.layout);
    g_d5_vk_interop->UnlockDevice();
    release();
    return composited;
}

// Runs one feature at its point in the frame, with the game's state as it is there:
//   D5_RUN_NR_PRE_HUD (DLSS 5, Input=0): the pre-HUD point, the HUD target bound; the output is
//     drawn over it. It is downstream of the scene end, so with DLAA on it gets DLAA's scene.
//   D5_RUN_DLAA and D5_RUN_NR_HDR (DLSS 5, Input=1): the scene end, the HDR scene target still
//     bound; the output goes back into the scene target before the post chain reads it. DLAA
//     runs first and DLSS 5 then reads DLAA's output: two round trips.
// Everything the bridge changes on the device is put back before it returns. True when the
// feature's output went back into the frame.
bool D5BridgeFrame(IDirect3DDevice9* device, D5Run run)
{
    const LONG state = g_d5_bridge_state;
    if (!g_d5_helper || state == D5_BRIDGE_FAILED) {
        return false;
    }
    if (state == D5_BRIDGE_OFF && !g_d5_self_test) {
        D5BridgeStart(device);
        return false;
    }
    D5InitFeatures();   // SelfTest never starts the bridge
    D5Feature& f = g_d5_features[run == D5_RUN_DLAA ? DSF_DLSS5_FEATURE_DLAA : DSF_DLSS5_FEATURE_NR];
    if (f.state == D5_FEATURE_OFF || (run == D5_RUN_DLAA && !DlaaSceneUsable())) {
        return false;   // DLAA: not on split-screen frames
    }

    const bool hdr = run != D5_RUN_NR_PRE_HUD;
    auto* target = static_cast<IDirect3DSurface9*>(const_cast<void*>(hdr ? g_d5_scene_rt : g_d5.rt0));
    D3DSURFACE_DESC desc = {};
    if (target == nullptr || FAILED(target->GetDesc(&desc))) {
        return false;
    }
    // The scene end takes only the fp16 scene target at the game's render size: never the
    // 3072x3072 shadow map, which can be the busiest target with a depth surface in some frames.
    const bool format_ok = hdr ? desc.Format == D3DFMT_A16B16G16R16F && desc.Width == D5RenderWidth() && desc.Height == D5RenderHeight() &&
            desc.MultiSampleType == D3DMULTISAMPLE_NONE :
        desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
    if (!format_ok) {
        return false;
    }
    if (!g_d5_self_test) {
        if (state != D5_BRIDGE_READY) {
            return false;   // still starting
        }
        const LONG feature_state = f.state;
        if (feature_state == D5_FEATURE_IDLE) {
            // The worker configures for this size; frames go without this feature meanwhile.
            f.want_width = desc.Width;
            f.want_height = desc.Height;
            InterlockedExchange(&f.state, D5_FEATURE_CONFIGURING);
            SetEvent(g_d5_worker_event);
            return false;
        }
        if (feature_state != D5_FEATURE_RUNNING) {
            return false;
        }
        if (f.width != desc.Width || f.height != desc.Height) {
            InterlockedExchange(&f.state, D5_FEATURE_IDLE);
            return false;
        }
        if (!D5PipeFree()) {
            return false;   // the other feature is being configured: the worker has the pipe
        }
    }
    IDirect3DTexture9* depth = D5IntzTextureOf(g_d5_scene_ds);
    if (depth == nullptr) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("DLSS5 helper: the scene depth is not an INTZ texture; waiting");
        }
        return false;
    }
    // DLAA reads the scene target itself (it writes it back afterwards, from its output).
    IDirect3DTexture9* scene_texture = nullptr;
    if (run == D5_RUN_DLAA &&
        (FAILED(target->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&scene_texture))) || scene_texture == nullptr)) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("DLSS5 helper: the scene target is not a texture; DLAA skipped");
        }
        depth->Release();
        return false;
    }
    g_d5_internal = true;
    if (!D5CompileShaders(device) || !D5EnsureD3D9(device, desc.Width, desc.Height) ||
        !D5EnsureFeatureD3D9(device, f, desc.Width, desc.Height, desc.Format)) {
        g_d5_internal = false;
        depth->Release();
        D5SafeRelease(scene_texture);
        D5BridgeFail("D3D9 shaders or resources failed");
        return false;
    }

    const double start = D5NowMs();
    const bool check_states = f.frames < 3 || g_d5_trace_frame != nullptr || (g_d5_self_test && f.frames < 30);
    D5StateCheck before_check;
    if (check_states) {
        D5ReadStates(device, &before_check);
    }
    D5SavedTargets saved;
    D5SaveTargets(device, &saved);
    g_d5_state->Capture();

    g_d5_step = 1;
    IDirect3DSurface9* color_source = target;
    const float hdr_params[8] = {g_d5_hdr_exposure, 1.0f / g_d5_hdr_exposure, 0.0f, 0.0f, 1.0f, 8.0f, 0.75f, 0.99f};
    if (run == D5_RUN_NR_HDR) {
        // Keep the scene as it was (the inverse reads it while it writes the scene target), and
        // tone-map it into the colour input.
        device->SetPixelShaderConstantF(14, hdr_params, 2);
        g_d5_stretch_rect(device, target, nullptr, f.rt_hdr_copy, nullptr, D3DTEXF_POINT);
        device->SetDepthStencilSurface(nullptr);
        D5DrawQuad(device, f.rt_color_in, g_d5_ps_hdr_tonemap, f.tex_hdr_copy);
        color_source = f.rt_color_in;
    } else if (run == D5_RUN_DLAA) {
        // The linear scene as it is (IsHDR), without NaNs or negatives.
        device->SetDepthStencilSurface(nullptr);
        D5DrawQuad(device, f.rt_color_in, g_d5_ps_hdr_sanitize, scene_texture);
        color_source = f.rt_color_in;
    }
    const bool have_motion = D5FrameInputs(device, depth, desc.Width, desc.Height);
    depth->Release();
    g_d5_step = 2;
    float gpu_ms = -1.0f;
    bool composited = false;
    if (g_d5_self_test) {
        // No helper: the feature's output is its input, so the frame should come back unchanged.
        IDirect3DSurface9* composite_surface = nullptr;
        composited = SUCCEEDED(f.composite->GetSurfaceLevel(0, &composite_surface)) &&
            SUCCEEDED(g_d5_stretch_rect(device, color_source, nullptr, composite_surface, nullptr, D3DTEXF_POINT));
        D5SafeRelease(composite_surface);
        f.frame_number += 1;
    } else {
        composited = g_d5_transport == D5_TRANSPORT_VULKAN ? D5VkFrame(f, color_source, desc.Width, desc.Height, &gpu_ms) :
            D5CpuFrame(device, f, color_source, desc.Width, desc.Height, &gpu_ms);
    }
    if (composited) {
        g_d5_step = 10;
        device->SetDepthStencilSurface(nullptr);
        if (run == D5_RUN_DLAA) {
            // Colour only: the scene target keeps its own alpha.
            D5DrawQuad(device, target, g_d5_ps_hdr_sanitize, f.composite, -1, nullptr, nullptr,
                D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        } else if (run == D5_RUN_NR_HDR) {
            device->SetPixelShaderConstantF(14, hdr_params, 2);
            D5DrawQuad(device, target, g_d5_ps_hdr_inverse, f.composite, -1, nullptr, f.tex_hdr_copy);
        } else {
            // The Vulkan output is R8G8B8A8 copied raw into an A8R8G8B8 texture: swap red and blue.
            const bool swap = g_d5_transport == D5_TRANSPORT_VULKAN && !g_d5_self_test;
            D5DrawQuad(device, target, swap ? g_d5_ps_copy_swap : g_d5_ps_copy, f.composite);
        }
    }
    D5RestoreTargets(device, &saved);
    g_d5_state->Apply();
    D5SafeRelease(scene_texture);
    const char* where = run == D5_RUN_DLAA ? "scene end, DLAA" : run == D5_RUN_NR_HDR ? "scene end, DLSS 5" : "pre-HUD";
    if (check_states) {
        D5StateCheck after_check;
        D5ReadStates(device, &after_check);
        D5CompareStates(before_check, after_check, where);
    }
    g_d5_internal = false;
    g_d5_step = 0;
    const double ms = D5NowMs() - start;
    if (!composited) {
        return false;
    }
    f.configure_attempts = 0;
    f.composited_frame = g_d5_frame_index;
    if (run == D5_RUN_DLAA) {
        g_d5_window_dlaa_ms += ms;
        ++g_d5_window_dlaa_frames;
        if (gpu_ms >= 0.0f) {
            g_d5_window_dlaa_gpu_ms += gpu_ms;
            ++g_d5_window_dlaa_gpu_frames;
        }
    } else {
        g_d5_window_bridge_ms += ms;
        ++g_d5_window_bridge_frames;
        if (gpu_ms >= 0.0f) {
            g_d5_window_gpu_ms += gpu_ms;
            ++g_d5_window_gpu_frames;
        }
    }
    f.ms_sum += ms;
    f.gpu_ms_sum += gpu_ms;
    const LONG frames = ++f.frames;
    if (frames <= 3 || frames % 600 == 0) {
        Log("DLSS5 helper: %s frame %llu composited at the %s, motion=%d, jitter %.3f,%.3f, bridge %.2f ms (mean %.2f) evaluate %.2f ms, transport %s%s",
            f.name, static_cast<unsigned long long>(f.frame_number), where, have_motion ? 1 : 0,
            run == D5_RUN_DLAA && g_dlaa_jitter_frame ? static_cast<double>(g_dlaa_jx) : 0.0,
            run == D5_RUN_DLAA && g_dlaa_jitter_frame ? static_cast<double>(g_dlaa_jy) : 0.0, ms, f.ms_sum / frames,
            static_cast<double>(gpu_ms), g_d5_self_test ? "none (SelfTest)" : g_d5_transport_name,
            g_d5_helper_bypass && run != D5_RUN_DLAA ? " (bypass)" : "");
    }
    return true;
}

// dlaa.inl, at the frame end: whether the next frame's scene may be jittered. Only while DLAA is
// producing output: its output went back this frame, at the render size, and the next frame can
// send it a FRAME (nothing is being configured).
bool D5DlaaProducing(UINT w, UINT h)
{
    const D5Feature& f = g_d5_features[DSF_DLSS5_FEATURE_DLAA];
    if (!g_d5_helper || !g_dlaa_enable || f.composited_frame != g_d5_frame_index || f.d9_width != w || f.d9_height != h) {
        return false;
    }
    if (g_d5_self_test) {
        return true;
    }
    return f.state == D5_FEATURE_RUNNING && f.width == w && f.height == h && D5PipeFree();
}

// Before IDirect3DDevice9::Reset: our default-pool resources must go.
void Dlss5OnReset()
{
    D5ReleaseD3D9();
    D5SafeRelease(g_d5_ps_depth);
    D5SafeRelease(g_d5_ps_motion);
    D5SafeRelease(g_d5_ps_copy);
    D5SafeRelease(g_d5_ps_copy_swap);
    D5SafeRelease(g_d5_ps_object_motion);
    D5SafeRelease(g_d5_ps_hdr_tonemap);
    D5SafeRelease(g_d5_ps_hdr_inverse);
    D5SafeRelease(g_d5_ps_hdr_sanitize);
    g_d5_object_shader_failed = false;
    D5ReleasePassTimers();
    g_d5_shaders_failed = false;
    D5ResetStencilShadows();
    // Reset releases every default-pool resource: forget the targets and textures the hooks saw,
    // so nothing calls into a freed surface before the game binds new ones.
    g_d5.rt0 = nullptr;
    g_d5.ds = nullptr;
    for (auto*& texture : g_d5.bound) {
        texture = nullptr;
    }
    for (auto*& texture : g_d5.vertex_bound) {
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
    DlaaOnReset();
}

// DLL_PROCESS_DETACH.
void Dlss5Shutdown()
{
    D5RemoveExceptionHandler();
}
