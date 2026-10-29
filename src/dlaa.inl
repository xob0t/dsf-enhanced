// DLAA, game side (research/dlaa.md). Everything here is off unless [DLAA] in dsf_enhanced.ini
// turns it on:
//  - Enable: DLAA (DLSS Super Resolution at the render size) through the helper, at the scene
//    end (dlss5_bridge.inl), with the three parts below. DisableMLAA and DisableSwapChainMSAA
//    follow it unless set.
//  - Jitter: moves the scene pass by a sub-pixel Halton (2,3) offset each frame, through vertex
//    shaders patched at creation (vs_jitter_patch.h). The constant J (c255) holds the clip-space
//    offset and is zero outside the scene pass, so shadows, the minimap, post and the HUD stay put,
//    and the game's own constants (the view-projection solve, motion vectors) stay unjittered. The
//    scene's texture LOD bias goes down by MipBias meanwhile. On its own (Jitter=1 without Enable)
//    the picture only shimmers: a debug switch. With Enable the scene is jittered only on frames
//    that follow a frame DLAA anti-aliased, so a failed, stalled or killed helper stops it at once.
//  - DisableMLAA: skips the game's MLAA, the edge pass (target set 24) and the blend-weight pass
//    (set 25), and clears the weights to 0, so the composite's blend copies the frame unchanged.
//    With Enable only on frames DLAA anti-aliased: on any other frame the game's MLAA runs.
//  - DisableSwapChainMSAA: creates the swap chain without the 4x MSAA AntiAliasing=1 asks for, which
//    only holds a StretchRect of the finished frame.
// Split screen (more than one viewport or pass in the scene) falls back to the game's MLAA.
// Every switch needs the DLSS 5 hooks (dlss5_feed.inl); they install them without Export.

bool g_dlaa_enable = false;          // Enable: DLAA through the helper
int g_dlaa_preset = 11;              // DLSS.Hint.Render.Preset.DLAA: K (11) or L (12)
bool g_dlaa_jitter = false;          // the vertex shaders are patched (Jitter, or Enable)
bool g_dlaa_jitter_frame = false;    // this frame's scene is jittered
bool g_dlaa_output_frame = false;    // DLAA's output went back into the scene this frame
int g_dlaa_jitter_phases = 16;
float g_dlaa_jitter_debug_pixels = 0.0f;   // JitterDebugPixels: a constant offset instead of Halton
float g_dlaa_mip_bias = -1.0f;
bool g_dlaa_disable_mlaa = false;
bool g_dlaa_disable_swapchain_msaa = false;

// This frame's jitter in scene pixels, +x right, +y down (the motion vectors' convention): pixel
// (x, y) is sampled at (x + 0.5 - jx, y + 0.5 - jy), the image moves by +j.
float g_dlaa_jx = 0.0f;
float g_dlaa_jy = 0.0f;
LONG g_dlaa_frame = 0;

std::unordered_map<const void*, int> g_dlaa_patched;   // patched vertex shader objects (-> J's register)
int g_dlaa_patched_count = 0;
int g_dlaa_rejected_count = 0;
int g_dlaa_vs_j = -1;                // J register of the bound vertex shader, -1 when not patched
float g_dlaa_j_shadow[4] = {};
bool g_dlaa_j_valid = false;         // the shadow holds what the device has in c255
bool g_dlaa_scene_bias_active = false;
DWORD g_dlaa_game_lod_bias[16] = {}; // MIPMAPLODBIAS as last set outside the scene's bias

// MLAA skip, per pass (set at SetRenderTarget).
bool g_dlaa_skip_pass = false;
bool g_dlaa_clear_pending = false;
bool g_dlaa_mlaa_skipped_last = false;   // for the log: MLAA was skipped in the last MLAA pass

