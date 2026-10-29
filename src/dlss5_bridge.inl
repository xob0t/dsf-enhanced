// DLSS 5 through dsf-dlss5-helper (dlss5/include/dsf_dlss5_protocol.h), prototype.
// [DLSS5] Helper=1 (with Export=1) turns it on.
//
// Each frame, at the pre-HUD point (dlss5_feed.inl):
//   1. two small D3D9 passes write the inputs the helper wants: depth as R32F z/w from the
//      scene depth (an INTZ texture that replaces the game's back-buffer-sized D24S8 surface)
//      and camera motion vectors as RG16F, reprojected through this and the previous frame's
//      view-projection;
//   2. the pre-HUD colour, depth and motion are read back to the CPU and uploaded into D3D11
//      textures shared with the helper (the CLIENT_CREATES direction);
//   3. fence_in = n, FRAME n, FRAME_ACK n, wait fence_out = n;
//   4. the helper's output is read back and drawn over the HUD target before the HUD.
// The CPU round trip makes this slow (several ms a frame), but it runs the same on native
// D3D9, dgVoodoo2 and DXVK. A zero-copy path needs the wrapper's own device (DXVK's
// ID3D9VkInteropDevice, the protocol's helper-creates direction); research/dlss5-renderer.md.
//
// Motion vectors are camera-only: every pixel is treated as static world, except pixels
// nearer than CockpitDepth metres (the car interior moves with the camera: zero motion).

using D3D11CreateDeviceFn = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDXGIFactory1Fn = HRESULT (WINAPI*)(REFIID, void**);
using D3DCompileFn = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT,
    ID3DBlob**, ID3DBlob**);

bool g_d5_helper_bypass = false;
char g_d5_helper_path[MAX_PATH] = {};
char g_d5_helper_runtime[MAX_PATH] = {};
char g_d5_helper_log[MAX_PATH] = {};
int g_d5_style = 0;
float g_d5_intensity = 1.0f;
float g_d5_tone_strength = 0.3f;
float g_d5_structure_strength = 0.7f;
float g_d5_skin_strength = 0.5f;
float g_d5_cockpit_depth = 1.5f;

void LoadDlss5BridgeConfig(const char* ini_path)
{
    g_d5_helper_bypass = GetPrivateProfileIntA("DLSS5", "HelperBypass", 0, ini_path) != 0;
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
}

// ---------------------------------------------------------------------------------------
// INTZ scene depth. The game's back-buffer-sized D24S8 depth surfaces are created as the top
// level of an INTZ texture instead, so the depth can be sampled. The surface keeps its texture
// alive; the map only remembers which texture a surface belongs to.

constexpr D3DFORMAT kD5FormatIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
SRWLOCK g_d5_intz_lock = SRWLOCK_INIT;
std::unordered_map<const void*, IDirect3DTexture9*> g_d5_intz;

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
    AcquireSRWLockExclusive(&g_d5_intz_lock);
    g_d5_intz[surface] = texture;
    ReleaseSRWLockExclusive(&g_d5_intz_lock);
    *out = surface;
    return true;
}

IDirect3DTexture9* D5IntzTextureOf(const void* surface)
{
    AcquireSRWLockShared(&g_d5_intz_lock);
    auto it = g_d5_intz.find(surface);
    IDirect3DTexture9* texture = it != g_d5_intz.end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_d5_intz_lock);
    return texture;
}

// ---------------------------------------------------------------------------------------
// Helper process and pipe (started on a worker thread; the render thread only checks state).

enum D5BridgeState : LONG
{
    D5_BRIDGE_OFF = 0,
    D5_BRIDGE_STARTING = 1,
    D5_BRIDGE_READY = 2,      // HELLO_ACK ok, D3D11 device made
    D5_BRIDGE_RUNNING = 3,    // CONFIGURE_ACK ok
    D5_BRIDGE_FAILED = 4,
};

