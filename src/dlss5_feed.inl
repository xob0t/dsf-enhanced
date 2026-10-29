// DLSS 5 groundwork (research/dlss5-renderer.md). Everything here is off unless [DLSS5] in
// dsf_enhanced.ini turns it on:
//  - Trace: logs a few frames' render passes (targets, formats, shaders, vertex constants)
//    and can dump render targets to BMP files.
//  - Export: finds the pre-HUD point each frame, keeps the current and previous scene
//    view-projection matrices, and publishes them in shared memory and to in-process
//    callbacks (DSFE_RegisterPreHudCallback, exported below).
//
// The game's draw calls don't all go through the device vtable (see the research note), so
// the methods are detoured at their first instructions, whatever module implements them
// (d3d9.dll, a ReShade proxy, DXVK or dgVoodoo).

// Uses dlss5_shared.h, included at the top of dsf_enhanced.cpp (outside the anonymous namespace).

using D5PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using D5SwapPresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using D5CreateTextureFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using D5CreateSurfaceFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using D5StretchRectFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using D5SetRenderTargetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using D5SetDepthStencilFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using D5ClearFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using D5SetViewportFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using D5SetRenderStateFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using D5SetTextureFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using D5DrawPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using D5DrawIndexedPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using D5DrawPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using D5DrawIndexedPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using D5CreateShaderFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, void**);
using D5SetShaderFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
using D5SetConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

D5PresentFn g_d5_present = nullptr;
D5SwapPresentFn g_d5_swap_present = nullptr;
D5CreateTextureFn g_d5_create_texture = nullptr;
D5CreateSurfaceFn g_d5_create_render_target = nullptr;
D5CreateSurfaceFn g_d5_create_depth_stencil = nullptr;
D5StretchRectFn g_d5_stretch_rect = nullptr;
D5SetRenderTargetFn g_d5_set_render_target = nullptr;
D5SetDepthStencilFn g_d5_set_depth_stencil = nullptr;
D5ClearFn g_d5_clear = nullptr;
D5SetViewportFn g_d5_set_viewport = nullptr;
D5SetRenderStateFn g_d5_set_render_state = nullptr;
D5SetTextureFn g_d5_set_texture = nullptr;
D5DrawPrimitiveFn g_d5_draw_primitive = nullptr;
D5DrawIndexedPrimitiveFn g_d5_draw_indexed_primitive = nullptr;
D5DrawPrimitiveUPFn g_d5_draw_primitive_up = nullptr;
D5DrawIndexedPrimitiveUPFn g_d5_draw_indexed_primitive_up = nullptr;
D5CreateShaderFn g_d5_create_vertex_shader = nullptr;
D5CreateShaderFn g_d5_create_pixel_shader = nullptr;
D5SetShaderFn g_d5_set_vertex_shader = nullptr;
D5SetShaderFn g_d5_set_pixel_shader = nullptr;
D5SetConstantFFn g_d5_set_vs_constant_f = nullptr;

bool g_d5_trace = false;
bool g_d5_helper = false;      // [DLSS5] Helper: run DLSS 5 through dsf-dlss5-helper (dlss5_bridge.inl)
thread_local bool g_d5_internal = false;   // DSF Enhanced's own D3D9 calls on this thread: the hooks pass them through
void LoadDlss5BridgeConfig(const char* ini_path);
void D5BridgeFrame(IDirect3DDevice9* device);
bool D5CreateIntzDepth(IDirect3DDevice9* device, UINT w, UINT h, IDirect3DSurface9** out);
void D5FrameTimeTick();
void D5NoteShader(const void* shader, const DWORD* function);
void D5NoteGameStencilState(D3DRENDERSTATETYPE state, DWORD value);
void D5TagSceneDraw(IDirect3DDevice9* device);
void D5EndSceneTagging(IDirect3DDevice9* device);
void D5FinishObjects();
void D5ResetObjectsFrame();
extern bool g_d5_object_motion;
bool g_d5_trace_dump = false;
bool g_d5_trace_scan = false;      // TraceScan: look for the VP in game memory (stalls that frame for seconds)
bool g_d5_trace_shaders = false;   // TraceShaders: write each vertex shader's disassembly to TraceDumpDir
bool g_d5_export = false;
int g_d5_trace_start_seconds = 60;
int g_d5_trace_interval_seconds = 20;
int g_d5_trace_count = 4;
char g_d5_dump_dir[MAX_PATH] = {};
int g_d5_wvp_register = 0;     // first VS register of a scene draw's World*View*Proj rows
int g_d5_world_register = 4;   // first VS register of its 3x4 world rows
int g_d5_pre_hud_target_set = 0; // render-queue target set of the composite + HUD pass

void LoadDlss5Config(const char* ini_path)
{
    g_d5_trace = GetPrivateProfileIntA("DLSS5", "Trace", 0, ini_path) != 0;
    g_d5_trace_dump = GetPrivateProfileIntA("DLSS5", "TraceDump", 0, ini_path) != 0;
    g_d5_trace_scan = GetPrivateProfileIntA("DLSS5", "TraceScan", 0, ini_path) != 0;
    g_d5_trace_shaders = GetPrivateProfileIntA("DLSS5", "TraceShaders", 0, ini_path) != 0;
    g_d5_export = GetPrivateProfileIntA("DLSS5", "Export", 0, ini_path) != 0;
    g_d5_helper = g_d5_export && GetPrivateProfileIntA("DLSS5", "Helper", 0, ini_path) != 0;
    LoadDlss5BridgeConfig(ini_path);
    g_d5_trace_start_seconds = ClampInt(GetPrivateProfileIntA("DLSS5", "TraceStartSeconds", 60, ini_path), 0, 3600);
    g_d5_trace_interval_seconds = ClampInt(GetPrivateProfileIntA("DLSS5", "TraceIntervalSeconds", 20, ini_path), 1, 3600);
    g_d5_trace_count = ClampInt(GetPrivateProfileIntA("DLSS5", "TraceCount", 4, ini_path), 0, 64);
    g_d5_wvp_register = ClampInt(GetPrivateProfileIntA("DLSS5", "WorldViewProjRegister", 0, ini_path), 0, 252);
    g_d5_world_register = ClampInt(GetPrivateProfileIntA("DLSS5", "WorldRegister", 4, ini_path), 0, 253);
    g_d5_pre_hud_target_set = GetPrivateProfileIntA("DLSS5", "PreHudTargetSet", 0, ini_path);
    GetPrivateProfileStringA("DLSS5", "TraceDumpDir", "", g_d5_dump_dir, MAX_PATH, ini_path);
    if (g_d5_dump_dir[0] == '\0') {
        BuildGamePath(g_d5_dump_dir, MAX_PATH, "dsf_dlss5_dumps");
    }
}

bool Dlss5Wanted()
{
    return g_d5_trace || g_d5_export;
}

// ---------------------------------------------------------------------------------------
// Prologue detours. A small x86 length decoder covers the instructions function entries in
// d3d9.dll, ReShade (MSVC), DXVK (MinGW) and dgVoodoo start with; anything relative (calls,
// jumps, branches) in the copied bytes refuses the detour, and the method falls back to a
// vtable slot write.

int D5ModRmLength(const std::uint8_t* p)
{
    const std::uint8_t modrm = p[0];
    const int mod = modrm >> 6;
    const int rm = modrm & 7;
    int length = 1;
    if (mod != 3 && rm == 4) {
        const std::uint8_t sib = p[1];
        ++length;
        if (mod == 0 && (sib & 7) == 5) {
            length += 4;
        }
    }
    if (mod == 0 && rm == 5) {
        length += 4;
    } else if (mod == 1) {
        length += 1;
    } else if (mod == 2) {
        length += 4;
    }
    return length;
}

int D5InstructionLength(const std::uint8_t* p)
{
    int prefix = 0;
    while (p[prefix] == 0x64 || p[prefix] == 0x65 || p[prefix] == 0x66 || p[prefix] == 0xF0) {
        ++prefix;
    }
    const std::uint8_t* q = p + prefix;
    const std::uint8_t op = q[0];
    const bool operand16 = prefix > 0 && std::memchr(p, 0x66, prefix) != nullptr;
    if (op >= 0x50 && op <= 0x5F) return prefix + 1;                  // push/pop r32
    if (op == 0x90) return prefix + 1;                                 // nop
    if (op == 0x6A) return prefix + 2;                                 // push imm8
    if (op == 0x68) return prefix + 5;                                 // push imm32
    if (op >= 0xB8 && op <= 0xBF) return prefix + (operand16 ? 3 : 5); // mov r32, imm32
    if (op >= 0xB0 && op <= 0xB7) return prefix + 2;                   // mov r8, imm8
    if (op == 0xA1 || op == 0xA3) return prefix + 5;                   // mov eax, [moffs32] and back
    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x39: case 0x3B: case 0x85: case 0x87: case 0x88: case 0x89:
    case 0x8A: case 0x8B: case 0x8D:
        return prefix + 1 + D5ModRmLength(q + 1);
    case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6:
        return prefix + 1 + D5ModRmLength(q + 1) + 1;
    case 0x81: case 0xC7:
        return prefix + 1 + D5ModRmLength(q + 1) + (operand16 ? 2 : 4);
    case 0xFF: {
        const int reg = (q[1] >> 3) & 7;
        if (reg == 6 || reg == 0 || reg == 1) {   // push r/m32, inc, dec
            return prefix + 1 + D5ModRmLength(q + 1);
        }
        return 0;
    }
    case 0x0F:
        // movss/movaps/xorps/movd-style loads and stores with a ModRM operand; no 0F 8x branches.
        if (q[1] == 0x10 || q[1] == 0x11 || q[1] == 0x28 || q[1] == 0x29 || q[1] == 0x57 || q[1] == 0xB6 ||
            q[1] == 0xB7 || q[1] == 0xBE || q[1] == 0xBF) {
            return prefix + 2 + D5ModRmLength(q + 2);
        }
        return 0;
    default:
        return 0;
    }
}

struct D5Detour
{
    void* target;
    void* replacement;
    void* trampoline;
};

D5Detour g_d5_detours[48] = {};
int g_d5_detour_count = 0;

bool D5IsOwnCode(const void* address)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&D5IsOwnCode), &self);
    HMODULE owner = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(address), &owner);
    return owner != nullptr && owner == self;
}

const char* D5ModuleName(const void* address, char* buffer, DWORD size)
{
    HMODULE owner = nullptr;
    buffer[0] = '\0';
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &owner) && owner != nullptr) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(owner, path, MAX_PATH);
        const char* slash = std::strrchr(path, '\\');
        std::snprintf(buffer, size, "%s+0x%lX", slash != nullptr ? slash + 1 : path,
            static_cast<unsigned long>(static_cast<const std::uint8_t*>(address) - reinterpret_cast<std::uint8_t*>(owner)));
    } else {
        std::snprintf(buffer, size, "%p", address);
    }
    return buffer;
}

// Detours the function in vtable[index]; *original gets a trampoline (or the old slot value
// when only the slot could be replaced).
void D5Hook(void** vtable, size_t index, void* replacement, void** original, const char* name)
{
    void* target = vtable[index];
    if (target == nullptr || D5IsOwnCode(target)) {
        Log("DLSS5: %s slot %u already points into DSF Enhanced, not detoured", name, static_cast<unsigned>(index));
        return;
    }
    for (int i = 0; i < g_d5_detour_count; ++i) {
        if (g_d5_detours[i].target == target) {
            if (*original == nullptr) {
                *original = g_d5_detours[i].trampoline;
            }
            return;
        }
    }

    char where[160] = {};
    auto* code = static_cast<std::uint8_t*>(target);
    int length = 0;
    while (length < 5) {
        const int n = D5InstructionLength(code + length);
        if (n == 0) {
            length = 0;
            break;
        }
        length += n;
    }

    if (length == 0 || g_d5_detour_count >= static_cast<int>(sizeof(g_d5_detours) / sizeof(g_d5_detours[0]))) {
        *original = target;
        WriteMemory(&vtable[index], &replacement, sizeof(replacement));
        Log("DLSS5: %s at %s: prologue %02X %02X %02X %02X %02X not decoded, vtable slot replaced instead",
            name, D5ModuleName(target, where, sizeof(where)), code[0], code[1], code[2], code[3], code[4]);
        return;
    }

    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        return;
    }
    std::memcpy(trampoline, code, length);
    trampoline[length] = 0xE9;
    *reinterpret_cast<std::int32_t*>(trampoline + length + 1) = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(code + length) - (reinterpret_cast<std::uintptr_t>(trampoline) + length + 5));

    std::uint8_t jump[16] = {};
    std::memset(jump, 0x90, sizeof(jump));
    jump[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(jump + 1) = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(replacement) - (reinterpret_cast<std::uintptr_t>(code) + 5));
    *original = trampoline;
    g_d5_detours[g_d5_detour_count++] = {target, replacement, trampoline};
    WriteMemory(code, jump, static_cast<size_t>(length));
    Log("DLSS5: detoured %s at %s (%d-byte prologue)", name, D5ModuleName(target, where, sizeof(where)), length);
}