// Split screen: a scene draw under a viewport smaller than the render size, or the scene target
// drawn into again after the scene end, means more than one view. DLAA then stays off (and the
// jitter, and MLAA stays on) until kDlaaSplitHoldFrames frames went by without either.
constexpr int kDlaaSplitHoldFrames = 60;
constexpr int kDlaaSecondPassDraws = 50;   // draws into the scene target after its end that make a second view
bool g_dlaa_viewport_full = true;
D3DVIEWPORT9 g_dlaa_viewport = {};
bool g_dlaa_split_frame = false;
int g_dlaa_second_pass_draws = 0;
int g_dlaa_split_hold = 0;

bool D5DlaaProducing(UINT w, UINT h);

// A [DLAA] switch that Enable sets unless the ini says otherwise: -1 when missing, empty or "auto".
int DlaaReadSwitch(const char* key, const char* ini_path)
{
    char value[16] = {};
    GetPrivateProfileStringA("DLAA", key, "", value, sizeof(value), ini_path);
    if (value[0] == '\0' || _stricmp(value, "auto") == 0) {
        return -1;
    }
    return std::atoi(value) != 0 ? 1 : 0;
}

// Preset: K or L, or NGX's numbers 11 and 12. Anything else is K.
int DlaaReadPreset(const char* ini_path)
{
    char value[16] = {};
    GetPrivateProfileStringA("DLAA", "Preset", "K", value, sizeof(value), ini_path);
    if (_stricmp(value, "L") == 0 || std::atoi(value) == 12) {
        return 12;
    }
    if (_stricmp(value, "K") != 0 && std::atoi(value) != 11) {
        Log("DLAA: Preset=%s is not K or L; K is used", value);
    }
    return 11;
}

// After LoadDlss5Config: Enable turns the helper bridge on (g_d5_helper) without [DLSS5].
void LoadDlaaConfig(const char* ini_path)
{
    g_dlaa_enable = GetPrivateProfileIntA("DLAA", "Enable", 0, ini_path) != 0;
    g_dlaa_preset = DlaaReadPreset(ini_path);
    g_dlaa_jitter = g_dlaa_enable || GetPrivateProfileIntA("DLAA", "Jitter", 0, ini_path) != 0;
    g_dlaa_jitter_frame = !g_dlaa_enable && g_dlaa_jitter;   // Jitter=1 alone: every frame
    g_dlaa_jitter_phases = ClampInt(GetPrivateProfileIntA("DLAA", "JitterPhases", 16, ini_path), 1, 64);
    g_dlaa_jitter_debug_pixels = std::max(-256.0f, std::min(ReadProfileFloat("DLAA", "JitterDebugPixels", 0.0f, ini_path), 256.0f));
    g_dlaa_mip_bias = std::max(-3.0f, std::min(ReadProfileFloat("DLAA", "MipBias", -1.0f, ini_path), 3.0f));
    const int mlaa = DlaaReadSwitch("DisableMLAA", ini_path);
    const int msaa = DlaaReadSwitch("DisableSwapChainMSAA", ini_path);
    g_dlaa_disable_mlaa = mlaa < 0 ? g_dlaa_enable : mlaa == 1;
    g_dlaa_disable_swapchain_msaa = msaa < 0 ? g_dlaa_enable : msaa == 1;
    for (DWORD& bias : g_dlaa_game_lod_bias) {
        bias = FloatBits(g_texture_lod_bias);
    }
    if (g_dlaa_enable) {
        g_d5_helper = true;
    }
    if (g_dlaa_enable || g_dlaa_jitter || g_dlaa_disable_mlaa || g_dlaa_disable_swapchain_msaa) {
        Log("DLAA: Enable=%d Preset=%c Jitter=%d JitterPhases=%d JitterDebugPixels=%.2f MipBias=%.2f DisableMLAA=%d DisableSwapChainMSAA=%d",
            g_dlaa_enable ? 1 : 0, g_dlaa_preset == 12 ? 'L' : 'K', g_dlaa_jitter ? 1 : 0, g_dlaa_jitter_phases,
            static_cast<double>(g_dlaa_jitter_debug_pixels), static_cast<double>(g_dlaa_mip_bias), g_dlaa_disable_mlaa ? 1 : 0,
            g_dlaa_disable_swapchain_msaa ? 1 : 0);
    }
}