volatile LONG g_d5_bridge_state = D5_BRIDGE_OFF;
HANDLE g_d5_pipe = INVALID_HANDLE_VALUE;
HANDLE g_d5_helper_process = nullptr;
ID3D11Device* g_d5_d11 = nullptr;
ID3D11Device5* g_d5_d11_5 = nullptr;
ID3D11DeviceContext4* g_d5_ctx = nullptr;
ID3D11Texture2D* g_d5_slot_tex[DSF_DLSS5_SLOT_COUNT] = {};
ID3D11Texture2D* g_d5_output_staging = nullptr;
ID3D11Fence* g_d5_fence_in = nullptr;
ID3D11Fence* g_d5_fence_out = nullptr;
UINT g_d5_bridge_width = 0;
UINT g_d5_bridge_height = 0;
std::uint64_t g_d5_frame_number = 0;
LONG g_d5_bridge_frames = 0;
double g_d5_bridge_ms_sum = 0.0;
double g_d5_bridge_gpu_ms_sum = 0.0;

template <typename T>
void D5SafeRelease(T*& object)
{
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

DWORD WINAPI D5BridgeStartThread(LPVOID)
{
    auto fail = [](const char* what, unsigned long code) {
        Log("DLSS5 helper: %s (%lu)", what, code);
        InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_FAILED);
        return 0ul;
    };

    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    auto create_device = d3d11 != nullptr ? reinterpret_cast<D3D11CreateDeviceFn>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    auto create_factory = dxgi != nullptr ? reinterpret_cast<CreateDXGIFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_device == nullptr || create_factory == nullptr) {
        return fail("d3d11.dll or dxgi.dll missing", GetLastError());
    }

    // The NVIDIA adapter: DLSS 5 runs there, and the helper is told its LUID.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
        return fail("CreateDXGIFactory1 failed", 0);
    }
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc = {};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE) {
            break;
        }
        D5SafeRelease(adapter);
    }
    factory->Release();
    if (adapter == nullptr) {
        return fail("no NVIDIA adapter", 0);
    }
    char adapter_name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapter_name, sizeof(adapter_name), nullptr, nullptr);

    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
    adapter->Release();
    if (FAILED(hr) || device == nullptr) {
        return fail("D3D11CreateDevice failed", static_cast<unsigned long>(hr));
    }
    device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_d5_d11_5));
    context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_d5_ctx));
    context->Release();
    g_d5_d11 = device;
    if (g_d5_d11_5 == nullptr || g_d5_ctx == nullptr) {
        return fail("D3D11.4 fences not available", 0);
    }

    // dsf-dlss5-helper.exe --game-pid <pid> --game-process <inherited handle> --runtime <dir> --log <file>
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
        Log("DLSS5 helper: could not start %s", command);
        return fail("CreateProcess failed", GetLastError());
    }
    CloseHandle(pi.hThread);
    g_d5_helper_process = pi.hProcess;
    Log("DLSS5 helper: started pid %lu: %s", pi.dwProcessId, command);

    wchar_t pipe_name[128] = {};
    swprintf_s(pipe_name, DSF_DLSS5_PIPE_FMT, GetCurrentProcessId());
    for (int attempt = 0; attempt < 300 && g_d5_pipe == INVALID_HANDLE_VALUE; ++attempt) {
        g_d5_pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_d5_pipe == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(pi.hProcess, &code);
                return fail("helper exited before opening its pipe", code);
            }
            Sleep(50);
        }
    }
    if (g_d5_pipe == INVALID_HANDLE_VALUE) {
        return fail("could not connect to the helper pipe", GetLastError());
    }

    DsfDlss5Hello hello = {GetCurrentProcessId(), DSF_DLSS5_API_D3D11, desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart};
    DsfDlss5HelloAck ack = {};
    if (!dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_HELLO, hello) ||
        !dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_HELLO_ACK, &ack)) {
        return fail("HELLO failed on the pipe", GetLastError());
    }
    ack.message[sizeof(ack.message) - 1] = '\0';
    ack.runtime_sha256[sizeof(ack.runtime_sha256) - 1] = '\0';
    Log("DLSS5 helper: HELLO_ACK status=%d helper_pid=%lu ngx=0x%08lX runtime %u.%u.%u.%u sha256=%s adapter=%s: %s", ack.status,
        static_cast<unsigned long>(ack.helper_pid), static_cast<unsigned long>(ack.ngx_result), ack.runtime_version[0],
        ack.runtime_version[1], ack.runtime_version[2], ack.runtime_version[3], ack.runtime_sha256, adapter_name, ack.message);
    if (ack.status != DSF_DLSS5_OK) {
        return fail("helper refused HELLO", static_cast<unsigned long>(ack.status));
    }
    InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_READY);
    return 0;
}