// ---------------------------------------------------------------------------------------
// Resources and shaders seen by the hooks.

struct D5Resource
{
    int id;
    char kind;      // T texture (render target or depth), S render-target surface, D depth surface
    UINT width;
    UINT height;
    D3DFORMAT format;
    DWORD usage;
    DWORD caller;
};

SRWLOCK g_d5_resource_lock = SRWLOCK_INIT;
std::unordered_map<const void*, D5Resource> g_d5_resources;    // texture or surface -> record
std::unordered_map<const void*, const void*> g_d5_surface_owner; // level-0 surface -> its texture
std::unordered_map<const void*, DWORD> g_d5_shader_hash;         // shader object -> FNV-1a of its bytecode
int g_d5_next_resource_id = 1;

DWORD D5Va(void* return_address)
{
    const ModuleRange exe = GetExeRange();
    const auto* p = static_cast<const std::uint8_t*>(return_address);
    if (p >= exe.base && p < exe.base + exe.size) {
        return static_cast<DWORD>(p - exe.base) + 0x400000;
    }
    return 0;
}

void D5Register(const void* object, char kind, UINT w, UINT h, D3DFORMAT format, DWORD usage, DWORD caller)
{
    AcquireSRWLockExclusive(&g_d5_resource_lock);
    const D5Resource r = {g_d5_next_resource_id++, kind, w, h, format, usage, caller};
    g_d5_resources[object] = r;
    ReleaseSRWLockExclusive(&g_d5_resource_lock);
    if (g_d5_trace) {
        Log("DLSS5 create %c#%d %ux%u fmt=%d usage=0x%lX caller=0x%08lX", kind, r.id, w, h, static_cast<int>(format),
            static_cast<unsigned long>(usage), static_cast<unsigned long>(caller));
    }
}

// The record of a render-target or depth surface, through its texture when it has one.
bool D5Lookup(const void* object, D5Resource* out)
{
    if (object == nullptr) {
        return false;
    }
    AcquireSRWLockShared(&g_d5_resource_lock);
    const void* key = object;
    auto owner = g_d5_surface_owner.find(object);
    if (owner != g_d5_surface_owner.end()) {
        key = owner->second;
    }
    auto it = g_d5_resources.find(key);
    const bool found = it != g_d5_resources.end();
    if (found) {
        *out = it->second;
    }
    ReleaseSRWLockShared(&g_d5_resource_lock);
    return found;
}

DWORD D5ShaderHash(const void* shader)
{
    if (shader == nullptr) {
        return 0;
    }
    AcquireSRWLockShared(&g_d5_resource_lock);
    auto it = g_d5_shader_hash.find(shader);
    const DWORD hash = it != g_d5_shader_hash.end() ? it->second : 1;
    ReleaseSRWLockShared(&g_d5_resource_lock);
    return hash;
}

DWORD D5HashBytecode(const DWORD* function)
{
    if (function == nullptr) {
        return 0;
    }
    // Shader model 1-3 bytecode ends with the 0x0000FFFF end token.
    DWORD hash = 2166136261u;
    for (size_t i = 0; i < 65536; ++i) {
        const DWORD token = function[i];
        for (int b = 0; b < 4; ++b) {
            hash ^= (token >> (b * 8)) & 0xFF;
            hash *= 16777619u;
        }
        if (token == 0x0000FFFF) {
            break;
        }
    }
    return hash;
}

void D5Describe(const void* object, char* buffer, size_t size)
{
    D5Resource r = {};
    if (object == nullptr) {
        std::snprintf(buffer, size, "-");
    } else if (D5Lookup(object, &r)) {
        std::snprintf(buffer, size, "%c#%d(%ux%u f%d)", r.kind, r.id, r.width, r.height, static_cast<int>(r.format));
    } else {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(static_cast<IDirect3DSurface9*>(const_cast<void*>(object))->GetDesc(&desc))) {
            std::snprintf(buffer, size, "surf(%ux%u f%d u0x%lX)", desc.Width, desc.Height, static_cast<int>(desc.Format),
                static_cast<unsigned long>(desc.Usage));
        } else {
            std::snprintf(buffer, size, "@%p", object);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Per-frame pass tracking (render thread). A pass starts at each SetRenderTarget(0, ...).

struct D5Pass
{
    const void* rt = nullptr;
    const void* ds = nullptr;
    DWORD caller = 0;
    D3DVIEWPORT9 vp = {};
    DWORD clears = 0;
    int draws = 0;
    UINT prims = 0;
    int blend_draws = 0;
    int z_draws = 0;
    int stencil_draws = 0;
    bool stretch = false;          // a StretchRect record: rt = destination, ds = source
    DWORD target_set = 0;          // the render queue's target-set id when the pass started
    DWORD layer = 0;
    char reads[256] = {};          // render-target textures bound during the pass's draws
    char shaders[192] = {};        // distinct pixel shader hashes (first few)
};

constexpr int kD5MaxPasses = 96;
constexpr int kD5MaxDrawLines = 1200;

struct D5Constant
{
    float v[4];
};

struct D5FrameTrace
{
    D5Pass passes[kD5MaxPasses];
    int pass_count = 0;
    // Vertex constants in the frame's largest pass: writes and value changes per register.
    int writes[256] = {};
    int changes[256] = {};
    D5Constant first[256] = {};
    D5Constant last[256] = {};
    bool seen[256] = {};
    char draw_lines[kD5MaxDrawLines][200] = {};
    int draw_line_count = 0;
};

struct D5State
{
    const void* rt0 = nullptr;
    const void* ds = nullptr;
    IDirect3DBaseTexture9* bound[16] = {};
    const void* vs = nullptr;
    const void* ps = nullptr;
    DWORD blend = 0;
    DWORD zenable = 1;
    DWORD zwrite = 1;
    DWORD stencil = 0;
    DWORD stencil_state[8] = {D3DCMP_ALWAYS, 0, 0xFFFFFFFF, 0xFFFFFFFF, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP,
        FALSE};   // func, ref, mask, writemask, pass, zfail, fail, twosided (the game's values)
    D5Constant vs_c[256] = {};
};

D5State g_d5;
D5FrameTrace g_d5_trace_buffers[2];
D5FrameTrace* g_d5_trace_frame = nullptr;   // non-null while a frame is traced
D5FrameTrace* g_d5_prev_trace = nullptr;    // the frame traced just before, for comparisons
int g_d5_traces_done = 0;
int g_d5_trace_pair_step = 0;               // 0 idle, 1 first of a pair, 2 second
DWORD g_d5_next_trace_tick = 0;
DWORD g_d5_device_tick = 0;
LONG g_d5_frame_index = 0;
int g_d5_dump_frames = 0;   // unused
int g_d5_scene_pass_draws_threshold = 200;

// Learned per frame: the scene colour target and depth surface (the pass with the most draws).
const void* g_d5_scene_rt = nullptr;
const void* g_d5_scene_ds = nullptr;
const void* g_d5_frame_best_rt = nullptr;
const void* g_d5_frame_best_ds = nullptr;
int g_d5_frame_best_draws = 0;
int g_d5_scene_draws_this_frame = 0;
bool g_d5_scene_done = false;         // the scene target was left after its draws this frame
bool g_d5_pre_hud_fired = false;
int g_d5_post_pass_index = 0;         // passes since the scene ended
int g_d5_pass_draws = 0;              // draws since the last SetRenderTarget(0)
DWORD g_d5_pass_target_set = 0xFFFFFFFF;

// The render-queue executor (0xBC4AC0) stores the id of the target set it is about to bind
// at 0x1D4AD10 and the item's layer byte at 0x1D4AD0C before calling the pass setup 0xBB0ED0.
constexpr DWORD kRenderQueueTargetSetRva = 0x0194AD10;
constexpr DWORD kRenderQueueLayerRva = 0x0194AD0C;

DWORD D5CurrentTargetSet(DWORD* layer)
{
    const ModuleRange exe = GetExeRange();
    if (!IsRvaInRange(exe, kRenderQueueTargetSetRva, sizeof(DWORD))) {
        return 0;
    }
    if (layer != nullptr) {
        *layer = *(exe.base + kRenderQueueLayerRva);
    }
    return *reinterpret_cast<const DWORD*>(exe.base + kRenderQueueTargetSetRva);
}

D5Pass* D5CurrentPass()
{
    D5FrameTrace* t = g_d5_trace_frame;
    if (t == nullptr) {
        return nullptr;
    }
    if (t->pass_count == 0) {
        t->passes[0] = D5Pass{};
        t->passes[0].rt = g_d5.rt0;
        t->passes[0].ds = g_d5.ds;
        t->pass_count = 1;
    }
    return &t->passes[t->pass_count - 1];
}

void D5AppendTag(char* list, size_t size, const char* tag)
{
    if (std::strstr(list, tag) == nullptr && std::strlen(list) + std::strlen(tag) + 1 < size) {
        std::strncat(list, tag, size - std::strlen(list) - 1);
    }
}

// ---------------------------------------------------------------------------------------
// BMP dumps of render targets (trace frames only).

float D5HalfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = (h >> 15) & 1;
    const std::uint32_t exponent = (h >> 10) & 0x1F;
    const std::uint32_t mantissa = h & 0x3FF;
    float value = 0.0f;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa != 0 ? 0.0f : 65504.0f;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -value : value;
}

std::uint8_t D5ToByte(float linear_hdr)
{
    // Reinhard, then a 1/2.2 gamma: enough to see an HDR target's content.
    const float x = std::max(0.0f, linear_hdr);
    const float mapped = std::pow(x / (1.0f + x), 1.0f / 2.2f);
    return static_cast<std::uint8_t>(std::min(255.0f, mapped * 255.0f + 0.5f));
}

void D5DumpSurface(IDirect3DDevice9* device, const void* surface_object, const char* label)
{
    if (!g_d5_trace_dump || device == nullptr || surface_object == nullptr) {
        return;
    }
    auto* surface = static_cast<IDirect3DSurface9*>(const_cast<void*>(surface_object));
    D3DSURFACE_DESC desc = {};
    if (FAILED(surface->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        return;
    }
    const bool half4 = desc.Format == D3DFMT_A16B16G16R16F;
    const bool bgra8 = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
    const bool depth32 = desc.Format == D3DFMT_R32F;      // z/w: shown as 1 - z stretched (near bright)
    const bool motion16 = desc.Format == D3DFMT_G16R16F;  // pixels: 128 + 4 * motion in red/green
    if (!half4 && !bgra8 && !depth32 && !motion16) {
        Log("DLSS5 dump %s: format %d not dumped", label, static_cast<int>(desc.Format));
        return;
    }

    IDirect3DSurface9* staging = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr))) {
        Log("DLSS5 dump %s: no staging surface", label);
        return;
    }
    HRESULT hr = device->GetRenderTargetData(surface, staging);
    D3DLOCKED_RECT locked = {};
    if (SUCCEEDED(hr)) {
        hr = staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    }
    if (FAILED(hr)) {
        Log("DLSS5 dump %s: readback failed hr=0x%08lX", label, static_cast<unsigned long>(hr));
        staging->Release();
        return;
    }

    char partial[MAX_PATH] = {};
    for (size_t i = 0; g_d5_dump_dir[i] != '\0' && i + 1 < sizeof(partial); ++i) {
        if ((g_d5_dump_dir[i] == '\\' || g_d5_dump_dir[i] == '/') && i > 2) {
            CreateDirectoryA(partial, nullptr);
        }
        partial[i] = g_d5_dump_dir[i];
    }
    CreateDirectoryA(g_d5_dump_dir, nullptr);
    char path[MAX_PATH] = {};
    std::snprintf(path, sizeof(path), "%s\\f%05ld-%s.bmp", g_d5_dump_dir, static_cast<long>(g_d5_frame_index), label);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (file != nullptr) {
        const UINT row_bytes = (desc.Width * 3 + 3) & ~3u;
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + row_bytes * desc.Height;
        ih.biSize = sizeof(ih);
        ih.biWidth = static_cast<LONG>(desc.Width);
        ih.biHeight = static_cast<LONG>(desc.Height);
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        std::fwrite(&fh, sizeof(fh), 1, file);
        std::fwrite(&ih, sizeof(ih), 1, file);
        std::vector<std::uint8_t> row(row_bytes, 0);
        for (UINT y = desc.Height; y-- > 0;) {
            const auto* src = static_cast<const std::uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                if (half4) {
                    const auto* h = reinterpret_cast<const std::uint16_t*>(src) + x * 4;
                    row[x * 3 + 0] = D5ToByte(D5HalfToFloat(h[2]));
                    row[x * 3 + 1] = D5ToByte(D5HalfToFloat(h[1]));
                    row[x * 3 + 2] = D5ToByte(D5HalfToFloat(h[0]));
                } else if (depth32) {
                    const float z = reinterpret_cast<const float*>(src)[x];
                    const std::uint8_t v = static_cast<std::uint8_t>(std::max(0.0f, std::min(255.0f, (1.0f - z) * 255.0f * 400.0f)));
                    row[x * 3 + 0] = v;
                    row[x * 3 + 1] = v;
                    row[x * 3 + 2] = v;
                } else if (motion16) {
                    const auto* h = reinterpret_cast<const std::uint16_t*>(src) + x * 2;
                    auto channel = [](float m) {
                        return static_cast<std::uint8_t>(std::max(0.0f, std::min(255.0f, 128.0f + 4.0f * m)));
                    };
                    row[x * 3 + 0] = 128;
                    row[x * 3 + 1] = channel(D5HalfToFloat(h[1]));
                    row[x * 3 + 2] = channel(D5HalfToFloat(h[0]));
                } else {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
            }
            std::fwrite(row.data(), 1, row_bytes, file);
        }
        std::fclose(file);
        Log("DLSS5 dump %s -> %s", label, path);
    }
    if (motion16) {
        // The motion in full as well: .rg32f, width * height pairs of float pixels, top row first.
        std::snprintf(path, sizeof(path), "%s\\f%05ld-%s.rg32f", g_d5_dump_dir, static_cast<long>(g_d5_frame_index), label);
        fopen_s(&file, path, "wb");
        if (file != nullptr) {
            std::vector<float> row(desc.Width * 2);
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* h = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch);
                for (UINT i = 0; i < desc.Width * 2; ++i) {
                    row[i] = D5HalfToFloat(h[i]);
                }
                std::fwrite(row.data(), sizeof(float), row.size(), file);
            }
            std::fclose(file);
        }
    }
    staging->UnlockRect();
    staging->Release();
}