bool DlaaWantsDeviceHooks()
{
    return g_dlaa_jitter || g_dlaa_disable_mlaa;
}

// CreateDevice and Reset.
void DlaaApplyPresentation(D3DPRESENT_PARAMETERS* pp, const char* source)
{
    if (!g_dlaa_disable_swapchain_msaa || pp == nullptr) {
        return;
    }
    Log("%s: MultiSampleType %d quality %lu requested, forced to none (DisableSwapChainMSAA)", source,
        static_cast<int>(pp->MultiSampleType), static_cast<unsigned long>(pp->MultiSampleQuality));
    pp->MultiSampleType = D3DMULTISAMPLE_NONE;
    pp->MultiSampleQuality = 0;
}

void DlaaOnReset()
{
    g_dlaa_j_valid = false;
    g_dlaa_scene_bias_active = false;   // Reset puts the sampler states back to their defaults
    for (DWORD& bias : g_dlaa_game_lod_bias) {
        bias = FloatBits(g_texture_lod_bias);
    }
    g_dlaa_frame = 0;   // the Halton cycle starts again
    g_dlaa_skip_pass = false;
    g_dlaa_clear_pending = false;
    if (g_dlaa_enable) {
        // No jitter until DLAA has anti-aliased a frame at the new size.
        g_dlaa_jitter_frame = false;
        g_dlaa_output_frame = false;
    }
    g_dlaa_viewport_full = true;
    g_dlaa_split_frame = false;
    g_dlaa_second_pass_draws = 0;
}

// ---------------------------------------------------------------------------------------
// Jitter.