void D5BridgeStart()
{
    if (!g_d5_helper || InterlockedCompareExchange(&g_d5_bridge_state, D5_BRIDGE_STARTING, D5_BRIDGE_OFF) != D5_BRIDGE_OFF) {
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, D5BridgeStartThread, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
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

void D5ReleaseShared()
{
    for (auto*& texture : g_d5_slot_tex) {
        D5SafeRelease(texture);
    }
    D5SafeRelease(g_d5_output_staging);
    D5SafeRelease(g_d5_fence_in);
    D5SafeRelease(g_d5_fence_out);
}

// CONFIGURE for this size (render thread; one-time cost while the helper creates the feature).
bool D5BridgeConfigure(UINT w, UINT h)
{
    D5ReleaseShared();
    const DXGI_FORMAT formats[DSF_DLSS5_SLOT_COUNT] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN};
    DsfDlss5Configure configure = {};
    configure.width = w;
    configure.height = h;
    configure.preset = 2;
    configure.flags = DSF_DLSS5_CONFIG_CLIENT_CREATES;
    std::uint64_t handles[DSF_DLSS5_SLOT_COUNT] = {};
    for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
        configure.formats[slot] = formats[slot];
        const UINT bind = D3D11_BIND_SHADER_RESOURCE | (slot == DSF_DLSS5_SLOT_OUTPUT ? D3D11_BIND_UNORDERED_ACCESS : 0);
        g_d5_slot_tex[slot] = D5CreateSharedTexture(w, h, formats[slot], bind, &handles[slot]);
        if (g_d5_slot_tex[slot] == nullptr) {
            Log("DLSS5 helper: shared texture %d (%ux%u format %d) failed", slot, w, h, static_cast<int>(formats[slot]));
            return false;
        }
        configure.textures[slot] = handles[slot];
    }
    DsfDlss5ConfigureAck ack = {};
    const bool sent = dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_CONFIGURE, configure) &&
        dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_CONFIGURE_ACK, &ack);
    for (std::uint64_t handle : handles) {
        if (handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)));
        }
    }
    ack.message[sizeof(ack.message) - 1] = '\0';
    Log("DLSS5 helper: CONFIGURE %ux%u -> status=%d ngx=0x%08lX formats %u/%u/%u/%u: %s", w, h, ack.status,
        static_cast<unsigned long>(ack.ngx_result), ack.formats[0], ack.formats[1], ack.formats[2], ack.formats[3], ack.message);
    if (!sent || ack.status != DSF_DLSS5_OK) {
        return false;
    }
    const HANDLE fence_in = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.fence_in));
    const HANDLE fence_out = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(ack.fence_out));
    const bool fences = SUCCEEDED(g_d5_d11_5->OpenSharedFence(fence_in, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_d5_fence_in))) &&
        SUCCEEDED(g_d5_d11_5->OpenSharedFence(fence_out, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_d5_fence_out)));
    CloseHandle(fence_in);
    CloseHandle(fence_out);
    if (!fences) {
        Log("DLSS5 helper: OpenSharedFence failed");
        return false;
    }
    D3D11_TEXTURE2D_DESC staging = {};
    g_d5_slot_tex[DSF_DLSS5_SLOT_OUTPUT]->GetDesc(&staging);
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_d5_d11->CreateTexture2D(&staging, nullptr, &g_d5_output_staging))) {
        return false;
    }
    g_d5_bridge_width = w;
    g_d5_bridge_height = h;
    g_d5_frame_number = 0;
    return true;
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
IDirect3DSurface9* g_d5_rt_depth = nullptr;    // R32F
IDirect3DSurface9* g_d5_rt_motion = nullptr;   // G16R16F
IDirect3DSurface9* g_d5_sys_color = nullptr;
IDirect3DSurface9* g_d5_sys_depth = nullptr;
IDirect3DSurface9* g_d5_sys_motion = nullptr;
IDirect3DTexture9* g_d5_composite = nullptr;   // dynamic A8R8G8B8, the helper's output
IDirect3DStateBlock9* g_d5_state = nullptr;
UINT g_d5_d9_width = 0;
UINT g_d5_d9_height = 0;
bool g_d5_shaders_failed = false;