// ---------------------------------------------------------------------------------------
// Export: shared memory and in-process callbacks.

HANDLE g_d5_mapping = nullptr;
Dsfe5Shared* g_d5_shared = nullptr;
float g_d5_vp_current[16] = {};
float g_d5_vp_previous[16] = {};
bool g_d5_vp_valid = false;
bool g_d5_vp_previous_valid = false;
LONG g_d5_vp_frame = -1;

struct D5Callback
{
    Dsfe5PreHudCallback fn;
    void* user;
};
D5Callback g_d5_callbacks[8] = {};
SRWLOCK g_d5_callback_lock = SRWLOCK_INIT;

void D5OpenSharedMemory()
{
    if (g_d5_shared != nullptr) {
        return;
    }
    char name[64] = {};
    std::snprintf(name, sizeof(name), DSFE5_SHARED_NAME_FMT, static_cast<unsigned long>(GetCurrentProcessId()));
    g_d5_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Dsfe5Shared), name);
    if (g_d5_mapping == nullptr) {
        Log("DLSS5: CreateFileMapping(%s) failed error=%lu", name, GetLastError());
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Someone else made this name first: never publish into a mapping we did not create.
        Log("DLSS5: shared memory %s already exists; not exported", name);
        CloseHandle(g_d5_mapping);
        g_d5_mapping = nullptr;
        return;
    }
    g_d5_shared = static_cast<Dsfe5Shared*>(MapViewOfFile(g_d5_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Dsfe5Shared)));
    if (g_d5_shared == nullptr) {
        Log("DLSS5: MapViewOfFile failed error=%lu", GetLastError());
        return;
    }
    std::memset(g_d5_shared, 0, sizeof(*g_d5_shared));
    g_d5_shared->magic = DSFE5_MAGIC;
    g_d5_shared->version = DSFE5_VERSION;
    g_d5_shared->size = sizeof(Dsfe5Shared);
    Log("DLSS5: shared memory %s (%u bytes)", name, static_cast<unsigned>(sizeof(Dsfe5Shared)));
}

void D5Publish(IDirect3DDevice9* device, Dsfe5Point point)
{
    Dsfe5FrameInfo info = {};
    info.size = sizeof(info);
    info.point = point;
    info.frame_index = static_cast<std::uint32_t>(g_d5_frame_index);
    info.flags = (g_d5_vp_valid ? DSFE5_FLAG_VIEW_PROJ : 0) | (g_d5_vp_previous_valid ? DSFE5_FLAG_PREV_VIEW_PROJ : 0);
    std::memcpy(info.view_proj, g_d5_vp_current, sizeof(info.view_proj));
    std::memcpy(info.prev_view_proj, g_d5_vp_previous, sizeof(info.prev_view_proj));
    D5Resource scene = {};
    if (D5Lookup(g_d5_scene_rt, &scene)) {
        info.width = scene.width;
        info.height = scene.height;
    }
    info.render_width = g_backbuffer_width;
    info.render_height = g_backbuffer_height;

    if (g_d5_shared != nullptr) {
        // Seqlock: odd while writing.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_d5_shared->sequence));
        g_d5_shared->frame = info;
        if (point == DSFE5_POINT_PRE_HUD) {
            g_d5_shared->pre_hud_count += 1;
        } else {
            g_d5_shared->scene_end_count += 1;
        }
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_d5_shared->sequence));
    }

    // Callbacks run under the shared lock: DSFE_UnregisterPreHudCallback takes it exclusively,
    // so once it returns no call is running. A callback must not register or unregister.
    auto* color = static_cast<IDirect3DSurface9*>(const_cast<void*>(point == DSFE5_POINT_PRE_HUD ? g_d5.rt0 : g_d5_scene_rt));
    auto* depth = static_cast<IDirect3DSurface9*>(const_cast<void*>(g_d5_scene_ds));
    AcquireSRWLockShared(&g_d5_callback_lock);
    for (const D5Callback& callback : g_d5_callbacks) {
        if (callback.fn != nullptr) {
            callback.fn(callback.user, device, color, depth, &info);
        }
    }
    ReleaseSRWLockShared(&g_d5_callback_lock);
}

// The scene's view-projection. The game uploads no per-frame matrix: each scene draw sets
// c0-c3 = World*View*Proj rows and c4-c6 = its 3x4 world rows (research/dlss5-renderer.md), so
// VP = WVP * inverse(World). Shaders with another constant layout give garbage here; the value
// most draws of the frame agree on wins.

struct D5VpBucket
{
    float vp[16];
    int count;
};

D5VpBucket g_d5_vp_buckets[64] = {};
int g_d5_vp_last_bucket = 0;
int g_d5_vp_evict_cursor = 0;
int g_d5_vp_bucket_count = 0;
int g_d5_vp_draws_tried = 0;

bool D5SolveViewProj(const D5Constant* c, float* vp)
{
    const float* w0 = c[g_d5_world_register + 0].v;
    const float* w1 = c[g_d5_world_register + 1].v;
    const float* w2 = c[g_d5_world_register + 2].v;
    // Inverse of the affine world [R t; 0 1] with R rows w0..w2 (xyz) and t = (w0.w, w1.w, w2.w).
    const float a = w0[0], b = w0[1], cc = w0[2];
    const float d = w1[0], e = w1[1], f = w1[2];
    const float g = w2[0], h = w2[1], k = w2[2];
    const float det = a * (e * k - f * h) - b * (d * k - f * g) + cc * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-8f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    const float r[9] = {
        (e * k - f * h) * inv_det, (cc * h - b * k) * inv_det, (b * f - cc * e) * inv_det,
        (f * g - d * k) * inv_det, (a * k - cc * g) * inv_det, (cc * d - a * f) * inv_det,
        (d * h - e * g) * inv_det, (b * g - a * h) * inv_det, (a * e - b * d) * inv_det,
    };
    const float t[3] = {w0[3], w1[3], w2[3]};
    const float inv[16] = {
        r[0], r[1], r[2], -(r[0] * t[0] + r[1] * t[1] + r[2] * t[2]),
        r[3], r[4], r[5], -(r[3] * t[0] + r[4] * t[1] + r[5] * t[2]),
        r[6], r[7], r[8], -(r[6] * t[0] + r[7] * t[1] + r[8] * t[2]),
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    for (int row = 0; row < 4; ++row) {
        const float* m = c[g_d5_wvp_register + row].v;
        for (int col = 0; col < 4; ++col) {
            vp[row * 4 + col] = m[0] * inv[0 * 4 + col] + m[1] * inv[1 * 4 + col] + m[2] * inv[2 * 4 + col] + m[3] * inv[3 * 4 + col];
            if (!std::isfinite(vp[row * 4 + col])) {
                return false;
            }
        }
    }
    return true;
}

bool D5SameMatrix(const float* x, const float* y)
{
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(x[i] - y[i]) > 1e-3f * std::max(1.0f, std::fabs(x[i]))) {
            return false;
        }
    }
    return true;
}

// A D3D perspective view-projection: row 3 (w) is the view direction, a unit vector, and row 2
// (z) is parallel to it. Rejects what other constant layouts produce.
bool D5LooksLikeViewProj(const float* vp)
{
    const float* z = vp + 8;
    const float* w = vp + 12;
    const float wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (wl < 0.99f || wl > 1.01f || zl < 1e-4f) {
        return false;
    }
    const float cosine = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / (zl * wl);
    const float xl = std::sqrt(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2]);
    const float yl = std::sqrt(vp[4] * vp[4] + vp[5] * vp[5] + vp[6] * vp[6]);
    return cosine > 0.999f && xl > 0.05f && yl > 0.05f;
}

// One scene draw's candidate; returns the bucket it joined (or -1).
int D5AddViewProjMatrix(const float* vp)
{
    if (g_d5_vp_last_bucket < g_d5_vp_bucket_count && D5SameMatrix(g_d5_vp_buckets[g_d5_vp_last_bucket].vp, vp)) {
        ++g_d5_vp_buckets[g_d5_vp_last_bucket].count;
        return g_d5_vp_last_bucket;
    }
    for (int i = 0; i < g_d5_vp_bucket_count; ++i) {
        if (D5SameMatrix(g_d5_vp_buckets[i].vp, vp)) {
            ++g_d5_vp_buckets[i].count;
            g_d5_vp_last_bucket = i;
            return i;
        }
    }
    if (g_d5_vp_bucket_count < static_cast<int>(sizeof(g_d5_vp_buckets) / sizeof(g_d5_vp_buckets[0]))) {
        D5VpBucket& bucket = g_d5_vp_buckets[g_d5_vp_bucket_count];
        std::memcpy(bucket.vp, vp, sizeof(bucket.vp));
        bucket.count = 1;
        return g_d5_vp_bucket_count++;
    }
    // Full: a candidate only one draw produced so far makes room (skinned and cockpit draws,
    // whose c0-c6 hold other data, come first in the scene and would otherwise fill every slot).
    for (int i = 0; i < g_d5_vp_bucket_count; ++i) {
        const int slot = (g_d5_vp_evict_cursor + i) % g_d5_vp_bucket_count;
        if (g_d5_vp_buckets[slot].count == 1) {
            std::memcpy(g_d5_vp_buckets[slot].vp, vp, sizeof(g_d5_vp_buckets[slot].vp));
            g_d5_vp_evict_cursor = slot + 1;
            return slot;
        }
    }
    return -1;
}