float DlaaHalton(int index, int base)
{
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

// The bytecode's length is not passed; a malformed shader must not take the game down here.
size_t DlaaTokenCount(const DWORD* function)
{
    __try {
        return vsjitter::TokenCount(reinterpret_cast<const std::uint32_t*>(function), 65536);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// CreateVertexShader: the patched copy, or false to create the original.
bool DlaaPatchedBytecode(const DWORD* function, std::vector<std::uint32_t>* patched, vsjitter::Info* info, DWORD hash)
{
    const size_t count = DlaaTokenCount(function);
    if (count == 0) {
        info->result = vsjitter::kMalformed;
    } else {
        *info = vsjitter::Patch(reinterpret_cast<const std::uint32_t*>(function), count, patched);
    }
    if (info->result == vsjitter::kPatched) {
        return true;
    }
    static std::unordered_map<DWORD, bool> said;
    if (!said[hash]) {
        said[hash] = true;
        ++g_dlaa_rejected_count;
        Log("DLAA: vs %08lX (version %08lX) left unpatched, not jittered: %s (max constant c%d)", static_cast<unsigned long>(hash),
            static_cast<unsigned long>(info->version), vsjitter::ResultName(info->result), info->max_constant);
    }
    return false;
}

void DlaaForgetShader(const void* shader)
{
    AcquireSRWLockExclusive(&g_d5_resource_lock);
    g_dlaa_patched.erase(shader);
    ReleaseSRWLockExclusive(&g_d5_resource_lock);
}

HRESULT DlaaCreateVertexShader(IDirect3DDevice9* device, const DWORD* function, void** out, DWORD hash)
{
    std::vector<std::uint32_t> patched;
    vsjitter::Info info;
    if (out != nullptr && function != nullptr && DlaaPatchedBytecode(function, &patched, &info, hash)) {
        const HRESULT hr = g_d5_create_vertex_shader(device, reinterpret_cast<const DWORD*>(patched.data()), out);
        if (SUCCEEDED(hr) && *out != nullptr) {
            AcquireSRWLockExclusive(&g_d5_resource_lock);
            g_dlaa_patched[*out] = info.jitter_register;
            ++g_dlaa_patched_count;
            ReleaseSRWLockExclusive(&g_d5_resource_lock);
            if (g_d5_trace) {
                Log("DLAA: vs %08lX patched (r%d, c%d, %d position writes, max constant c%d; %d patched, %d left)",
                    static_cast<unsigned long>(hash), info.temp, info.jitter_register, info.position_writes, info.max_constant,
                    g_dlaa_patched_count, g_dlaa_rejected_count);
            }
            return hr;
        }
        Log("DLAA: vs %08lX: the device refused the patched copy (hr=0x%08lX), created unpatched", static_cast<unsigned long>(hash),
            static_cast<unsigned long>(hr));
    }
    const HRESULT hr = g_d5_create_vertex_shader(device, function, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        DlaaForgetShader(*out);   // a released patched shader's address can come back
    }
    return hr;
}

void DlaaSetVertexShader(const void* shader)
{
    int j = -1;
    if (shader != nullptr) {
        AcquireSRWLockShared(&g_d5_resource_lock);
        auto it = g_dlaa_patched.find(shader);
        if (it != g_dlaa_patched.end()) {
            j = it->second;
        }
        ReleaseSRWLockShared(&g_d5_resource_lock);
    }
    g_dlaa_vs_j = j;
}

// SetVertexShaderConstantF from the game: a write over J makes the shadow stale. None of the
// game's vertex shaders reaches past c215 (tests/vs_jitter_test.cpp over its packages), so a write
// to c255 is unexpected: logged once.
void DlaaNoteConstants(UINT start, UINT count)
{
    const UINT j = vsjitter::kJitterRegister;
    if (start <= j && j < start + count) {
        g_dlaa_j_valid = false;
        static bool said = false;
        if (!said) {
            said = true;
            Log("DLAA: the game writes vertex constant c%u (the jitter register), first at c%u+%u", j, start, count);
        }
    }
}

// Scene pass texture LOD bias: MipBias on top of what the game (and TextureLODBias) set, from the
// first jittered draw to the scene end.
void DlaaSetLodBias(IDirect3DDevice9* device, DWORD sampler, DWORD value)
{
    if (g_real_set_sampler_state != nullptr) {
        g_real_set_sampler_state(device, sampler, D3DSAMP_MIPMAPLODBIAS, value);
    } else {
        device->SetSamplerState(sampler, D3DSAMP_MIPMAPLODBIAS, value);
    }
}

float DlaaBitsFloat(DWORD bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void DlaaBeginSceneBias(IDirect3DDevice9* device)
{
    if (g_dlaa_scene_bias_active || g_dlaa_mip_bias == 0.0f) {
        return;
    }
    g_dlaa_scene_bias_active = true;
    for (DWORD s = 0; s < 16; ++s) {
        DlaaSetLodBias(device, s, FloatBits(DlaaBitsFloat(g_dlaa_game_lod_bias[s]) + g_dlaa_mip_bias));
    }
}

void DlaaEndSceneBias(IDirect3DDevice9* device)
{
    if (!g_dlaa_scene_bias_active) {
        return;
    }
    g_dlaa_scene_bias_active = false;
    for (DWORD s = 0; s < 16; ++s) {
        DlaaSetLodBias(device, s, g_dlaa_game_lod_bias[s]);
    }
}

// ProxySetSamplerState, MIPMAPLODBIAS on samplers 0-15 (after TextureLODBias): remembered, and
// biased while the scene's bias is on.
DWORD DlaaSamplerLodBias(DWORD sampler, DWORD value)
{
    g_dlaa_game_lod_bias[sampler] = value;
    return g_dlaa_scene_bias_active ? FloatBits(DlaaBitsFloat(value) + g_dlaa_mip_bias) : value;
}

// Enable, at the frame end: split-screen bookkeeping, and whether the next frame is jittered.
void DlaaDecideNextFrame()
{
    if (g_dlaa_second_pass_draws >= kDlaaSecondPassDraws) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("DLAA: the scene target got %d more draws after the scene end (a second view?): DLAA off, MLAA on, for now",
                g_dlaa_second_pass_draws);
        }
        g_dlaa_split_frame = true;
    }
    g_dlaa_split_hold = g_dlaa_split_frame ? kDlaaSplitHoldFrames : std::max(0, g_dlaa_split_hold - 1);
    const bool jitter = g_dlaa_split_hold == 0 && D5DlaaProducing(D5RenderWidth(), D5RenderHeight());
    static int changes = 0;
    if (jitter != g_dlaa_jitter_frame && changes < 20) {
        ++changes;
        Log("DLAA: jitter %s from frame %ld%s", jitter ? "on" : "off", static_cast<long>(g_d5_frame_index + 1),
            jitter ? "" : g_dlaa_split_hold > 0 ? " (split screen)" : " (DLAA had no output this frame)");
    }
    g_dlaa_jitter_frame = jitter;
    g_dlaa_output_frame = false;
    g_dlaa_split_frame = false;
    g_dlaa_second_pass_draws = 0;
}

// Frame end: the next frame's jitter, Halton (2,3) indices 1..JitterPhases, minus 0.5.
void DlaaFrameEnd(IDirect3DDevice9* device)
{
    if (!g_dlaa_jitter) {
        return;
    }
    DlaaEndSceneBias(device);
    if (g_dlaa_enable) {
        DlaaDecideNextFrame();
    }
    if (g_dlaa_jitter_debug_pixels != 0.0f) {
        g_dlaa_jx = g_dlaa_jy = g_dlaa_jitter_debug_pixels;
        return;
    }
    const int phase = static_cast<int>(InterlockedIncrement(&g_dlaa_frame) % g_dlaa_jitter_phases) + 1;
    g_dlaa_jx = DlaaHalton(phase, 2) - 0.5f;
    g_dlaa_jy = DlaaHalton(phase, 3) - 0.5f;
}

// Every viewport the game sets (Enable), and SetRenderTarget(0) with nullptr: D3D9 then sets
// the whole target.
void DlaaNoteViewport(const D3DVIEWPORT9* vp)
{
    if (vp == nullptr) {
        g_dlaa_viewport_full = true;
        return;
    }
    g_dlaa_viewport = *vp;
    g_dlaa_viewport_full = vp->X == 0 && vp->Y == 0 && vp->Width >= D5RenderWidth() && vp->Height >= D5RenderHeight();
}

// Every draw the hooks see (not DSF Enhanced's own): J for the bound vertex shader, the jitter in
// the scene pass and zero elsewhere. One constant upload when the value changes.
void DlaaBeforeDraw(IDirect3DDevice9* device)
{
    // The scene pass: its target, under the render queue's scene target set, before its end.
    const bool scene_target = D5IsScenePass() && D5IsSceneTargetSet(g_d5_pass_target_set);
    const bool scene = scene_target && !g_d5_scene_done;
    if (g_dlaa_enable) {
        if (scene && !g_dlaa_viewport_full && !g_dlaa_split_frame) {
            g_dlaa_split_frame = true;
            static bool said = false;
            if (!said) {
                said = true;
                Log("DLAA: scene draw under viewport %lu,%lu %lux%lu, not the whole %ux%u (split screen?): DLAA off, MLAA on, for now",
                    g_dlaa_viewport.X, g_dlaa_viewport.Y, g_dlaa_viewport.Width, g_dlaa_viewport.Height, D5RenderWidth(), D5RenderHeight());
            }
        } else if (scene_target && g_d5_scene_done) {
            ++g_dlaa_second_pass_draws;
        }
    }
    const bool jitter = scene && g_dlaa_jitter_frame;
    if (jitter) {
        DlaaBeginSceneBias(device);
    }
    if (g_dlaa_vs_j < 0) {
        return;
    }
    float j[4] = {};
    if (jitter) {
        j[0] = 2.0f * g_dlaa_jx / static_cast<float>(D5RenderWidth());
        j[1] = -2.0f * g_dlaa_jy / static_cast<float>(D5RenderHeight());
    }
    if (g_dlaa_j_valid && std::memcmp(g_dlaa_j_shadow, j, sizeof(j)) == 0) {
        return;
    }
    if (SUCCEEDED(g_d5_set_vs_constant_f(device, static_cast<UINT>(g_dlaa_vs_j), j, 1))) {
        std::memcpy(g_dlaa_j_shadow, j, sizeof(j));
        g_dlaa_j_valid = true;
    }
}

// ---------------------------------------------------------------------------------------
// MLAA (research/dlaa.md, "The visible AA is MLAA in the post chain"): set 24 writes the edges
// (R5G6B5, pixel shader 3405C219), set 25 the blend weights (A8R8G8B8, AECA6459), both at the
// render size. With the weights cleared to 0 the composite's blend copies the tone-mapped frame.

// The scene end, after DLAA's pass: dlaa_ran says whether its output went back into the scene.
// Split-screen frames are not given to DLAA at all.
void DlaaNoteSceneEnd(IDirect3DDevice9* device, bool dlaa_ran)
{
    (void)device;
    g_dlaa_output_frame = dlaa_ran;
    if (g_d5_trace_frame != nullptr) {
        Log("DLAA  frame %ld: output %d, jitter %d (%.4f, %.4f) px, phase %ld of %d", static_cast<long>(g_d5_frame_index), dlaa_ran ? 1 : 0,
            g_dlaa_jitter_frame ? 1 : 0, static_cast<double>(g_dlaa_jitter_frame ? g_dlaa_jx : 0.0f),
            static_cast<double>(g_dlaa_jitter_frame ? g_dlaa_jy : 0.0f), static_cast<long>(g_dlaa_frame % g_dlaa_jitter_phases) + 1,
            g_dlaa_jitter_phases);
    }
}

// Whether DLAA may run on this frame's scene (the bridge asks at the scene end).
bool DlaaSceneUsable()
{
    return !g_dlaa_split_frame && g_dlaa_split_hold == 0;
}

// SetRenderTarget(0): whether this pass is an MLAA pass to skip.
void DlaaNotePass(const void* surface, DWORD target_set)
{
    g_dlaa_skip_pass = false;
    g_dlaa_clear_pending = false;
    if (target_set != 24 && target_set != 25) {
        return;
    }
    // Enable: MLAA goes only on frames DLAA anti-aliased. It can come back on any frame: the game
    // draws both passes every frame, so skipping or not is decided pass by pass.
    if (g_dlaa_enable && !g_dlaa_output_frame) {
        if (g_dlaa_mlaa_skipped_last) {
            g_dlaa_mlaa_skipped_last = false;
            static int said = 0;
            if (said++ < 10) {
                Log("DLAA: no DLAA output in frame %ld, the game's MLAA runs", static_cast<long>(g_d5_frame_index));
            }
        }
        return;
    }
    D3DSURFACE_DESC desc = {};
    const bool known = surface != nullptr &&
        SUCCEEDED(static_cast<IDirect3DSurface9*>(const_cast<void*>(surface))->GetDesc(&desc)) &&
        desc.Width == D5RenderWidth() && desc.Height == D5RenderHeight() &&
        desc.Format == (target_set == 24 ? D3DFMT_R5G6B5 : D3DFMT_A8R8G8B8);
    if (!known) {
        static bool said = false;
        if (!said) {
            said = true;
            Log("DLAA: target set %lu draws to %ux%u format %d, not the MLAA pass expected: MLAA kept",
                static_cast<unsigned long>(target_set), desc.Width, desc.Height, static_cast<int>(desc.Format));
        }
        return;
    }
    g_dlaa_skip_pass = true;
    g_dlaa_clear_pending = target_set == 25;
    g_dlaa_mlaa_skipped_last = true;
}

// A draw in a skipped MLAA pass: dropped. The first one of the weight pass clears its target.
void DlaaSkipDraw(IDirect3DDevice9* device)
{
    static bool said[2] = {};
    bool& first = said[g_d5_pass_target_set == 24 ? 0 : 1];
    if (!first) {
        first = true;
        Log("DLAA: MLAA pass skipped (target set %lu, pixel shader %08lX)", static_cast<unsigned long>(g_d5_pass_target_set),
            static_cast<unsigned long>(D5ShaderHash(g_d5.ps)));
    }
    if (g_dlaa_clear_pending) {
        g_dlaa_clear_pending = false;
        g_d5_clear(device, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    }
}