const char kD5ShaderSource[] = R"(
sampler2D tex0 : register(s0);
float4 invVp[4] : register(c0);    // rows of inverse(view-proj of this frame)
float4 prevVp[4] : register(c4);   // rows of the previous frame's view-proj
float4 size : register(c8);        // width, height, cockpit depth (m), 0

float4 DepthPS(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(tex0, uv).rrrr;
}

float4 MotionPS(float2 uv : TEXCOORD0) : COLOR0
{
    float z = tex2D(tex0, uv).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, z, 1);
    float4 world = float4(dot(invVp[0], ndc), dot(invVp[1], ndc), dot(invVp[2], ndc), dot(invVp[3], ndc));
    world /= world.w;
    float4 prev = float4(dot(prevVp[0], world), dot(prevVp[1], world), dot(prevVp[2], world), dot(prevVp[3], world));
    float2 prevUv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    float2 motion = (prevUv - uv) * size.xy;
    // Clip-space w is the view depth. Nearer than the cockpit depth: the car interior, which
    // moves with the camera. Behind the camera last frame: no usable history either.
    if (prev.w < size.z) motion = 0;
    return float4(motion, 0, 1);
}

float4 CopyPS(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2D(tex0, uv);
}
)";

bool D5CompileShaders(IDirect3DDevice9* device)
{
    if (g_d5_ps_copy != nullptr) {
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
    const char* entries[3] = {"DepthPS", "MotionPS", "CopyPS"};
    IDirect3DPixelShader9** targets[3] = {&g_d5_ps_depth, &g_d5_ps_motion, &g_d5_ps_copy};
    for (int i = 0; i < 3; ++i) {
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
    D5SafeRelease(g_d5_sys_color);
    D5SafeRelease(g_d5_sys_depth);
    D5SafeRelease(g_d5_sys_motion);
    D5SafeRelease(g_d5_composite);
    D5SafeRelease(g_d5_state);
    g_d5_d9_width = 0;
    g_d5_d9_height = 0;
}

bool D5EnsureD3D9(IDirect3DDevice9* device, UINT w, UINT h)
{
    if (g_d5_d9_width == w && g_d5_d9_height == h && g_d5_composite != nullptr) {
        return true;
    }
    D5ReleaseD3D9();
    IDirect3DTexture9* texture = nullptr;
    bool ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &texture, nullptr)) &&
        SUCCEEDED(texture->GetSurfaceLevel(0, &g_d5_rt_depth));
    D5SafeRelease(texture);
    ok = ok && SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &texture, nullptr)) &&
        SUCCEEDED(texture->GetSurfaceLevel(0, &g_d5_rt_motion));
    D5SafeRelease(texture);
    ok = ok && SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_d5_sys_color, nullptr)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &g_d5_sys_depth, nullptr)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(w, h, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &g_d5_sys_motion, nullptr)) &&
        SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_d5_composite, nullptr)) &&
        SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &g_d5_state));
    if (!ok) {
        Log("DLSS5 helper: D3D9 resources %ux%u could not be created", w, h);
        D5ReleaseD3D9();
        return false;
    }
    g_d5_d9_width = w;
    g_d5_d9_height = h;
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