// A scene draw's vote: WVP * inverse(World). Returns its bucket, or -1.
int g_d5_last_vote_kind = 0;   // 1 when the draw's constants gave a candidate

int D5AddViewProjCandidate()
{
    g_d5_last_vote_kind = 0;
    if (g_d5_vp_draws_tried >= 4000) {
        return -1;
    }
    ++g_d5_vp_draws_tried;
    int bucket = -1;
    float vp[16] = {};
    if (D5SolveViewProj(g_d5.vs_c, vp) && D5LooksLikeViewProj(vp)) {
        bucket = D5AddViewProjMatrix(vp);
        g_d5_last_vote_kind = 1;
    }
    return bucket;
}

void D5LogMatrix(const char* label, const float* m)
{
    for (int row = 0; row < 4; ++row) {
        Log("DLSS5  %s row%d %.6f %.6f %.6f %.6f", label, row, static_cast<double>(m[row * 4 + 0]),
            static_cast<double>(m[row * 4 + 1]), static_cast<double>(m[row * 4 + 2]), static_cast<double>(m[row * 4 + 3]));
    }
}

double D5NowMs();

// Per-object motion, measurement only (Trace=1): which scene draws move between two traced
// frames, and how wrong camera-only motion vectors are for them. Each scene draw is keyed by
// its vertex shader and draw parameters; its world rows (c4-c6) are compared with the previous
// frame's draws of the same key: equal means static, otherwise the nearest one by translation
// is taken as the same object one frame earlier. research/dlss5-renderer.md, "Per-object motion".

struct D5ObjectDraw
{
    DWORD key;
    float wvp[16];
    float world[12];
};

std::vector<D5ObjectDraw> g_d5_object_draws;        // this traced frame's scene draws
std::vector<D5ObjectDraw> g_d5_object_draws_prev;   // the previous traced frame's, VP-checked
float g_d5_object_vp_prev[16] = {};
double g_d5_object_time_prev = 0.0;
LONG g_d5_object_frame_prev = -100;
DWORD g_d5_draw_key = 0;   // set by the draw hooks before D5BeforeDraw

DWORD D5DrawKey(DWORD a, DWORD b, DWORD c, DWORD d)
{
    DWORD hash = 2166136261u;
    const DWORD values[5] = {D5ShaderHash(g_d5.vs), a, b, c, d};
    for (DWORD v : values) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (v >> (i * 8)) & 0xFF;
            hash *= 16777619u;
        }
    }
    return hash;
}

void D5RecordObjectDraw()
{
    if (g_d5_object_draws.size() >= 8000) {
        return;
    }
    D5ObjectDraw draw = {};
    draw.key = g_d5_draw_key;
    for (int row = 0; row < 4; ++row) {
        std::memcpy(&draw.wvp[row * 4], g_d5.vs_c[g_d5_wvp_register + row].v, sizeof(float) * 4);
    }
    for (int row = 0; row < 3; ++row) {
        std::memcpy(&draw.world[row * 4], g_d5.vs_c[g_d5_world_register + row].v, sizeof(float) * 4);
    }
    g_d5_object_draws.push_back(draw);
}

bool D5Project(const float* vp, const float* p, float* px)
{
    float clip[4] = {};
    for (int row = 0; row < 4; ++row) {
        clip[row] = vp[row * 4 + 0] * p[0] + vp[row * 4 + 1] * p[1] + vp[row * 4 + 2] * p[2] + vp[row * 4 + 3];
    }
    if (clip[3] <= 0.01f) {
        return false;
    }
    px[0] = (clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<float>(g_backbuffer_width);
    px[1] = (0.5f - clip[1] / clip[3] * 0.5f) * static_cast<float>(g_backbuffer_height);
    return true;
}

// Scene end of a traced frame: keep the draws whose constants give this frame's VP, then compare
// with the previous traced frame when it was the frame just before.
void D5AnalyseObjectMotion()
{
    std::vector<D5ObjectDraw> valid;
    for (const D5ObjectDraw& draw : g_d5_object_draws) {
        D5Constant c[8] = {};
        for (int row = 0; row < 4; ++row) {
            std::memcpy(c[g_d5_wvp_register + row].v, &draw.wvp[row * 4], sizeof(float) * 4);
        }
        for (int row = 0; row < 3; ++row) {
            std::memcpy(c[g_d5_world_register + row].v, &draw.world[row * 4], sizeof(float) * 4);
        }
        float vp[16] = {};
        if (g_d5_wvp_register + 4 <= 8 && g_d5_world_register + 3 <= 8 && D5SolveViewProj(c, vp) && D5SameMatrix(vp, g_d5_vp_current)) {
            valid.push_back(draw);
        }
    }
    const double now = D5NowMs();
    if (g_d5_object_frame_prev == g_d5_frame_index - 1 && !g_d5_object_draws_prev.empty()) {
        std::unordered_map<DWORD, std::vector<const D5ObjectDraw*>> previous;
        for (const D5ObjectDraw& draw : g_d5_object_draws_prev) {
            previous[draw.key].push_back(&draw);
        }
        struct Moving
        {
            float world[12];
            float prev_t[3];
            int draws;
            float speed;
            float error_px;
            bool on_screen;
        };
        std::vector<Moving> objects;
        int unchanged = 0;
        int moved = 0;
        int unmatched = 0;
        const float dt = static_cast<float>(std::max(1.0, now - g_d5_object_time_prev)) / 1000.0f;
        for (const D5ObjectDraw& draw : valid) {
            auto it = previous.find(draw.key);
            if (it == previous.end()) {
                ++unmatched;
                continue;
            }
            const D5ObjectDraw* best = nullptr;
            float best_distance = 1e30f;
            for (const D5ObjectDraw* old : it->second) {
                if (std::memcmp(old->world, draw.world, sizeof(draw.world)) == 0) {
                    best = old;
                    best_distance = 0.0f;
                    break;
                }
                const float dx = old->world[3] - draw.world[3];
                const float dy = old->world[7] - draw.world[7];
                const float dz = old->world[11] - draw.world[11];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = old;
                }
            }
            if (best_distance == 0.0f) {
                ++unchanged;
                continue;
            }
            if (best == nullptr || best_distance > 10.0f) {
                ++unmatched;
                continue;
            }
            ++moved;
            // One object = one world matrix; its draws share it.
            Moving* object = nullptr;
            for (Moving& m : objects) {
                if (std::memcmp(m.world, draw.world, sizeof(m.world)) == 0) {
                    object = &m;
                    break;
                }
            }
            if (object == nullptr) {
                objects.push_back(Moving{});
                object = &objects.back();
                std::memcpy(object->world, draw.world, sizeof(object->world));
                object->prev_t[0] = best->world[3];
                object->prev_t[1] = best->world[7];
                object->prev_t[2] = best->world[11];
                object->speed = best_distance / dt;
                // The object's origin now, where it was, and where camera-only motion says it was.
                const float now_p[3] = {draw.world[3], draw.world[7], draw.world[11]};
                float px_now[2] = {};
                float px_true[2] = {};
                float px_camera[2] = {};
                object->on_screen = D5Project(g_d5_vp_current, now_p, px_now) && px_now[0] >= 0.0f &&
                    px_now[0] < static_cast<float>(g_backbuffer_width) && px_now[1] >= 0.0f &&
                    px_now[1] < static_cast<float>(g_backbuffer_height);
                if (D5Project(g_d5_object_vp_prev, object->prev_t, px_true) && D5Project(g_d5_object_vp_prev, now_p, px_camera)) {
                    object->error_px = std::sqrt((px_true[0] - px_camera[0]) * (px_true[0] - px_camera[0]) +
                        (px_true[1] - px_camera[1]) * (px_true[1] - px_camera[1]));
                }
            }
            ++object->draws;
        }
        std::sort(objects.begin(), objects.end(), [](const Moving& a, const Moving& b) { return a.error_px > b.error_px; });
        int on_screen = 0;
        for (const Moving& m : objects) {
            on_screen += m.on_screen ? 1 : 0;
        }
        Log("DLSS5  objects frame %ld (%.1f ms after the last): %u VP-layout scene draws, %d unchanged, %d moved (%u objects, %d on screen), %d new or unmatched",
            static_cast<long>(g_d5_frame_index), dt * 1000.0f, static_cast<unsigned>(valid.size()), unchanged, moved,
            static_cast<unsigned>(objects.size()), on_screen, unmatched);
        for (size_t i = 0; i < objects.size() && i < 10; ++i) {
            const Moving& m = objects[i];
            Log("DLSS5  object at %.1f %.1f %.1f, %d draws, %.1f km/h, camera-only motion off by %.1f px%s", m.world[3], m.world[7],
                m.world[11], m.draws, m.speed * 3.6f, m.error_px, m.on_screen ? "" : " (off screen)");
        }
    }
    g_d5_object_draws_prev.swap(valid);
    std::memcpy(g_d5_object_vp_prev, g_d5_vp_current, sizeof(g_d5_object_vp_prev));
    g_d5_object_time_prev = now;
    g_d5_object_frame_prev = g_d5_frame_index;
    g_d5_object_draws.clear();
}

double D5NowMs();

// Camera position, unit view direction and horizontal projection scale of a view-projection
// (rows as uploaded). Row 3 is the view direction times the projection's w scale; rows 0, 1 and
// 3 vanish at the camera, so the camera solves R * c = -(r0.w, r1.w, r3.w).
bool D5CameraOf(const float* vp, double* position, double* direction, double* scale_x)
{
    const double m[12] = {vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[12], vp[13], vp[14], vp[15]};
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    const double length = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12 || !(length > 1e-9)) {
        return false;
    }
    const double r[9] = {(e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det, (f * g - d * k) / det, (a * k - c * g) / det,
        (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    const double rhs[3] = {-m[3], -m[7], -m[11]};
    for (int i = 0; i < 3; ++i) {
        position[i] = r[i * 3 + 0] * rhs[0] + r[i * 3 + 1] * rhs[1] + r[i * 3 + 2] * rhs[2];
        direction[i] = m[8 + i] / length;
    }
    *scale_x = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) / length;
    return true;
}

// Camera cuts: view switches, replays, the Shift zoom starting or ending. Not a fixed distance a
// frame, because the camera's speed varies (the Shift zoom moves 20-48 m a frame) and so does
// the frame time (a hitch). A move is fine if it is within max(4 m, 90 m/s * dt) of standing
// still, or within max(4 m, 90 m/s * dt, half of it) of the previous move scaled to this
// frame's time. A cut is a move that is neither; a turn of more than max(20 degrees,
// 360 degrees/s * dt); or a change of the field of view by more than 5 % in one frame. The move
// is kept even across a cut, so a camera that keeps flying (the zoom) cuts once, and one that
// jumped and stops (a view switch) does too.
struct D5CameraTrack
{
    double time_ms = 0.0;
    double dt_ms = 0.0;
    double move[3] = {};
    bool move_valid = false;
};
D5CameraTrack g_d5_camera_track;
float g_d5_scene_dt_ms = 0.0f;   // time between the last two scene ends (0: unknown)

bool D5IsCameraCut(const float* previous, const float* current, double dt_ms, char* why, size_t why_size)
{
    double p0[3] = {}, d0[3] = {}, p1[3] = {}, d1[3] = {}, s0 = 0.0, s1 = 0.0;
    if (!D5CameraOf(previous, p0, d0, &s0) || !D5CameraOf(current, p1, d1, &s1)) {
        std::snprintf(why, why_size, "no camera");
        return true;
    }
    const double dt = std::max(dt_ms, 1.0) / 1000.0;
    double move[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    double expected[3] = {};
    if (g_d5_camera_track.move_valid && g_d5_camera_track.dt_ms > 0.0) {
        const double ratio = std::min(std::max(dt_ms, 1.0) / g_d5_camera_track.dt_ms, 4.0);
        for (int i = 0; i < 3; ++i) {
            expected[i] = g_d5_camera_track.move[i] * ratio;
        }
    }
    const double off_expected = std::sqrt((move[0] - expected[0]) * (move[0] - expected[0]) + (move[1] - expected[1]) * (move[1] - expected[1]) +
        (move[2] - expected[2]) * (move[2] - expected[2]));
    const double expected_length = std::sqrt(expected[0] * expected[0] + expected[1] * expected[1] + expected[2] * expected[2]);
    const double distance = std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
    const double still_limit = std::max(4.0, 90.0 * dt);
    const double move_limit = std::max(still_limit, 0.5 * expected_length);
    const bool move_ok = distance <= still_limit || off_expected <= move_limit;
    const double cosine = d0[0] * d1[0] + d0[1] * d1[1] + d0[2] * d1[2];
    const double turn_limit = std::min(std::max(20.0, 360.0 * dt), 90.0) * 3.14159265358979 / 180.0;
    const double fov_change = s0 > 0.0 ? std::fabs(s1 / s0 - 1.0) : 0.0;
    std::snprintf(why, why_size, "moved %.2f m (%.2f m off the expected move, limits %.2f/%.2f), direction cosine %.3f, fov change %.1f %%, dt %.1f ms",
        distance, off_expected, still_limit, move_limit, cosine, fov_change * 100.0, dt_ms);
    std::memcpy(g_d5_camera_track.move, move, sizeof(move));
    g_d5_camera_track.move_valid = true;
    return !move_ok || cosine < std::cos(turn_limit) || fov_change > 0.05;
}

// Called once per frame at the scene end: the best bucket becomes this frame's VP.
void D5FinishViewProj()
{
    if (g_d5_vp_frame == g_d5_frame_index) {
        return;
    }
    int best = -1;
    for (int i = 0; i < g_d5_vp_bucket_count; ++i) {
        if (best < 0 || g_d5_vp_buckets[i].count > g_d5_vp_buckets[best].count) {
            best = i;
        }
    }
    // Last frame's VP is a valid history only if it is from the frame just before this one;
    // after menus, pauses or loading it can be many frames old (the helper then gets a RESET).
    g_d5_vp_previous_valid = g_d5_vp_valid && g_d5_vp_frame == g_d5_frame_index - 1;
    if (g_d5_vp_valid) {
        std::memcpy(g_d5_vp_previous, g_d5_vp_current, sizeof(g_d5_vp_previous));
    }
    const double now_ms = D5NowMs();
    const double dt_ms = g_d5_camera_track.time_ms > 0.0 ? now_ms - g_d5_camera_track.time_ms : 0.0;
    g_d5_scene_dt_ms = g_d5_vp_frame == g_d5_frame_index - 1 ? static_cast<float>(dt_ms) : 0.0f;
    g_d5_camera_track.time_ms = now_ms;
    if (best >= 0 && g_d5_vp_buckets[best].count >= 3) {
        std::memcpy(g_d5_vp_current, g_d5_vp_buckets[best].vp, sizeof(g_d5_vp_current));
        g_d5_vp_valid = true;
        char why[192] = {};
        if (!g_d5_vp_previous_valid) {
            g_d5_camera_track.move_valid = false;
        } else if (D5IsCameraCut(g_d5_vp_previous, g_d5_vp_current, dt_ms, why, sizeof(why))) {
            g_d5_vp_previous_valid = false;   // no history across a cut: zero motion and RESET
            if (g_d5_trace_frame != nullptr) {
                Log("DLSS5  camera cut at frame %ld: %s", static_cast<long>(g_d5_frame_index), why);
            }
        } else if (g_d5_trace_frame != nullptr) {
            Log("DLSS5  camera frame %ld: %s", static_cast<long>(g_d5_frame_index), why);
        }
        g_d5_camera_track.dt_ms = dt_ms;
    } else {
        g_d5_vp_valid = false;
        g_d5_camera_track.move_valid = false;
    }
    g_d5_vp_frame = g_d5_frame_index;
    if (g_d5_trace_frame != nullptr) {
        char counts[96] = {};
        size_t used = 0;
        for (int i = 0; i < g_d5_vp_bucket_count && used + 8 < sizeof(counts); ++i) {
            if (g_d5_vp_buckets[i].count >= 3) {
                used += std::snprintf(counts + used, sizeof(counts) - used, " %d", g_d5_vp_buckets[i].count);
            }
        }
        Log("DLSS5  view-proj frame %ld from %d draws, %d buckets, those of 3+:%s valid=%d", static_cast<long>(g_d5_frame_index),
            g_d5_vp_draws_tried, g_d5_vp_bucket_count, counts, g_d5_vp_valid ? 1 : 0);
        D5LogMatrix("view-proj", g_d5_vp_current);
        int second = -1;
        for (int i = 0; i < g_d5_vp_bucket_count; ++i) {
            if (i != best && (second < 0 || g_d5_vp_buckets[i].count > g_d5_vp_buckets[second].count)) {
                second = i;
            }
        }
        if (second >= 0 && g_d5_vp_buckets[second].count >= 20) {
            Log("DLSS5  second view-proj (%d draws):", g_d5_vp_buckets[second].count);
            D5LogMatrix("view-proj-2", g_d5_vp_buckets[second].vp);
        }
        if (g_d5_vp_valid) {
            D5AnalyseObjectMotion();
        }
    }
}

void D5ResetViewProjFrame()
{
    g_d5_vp_last_bucket = 0;
    g_d5_vp_evict_cursor = 0;
    g_d5_vp_bucket_count = 0;
    g_d5_vp_draws_tried = 0;
}

// ---------------------------------------------------------------------------------------
// Hook bodies.

bool D5IsScenePass()
{
    return g_d5_scene_rt != nullptr && g_d5.rt0 == g_d5_scene_rt;
}

void D5BeforeDraw(IDirect3DDevice9* device, UINT prims, UINT vertices)
{
    ++g_d5_pass_draws;
    if (D5IsScenePass()) {
        ++g_d5_scene_draws_this_frame;
        const int bucket = D5AddViewProjCandidate();
        if (g_d5_helper && g_d5_object_motion && !g_d5_scene_done) {
            D5TagSceneDraw(device);   // not after the scene end: the scene target can be re-entered
        }
        if (g_d5_trace_frame != nullptr) {
            D5RecordObjectDraw();
            // Draws whose c0-c6 do not give a perspective VP: dump c0-c19 (other constant layouts).
            float vp[16] = {};
            static int layout_lines = 0;
            static std::unordered_map<DWORD, int> layout_per_shader;
            const DWORD layout_vs = D5ShaderHash(g_d5.vs);
            if (g_d5_trace_pair_step == 2 && layout_lines < 400 && layout_per_shader[layout_vs] < 3 &&
                !(D5SolveViewProj(g_d5.vs_c, vp) && D5LooksLikeViewProj(vp))) {
                ++layout_lines;
                ++layout_per_shader[layout_vs];
                static char text[256 * 48 + 128] = {};
                size_t used = std::snprintf(text, sizeof(text), "DLSS5  layout vs=%08lX prims=%u:", static_cast<unsigned long>(layout_vs), prims);
                for (int reg = 0; reg < 256 && used < sizeof(text) - 64; ++reg) {
                    const float* v = g_d5.vs_c[reg].v;
                    used += std::snprintf(text + used, sizeof(text) - used, " c%d=%.6g,%.6g,%.6g,%.6g", reg, v[0], v[1], v[2], v[3]);
                }
                Log("%s", text);
            }
        }
        if (g_d5_trace_frame != nullptr && g_d5_scene_draws_this_frame <= 150 &&
            g_d5_trace_frame->draw_line_count < kD5MaxDrawLines) {
            const D5Constant* c = g_d5.vs_c;
            std::snprintf(g_d5_trace_frame->draw_lines[g_d5_trace_frame->draw_line_count++], sizeof(g_d5_trace_frame->draw_lines[0]),
                "scene d%d vs=%08lX vote=%d/%d c0=%.3f,%.3f,%.3f,%.1f c3=%.3f,%.3f,%.3f,%.1f c4=%.3f,%.3f,%.3f,%.1f c6=%.3f,%.3f,%.3f,%.1f",
                g_d5_scene_draws_this_frame, static_cast<unsigned long>(D5ShaderHash(g_d5.vs)), bucket, g_d5_last_vote_kind,
                c[0].v[0], c[0].v[1], c[0].v[2], c[0].v[3], c[3].v[0], c[3].v[1], c[3].v[2], c[3].v[3],
                c[4].v[0], c[4].v[1], c[4].v[2], c[4].v[3], c[6].v[0], c[6].v[1], c[6].v[2], c[6].v[3]);
        }
    } else if (g_d5_scene_done && !g_d5_pre_hud_fired && g_d5_pass_draws == 2 &&
               g_d5_pass_target_set == static_cast<DWORD>(g_d5_pre_hud_target_set)) {
        // The composite + HUD pass: draw 1 is the full-screen composite of the post chain,
        // the HUD starts with draw 2.
        g_d5_pre_hud_fired = true;
        if (g_d5_trace_frame != nullptr && g_d5_trace_dump && g_d5_trace_pair_step != 0) {
            D5DumpSurface(device, g_d5.rt0, "pre-hud");
        }
        if (g_d5_export) {
            D5Publish(device, DSFE5_POINT_PRE_HUD);
            D5BridgeFrame(device);
            if (g_d5_trace_frame != nullptr && g_d5_trace_dump && g_d5_trace_pair_step != 0 && g_d5_helper) {
                D5DumpSurface(device, g_d5.rt0, "pre-hud-dlss5");
            }
        }
    }

    D5FrameTrace* t = g_d5_trace_frame;
    if (t == nullptr) {
        return;
    }
    D5Pass* pass = D5CurrentPass();
    ++pass->draws;
    pass->prims += prims;
    if (g_d5.blend != 0) {
        ++pass->blend_draws;
    }
    if (g_d5.stencil != 0) {
        ++pass->stencil_draws;
        if (t->draw_line_count < kD5MaxDrawLines && pass->rt == g_d5_scene_rt) {
            const DWORD* st = g_d5.stencil_state;
            std::snprintf(t->draw_lines[t->draw_line_count++], sizeof(t->draw_lines[0]),
                "stencil scene d%d vs=%08lX ps=%08lX prims=%u z=%lu/%lu blend=%lu func=%lu ref=%lu mask=%lX wmask=%lX pass=%lu zfail=%lu fail=%lu two=%lu",
                pass->draws, static_cast<unsigned long>(D5ShaderHash(g_d5.vs)), static_cast<unsigned long>(D5ShaderHash(g_d5.ps)), prims,
                static_cast<unsigned long>(g_d5.zenable), static_cast<unsigned long>(g_d5.zwrite), static_cast<unsigned long>(g_d5.blend),
                static_cast<unsigned long>(st[0]), static_cast<unsigned long>(st[1]), static_cast<unsigned long>(st[2]),
                static_cast<unsigned long>(st[3]), static_cast<unsigned long>(st[4]), static_cast<unsigned long>(st[5]),
                static_cast<unsigned long>(st[6]), static_cast<unsigned long>(st[7]));
        }
    }
    if (g_d5.zenable != 0) {
        ++pass->z_draws;
    }

    char reads[160] = {};
    for (int stage = 0; stage < 16; ++stage) {
        D5Resource r = {};
        if (g_d5.bound[stage] != nullptr && D5Lookup(g_d5.bound[stage], &r)) {
            char tag[24] = {};
            std::snprintf(tag, sizeof(tag), " %c#%d@s%d", r.kind, r.id, stage);
            D5AppendTag(pass->reads, sizeof(pass->reads), tag);
            if (std::strlen(reads) + std::strlen(tag) + 1 < sizeof(reads)) {
                std::strncat(reads, tag, sizeof(reads) - std::strlen(reads) - 1);
            }
        }
    }
    const DWORD ps_hash = D5ShaderHash(g_d5.ps);
    char ps_tag[16] = {};
    std::snprintf(ps_tag, sizeof(ps_tag), " %08lX", static_cast<unsigned long>(ps_hash));
    D5AppendTag(pass->shaders, sizeof(pass->shaders), ps_tag);

    // Per-draw lines outside the big pass (the scene's draws are summarised by constants).
    const bool big_pass = pass->rt == g_d5_scene_rt && g_d5_scene_rt != nullptr;
    if (!big_pass && t->draw_line_count < kD5MaxDrawLines) {
        int other = 0;
        for (int stage = 0; stage < 16; ++stage) {
            D5Resource r = {};
            if (g_d5.bound[stage] != nullptr && !D5Lookup(g_d5.bound[stage], &r)) {
                ++other;
            }
        }
        std::snprintf(t->draw_lines[t->draw_line_count++], sizeof(t->draw_lines[0]),
            "p%d d%d prims=%u verts=%u vs=%08lX ps=%08lX blend=%lu z=%lu/%lu tex:%s +%d other c0=%.3f,%.3f,%.3f,%.3f",
            t->pass_count - 1, pass->draws, prims, vertices,
            static_cast<unsigned long>(D5ShaderHash(g_d5.vs)), static_cast<unsigned long>(ps_hash),
            static_cast<unsigned long>(g_d5.blend), static_cast<unsigned long>(g_d5.zenable), static_cast<unsigned long>(g_d5.zwrite),
            reads, other,
            static_cast<double>(g_d5.vs_c[0].v[0]), static_cast<double>(g_d5.vs_c[0].v[1]),
            static_cast<double>(g_d5.vs_c[0].v[2]), static_cast<double>(g_d5.vs_c[0].v[3]));
    }

    // Final-size LDR passes after the scene: dump after the first few draws to see where the
    // HUD starts.
    (void)device;
}

void D5AfterDraw(IDirect3DDevice9* device)
{
    D5FrameTrace* t = g_d5_trace_frame;
    if (t == nullptr || !g_d5_trace_dump || g_d5_trace_pair_step != 1 || !g_d5_scene_done) {
        return;
    }
    D5Pass* pass = D5CurrentPass();
    D5Resource r = {};
    if (!D5Lookup(pass->rt, &r)) {
        D3DSURFACE_DESC desc = {};
        if (pass->rt == nullptr || FAILED(static_cast<IDirect3DSurface9*>(const_cast<void*>(pass->rt))->GetDesc(&desc))) {
            return;
        }
        r.width = desc.Width;
        r.height = desc.Height;
    }
    if (r.width != g_backbuffer_width || r.height != g_backbuffer_height) {
        return;
    }
    const int d = pass->draws;
    if (d <= 4 || d == 6 || d == 8 || d == 12 || d == 16 || d == 24 || d == 32 || d == 48) {
        char label[48] = {};
        std::snprintf(label, sizeof(label), "p%02d-after-d%02d", t->pass_count - 1, d);
        D5DumpSurface(device, pass->rt, label);
    }
}

void D5LeavePass(IDirect3DDevice9* device)
{
    // Called before the render target changes, a StretchRect, or Present.
    if (g_d5_scene_rt != nullptr && g_d5.rt0 == g_d5_scene_rt && g_d5_scene_draws_this_frame > 0 && !g_d5_scene_done) {
        D5FrameTrace* t = g_d5_trace_frame;
        // The scene target can be left and re-entered (StretchRect copies, small passes); the
        // export treats the first exit after the scene's draws as the scene end.
        g_d5_scene_done = true;
        g_d5_post_pass_index = 0;
        if (g_d5_helper && g_d5_object_motion) {
            D5EndSceneTagging(device);
        }
        D5FinishViewProj();
        if (g_d5_helper && g_d5_object_motion) {
            D5FinishObjects();
        }
        if (t != nullptr && g_d5_trace_dump && g_d5_trace_pair_step == 1) {
            D5DumpSurface(device, g_d5_scene_rt, "scene-end");
        }
        if (g_d5_export) {
            D5Publish(device, DSFE5_POINT_SCENE_END);
        }
    }
}

void D5NewPass(DWORD caller)
{
    if (g_d5_scene_done) {
        ++g_d5_post_pass_index;
    }
    D5FrameTrace* t = g_d5_trace_frame;
    if (t == nullptr || t->pass_count >= kD5MaxPasses) {
        return;
    }
    D5Pass& pass = t->passes[t->pass_count++];
    pass = D5Pass{};
    pass.rt = g_d5.rt0;
    pass.ds = g_d5.ds;
    pass.caller = caller;
    pass.target_set = D5CurrentTargetSet(&pass.layer);
}

void D5LogTrace(const D5FrameTrace* t, const D5FrameTrace* previous)
{
    Log("DLSS5 trace frame %ld: %d passes, scene rt draws=%d", static_cast<long>(g_d5_frame_index), t->pass_count,
        g_d5_scene_draws_this_frame);
    for (int i = 0; i < t->pass_count; ++i) {
        const D5Pass& p = t->passes[i];
        char rt[64] = {};
        char ds[64] = {};
        D5Describe(p.rt, rt, sizeof(rt));
        D5Describe(p.ds, ds, sizeof(ds));
        if (p.stretch) {
            Log("DLSS5  %2d StretchRect %s -> %s caller=0x%08lX", i, ds, rt, static_cast<unsigned long>(p.caller));
            continue;
        }
        if (p.draws == 0 && p.clears == 0) {
            continue;
        }
        Log("DLSS5  %2d rt=%s ds=%s caller=0x%08lX set=%lu/%lu vp=%lu,%lu %lux%lu clear=%lu draws=%d prims=%u blend=%d z=%d stencil=%d reads:%s ps:%s",
            i, rt, ds, static_cast<unsigned long>(p.caller), static_cast<unsigned long>(p.target_set), static_cast<unsigned long>(p.layer), p.vp.X, p.vp.Y, p.vp.Width, p.vp.Height,
            static_cast<unsigned long>(p.clears), p.draws, p.prims, p.blend_draws, p.z_draws, p.stencil_draws, p.reads, p.shaders);
    }

    // Vertex constants in the scene pass: registers written, how often, and whether the value
    // changed during the pass. Per-frame constants (written, never changed) are printed whole.
    char line[512] = {};
    size_t used = 0;
    for (int reg = 0; reg < 256; ++reg) {
        if (!t->seen[reg]) {
            continue;
        }
        used += std::snprintf(line + used, sizeof(line) - used, " c%d:%d/%d", reg, t->writes[reg], t->changes[reg]);
        if (used > 400) {
            Log("DLSS5  vs constants (writes/changes):%s", line);
            used = 0;
            line[0] = '\0';
        }
    }
    if (used > 0) {
        Log("DLSS5  vs constants (writes/changes):%s", line);
    }
    for (int reg = 0; reg < 256; ++reg) {
        if (!t->seen[reg]) {
            continue;
        }
        const D5Constant& f = t->first[reg];
        const D5Constant& l = t->last[reg];
        Log("DLSS5  c%-3d first %.5f %.5f %.5f %.5f last %.5f %.5f %.5f %.5f%s", reg,
            static_cast<double>(f.v[0]), static_cast<double>(f.v[1]), static_cast<double>(f.v[2]), static_cast<double>(f.v[3]),
            static_cast<double>(l.v[0]), static_cast<double>(l.v[1]), static_cast<double>(l.v[2]), static_cast<double>(l.v[3]),
            t->changes[reg] == 0 ? " (per frame)" : "");
    }

    // A register whose value this frame equals another register's value last frame: a
    // previous-frame matrix uploaded for motion effects.
    if (previous != nullptr) {
        for (int reg = 0; reg < 256; ++reg) {
            if (!t->seen[reg] || t->changes[reg] != 0) {
                continue;
            }
            for (int old = 0; old < 256; ++old) {
                if (old == reg || !previous->seen[old] || previous->changes[old] != 0) {
                    continue;
                }
                if (std::memcmp(t->first[reg].v, previous->first[old].v, sizeof(float) * 4) == 0 &&
                    std::memcmp(t->first[reg].v, previous->first[reg].v, sizeof(float) * 4) != 0) {
                    Log("DLSS5  c%d this frame == c%d last frame", reg, old);
                }
            }
        }
    }

    for (int i = 0; i < t->draw_line_count; ++i) {
        Log("DLSS5   %s", t->draw_lines[i]);
    }
}

// Scans the game's writable memory for four consecutive floats close to `row` (the solved
// view-projection's first row or column), to find where the game keeps its camera matrices.
bool D5Near(float a, float b)
{
    return std::fabs(a - b) <= 2e-3f * std::max(1.0f, std::fabs(b));
}

void D5ScanMemoryFor(const float* row, const char* label)
{
    const ModuleRange exe = GetExeRange();
    int hits = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    for (auto* address = reinterpret_cast<std::uint8_t*>(0x10000);
         address < reinterpret_cast<std::uint8_t*>(0x7FFE0000) && hits < 24;
         address = static_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const DWORD protect = mbi.Protect & 0xFF;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || mbi.Type == MEM_MAPPED ||
            !(protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY)) {
            continue;
        }
        const auto* begin = static_cast<const float*>(mbi.BaseAddress);
        const size_t count = mbi.RegionSize / sizeof(float);
        for (size_t i = 0; i + 16 <= count && hits < 24; ++i) {
            const float* p = begin + i;
            if (!D5Near(p[0], row[0]) || !D5Near(p[1], row[1]) || !D5Near(p[2], row[2]) || !D5Near(p[3], row[3])) {
                continue;
            }
            const auto* byte = reinterpret_cast<const std::uint8_t*>(p);
            const bool in_exe = byte >= exe.base && byte < exe.base + exe.size;
            Log("DLSS5  scan %s: %p%s next %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f", label, p,
                in_exe ? " (Driver.exe image)" : "",
                static_cast<double>(p[4]), static_cast<double>(p[5]), static_cast<double>(p[6]), static_cast<double>(p[7]),
                static_cast<double>(p[8]), static_cast<double>(p[9]), static_cast<double>(p[10]), static_cast<double>(p[11]),
                static_cast<double>(p[12]), static_cast<double>(p[13]), static_cast<double>(p[14]), static_cast<double>(p[15]));
            ++hits;
        }
    }
    Log("DLSS5  scan %s: %d hits for %.5f %.5f %.5f %.5f", label, hits, static_cast<double>(row[0]), static_cast<double>(row[1]),
        static_cast<double>(row[2]), static_cast<double>(row[3]));
}

void D5FrameEnd(IDirect3DDevice9* device)
{
    D5LeavePass(device);

    // The scene target for the next frame: this frame's busiest pass with a depth surface.
    if (g_d5_frame_best_rt != nullptr && g_d5_frame_best_draws >= g_d5_scene_pass_draws_threshold) {
        if (g_d5_frame_best_rt != g_d5_scene_rt) {
            char rt[64] = {};
            char ds[64] = {};
            D5Describe(g_d5_frame_best_rt, rt, sizeof(rt));
            D5Describe(g_d5_frame_best_ds, ds, sizeof(ds));
            Log("DLSS5: scene target %s depth %s (%d draws, frame %ld)", rt, ds, g_d5_frame_best_draws,
                static_cast<long>(g_d5_frame_index));
        }
        g_d5_scene_rt = g_d5_frame_best_rt;
        g_d5_scene_ds = g_d5_frame_best_ds;
    }

    D5FrameTrace* t = g_d5_trace_frame;
    if (t != nullptr) {
        D5LogTrace(t, g_d5_trace_pair_step == 2 ? g_d5_prev_trace : nullptr);
        if (g_d5_trace_pair_step == 1 && g_d5_traces_done < 2) {
            // Where does the game keep this frame's view-projection? (first two pairs only)
            if (g_d5_vp_valid && g_d5_trace_scan) {
                const float* m = g_d5_vp_current;
                const float column[4] = {m[0], m[4], m[8], m[12]};
                D5ScanMemoryFor(m, "view-proj row0");
                D5ScanMemoryFor(column, "view-proj column0");
            }
        }
        if (g_d5_trace_dump && g_d5_scene_done) {
            ++g_d5_dump_frames;
        }
        g_d5_prev_trace = t;
        g_d5_trace_frame = nullptr;
        if (g_d5_trace_pair_step == 1) {
            g_d5_trace_pair_step = 2;   // trace the next frame too
        } else {
            g_d5_trace_pair_step = 0;
            ++g_d5_traces_done;
        }
    }

    D5FrameTimeTick();
    InterlockedIncrement(&g_d5_frame_index);
    g_d5_frame_best_rt = nullptr;
    g_d5_frame_best_ds = nullptr;
    g_d5_frame_best_draws = 0;
    g_d5_scene_draws_this_frame = 0;
    if (g_d5_scene_done && !g_d5_pre_hud_fired && g_d5_shared != nullptr) {
        g_d5_shared->pre_hud_missed += 1;
    }
    g_d5_scene_done = false;
    g_d5_pre_hud_fired = false;
    g_d5_post_pass_index = 0;
    g_d5_pass_draws = 0;
    D5ResetViewProjFrame();
    D5EndSceneTagging(device);   // in case the scene end was never seen this frame
    D5ResetObjectsFrame();

    if (g_d5_trace && g_d5_traces_done < g_d5_trace_count) {
        const DWORD now = GetTickCount();
        bool start = g_d5_trace_pair_step == 2;
        if (g_d5_trace_pair_step == 0 && now >= g_d5_next_trace_tick &&
            now - g_d5_device_tick >= static_cast<DWORD>(g_d5_trace_start_seconds) * 1000) {
            g_d5_trace_pair_step = 1;
            g_d5_next_trace_tick = now + static_cast<DWORD>(g_d5_trace_interval_seconds) * 1000;
            start = true;
        }
        if (start) {
            D5FrameTrace* next = g_d5_prev_trace == &g_d5_trace_buffers[0] ? &g_d5_trace_buffers[1] : &g_d5_trace_buffers[0];
            std::memset(static_cast<void*>(next), 0, sizeof(*next));
            g_d5_trace_frame = next;
        }
    }

    if (g_d5_shared != nullptr) {
        g_d5_shared->present_count += 1;
    }
}

// Device Present and swap-chain Present both end a frame; a runtime whose device Present calls
// its own swap chain's Present (DXVK) must count once.
volatile LONG g_d5_in_present = 0;

HRESULT STDMETHODCALLTYPE D5Present(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty)
{
    const bool outer = InterlockedIncrement(&g_d5_in_present) == 1;
    if (outer) {
        D5FrameEnd(device);
    }
    const HRESULT hr = g_d5_present(device, src, dst, window, dirty);
    InterlockedDecrement(&g_d5_in_present);
    return hr;
}

HRESULT STDMETHODCALLTYPE D5SwapPresent(IDirect3DSwapChain9* chain, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty, DWORD flags)
{
    const bool outer = InterlockedIncrement(&g_d5_in_present) == 1;
    IDirect3DDevice9* device = nullptr;
    if (outer && SUCCEEDED(chain->GetDevice(&device)) && device != nullptr) {
        D5FrameEnd(device);
        device->Release();
    }
    const HRESULT hr = g_d5_swap_present(chain, src, dst, window, dirty, flags);
    InterlockedDecrement(&g_d5_in_present);
    return hr;
}

HRESULT STDMETHODCALLTYPE D5CreateTexture(IDirect3DDevice9* device, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared)
{
    const HRESULT hr = g_d5_create_texture(device, w, h, levels, usage, format, pool, out, shared);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0) {
        D5Register(*out, 'T', w, h, format, usage, D5Va(_ReturnAddress()));
        IDirect3DSurface9* surface = nullptr;
        if (SUCCEEDED((*out)->GetSurfaceLevel(0, &surface)) && surface != nullptr) {
            AcquireSRWLockExclusive(&g_d5_resource_lock);
            g_d5_surface_owner[surface] = *out;
            ReleaseSRWLockExclusive(&g_d5_resource_lock);
            surface->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D5CreateRenderTarget(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
    DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared)
{
    const HRESULT hr = g_d5_create_render_target(device, w, h, format, ms, quality, lockable, out, shared);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        D5Register(*out, 'S', w, h, format, static_cast<DWORD>(ms), D5Va(_ReturnAddress()));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D5CreateDepthStencil(IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
    DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* shared)
{
    if (g_d5_helper && out != nullptr && ms == D3DMULTISAMPLE_NONE && w == g_backbuffer_width && h == g_backbuffer_height &&
        (format == D3DFMT_D24S8 || format == D3DFMT_D24X8) && D5CreateIntzDepth(device, w, h, out)) {
        D5Register(*out, 'D', w, h, format, static_cast<DWORD>(ms), D5Va(_ReturnAddress()));
        Log("DLSS5: depth surface %ux%u created as INTZ (readable scene depth)", w, h);
        return D3D_OK;
    }
    const HRESULT hr = g_d5_create_depth_stencil(device, w, h, format, ms, quality, discard, out, shared);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        D5Register(*out, 'D', w, h, format, static_cast<DWORD>(ms), D5Va(_ReturnAddress()));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D5StretchRect(IDirect3DDevice9* device, IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst,
    const RECT* dst_rect, D3DTEXTUREFILTERTYPE filter)
{
    if (g_d5_internal) {
        return g_d5_stretch_rect(device, src, src_rect, dst, dst_rect, filter);
    }
    D5FrameTrace* t = g_d5_trace_frame;
    if (t != nullptr && t->pass_count < kD5MaxPasses) {
        D5Pass& pass = t->passes[t->pass_count++];
        pass = D5Pass{};
        pass.rt = dst;
        pass.ds = src;
        pass.caller = D5Va(_ReturnAddress());
        pass.stretch = true;
        // Draws after a StretchRect still belong to the current target: reopen it.
        if (t->pass_count < kD5MaxPasses) {
            D5Pass& resume = t->passes[t->pass_count++];
            resume = D5Pass{};
            resume.rt = g_d5.rt0;
            resume.ds = g_d5.ds;
        }
    }
    return g_d5_stretch_rect(device, src, src_rect, dst, dst_rect, filter);
}

HRESULT STDMETHODCALLTYPE D5SetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
{
    if (g_d5_internal) {
        return g_d5_set_render_target(device, index, surface);
    }
    if (index == 0) {
        g_d5_pass_draws = 0;
        g_d5_pass_target_set = D5CurrentTargetSet(nullptr);
    }
    if (index == 0 && surface != g_d5.rt0) {
        D5LeavePass(device);
        g_d5.rt0 = surface;
        D5NewPass(D5Va(_ReturnAddress()));
    } else if (index == 0) {
        D5NewPass(D5Va(_ReturnAddress()));
    }
    return g_d5_set_render_target(device, index, surface);
}

HRESULT STDMETHODCALLTYPE D5SetDepthStencil(IDirect3DDevice9* device, IDirect3DSurface9* surface)
{
    if (g_d5_internal) {
        return g_d5_set_depth_stencil(device, surface);
    }
    g_d5.ds = surface;
    D5Pass* pass = D5CurrentPass();
    if (pass != nullptr && pass->draws == 0) {
        pass->ds = surface;
    }
    return g_d5_set_depth_stencil(device, surface);
}

HRESULT STDMETHODCALLTYPE D5Clear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil)
{
    if (g_d5_internal) {
        return g_d5_clear(device, count, rects, flags, colour, z, stencil);
    }
    D5Pass* pass = D5CurrentPass();
    if (pass != nullptr) {
        pass->clears |= flags;
    }
    return g_d5_clear(device, count, rects, flags, colour, z, stencil);
}

HRESULT STDMETHODCALLTYPE D5SetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* vp)
{
    if (g_d5_internal) {
        return g_d5_set_viewport(device, vp);
    }
    D5Pass* pass = D5CurrentPass();
    if (pass != nullptr && vp != nullptr) {
        pass->vp = *vp;
    }
    return g_d5_set_viewport(device, vp);
}

HRESULT STDMETHODCALLTYPE D5SetRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value)
{
    if (g_d5_internal) {
        return g_d5_set_render_state(device, state, value);
    }
    if (state == D3DRS_ALPHABLENDENABLE) {
        g_d5.blend = value;
    } else if (state == D3DRS_ZENABLE) {
        g_d5.zenable = value;
    }
    if (state >= D3DRS_STENCILENABLE && state <= D3DRS_STENCILWRITEMASK) {
        D5NoteGameStencilState(state, value);
    }
    if (state == D3DRS_STENCILENABLE) {
        g_d5.stencil = value;
    } else if (state == D3DRS_STENCILFUNC) {
        g_d5.stencil_state[0] = value;
    } else if (state == D3DRS_STENCILREF) {
        g_d5.stencil_state[1] = value;
    } else if (state == D3DRS_STENCILMASK) {
        g_d5.stencil_state[2] = value;
    } else if (state == D3DRS_STENCILWRITEMASK) {
        g_d5.stencil_state[3] = value;
    } else if (state == D3DRS_STENCILPASS) {
        g_d5.stencil_state[4] = value;
    } else if (state == D3DRS_STENCILZFAIL) {
        g_d5.stencil_state[5] = value;
    } else if (state == D3DRS_STENCILFAIL) {
        g_d5.stencil_state[6] = value;
    } else if (state == D3DRS_TWOSIDEDSTENCILMODE) {
        g_d5.stencil_state[7] = value;
    } else if (state == D3DRS_ZWRITEENABLE) {
        g_d5.zwrite = value;
    }
    return g_d5_set_render_state(device, state, value);
}

HRESULT STDMETHODCALLTYPE D5SetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (g_d5_internal) {
        return g_d5_set_texture(device, stage, texture);
    }
    if (stage < 16) {
        g_d5.bound[stage] = texture;
    }
    return g_d5_set_texture(device, stage, texture);
}

// The scene candidate: the busiest render target this frame that draws with a depth surface and
// covers the back buffer (the distant-scene strips, 128x832, can draw more than the scene).
const void* g_d5_counting_rt = nullptr;
int g_d5_counting_draws = 0;
LONG g_d5_counting_frame = -1;
bool g_d5_counting_full = false;

void D5NoteBusiestPass()
{
    const void*& counting_rt = g_d5_counting_rt;
    int& counting_draws = g_d5_counting_draws;
    LONG& counting_frame = g_d5_counting_frame;
    bool& counting_full = g_d5_counting_full;
    if (counting_frame != g_d5_frame_index || counting_rt != g_d5.rt0) {
        counting_frame = g_d5_frame_index;
        counting_rt = g_d5.rt0;
        counting_draws = 0;
        D3DSURFACE_DESC desc = {};
        counting_full = g_d5.rt0 != nullptr &&
            SUCCEEDED(static_cast<IDirect3DSurface9*>(const_cast<void*>(g_d5.rt0))->GetDesc(&desc)) &&
            desc.Width * 2 >= g_backbuffer_width && desc.Height * 2 >= g_backbuffer_height;
    }
    ++counting_draws;
    if (counting_full && g_d5.ds != nullptr && counting_draws > g_d5_frame_best_draws) {
        g_d5_frame_best_draws = counting_draws;
        g_d5_frame_best_rt = g_d5.rt0;
        g_d5_frame_best_ds = g_d5.ds;
    }
}

HRESULT STDMETHODCALLTYPE D5DrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT prims)
{
    if (g_d5_internal) {
        return g_d5_draw_primitive(device, type, start, prims);
    }
    D5NoteBusiestPass();
    g_d5_draw_key = g_d5_trace_frame != nullptr ? D5DrawKey(start, prims, 0, 1) : 0;
    D5BeforeDraw(device, prims, 0);
    const HRESULT hr = g_d5_draw_primitive(device, type, start, prims);
    D5AfterDraw(device);
    return hr;
}