bool D5Invert4x4(const float* m, float* out)
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
            out[r * 4 + c] = static_cast<float>(a[r][4 + c]);
        }
    }
    return true;
}

bool D5Readback(IDirect3DDevice9* device, IDirect3DSurface9* source, IDirect3DSurface9* staging, ID3D11Texture2D* target, UINT bytes_per_pixel)
{
    if (FAILED(device->GetRenderTargetData(source, staging))) {
        return false;
    }
    D3DLOCKED_RECT locked = {};
    if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        return false;
    }
    g_d5_ctx->UpdateSubresource(target, 0, nullptr, locked.pBits, static_cast<UINT>(locked.Pitch), 0);
    (void)bytes_per_pixel;
    staging->UnlockRect();
    return true;
}

void D5BridgeFail(const char* what)
{
    Log("DLSS5 helper: %s; DLSS 5 off for this session", what);
    InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_FAILED);
}

// Runs at the pre-HUD point with the HUD target bound and the composite drawn into it.
void D5BridgeFrame(IDirect3DDevice9* device)
{
    const LONG state = g_d5_bridge_state;
    if (!g_d5_helper || state == D5_BRIDGE_FAILED) {
        return;
    }
    if (state == D5_BRIDGE_OFF) {
        D5BridgeStart();
        return;
    }
    if (state == D5_BRIDGE_STARTING) {
        return;
    }

    auto* target = static_cast<IDirect3DSurface9*>(const_cast<void*>(g_d5.rt0));
    D3DSURFACE_DESC desc = {};
    if (target == nullptr || FAILED(target->GetDesc(&desc)) ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) {
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
    if (!D5CompileShaders(device) || !D5EnsureD3D9(device, desc.Width, desc.Height)) {
        D5BridgeFail("D3D9 shaders or resources failed");
        return;
    }
    if (state == D5_BRIDGE_READY || g_d5_bridge_width != desc.Width || g_d5_bridge_height != desc.Height) {
        if (!D5BridgeConfigure(desc.Width, desc.Height)) {
            D5BridgeFail("CONFIGURE failed");
            return;
        }
        InterlockedExchange(&g_d5_bridge_state, D5_BRIDGE_RUNNING);
    }

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER start = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    // 1. Depth and motion into our render targets.
    float inv_vp[16] = {};
    const bool have_motion = g_d5_vp_valid && g_d5_vp_previous_valid && D5Invert4x4(g_d5_vp_current, inv_vp);
    g_d5_internal = true;
    IDirect3DSurface9* saved_ds = nullptr;
    device->GetDepthStencilSurface(&saved_ds);
    g_d5_state->Capture();
    device->SetDepthStencilSurface(nullptr);
    D5DrawQuad(device, g_d5_rt_depth, g_d5_ps_depth, depth);
    float constants[9 * 4] = {};
    std::memcpy(constants, inv_vp, sizeof(inv_vp));
    std::memcpy(constants + 16, have_motion ? g_d5_vp_previous : g_d5_vp_current, sizeof(float) * 16);
    constants[32] = static_cast<float>(desc.Width);
    constants[33] = static_cast<float>(desc.Height);
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

    // 2. Upload.
    const bool uploaded = D5Readback(device, target, g_d5_sys_color, g_d5_slot_tex[DSF_DLSS5_SLOT_COLOR], 4) &&
        D5Readback(device, g_d5_rt_depth, g_d5_sys_depth, g_d5_slot_tex[DSF_DLSS5_SLOT_DEPTH], 4) &&
        D5Readback(device, g_d5_rt_motion, g_d5_sys_motion, g_d5_slot_tex[DSF_DLSS5_SLOT_MOTION], 4);
    if (!uploaded) {
        g_d5_internal = false;
        D5SafeRelease(saved_ds);
        D5BridgeFail("readback failed");
        return;
    }

    // 3. One DLSS 5 evaluate.
    const std::uint64_t n = ++g_d5_frame_number;
    g_d5_ctx->Signal(g_d5_fence_in, n);
    g_d5_ctx->Flush();
    DsfDlss5Frame frame = {};
    frame.frame = n;
    frame.flags = DSF_DLSS5_FRAME_AUTO_MASK | DSF_DLSS5_FRAME_ACK_AFTER_GPU | (n == 1 ? DSF_DLSS5_FRAME_RESET : 0) |
        (g_d5_helper_bypass ? DSF_DLSS5_FRAME_BYPASS : 0);
    frame.style = static_cast<std::uint32_t>(g_d5_style);
    frame.mv_scale[0] = 1.0f;
    frame.mv_scale[1] = 1.0f;
    frame.intensity = g_d5_intensity;
    frame.tone_strength = g_d5_tone_strength;
    frame.structure_strength = g_d5_structure_strength;
    frame.skin_strength = g_d5_skin_strength;
    DsfDlss5FrameAck ack = {};
    if (!dsf_dlss5::PipeSend(g_d5_pipe, DSF_DLSS5_MSG_FRAME, frame) || !dsf_dlss5::PipeReceive(g_d5_pipe, DSF_DLSS5_MSG_FRAME_ACK, &ack)) {
        g_d5_internal = false;
        D5SafeRelease(saved_ds);
        D5BridgeFail("FRAME failed on the pipe");
        return;
    }
    if (ack.status != DSF_DLSS5_OK || ack.frame != n) {
        Log("DLSS5 helper: FRAME %llu status=%d ngx=0x%08lX", static_cast<unsigned long long>(n), ack.status,
            static_cast<unsigned long>(ack.ngx_result));
    }
    g_d5_ctx->Wait(g_d5_fence_out, n);
    g_d5_ctx->CopyResource(g_d5_output_staging, g_d5_slot_tex[DSF_DLSS5_SLOT_OUTPUT]);

    // 4. Output (RGBA8) into the composite texture (BGRA8 in memory), drawn over the HUD target.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    D3DLOCKED_RECT locked = {};
    bool composited = false;
    if (ack.status == DSF_DLSS5_OK && SUCCEEDED(g_d5_ctx->Map(g_d5_output_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        if (SUCCEEDED(g_d5_composite->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD))) {
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                auto* dst = static_cast<std::uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
                for (UINT x = 0; x < desc.Width; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            g_d5_composite->UnlockRect(0);
            composited = true;
        }
        g_d5_ctx->Unmap(g_d5_output_staging, 0);
    }
    if (composited) {
        g_d5_state->Capture();
        D5DrawQuad(device, target, g_d5_ps_copy, g_d5_composite);
        device->SetDepthStencilSurface(saved_ds);
        g_d5_state->Apply();
    }
    D5SafeRelease(saved_ds);
    g_d5_internal = false;

    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&end);
    const double ms = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    g_d5_bridge_ms_sum += ms;
    g_d5_bridge_gpu_ms_sum += ack.gpu_ms;
    const LONG frames = InterlockedIncrement(&g_d5_bridge_frames);
    if (frames <= 3 || frames % 600 == 0) {
        Log("DLSS5 helper: frame %llu ack=%d composited=%d motion=%d bridge %.2f ms (mean %.2f) evaluate %.2f ms (mean %.2f)%s",
            static_cast<unsigned long long>(n), ack.status, composited ? 1 : 0, have_motion ? 1 : 0, ms, g_d5_bridge_ms_sum / frames,
            static_cast<double>(ack.gpu_ms), g_d5_bridge_gpu_ms_sum / frames, g_d5_helper_bypass ? " (bypass)" : "");
    }
}

// Before IDirect3DDevice9::Reset: our default-pool resources must go.
void Dlss5OnReset()
{
    D5ReleaseD3D9();
    D5SafeRelease(g_d5_ps_depth);
    D5SafeRelease(g_d5_ps_motion);
    D5SafeRelease(g_d5_ps_copy);
    g_d5_shaders_failed = false;
    AcquireSRWLockExclusive(&g_d5_intz_lock);
    g_d5_intz.clear();
    ReleaseSRWLockExclusive(&g_d5_intz_lock);
}