HRESULT STDMETHODCALLTYPE D5DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT min_index, UINT vertices,
    UINT start, UINT prims)
{
    if (g_d5_internal) {
        return g_d5_draw_indexed_primitive(device, type, base, min_index, vertices, start, prims);
    }
    D5NoteBusiestPass();
    g_d5_draw_key = g_d5_trace_frame != nullptr ? D5DrawKey(static_cast<DWORD>(base), start, prims, vertices) : 0;
    D5BeforeDraw(device, prims, vertices);
    const HRESULT hr = g_d5_draw_indexed_primitive(device, type, base, min_index, vertices, start, prims);
    D5AfterDraw(device);
    return hr;
}

HRESULT STDMETHODCALLTYPE D5DrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride)
{
    if (g_d5_internal) {
        return g_d5_draw_primitive_up(device, type, prims, data, stride);
    }
    D5NoteBusiestPass();
    g_d5_draw_key = g_d5_trace_frame != nullptr ? D5DrawKey(prims, stride, 0, 2) : 0;
    D5BeforeDraw(device, prims, 0);
    const HRESULT hr = g_d5_draw_primitive_up(device, type, prims, data, stride);
    D5AfterDraw(device);
    return hr;
}

HRESULT STDMETHODCALLTYPE D5DrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT min_index, UINT vertices,
    UINT prims, const void* indices, D3DFORMAT index_format, const void* data, UINT stride)
{
    if (g_d5_internal) {
        return g_d5_draw_indexed_primitive_up(device, type, min_index, vertices, prims, indices, index_format, data, stride);
    }
    D5NoteBusiestPass();
    g_d5_draw_key = g_d5_trace_frame != nullptr ? D5DrawKey(prims, vertices, stride, 3) : 0;
    D5BeforeDraw(device, prims, vertices);
    const HRESULT hr = g_d5_draw_indexed_primitive_up(device, type, min_index, vertices, prims, indices, index_format, data, stride);
    D5AfterDraw(device);
    return hr;
}

// TraceShaders=1: vs_<hash>.asm in TraceDumpDir, the shader's disassembly (D3DDisassemble).
void D5DumpShader(const DWORD* function, DWORD hash, const char* prefix)
{
    using D3DDisassembleFn = HRESULT (WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
    static D3DDisassembleFn disassemble = nullptr;
    if (disassemble == nullptr) {
        HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
        disassemble = compiler != nullptr ? reinterpret_cast<D3DDisassembleFn>(GetProcAddress(compiler, "D3DDisassemble")) : nullptr;
        if (disassemble == nullptr) {
            return;
        }
    }
    size_t tokens = 0;
    while (tokens < 65536 && function[tokens] != 0x0000FFFF) {
        ++tokens;
    }
    ID3DBlob* text = nullptr;
    if (FAILED(disassemble(function, (tokens + 1) * sizeof(DWORD), 0, nullptr, &text)) || text == nullptr) {
        return;
    }
    CreateDirectoryA(g_d5_dump_dir, nullptr);
    char path[MAX_PATH] = {};
    std::snprintf(path, sizeof(path), "%s\\%s_%08lX.asm", g_d5_dump_dir, prefix, static_cast<unsigned long>(hash));
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (file != nullptr) {
        std::fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() > 0 ? text->GetBufferSize() - 1 : 0, file);
        std::fclose(file);
    }
    text->Release();
}

HRESULT STDMETHODCALLTYPE D5CreateVertexShader(IDirect3DDevice9* device, const DWORD* function, void** out)
{
    const HRESULT hr = g_d5_create_vertex_shader(device, function, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        const DWORD hash = D5HashBytecode(function);
        AcquireSRWLockExclusive(&g_d5_resource_lock);
        g_d5_shader_hash[*out] = hash;
        ReleaseSRWLockExclusive(&g_d5_resource_lock);
        if (g_d5_trace_shaders) {
            D5DumpShader(function, hash, "vs");
        }
        if (g_d5_helper && g_d5_object_motion) {
            D5NoteShader(*out, function);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D5CreatePixelShader(IDirect3DDevice9* device, const DWORD* function, void** out)
{
    const HRESULT hr = g_d5_create_pixel_shader(device, function, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        const DWORD hash = D5HashBytecode(function);
        AcquireSRWLockExclusive(&g_d5_resource_lock);
        g_d5_shader_hash[*out] = hash;
        ReleaseSRWLockExclusive(&g_d5_resource_lock);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D5SetVertexShader(IDirect3DDevice9* device, void* shader)
{
    if (g_d5_internal) {
        return g_d5_set_vertex_shader(device, shader);
    }
    g_d5.vs = shader;
    return g_d5_set_vertex_shader(device, shader);
}

HRESULT STDMETHODCALLTYPE D5SetPixelShader(IDirect3DDevice9* device, void* shader)
{
    if (g_d5_internal) {
        return g_d5_set_pixel_shader(device, shader);
    }
    g_d5.ps = shader;
    return g_d5_set_pixel_shader(device, shader);
}

HRESULT STDMETHODCALLTYPE D5SetVsConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT count)
{
    if (g_d5_internal) {
        return g_d5_set_vs_constant_f(device, start, data, count);
    }
    if (data != nullptr) {
        D5FrameTrace* t = g_d5_trace_frame;
        const bool scene = D5IsScenePass();
        for (UINT i = 0; i < count && start + i < 256; ++i) {
            const UINT reg = start + i;
            D5Constant value = {};
            std::memcpy(value.v, data + i * 4, sizeof(value.v));
            if (t != nullptr && scene) {
                if (!t->seen[reg]) {
                    t->seen[reg] = true;
                    t->first[reg] = value;
                } else if (std::memcmp(t->last[reg].v, value.v, sizeof(value.v)) != 0) {
                    ++t->changes[reg];
                }
                ++t->writes[reg];
                t->last[reg] = value;
            }
            g_d5.vs_c[reg] = value;
        }
    }
    return g_d5_set_vs_constant_f(device, start, data, count);
}

void InstallDlss5Hooks(IDirect3DDevice9* device)
{
    static bool installed = false;
    if (!Dlss5Wanted() || installed || device == nullptr) {
        return;
    }
    installed = true;
    g_d5_device_tick = GetTickCount();
    if (g_d5_export) {
        D5OpenSharedMemory();
    }

    void** vtable = *reinterpret_cast<void***>(device);
    D5Hook(vtable, 17, reinterpret_cast<void*>(&D5Present), reinterpret_cast<void**>(&g_d5_present), "Present");
    D5Hook(vtable, 23, reinterpret_cast<void*>(&D5CreateTexture), reinterpret_cast<void**>(&g_d5_create_texture), "CreateTexture");
    D5Hook(vtable, 28, reinterpret_cast<void*>(&D5CreateRenderTarget), reinterpret_cast<void**>(&g_d5_create_render_target), "CreateRenderTarget");
    D5Hook(vtable, 29, reinterpret_cast<void*>(&D5CreateDepthStencil), reinterpret_cast<void**>(&g_d5_create_depth_stencil), "CreateDepthStencilSurface");
    D5Hook(vtable, 34, reinterpret_cast<void*>(&D5StretchRect), reinterpret_cast<void**>(&g_d5_stretch_rect), "StretchRect");
    D5Hook(vtable, 37, reinterpret_cast<void*>(&D5SetRenderTarget), reinterpret_cast<void**>(&g_d5_set_render_target), "SetRenderTarget");
    D5Hook(vtable, 39, reinterpret_cast<void*>(&D5SetDepthStencil), reinterpret_cast<void**>(&g_d5_set_depth_stencil), "SetDepthStencilSurface");
    D5Hook(vtable, 43, reinterpret_cast<void*>(&D5Clear), reinterpret_cast<void**>(&g_d5_clear), "Clear");
    D5Hook(vtable, 47, reinterpret_cast<void*>(&D5SetViewport), reinterpret_cast<void**>(&g_d5_set_viewport), "SetViewport");
    D5Hook(vtable, 57, reinterpret_cast<void*>(&D5SetRenderState), reinterpret_cast<void**>(&g_d5_set_render_state), "SetRenderState");
    D5Hook(vtable, 65, reinterpret_cast<void*>(&D5SetTexture), reinterpret_cast<void**>(&g_d5_set_texture), "SetTexture");
    D5Hook(vtable, 81, reinterpret_cast<void*>(&D5DrawPrimitive), reinterpret_cast<void**>(&g_d5_draw_primitive), "DrawPrimitive");
    D5Hook(vtable, 82, reinterpret_cast<void*>(&D5DrawIndexedPrimitive), reinterpret_cast<void**>(&g_d5_draw_indexed_primitive), "DrawIndexedPrimitive");
    D5Hook(vtable, 83, reinterpret_cast<void*>(&D5DrawPrimitiveUP), reinterpret_cast<void**>(&g_d5_draw_primitive_up), "DrawPrimitiveUP");
    D5Hook(vtable, 84, reinterpret_cast<void*>(&D5DrawIndexedPrimitiveUP), reinterpret_cast<void**>(&g_d5_draw_indexed_primitive_up), "DrawIndexedPrimitiveUP");
    D5Hook(vtable, 91, reinterpret_cast<void*>(&D5CreateVertexShader), reinterpret_cast<void**>(&g_d5_create_vertex_shader), "CreateVertexShader");
    D5Hook(vtable, 92, reinterpret_cast<void*>(&D5SetVertexShader), reinterpret_cast<void**>(&g_d5_set_vertex_shader), "SetVertexShader");
    D5Hook(vtable, 94, reinterpret_cast<void*>(&D5SetVsConstantF), reinterpret_cast<void**>(&g_d5_set_vs_constant_f), "SetVertexShaderConstantF");
    D5Hook(vtable, 106, reinterpret_cast<void*>(&D5CreatePixelShader), reinterpret_cast<void**>(&g_d5_create_pixel_shader), "CreatePixelShader");
    D5Hook(vtable, 107, reinterpret_cast<void*>(&D5SetPixelShader), reinterpret_cast<void**>(&g_d5_set_pixel_shader), "SetPixelShader");

    IDirect3DSwapChain9* chain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain != nullptr) {
        D5Hook(*reinterpret_cast<void***>(chain), 3, reinterpret_cast<void*>(&D5SwapPresent), reinterpret_cast<void**>(&g_d5_swap_present),
            "IDirect3DSwapChain9::Present");
        chain->Release();
    }

    // The back buffer as created, for the logs.
    IDirect3DSurface9* back = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back != nullptr) {
        char text[64] = {};
        D5Describe(back, text, sizeof(text));
        Log("DLSS5: back buffer %s", text);
        back->Release();
    }
    Log("DLSS5: hooks installed trace=%d dump=%d export=%d wvp=c%d world=c%d pre_hud_set=%d",
        g_d5_trace ? 1 : 0, g_d5_trace_dump ? 1 : 0, g_d5_export ? 1 : 0, g_d5_wvp_register, g_d5_world_register,
        g_d5_pre_hud_target_set);
}

} // namespace

// In-process consumers (a ReShade add-on, the helper's 32-bit side) register here; the
// callback runs on the game's render thread at the scene end and at the pre-HUD point.
extern "C" __declspec(dllexport) int __cdecl DSFE_RegisterPreHudCallback(Dsfe5PreHudCallback callback, void* user)
{
    AcquireSRWLockExclusive(&g_d5_callback_lock);
    for (D5Callback& slot : g_d5_callbacks) {
        if (slot.fn == nullptr) {
            slot = {callback, user};
            ReleaseSRWLockExclusive(&g_d5_callback_lock);
            return 1;
        }
    }
    ReleaseSRWLockExclusive(&g_d5_callback_lock);
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl DSFE_UnregisterPreHudCallback(Dsfe5PreHudCallback callback, void* user)
{
    int removed = 0;
    AcquireSRWLockExclusive(&g_d5_callback_lock);
    for (D5Callback& slot : g_d5_callbacks) {
        if (slot.fn == callback && slot.user == user) {
            slot = {};
            removed = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_d5_callback_lock);
    return removed;
}

namespace {
