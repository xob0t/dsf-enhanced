#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "cinematic_fit.h"

namespace {

bool g_log_enabled = true;
bool g_remove_vsync = false;
bool g_borderless_windowed = false;
bool g_ultrawide_enabled = true;
bool g_fov_scaling = true;
bool g_cinematic_draw_patch = true;
int g_anisotropic_filtering = 16;
bool g_force_trilinear_filtering = true;
bool g_force_highest_mip = true;
float g_texture_lod_bias = 0.0f;
float g_lod_distance_scale = 1.0f;
float g_lod_engine_threshold_scale = 0.25f;
float g_map_detail_lod_threshold_scale = 1.0f;
float g_imposter_draw_distance = 2800.0f;
float g_traffic_spawn_radius = 350.0f;
float g_vehicle_lod_threshold_scale = 1.0f;
bool g_vehicle_full_detail = false;
float g_vehicle_draw_distance = 0.0f;
bool g_traffic_diagnostics = false;
float g_interesting_vehicle_min_distance_scale = 1.0f;
float g_interesting_vehicle_max_distance_scale = 1.0f;
int g_interesting_vehicle_slot_count = 0;
bool g_preload_all_traffic_vehicle_pools = false;
float g_traffic_candidate_window_scale = 1.0f;
float g_traffic_spawn_interval_scale = 1.0f;
bool g_skip_legal_screen = true;
bool g_suppress_ubisoft_server_lost_prompt = true;
bool g_updown_switches_maker = true;
float g_target_aspect = 0.0f;
float g_fov_scalar = 0.0f;
float g_current_aspect = 16.0f / 9.0f;
UINT g_backbuffer_width = 0;
UINT g_backbuffer_height = 0;
char g_log_path[MAX_PATH] = {};
bool g_aspect_constant_patched = false;
bool g_global_fov_scalar_patched = false;
LONG g_active_bink_count = 0;
LONG g_active_cinematic_bink_count = 0;
void* volatile g_last_d3d_device = nullptr;

constexpr DWORD kAspect16x9Rva = 0x009E50B0;
constexpr DWORD kGlobalFovScalarRva = 0x009D9A80;
constexpr DWORD kMovieDrawRva = 0x0036C5D0;
constexpr DWORD kLegalScreenWaitBranchRva = 0x00399C4A;
constexpr DWORD kLegalScreenBeginLoadingCallRva = 0x00399C92;   // call 0x4A20C0 when the legal screen ends
constexpr DWORD kLoadingScreenBeginRva = 0x000A20C0;
constexpr DWORD kPendingResourceReferenceClearRva = 0x000A2153; // mov dword [0x188A6B8],0
constexpr DWORD kPendingResourceReferenceCountRva = 0x0148A6B8; // resource manager 0x1732BF0 + 0x157AC8
// Resource manager 0x1732BF0: ResourceHashPool_Acquire (0xBAED90) takes a 24-byte node from the
// free list at +0x127AC0, else bumps +0x127AC4 through 45,000 nodes stored at +0x20000..+0x127AC0,
// with no end check. The 45,001st resident resource overwrites the free-list head and the bump
// pointer, and the next acquire faults reading garbage (0xBAEDC7).
constexpr DWORD kResourceHashBumpRva = 0x007AEDB6;          // mov eax,[ecx+0x127AC4]; lea ebx,[eax+18h]; mov [ecx+0x127AC4],ebx; jmp
constexpr DWORD kResourceHashBumpContinueRva = 0x007AEDD0;  // fills the node in eax
constexpr DWORD kErrorMessageUpdateRva = 0x0032AD80;
constexpr DWORD kEngineSingletonRva = 0x00BE7228;
constexpr DWORD kLodDirtyFlagRva = 0x00D8D6E0;
constexpr DWORD kEngineSetLodThresholdRva = 0x002A4F40;
constexpr DWORD kEngineGetLodThresholdRva = 0x002A4F70;
constexpr DWORD kImposterApplyDistanceRva = 0x002B3560;
constexpr DWORD kTrafficSettingsTableRva = 0x00BE7AC0;
constexpr DWORD kTrafficApplySettingsVehiclePoolRva = 0x0024E2D0;
constexpr DWORD kInterestingVehicleSetMinDistanceRva = 0x003EB3A0;
constexpr DWORD kInterestingVehicleSetMaxDistanceRva = 0x003EB3C0;
constexpr DWORD kInterestingVehicleSetSlotCountRva = 0x003EB3E0;
constexpr DWORD kTrafficCandidateLateralWindowRva = 0x00B2A278;
constexpr DWORD kTrafficCandidateForwardWindowRva = 0x00B2A27C;
constexpr DWORD kOriginalAspect16x9Bits = 0x3FE38E39;
constexpr DWORD kOriginalFovScalarBits = 0x3C8EFA35;
constexpr float kTrafficCandidateOriginalLateralWindow = 20.0f;
constexpr float kTrafficCandidateOriginalForwardWindow = 6.0f;

// Traffic road radius (research: draw-distance.md, plan A). 0x772800 builds the wanted road set
// with 0x7723A0(list, position, radius); the radius comes from these two movss loads.
constexpr DWORD kTrafficRadiusLoadRva = 0x00372A00;       // movss xmm0,[0xDE2344] 350 m (single player)
constexpr DWORD kTrafficRadiusSplitLoadRva = 0x003729F6;  // movss xmm0,[0xDE2454] 256 m ([[engine+0x754]] != 0)
constexpr DWORD kTrafficRadiusConstantRva = 0x009E2344;   // shared constants: never written
constexpr DWORD kTrafficRadiusSplitConstantRva = 0x009E2454;
constexpr float kTrafficStockRadius = 350.0f;
constexpr float kTrafficStockSplitRadius = 256.0f;
// 500 m: above it the traffic pools overflow at stock density. At 700 m the vehicle store
// (500 cars, 0x77AA50) stayed full and the lane-carrier pool at manager+0x503648 (12,000 nodes of
// 0x24 bytes, bump-allocated by 0x77BD30 with no bound check) ran into the vehicle store behind
// it, corrupting its count and vehicle pointers (crash at 0x8CD230). Raise only with those pools.
constexpr float kTrafficMaxRadius = 500.0f;
// 0x7723A0 lists the 240 m road-grid cells inside the radius square in a 21-entry stack array
// with no bound check. Above ~360 m the square spans 25+ cells and entry 21 lands on the count,
// so cells (often the player's own) are silently lost and near roads ping out and back in.
// These five instructions address the array; they are pointed at a plugin-owned buffer.
constexpr DWORD kTrafficCellWriteRva = 0x0037247B;       // lea eax,[esp+eax*8+0xBC]
constexpr DWORD kTrafficCellSortEndRva = 0x003724B7;     // lea eax,[esp+eax*8+0xC4]
constexpr DWORD kTrafficCellSortBeginRva = 0x003724C0;   // lea edx,[esp+0xC4]
constexpr DWORD kTrafficCellLoopBeginRva = 0x003724DD;   // lea edi,[esp+0xD0]
constexpr DWORD kTrafficCellLoopEndRva = 0x003724E4;     // lea eax,[esp+eax*8+0xD0]
constexpr int kTrafficCellCapacity = 1024;               // 500 m needs at most 6 x 6 cells; room to spare
// Traffic manager [engine+0x758] fields read by TrafficDiagnostics.
constexpr DWORD kTrafficActiveNodeCountOffset = 0x0C;        // active road directions (pool of 512)
constexpr DWORD kTrafficActiveLaneCountOffset = 0x501818;
constexpr DWORD kTrafficWantedCountOffset = 0x50282C;        // wanted list at +0x50182C, cap 512
constexpr DWORD kTrafficVehicleCountOffset = 0x56CDE4;       // vehicle store +0x56CDDC, count +8, cap 500

// Vehicle LOD (category 2). Setter/getter keys per selector slot: key 2 = slot 0 (draws mask 4,
// full detail), 4 = slot 1 (mask 8), 8 = slot 2 (mask 16), 16 = slot 3 (mask 1 hull; its
// threshold is the cull), 1 = slot 4 (draws nothing). Decoded from 0x6A4E10.
constexpr int kLodSlotKeys[5] = {2, 4, 8, 16, 1};
constexpr int kLodCategoryCount = 5;
constexpr const char* kLodCategoryNames[kLodCategoryCount] = {"Static", "Terrain", "Vehicle", "Pedestrian", "Driver"};

// DRL flare curves: float[2][3][4] per flare type at 0x120D198 (type 0 emergency, 1 DRL);
// rows 0-2 distances, rows 3-5 values; columns scale, transparency, brightness. Filled by the
// static initialiser 0xD31C30, read by 0x50B6C0, written by Lua Sfx.DRLFlare* via 0x50B410.
constexpr DWORD kDrlFlareTableRva = 0x00E0D1F8;

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetSamplerStateFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SetPixelShaderConstantFFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using EngineSetLodThresholdFn = void (__thiscall*)(void*, int, int, float);
using EngineGetLodThresholdFn = float (__thiscall*)(void*, int, int);
using ImposterApplyDistanceFn = void (__thiscall*)(void*, float, float);
using InterestingVehicleSetDistanceFn = void (__thiscall*)(void*, float);
using InterestingVehicleSetSlotCountFn = void (__thiscall*)(void*, int);
using TrafficApplySettingsVehiclePoolFn = bool (__thiscall*)(void*, void*);
using BinkOpenFn = void* (WINAPI*)(const char*, UINT);
using BinkCloseFn = void (WINAPI*)(void*);

Direct3DCreate9Fn g_real_direct3d_create9 = nullptr;
ResetFn g_real_reset = nullptr;
SetSamplerStateFn g_real_set_sampler_state = nullptr;
PresentFn g_real_present = nullptr;
SetPixelShaderConstantFFn g_real_set_pixel_shader_constant_f = nullptr;
BinkOpenFn g_real_bink_open = nullptr;
BinkCloseFn g_real_bink_close = nullptr;
volatile LONG g_device_hooks_installed = 0;
volatile LONG g_cinematic_movie_draw_count = 0;
volatile LONG g_cinematic_movie_draw_log_count = 0;
volatile LONG g_sampler_quality_log_count = 0;
volatile LONG g_lod_distance_scale_log_count = 0;
bool g_traffic_radius_patch_applied = false;
volatile LONG g_present_count = 0;
volatile LONG g_imposter_log_count = 0;
void* g_imposter_last_manager = nullptr;
bool g_imposter_distance_patch_applied = false;
volatile LONG g_traffic_runtime_log_count = 0;
void* g_traffic_last_pool_manager = nullptr;
bool g_traffic_vehicle_pools_preloaded = false;
void* g_traffic_last_runtime_manager = nullptr;
volatile LONG g_interesting_vehicle_log_count = 0;
void* g_traffic_last_interesting_manager = nullptr;
bool g_interesting_vehicle_patch_applied = false;
void* g_movie_draw_continue = nullptr;
int g_resource_arena_extra_nodes = 135000;
int g_resource_arena_stock_nodes = 45000;
DWORD g_arena_stock_end_offset = 0x127AC0;  // manager offset where the stock nodes the hook uses end
std::uint8_t* g_arena_extension_begin = nullptr;
std::uint8_t* volatile g_arena_extension_next = nullptr;
std::uint8_t* g_arena_extension_end = nullptr;
void* g_arena_bump_continue = nullptr;
DWORD g_arena_extension_logged_nodes = 0;
void* g_error_message_update_continue = nullptr;
volatile LONG g_ubisoft_server_lost_prompt_log_count = 0;

struct BinkHandleRecord
{
    void* handle;
    bool cinematic;
};

BinkHandleRecord g_bink_handles[16] = {};
SRWLOCK g_bink_handle_lock = SRWLOCK_INIT;

struct ModuleRange
{
    std::uint8_t* base;
    DWORD size;
};

void MarkEngineLodThresholdsDirty(const ModuleRange& range);

DWORD CallerRva(void* caller);
void HookDeviceMethods(IDirect3DDevice9* device);
void TrackLastD3DDevice(IDirect3DDevice9* device);
void RehookTrackedD3DDevice(const char* source);

void BuildGamePath(char* out, DWORD out_size, const char* name)
{
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, module_path, MAX_PATH);

    char* slash = std::strrchr(module_path, '\\');
    if (slash != nullptr) {
        slash[1] = '\0';
    } else {
        module_path[0] = '\0';
    }

    std::snprintf(out, out_size, "%s%s", module_path, name);
}

void Log(const char* fmt, ...)
{
    if (!g_log_enabled || g_log_path[0] == '\0') {
        return;
    }

    FILE* file = nullptr;
    fopen_s(&file, g_log_path, "a");
    if (file == nullptr) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(
        file,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds
    );

    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fprintf(file, "\n");
    std::fclose(file);
}

float ReadProfileFloat(const char* section, const char* key, float fallback, const char* ini_path)
{
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.6f", fallback);
    GetPrivateProfileStringA(section, key, buffer, buffer, sizeof(buffer), ini_path);
    return static_cast<float>(std::atof(buffer));
}

int ClampInt(int value, int low, int high)
{
    return std::max(low, std::min(value, high));
}

struct FlareCurve
{
    bool enabled;
    float distances[3];
    float values[3];
};

// DRL flare attributes in table column order.
constexpr const char* kFlareAttributeKeys[3] = {"DRLFlareScale", "DRLFlareTransparency", "DRLFlareBrightness"};
FlareCurve g_drl_flare_curves[3] = {};
bool g_drl_flares_written = false;

// "d1:v1, d2:v2, d3:v3"; empty leaves the game's curve. Distances must increase strictly,
// as the game's own Lua setter requires (0x67F770).
void LoadFlareConfig(const char* ini_path)
{
    for (int attr = 0; attr < 3; ++attr) {
        FlareCurve& curve = g_drl_flare_curves[attr];
        curve = {};
        char buffer[128] = {};
        GetPrivateProfileStringA("Lights", kFlareAttributeKeys[attr], "", buffer, sizeof(buffer), ini_path);
        if (buffer[0] == '\0') {
            continue;
        }
        float d[3] = {};
        float v[3] = {};
        bool valid = std::sscanf(buffer, " %f : %f , %f : %f , %f : %f", &d[0], &v[0], &d[1], &v[1], &d[2], &v[2]) == 6 &&
            d[0] >= 0.0f && d[0] < d[1] && d[1] < d[2];
        for (int i = 0; i < 3; ++i) {
            valid = valid && std::isfinite(d[i]) && std::isfinite(v[i]) && v[i] >= 0.0f && v[i] <= 16.0f;
        }
        if (!valid) {
            Log("Lights: ignoring %s=%s (expected d1:v1, d2:v2, d3:v3 with increasing distances)", kFlareAttributeKeys[attr], buffer);
            continue;
        }
        curve.enabled = true;
        std::memcpy(curve.distances, d, sizeof(d));
        std::memcpy(curve.values, v, sizeof(v));
    }
}

void LoadConfig()
{
    char ini_path[MAX_PATH] = {};
    BuildGamePath(ini_path, MAX_PATH, "dsf_enhanced.ini");
    BuildGamePath(g_log_path, MAX_PATH, "dsf_enhanced.log");

    g_log_enabled = GetPrivateProfileIntA("General", "Log", 1, ini_path) != 0;
    g_traffic_diagnostics = GetPrivateProfileIntA("General", "TrafficDiagnostics", 0, ini_path) != 0;
    g_remove_vsync = GetPrivateProfileIntA("Display", "RemoveVSync", 0, ini_path) != 0;
    g_borderless_windowed = GetPrivateProfileIntA("Display", "BorderlessWindowed", 0, ini_path) != 0;
    g_ultrawide_enabled = GetPrivateProfileIntA("Ultrawide", "Enabled", 1, ini_path) != 0;
    g_target_aspect = ReadProfileFloat("Ultrawide", "TargetAspect", 0.0f, ini_path);
    g_fov_scalar = ReadProfileFloat("Ultrawide", "FOVScalar", 0.0f, ini_path);
    g_fov_scaling = GetPrivateProfileIntA("Ultrawide", "FOVScaling", 1, ini_path) != 0;
    g_cinematic_draw_patch = GetPrivateProfileIntA("Ultrawide", "CinematicDrawPatch", 1, ini_path) != 0;
    g_anisotropic_filtering = ClampInt(GetPrivateProfileIntA("Quality", "AnisotropicFiltering", 16, ini_path), 0, 16);
    g_force_trilinear_filtering = GetPrivateProfileIntA("Quality", "ForceTrilinearFiltering", 0, ini_path) != 0;
    g_force_highest_mip = GetPrivateProfileIntA("Quality", "ForceHighestMip", 0, ini_path) != 0;
    g_texture_lod_bias = ReadProfileFloat("Quality", "TextureLODBias", 0.0f, ini_path);
    g_texture_lod_bias = std::max(-3.0f, std::min(g_texture_lod_bias, 3.0f));
    g_lod_distance_scale = ReadProfileFloat("Quality", "LODDistanceScale", 1.0f, ini_path);
    g_lod_distance_scale = std::max(0.25f, std::min(g_lod_distance_scale, 8.0f));
    g_lod_engine_threshold_scale = ReadProfileFloat("Quality", "LODEngineThresholdScale", 1.0f, ini_path);
    g_lod_engine_threshold_scale = std::max(0.25f, std::min(g_lod_engine_threshold_scale, 8.0f));
    g_vehicle_lod_threshold_scale = ReadProfileFloat("Quality", "VehicleLODThresholdScale", 1.0f, ini_path);
    g_vehicle_lod_threshold_scale = std::max(0.25f, std::min(g_vehicle_lod_threshold_scale, 8.0f));
    g_vehicle_full_detail = GetPrivateProfileIntA("Quality", "VehicleFullDetail", 0, ini_path) != 0;
    g_vehicle_draw_distance = ReadProfileFloat("Quality", "VehicleDrawDistance", 0.0f, ini_path);
    g_vehicle_draw_distance = g_vehicle_draw_distance <= 0.0f ? 0.0f : std::max(200.0f, std::min(g_vehicle_draw_distance, 3000.0f));
    g_map_detail_lod_threshold_scale = ReadProfileFloat("Quality", "MapDetailLODThresholdScale", 1.0f, ini_path);
    g_map_detail_lod_threshold_scale = std::max(0.25f, std::min(g_map_detail_lod_threshold_scale, 8.0f));
    g_traffic_spawn_radius = ReadProfileFloat("Quality", "TrafficSpawnRadius", kTrafficStockRadius, ini_path);
    g_traffic_spawn_radius = std::max(kTrafficStockRadius, std::min(g_traffic_spawn_radius, kTrafficMaxRadius));
    g_imposter_draw_distance = ReadProfileFloat("Quality", "ImposterDrawDistance", 2800.0f, ini_path);
    g_imposter_draw_distance = std::max(-1.0f, std::min(g_imposter_draw_distance, 7000.0f));
    g_interesting_vehicle_min_distance_scale =
        ReadProfileFloat("Quality", "InterestingVehicleMinDistanceScale", 1.0f, ini_path);
    g_interesting_vehicle_min_distance_scale =
        std::max(0.25f, std::min(g_interesting_vehicle_min_distance_scale, 16.0f));
    g_interesting_vehicle_max_distance_scale =
        ReadProfileFloat("Quality", "InterestingVehicleMaxDistanceScale", 1.0f, ini_path);
    g_interesting_vehicle_max_distance_scale =
        std::max(0.25f, std::min(g_interesting_vehicle_max_distance_scale, 16.0f));
    g_interesting_vehicle_slot_count =
        ClampInt(GetPrivateProfileIntA("Quality", "InterestingVehicleSlotCount", 0, ini_path), 0, 10);
    g_preload_all_traffic_vehicle_pools = GetPrivateProfileIntA("Quality", "PreloadAllTrafficVehiclePools", 0, ini_path) != 0;
    g_traffic_candidate_window_scale = ReadProfileFloat("Quality", "TrafficCandidateWindowScale", 1.0f, ini_path);
    g_traffic_candidate_window_scale = std::max(1.0f, std::min(g_traffic_candidate_window_scale, 16.0f));
    g_traffic_spawn_interval_scale = ReadProfileFloat("Quality", "TrafficSpawnIntervalScale", 1.0f, ini_path);
    g_traffic_spawn_interval_scale = std::max(0.05f, std::min(g_traffic_spawn_interval_scale, 1.0f));
    g_skip_legal_screen = GetPrivateProfileIntA("Startup", "SkipLegalScreen", 1, ini_path) != 0;
    g_suppress_ubisoft_server_lost_prompt = GetPrivateProfileIntA("Startup", "SuppressUbisoftServerLostPrompt", 1, ini_path) != 0;
    g_updown_switches_maker = GetPrivateProfileIntA("Garage", "UpDownSwitchesMaker", 1, ini_path) != 0;
    g_resource_arena_extra_nodes =
        ClampInt(GetPrivateProfileIntA("Limits", "ResourceArenaExtraNodes", 135000, ini_path), 0, 2000000);
    g_resource_arena_stock_nodes =
        ClampInt(GetPrivateProfileIntA("Limits", "ResourceArenaStockNodes", 45000, ini_path), 1, 45000);
    LoadFlareConfig(ini_path);
}

bool WriteMemory(void* address, const void* data, size_t size)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    return true;
}

DWORD FloatBits(float value)
{
    DWORD bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool IsCommittedWritableMemory(const void* address, size_t size)
{
    if (address == nullptr || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }

    if ((mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_READONLY) {
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return begin >= region_begin && (begin + size) <= (region_begin + mbi.RegionSize);
}

bool IsReadableMemory(const void* address, size_t size)
{
    if (address == nullptr || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }

    if ((mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) {
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return begin >= region_begin && (begin + size) <= (region_begin + mbi.RegionSize);
}

bool IsRvaInRange(const ModuleRange& range, DWORD rva, size_t size)
{
    return range.base != nullptr && size != 0 && rva <= range.size && size <= range.size - rva;
}

ModuleRange GetExeRange()
{
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
    if (base == nullptr) {
        return ModuleRange{};
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return ModuleRange{};
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return ModuleRange{};
    }

    return ModuleRange{base, nt->OptionalHeader.SizeOfImage};
}

bool ResolveEngineInstance(const ModuleRange& range, std::uint8_t** out_engine)
{
    if (out_engine == nullptr) {
        return false;
    }
    *out_engine = nullptr;

    if (!IsRvaInRange(range, kEngineSingletonRva, sizeof(void*))) {
        return false;
    }

    auto** engine_slot = reinterpret_cast<std::uint8_t**>(range.base + kEngineSingletonRva);
    if (!IsReadableMemory(engine_slot, sizeof(*engine_slot))) {
        return false;
    }

    std::uint8_t* engine = *engine_slot;
    if (engine == nullptr || !IsReadableMemory(engine, sizeof(void*))) {
        return false;
    }

    *out_engine = engine;
    return true;
}

bool TrySetInterestingVehicleDistance(InterestingVehicleSetDistanceFn setter, void* manager, float value)
{
    if (setter == nullptr || manager == nullptr) {
        return false;
    }

    __try {
        setter(manager, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySetInterestingVehicleSlotCount(InterestingVehicleSetSlotCountFn setter, void* manager, int value)
{
    if (setter == nullptr || manager == nullptr) {
        return false;
    }

    __try {
        setter(manager, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyInterestingVehiclePatch(const ModuleRange& range, const char* source)
{
    if (std::fabs(g_interesting_vehicle_min_distance_scale - 1.0f) < 0.001f &&
        std::fabs(g_interesting_vehicle_max_distance_scale - 1.0f) < 0.001f &&
        g_interesting_vehicle_slot_count <= 0) {
        return;
    }

    std::uint8_t* engine = nullptr;
    if (!ResolveEngineInstance(range, &engine) || engine == nullptr || !IsReadableMemory(engine + 0x6d8, sizeof(void*))) {
        return;
    }

    auto* manager = *reinterpret_cast<std::uint8_t**>(engine + 0x6d8);
    if (manager == nullptr || !IsReadableMemory(manager + 0x29b0, 1)) {
        return;
    }

    if (manager != g_traffic_last_interesting_manager) {
        g_traffic_last_interesting_manager = manager;
        g_interesting_vehicle_patch_applied = false;
        g_interesting_vehicle_log_count = 0;
        Log("InterestingVehicles: manager=0x%p from %s", manager, source != nullptr ? source : "Loop");
    }

    const float original_min = IsReadableMemory(manager + 0x29a0, sizeof(float)) ?
        *reinterpret_cast<float*>(manager + 0x29a0) : 0.0f;
    const float original_max = IsReadableMemory(manager + 0x29a4, sizeof(float)) ?
        *reinterpret_cast<float*>(manager + 0x29a4) : 0.0f;
    const int original_slots = IsReadableMemory(manager + 0x29a8, sizeof(int)) ?
        *reinterpret_cast<int*>(manager + 0x29a8) : 0;
    const int enabled = manager[0x29b0] ? 1 : 0;

    float target_min = original_min;
    float target_max = original_max;
    const float base_min = 128.0f;
    const float base_max = 256.0f;
    if (std::fabs(g_interesting_vehicle_min_distance_scale - 1.0f) >= 0.001f) {
        target_min = std::min(base_min * g_interesting_vehicle_min_distance_scale, 500000.0f);
    }
    if (std::fabs(g_interesting_vehicle_max_distance_scale - 1.0f) >= 0.001f) {
        target_max = std::min(base_max * g_interesting_vehicle_max_distance_scale, 500000.0f);
    }

    bool min_ok = true;
    bool max_ok = true;
    bool slots_ok = true;
    if (std::fabs(target_min - original_min) >= 0.001f) {
        auto setter = reinterpret_cast<InterestingVehicleSetDistanceFn>(range.base + kInterestingVehicleSetMinDistanceRva);
        min_ok = TrySetInterestingVehicleDistance(setter, manager, target_min);
    }
    if (std::fabs(target_max - original_max) >= 0.001f) {
        auto setter = reinterpret_cast<InterestingVehicleSetDistanceFn>(range.base + kInterestingVehicleSetMaxDistanceRva);
        max_ok = TrySetInterestingVehicleDistance(setter, manager, target_max);
    }
    if (g_interesting_vehicle_slot_count > 0 && original_slots != g_interesting_vehicle_slot_count) {
        auto setter = reinterpret_cast<InterestingVehicleSetSlotCountFn>(range.base + kInterestingVehicleSetSlotCountRva);
        slots_ok = TrySetInterestingVehicleSlotCount(setter, manager, g_interesting_vehicle_slot_count);
    }

    const float after_min = IsReadableMemory(manager + 0x29a0, sizeof(float)) ?
        *reinterpret_cast<float*>(manager + 0x29a0) : 0.0f;
    const float after_max = IsReadableMemory(manager + 0x29a4, sizeof(float)) ?
        *reinterpret_cast<float*>(manager + 0x29a4) : 0.0f;
    const int after_slots = IsReadableMemory(manager + 0x29a8, sizeof(int)) ?
        *reinterpret_cast<int*>(manager + 0x29a8) : 0;

    const bool changed =
        std::fabs(after_min - original_min) >= 0.001f ||
        std::fabs(after_max - original_max) >= 0.001f ||
        after_slots != original_slots ||
        !g_interesting_vehicle_patch_applied;
    if (changed) {
        const LONG count = InterlockedIncrement(&g_interesting_vehicle_log_count);
        if (count <= 12) {
            Log(
                "InterestingVehicles: minScale=%.3f maxScale=%.3f min%s %.3f -> %.3f max%s %.3f -> %.3f slots%s %d -> %d enabled=%d",
                static_cast<double>(g_interesting_vehicle_min_distance_scale),
                static_cast<double>(g_interesting_vehicle_max_distance_scale),
                min_ok ? "" : "_failed",
                static_cast<double>(original_min),
                static_cast<double>(after_min),
                max_ok ? "" : "_failed",
                static_cast<double>(original_max),
                static_cast<double>(after_max),
                slots_ok ? "" : "_failed",
                original_slots,
                after_slots,
                enabled
            );
        }
    }

    g_interesting_vehicle_patch_applied = min_ok && max_ok && slots_ok;
}

bool TryApplyTrafficSettingsVehiclePool(TrafficApplySettingsVehiclePoolFn apply_pool, void* manager, void* settings_record)
{
    if (apply_pool == nullptr || manager == nullptr || settings_record == nullptr) {
        return false;
    }

    __try {
        return apply_pool(manager, settings_record);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyTrafficVehiclePoolPreload(const ModuleRange& range, const char* source)
{
    if (!g_preload_all_traffic_vehicle_pools) {
        return;
    }

    if (!IsRvaInRange(range, kTrafficSettingsTableRva, sizeof(void*)) ||
        !IsRvaInRange(range, kTrafficApplySettingsVehiclePoolRva, 1)) {
        return;
    }

    std::uint8_t* engine = nullptr;
    if (!ResolveEngineInstance(range, &engine) || engine == nullptr || !IsReadableMemory(engine + 0x758, sizeof(void*))) {
        return;
    }

    auto* manager = *reinterpret_cast<std::uint8_t**>(engine + 0x758);
    if (manager == nullptr || !IsReadableMemory(manager, sizeof(DWORD))) {
        return;
    }

    if (manager != g_traffic_last_pool_manager) {
        g_traffic_last_pool_manager = manager;
        g_traffic_vehicle_pools_preloaded = false;
        Log("TrafficPools: manager=0x%p from %s", manager, source != nullptr ? source : "Loop");
    }

    if (g_traffic_vehicle_pools_preloaded) {
        return;
    }

    auto** table_slot = reinterpret_cast<std::uint8_t**>(range.base + kTrafficSettingsTableRva);
    if (!IsReadableMemory(table_slot, sizeof(*table_slot))) {
        return;
    }

    auto* table = *table_slot;
    if (table == nullptr || !IsReadableMemory(table, 8)) {
        return;
    }

    const int raw_count = *reinterpret_cast<int*>(table);
    auto* records = *reinterpret_cast<std::uint8_t**>(table + 4);
    const int count = ClampInt(raw_count, 0, 256);
    if (records == nullptr || count == 0 || !IsReadableMemory(records, sizeof(DWORD))) {
        return;
    }

    auto apply_pool = reinterpret_cast<TrafficApplySettingsVehiclePoolFn>(range.base + kTrafficApplySettingsVehiclePoolRva);
    int applied = 0;
    int failed = 0;
    for (int index = 0; index < count; ++index) {
        auto* record = records + index * 0x34;
        if (!IsReadableMemory(record, 0x34)) {
            ++failed;
            continue;
        }

        if (TryApplyTrafficSettingsVehiclePool(apply_pool, manager, record)) {
            ++applied;
        } else {
            ++failed;
        }
    }

    Log(
        "TrafficPools: table=0x%p records=0x%p count=%d applied=%d failed=%d",
        table,
        records,
        count,
        applied,
        failed
    );
    g_traffic_vehicle_pools_preloaded = true;
}

void ApplyTrafficRuntimePatches(const ModuleRange& range, const char* source)
{
    const bool patch_candidate_window = g_traffic_candidate_window_scale > 1.001f;
    const bool patch_spawn_interval = g_traffic_spawn_interval_scale < 0.999f;
    if (!patch_candidate_window && !patch_spawn_interval) {
        return;
    }

    if (patch_candidate_window &&
        IsRvaInRange(range, kTrafficCandidateLateralWindowRva, sizeof(float)) &&
        IsRvaInRange(range, kTrafficCandidateForwardWindowRva, sizeof(float))) {
        auto* lateral_window = reinterpret_cast<float*>(range.base + kTrafficCandidateLateralWindowRva);
        auto* forward_window = reinterpret_cast<float*>(range.base + kTrafficCandidateForwardWindowRva);
        if (IsReadableMemory(lateral_window, sizeof(float)) &&
            IsReadableMemory(forward_window, sizeof(float))) {
            const float original_lateral = *lateral_window;
            const float original_forward = *forward_window;
            const float target_lateral = kTrafficCandidateOriginalLateralWindow * g_traffic_candidate_window_scale;
            const float target_forward = kTrafficCandidateOriginalForwardWindow * g_traffic_candidate_window_scale;
            if (std::fabs(original_lateral - target_lateral) > 0.001f) {
                WriteMemory(lateral_window, &target_lateral, sizeof(target_lateral));
            }
            if (std::fabs(original_forward - target_forward) > 0.001f) {
                WriteMemory(forward_window, &target_forward, sizeof(target_forward));
            }

            const LONG log_count = InterlockedIncrement(&g_traffic_runtime_log_count);
            if (log_count <= 8) {
                Log(
                    "TrafficRuntime: candidateWindow source=%s scale=%.3f lateral %.3f -> %.3f forward %.3f -> %.3f",
                    source != nullptr ? source : "Loop",
                    static_cast<double>(g_traffic_candidate_window_scale),
                    static_cast<double>(original_lateral),
                    static_cast<double>(*lateral_window),
                    static_cast<double>(original_forward),
                    static_cast<double>(*forward_window)
                );
            }
        }
    }

    std::uint8_t* engine = nullptr;
    if (!ResolveEngineInstance(range, &engine) || engine == nullptr || !IsReadableMemory(engine + 0x758, sizeof(void*))) {
        return;
    }

    auto* manager = *reinterpret_cast<std::uint8_t**>(engine + 0x758);
    if (manager == nullptr || !IsReadableMemory(manager + 0x5ca854, sizeof(DWORD))) {
        return;
    }

    if (manager != g_traffic_last_runtime_manager) {
        g_traffic_last_runtime_manager = manager;
        g_traffic_runtime_log_count = 0;
        Log("TrafficRuntime: manager=0x%p from %s", manager, source != nullptr ? source : "Loop");
    }

    float next_spawn_time = 0.0f;
    float last_update_time = 0.0f;
    std::uint8_t traffic_enabled = 0;
    std::uint8_t spawn_pending = 0;
    std::uint8_t spawn_scan_active = 0;
    std::uint8_t force_refresh = 0;
    DWORD active_road_count = 0;
    DWORD active_road_phase = 0;
    DWORD active_lane_phase = 0;
    DWORD pending_vehicle_count = 0;

    const bool can_read_runtime =
        IsReadableMemory(manager + 0x501818, sizeof(DWORD)) &&
        IsReadableMemory(manager + 0x50181c, sizeof(DWORD)) &&
        IsReadableMemory(manager + 0x501820, sizeof(DWORD)) &&
        IsReadableMemory(manager + 0x503640, sizeof(float)) &&
        IsReadableMemory(manager + 0x503644, sizeof(std::uint8_t)) &&
        IsReadableMemory(manager + 0x503645, sizeof(std::uint8_t)) &&
        IsReadableMemory(manager + 0x503646, sizeof(std::uint8_t)) &&
        IsReadableMemory(manager + 0x5ca84c, sizeof(std::uint8_t)) &&
        IsReadableMemory(manager + 0x5ca848, sizeof(float)) &&
        IsReadableMemory(manager + 0x5ca858, sizeof(DWORD));
    if (!can_read_runtime) {
        return;
    }

    active_road_count = *reinterpret_cast<DWORD*>(manager + 0x501818);
    active_road_phase = *reinterpret_cast<DWORD*>(manager + 0x50181c);
    active_lane_phase = *reinterpret_cast<DWORD*>(manager + 0x501820);
    next_spawn_time = *reinterpret_cast<float*>(manager + 0x503640);
    spawn_pending = manager[0x503644];
    spawn_scan_active = manager[0x503645];
    force_refresh = manager[0x503646];
    traffic_enabled = manager[0x5ca84c];
    last_update_time = *reinterpret_cast<float*>(manager + 0x5ca848);
    pending_vehicle_count = *reinterpret_cast<DWORD*>(manager + 0x5ca858);

    bool interval_changed = false;
    float target_next_spawn_time = next_spawn_time;
    if (patch_spawn_interval &&
        traffic_enabled != 0 &&
        std::isfinite(next_spawn_time) &&
        std::isfinite(last_update_time) &&
        next_spawn_time > last_update_time &&
        next_spawn_time > 0.0f &&
        last_update_time > 0.0f) {
        const float original_delta = next_spawn_time - last_update_time;
        target_next_spawn_time = last_update_time + original_delta * g_traffic_spawn_interval_scale;
        if (target_next_spawn_time < next_spawn_time) {
            WriteMemory(manager + 0x503640, &target_next_spawn_time, sizeof(target_next_spawn_time));
            interval_changed = true;
        }
    }

    if (interval_changed) {
        const LONG log_count = InterlockedIncrement(&g_traffic_runtime_log_count);
        if (log_count <= 40) {
            char interval_text[64] = {};
            if (interval_changed) {
                std::snprintf(
                    interval_text,
                    sizeof(interval_text),
                    " -> %.3f",
                    static_cast<double>(target_next_spawn_time)
                );
            }

            Log(
                "TrafficRuntime: enabled=%u activeRoads=%lu roadPhase=%lu lanePhase=%lu pending=%lu spawnFlags=%u/%u/%u next=%.3f last=%.3f intervalScale=%.3f%s",
                static_cast<unsigned>(traffic_enabled),
                static_cast<unsigned long>(active_road_count),
                static_cast<unsigned long>(active_road_phase),
                static_cast<unsigned long>(active_lane_phase),
                static_cast<unsigned long>(pending_vehicle_count),
                static_cast<unsigned>(spawn_pending),
                static_cast<unsigned>(spawn_scan_active),
                static_cast<unsigned>(force_refresh),
                static_cast<double>(next_spawn_time),
                static_cast<double>(last_update_time),
                static_cast<double>(g_traffic_spawn_interval_scale),
                interval_text
            );
        }
    }
}

bool ResolveEngineLodManager(const ModuleRange& range, void** out_manager)
{
    if (out_manager == nullptr) {
        return false;
    }
    *out_manager = nullptr;

    if (!IsRvaInRange(range, kEngineSingletonRva, sizeof(void*))) {
        return false;
    }

    auto** engine_slot = reinterpret_cast<std::uint8_t**>(range.base + kEngineSingletonRva);
    if (!IsReadableMemory(engine_slot, sizeof(*engine_slot))) {
        return false;
    }

    std::uint8_t* engine = *engine_slot;
    if (engine == nullptr) {
        return false;
    }
    if (!IsReadableMemory(engine + 0x334, sizeof(void*))) {
        return false;
    }

    auto* city_state = *reinterpret_cast<std::uint8_t**>(engine + 0x334);
    if (city_state == nullptr) {
        return false;
    }
    if (!IsReadableMemory(city_state + 0x10, sizeof(void*))) {
        return false;
    }

    auto* lod_manager = *reinterpret_cast<std::uint8_t**>(city_state + 0x10);
    if (lod_manager == nullptr) {
        return false;
    }
    if (!IsReadableMemory(lod_manager, sizeof(void*))) {
        return false;
    }

    *out_manager = lod_manager;
    return true;
}

bool TryGetEngineLodThreshold(EngineGetLodThresholdFn getter, void* manager, int category, int mask, float* out_value)
{
    if (getter == nullptr || manager == nullptr || out_value == nullptr) {
        return false;
    }

    __try {
        *out_value = getter(manager, category, mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySetEngineLodThreshold(EngineSetLodThresholdFn setter, void* manager, int category, int mask, float value)
{
    if (setter == nullptr || manager == nullptr) {
        return false;
    }

    __try {
        setter(manager, category, mask, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void MarkEngineLodThresholdsDirty(const ModuleRange& range)
{
    if (!IsRvaInRange(range, kLodDirtyFlagRva, sizeof(std::uint8_t))) {
        return;
    }

    auto* dirty_flag = range.base + kLodDirtyFlagRva;
    const std::uint8_t dirty = 1;
    WriteMemory(dirty_flag, &dirty, sizeof(dirty));
}

struct LodCategoryState
{
    void* manager;
    bool applied;
    float original[5];
    float written[5];
    LONG log_count;
};

LodCategoryState g_lod_state[kLodCategoryCount] = {};

float GetTargetFovScalar();

// Vehicle LOD metric constant for a mid-size car (Desanne) in the current projection. Measured
// K = 57.5 at 21:9 with the ultrawide FOV scaling, where P00 * P11 = 3.33 (research:
// draw-distance.md); K scales with P00 * P11. The metric is about K / (d - 7.5)^2.
float VehicleLodMetricConstant()
{
    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    constexpr float kStockFovScalar = 0.017453f;
    const float aspect = g_ultrawide_enabled && g_current_aspect > 0.0f ? g_current_aspect : 16.0f / 9.0f;
    float fov_scale = 1.0f;
    if (g_ultrawide_enabled && g_fov_scaling) {
        const float scalar = GetTargetFovScalar();
        if (scalar > 0.0f) {
            fov_scale = scalar / kStockFovScalar;
        }
    }
    const float horizontal_fov = std::min(60.0f * fov_scale, 170.0f) * kDegreesToRadians;
    const float p00 = 1.0f / std::tan(horizontal_fov * 0.5f);
    const float p11 = aspect * p00;
    return 57.5f * (p00 * p11) / 3.33f;
}

float VehicleLodThresholdForDistance(float distance)
{
    const float d = std::max(distance - 7.5f, 10.0f);
    return VehicleLodMetricConstant() / (d * d);
}

// Spawned cars appear anywhere on a newly activated road, up to about the radius + 320 m
// (stock: 318 to 666 m). The cull has to lie beyond that, or cars pop in at the cull line.
float VehicleMinimumCullDistance()
{
    return g_traffic_radius_patch_applied ? g_traffic_spawn_radius + 350.0f : 0.0f;
}

void ScaleLodThresholds(const float original[5], float target[5], float scale)
{
    for (int slot = 0; slot < 5; ++slot) {
        target[slot] = original[slot];
        if (std::isfinite(original[slot]) && original[slot] > 0.0f) {
            target[slot] = std::min(original[slot] * scale, 500000.0f);
        }
    }
}

bool VehicleLodEnabled()
{
    return g_vehicle_full_detail || g_vehicle_draw_distance > 0.0f ||
        std::fabs(g_vehicle_lod_threshold_scale - 1.0f) >= 0.001f || VehicleMinimumCullDistance() > 0.0f;
}

bool LodCategoryEnabled(int category)
{
    switch (category) {
    case 0: return std::fabs(g_map_detail_lod_threshold_scale - 1.0f) >= 0.001f;
    case 2: return VehicleLodEnabled();
    default: return std::fabs(g_lod_engine_threshold_scale - 1.0f) >= 0.001f;
    }
}

void ComputeLodTargets(int category, const float original[5], float target[5])
{
    if (category == 0) {
        ScaleLodThresholds(original, target, g_map_detail_lod_threshold_scale);
        return;
    }
    if (category != 2) {
        ScaleLodThresholds(original, target, g_lod_engine_threshold_scale);
        return;
    }

    // With full detail the scale would only move the cull; VehicleDrawDistance does that.
    ScaleLodThresholds(original, target, g_vehicle_full_detail ? 1.0f : g_vehicle_lod_threshold_scale);
    // Slot 3 (the mask-1 hull) holds the cull threshold: below it nothing is drawn.
    float cull = target[3];
    if (g_vehicle_draw_distance > 0.0f) {
        cull = VehicleLodThresholdForDistance(g_vehicle_draw_distance);
    }
    const float minimum_cull = VehicleMinimumCullDistance();
    if (minimum_cull > 0.0f) {
        cull = std::min(cull, VehicleLodThresholdForDistance(minimum_cull));
    }
    target[3] = cull;
    for (int slot = 0; slot < 3; ++slot) {
        // Full detail: slot 0 (mask 4) matches everything that is drawn at all.
        target[slot] = g_vehicle_full_detail ? cull : std::max(target[slot], cull);
    }
    target[4] = std::min(target[4], cull);
}

// Applies one LOD category and re-applies it whenever the game rewrites it: a graphics-settings
// change (0x6FCC90 -> 0xBAB070) restores the preset, which shows up as values that differ from
// the ones written last.
void ApplyLodCategory(const ModuleRange& range, int category, const char* source)
{
    if (!LodCategoryEnabled(category) ||
        !IsRvaInRange(range, kEngineGetLodThresholdRva, 1) ||
        !IsRvaInRange(range, kEngineSetLodThresholdRva, 1)) {
        return;
    }

    void* manager = nullptr;
    if (!ResolveEngineLodManager(range, &manager)) {
        return;
    }

    auto getter = reinterpret_cast<EngineGetLodThresholdFn>(range.base + kEngineGetLodThresholdRva);
    auto setter = reinterpret_cast<EngineSetLodThresholdFn>(range.base + kEngineSetLodThresholdRva);
    LodCategoryState& state = g_lod_state[category];

    float current[5] = {};
    bool have_value = false;
    for (int slot = 0; slot < 5; ++slot) {
        if (!TryGetEngineLodThreshold(getter, manager, category, kLodSlotKeys[slot], &current[slot])) {
            return;
        }
        have_value = have_value || (std::isfinite(current[slot]) && current[slot] > 0.0f);
    }
    if (!have_value) {
        return;
    }
    if (state.applied && state.manager == manager && std::memcmp(current, state.written, sizeof(current)) == 0) {
        return;
    }

    const bool reapply = state.applied;
    // Per slot: a value we wrote keeps its stored preset value, anything else is a new preset
    // value (the game writes the records one by one, so a check can land mid-rewrite).
    float original[5] = {};
    for (int slot = 0; slot < 5; ++slot) {
        const bool ours = reapply && state.manager == manager && FloatBits(current[slot]) == FloatBits(state.written[slot]);
        original[slot] = ours ? state.original[slot] : current[slot];
    }
    float target[5] = {};
    ComputeLodTargets(category, original, target);
    for (int slot = 0; slot < 5; ++slot) {
        if (FloatBits(target[slot]) != FloatBits(current[slot]) &&
            !TrySetEngineLodThreshold(setter, manager, category, kLodSlotKeys[slot], target[slot])) {
            Log("LOD: setter failed category=%s slot=%d", kLodCategoryNames[category], slot);
            return;
        }
    }
    float after[5] = {};
    for (int slot = 0; slot < 5; ++slot) {
        if (!TryGetEngineLodThreshold(getter, manager, category, kLodSlotKeys[slot], &after[slot])) {
            after[slot] = target[slot];
        }
    }

    MarkEngineLodThresholdsDirty(range);
    std::memcpy(state.original, original, sizeof(original));
    std::memcpy(state.written, after, sizeof(after));
    state.manager = manager;
    state.applied = true;
    if (InterlockedIncrement(&state.log_count) <= 24) {
        Log(
            "LOD: %s category=%s/%d source=%s manager=0x%p T0..T4 (draw mask 4, 8, 16, 1, none) "
            "[%.6g, %.6g, %.6g, %.6g, %.6g] -> [%.6g, %.6g, %.6g, %.6g, %.6g]",
            reapply ? "re-applied after the game rewrote" : "applied",
            kLodCategoryNames[category], category, source != nullptr ? source : "Loop", manager,
            static_cast<double>(original[0]), static_cast<double>(original[1]), static_cast<double>(original[2]),
            static_cast<double>(original[3]), static_cast<double>(original[4]),
            static_cast<double>(after[0]), static_cast<double>(after[1]), static_cast<double>(after[2]),
            static_cast<double>(after[3]), static_cast<double>(after[4])
        );
        if (category == 2) {
            const float k = VehicleLodMetricConstant();
            auto distance = [k](float threshold) {
                return threshold > 0.0f ? 7.5 + std::sqrt(static_cast<double>(k / threshold)) : 0.0;
            };
            Log(
                "LOD: Vehicle K=%.2f mid-size car: mask 4 to %.0f m, mask 8 to %.0f m, mask 16 to %.0f m, culled beyond %.0f m",
                static_cast<double>(k), distance(after[0]), distance(after[1]), distance(after[2]), distance(after[3])
            );
        }
    }
}

void ApplyLodThresholds(const ModuleRange& range, const char* source)
{
    for (int category = 0; category < kLodCategoryCount; ++category) {
        ApplyLodCategory(range, category, source);
    }
}

bool ResolveImposterManager(const ModuleRange& range, void** out_manager)
{
    if (out_manager == nullptr) {
        return false;
    }
    *out_manager = nullptr;

    if (!IsRvaInRange(range, kEngineSingletonRva, sizeof(void*))) {
        return false;
    }

    auto** engine_slot = reinterpret_cast<std::uint8_t**>(range.base + kEngineSingletonRva);
    if (!IsReadableMemory(engine_slot, sizeof(*engine_slot))) {
        return false;
    }

    std::uint8_t* engine = *engine_slot;
    if (engine == nullptr || !IsReadableMemory(engine + 0x18, sizeof(void*))) {
        return false;
    }

    auto* imposter_manager = *reinterpret_cast<std::uint8_t**>(engine + 0x18);
    if (imposter_manager == nullptr || !IsReadableMemory(imposter_manager, sizeof(void*))) {
        return false;
    }

    *out_manager = imposter_manager;
    return true;
}

bool TryApplyImposterDistance(ImposterApplyDistanceFn apply_distance, void* bank, float base_distance, float draw_distance)
{
    if (apply_distance == nullptr || bank == nullptr) {
        return false;
    }

    __try {
        apply_distance(bank, base_distance, draw_distance);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyImposterDrawDistancePatch(const ModuleRange& range, const char* source)
{
    if (g_imposter_draw_distance < 0.0f) {
        return;
    }

    if (!IsRvaInRange(range, kImposterApplyDistanceRva, 1)) {
        return;
    }

    void* manager = nullptr;
    if (!ResolveImposterManager(range, &manager)) {
        const LONG count = InterlockedIncrement(&g_imposter_log_count);
        if (count <= 3) {
            Log("Imposter: manager not ready from %s", source != nullptr ? source : "Loop");
        }
        return;
    }

    if (manager != g_imposter_last_manager) {
        g_imposter_last_manager = manager;
        g_imposter_distance_patch_applied = false;
        Log("Imposter: manager=0x%p from %s", manager, source != nullptr ? source : "Loop");
    }

    if (g_imposter_distance_patch_applied) {
        return;
    }

    auto apply_distance = reinterpret_cast<ImposterApplyDistanceFn>(range.base + kImposterApplyDistanceRva);
    auto* base = reinterpret_cast<std::uint8_t*>(manager);
    constexpr int kBankCount = 2;
    constexpr int kBankStride = 0x43F0;
    constexpr int kActiveOffset = 0x43B9;
    constexpr int kCountOffset = 0x43C4;
    bool saw_active_bank = false;

    for (int bank_index = 0; bank_index < kBankCount; ++bank_index) {
        auto* bank = base + bank_index * kBankStride;
        if (!IsReadableMemory(bank + kActiveOffset, 1) || !IsReadableMemory(bank + kCountOffset, sizeof(int))) {
            continue;
        }

        const bool active = bank[kActiveOffset] != 0;
        const int raw_count = *reinterpret_cast<int*>(bank + kCountOffset);
        Log(
            "Imposter: bank=%d active=%d raw_count=%d",
            bank_index,
            active ? 1 : 0,
            raw_count
        );

        if (!active) {
            continue;
        }

        saw_active_bank = true;
        if (!TryApplyImposterDistance(apply_distance, bank, -1.0f, g_imposter_draw_distance)) {
            Log("Imposter: native setter failed bank=%d draw=%.3f", bank_index, static_cast<double>(g_imposter_draw_distance));
        } else {
            Log("Imposter: native setter bank=%d base=-1.000 draw=%.3f", bank_index, static_cast<double>(g_imposter_draw_distance));
        }
    }

    if (!saw_active_bank) {
        Log("Imposter: no active banks yet from %s, retrying", source != nullptr ? source : "Loop");
        return;
    }

    g_imposter_distance_patch_applied = true;
}

bool PatchImportByName(const ModuleRange& range, const char* dll_name, const char* import_name, void* replacement, void** original)
{
    if (range.base == nullptr) {
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(range.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(range.base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports_dir.VirtualAddress == 0 || imports_dir.Size == 0) {
        return false;
    }

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(range.base + imports_dir.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        const char* current_dll = reinterpret_cast<const char*>(range.base + imports->Name);
        if (_stricmp(current_dll, dll_name) != 0) {
            continue;
        }

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(range.base + imports->FirstThunk);
        auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(range.base + imports->OriginalFirstThunk);
        for (; original_thunk->u1.AddressOfData != 0; ++thunk, ++original_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) {
                continue;
            }

            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(range.base + original_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), import_name) != 0) {
                continue;
            }

            if (original != nullptr) {
                *original = reinterpret_cast<void*>(thunk->u1.Function);
            }

            void* patch_address = &thunk->u1.Function;
            return WriteMemory(patch_address, &replacement, sizeof(replacement));
        }
    }

    return false;
}

void UpdateBackbuffer(UINT width, UINT height, const char* source)
{
    if (width == 0 || height == 0) {
        return;
    }

    g_backbuffer_width = width;
    g_backbuffer_height = height;
    g_current_aspect = g_target_aspect > 0.0f ? g_target_aspect : static_cast<float>(width) / static_cast<float>(height);

    Log(
        "%s: backbuffer=%ux%u aspect=%.6f effective_aspect=%.6f",
        source,
        width,
        height,
        static_cast<double>(static_cast<float>(width) / static_cast<float>(height)),
        static_cast<double>(g_current_aspect)
    );
}

void ApplyAspectConstantPatch(const ModuleRange& range, const char* source)
{
    if (!g_ultrawide_enabled || g_current_aspect <= 0.0f || range.base == nullptr) {
        return;
    }

    if (!IsRvaInRange(range, kAspect16x9Rva, sizeof(float))) {
        Log("%s: aspect constant RVA 0x%08lX is outside the loaded image", source, static_cast<unsigned long>(kAspect16x9Rva));
        return;
    }

    auto* aspect_constant = reinterpret_cast<float*>(range.base + kAspect16x9Rva);
    const DWORD current_bits = *reinterpret_cast<DWORD*>(aspect_constant);
    if (current_bits != kOriginalAspect16x9Bits && !g_aspect_constant_patched) {
        Log(
            "%s: aspect constant mismatch at RVA 0x%08lX current=0x%08lX expected=0x%08lX",
            source,
            static_cast<unsigned long>(kAspect16x9Rva),
            static_cast<unsigned long>(current_bits),
            static_cast<unsigned long>(kOriginalAspect16x9Bits)
        );
        return;
    }

    if (WriteMemory(aspect_constant, &g_current_aspect, sizeof(g_current_aspect))) {
        if (!g_aspect_constant_patched || current_bits != FloatBits(g_current_aspect)) {
            Log(
                "%s: patched 16:9 aspect constant RVA 0x%08lX to %.6f",
                source,
                static_cast<unsigned long>(kAspect16x9Rva),
                static_cast<double>(g_current_aspect)
            );
        }
        g_aspect_constant_patched = true;
    } else {
        Log("%s: failed to patch aspect constant", source);
    }
}

float GetTargetFovScalar()
{
    if (!g_fov_scaling) {
        return 0.0f;
    }

    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    if (g_fov_scalar > 0.0f) {
        return g_fov_scalar;
    }

    const float aspect = g_current_aspect > 0.0f ? g_current_aspect : 16.0f / 9.0f;
    return kDegreesToRadians * (aspect / (16.0f / 9.0f));
}

void ApplyGlobalFovScalarPatch(const ModuleRange& range, const char* source)
{
    if (!g_ultrawide_enabled || !g_fov_scaling || range.base == nullptr) {
        return;
    }

    if (!IsRvaInRange(range, kGlobalFovScalarRva, sizeof(float))) {
        Log("%s: global FOV scalar RVA 0x%08lX is outside the loaded image", source, static_cast<unsigned long>(kGlobalFovScalarRva));
        return;
    }

    auto* fov_scalar = reinterpret_cast<float*>(range.base + kGlobalFovScalarRva);
    const float target = GetTargetFovScalar();
    if (target <= 0.0f) {
        return;
    }

    const DWORD current_bits = *reinterpret_cast<DWORD*>(fov_scalar);
    const DWORD target_bits = FloatBits(target);
    if (current_bits == target_bits) {
        return;
    }

    if (current_bits != kOriginalFovScalarBits && !g_global_fov_scalar_patched) {
        Log(
            "%s: global FOV scalar mismatch at RVA 0x%08lX current=0x%08lX expected=0x%08lX target=%.6f",
            source,
            static_cast<unsigned long>(kGlobalFovScalarRva),
            static_cast<unsigned long>(current_bits),
            static_cast<unsigned long>(kOriginalFovScalarBits),
            static_cast<double>(target)
        );
        return;
    }

    if (WriteMemory(fov_scalar, &target, sizeof(target))) {
        Log(
            "%s: patched global FOV scalar RVA 0x%08lX %.6f -> %.6f",
            source,
            static_cast<unsigned long>(kGlobalFovScalarRva),
            static_cast<double>(*reinterpret_cast<const float*>(&current_bits)),
            static_cast<double>(target)
        );
        g_global_fov_scalar_patched = true;
    } else {
        Log("%s: failed to patch global FOV scalar", source);
    }
}

bool ContainsInsensitive(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = std::strlen(needle);
    for (const char* cursor = haystack; *cursor != '\0'; ++cursor) {
        if (_strnicmp(cursor, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

bool IsCinematicBinkPath(const char* path)
{
    return ContainsInsensitive(path, "fmv/cinematics/") ||
        ContainsInsensitive(path, "fmv\\cinematics\\");
}

const char* BinkSourceForLog(const char* file_name, char* buffer, size_t buffer_size)
{
    if (file_name == nullptr) {
        return "(null)";
    }

    if (IsReadableMemory(file_name, 4)) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(file_name);
        if (data[0] == 'B' && data[1] == 'I' && data[2] == 'K' && data[3] == 'i') {
            std::snprintf(buffer, buffer_size, "(memory:unknown-bik)");
            return buffer;
        }
    }

    return file_name;
}

DWORD RvaFromAddress(void* address, const ModuleRange& range)
{
    if (address == nullptr || range.base == nullptr) {
        return 0;
    }

    auto* pointer = reinterpret_cast<std::uint8_t*>(address);
    if (pointer < range.base || pointer >= range.base + range.size) {
        return 0;
    }

    return static_cast<DWORD>(pointer - range.base);
}

void TrackBinkHandle(void* handle, bool cinematic)
{
    if (handle == nullptr) return;
    AcquireSRWLockExclusive(&g_bink_handle_lock);
    for (auto& record : g_bink_handles) {
        if (record.handle == nullptr || record.handle == handle) {
            record = {handle, cinematic};
            ReleaseSRWLockExclusive(&g_bink_handle_lock);
            return;
        }
    }
    ReleaseSRWLockExclusive(&g_bink_handle_lock);
}

bool IsTrackedCinematicHandle(void* handle)
{
    bool cinematic = false;
    AcquireSRWLockShared(&g_bink_handle_lock);
    for (const auto& record : g_bink_handles) {
        if (record.handle == handle) {
            cinematic = record.cinematic;
            break;
        }
    }
    ReleaseSRWLockShared(&g_bink_handle_lock);
    return cinematic;
}

#include "cinematic_movie_context.inl"

bool LooksLikeDistanceValue(float value)
{
    const float magnitude = std::fabs(value);
    return std::isfinite(value) && magnitude >= 25.0f && magnitude <= 100000.0f;
}

bool ScalePixelShaderDistanceConstants(
    UINT start_register,
    const float* constant_data,
    UINT vector4f_count,
    float* patched_constants,
    size_t patched_capacity
)
{
    if (g_lod_distance_scale == 1.0f || constant_data == nullptr || patched_constants == nullptr || vector4f_count == 0) {
        return false;
    }

    const size_t total_floats = static_cast<size_t>(vector4f_count) * 4;
    if (total_floats > patched_capacity) {
        return false;
    }

    bool patched_any = false;
    std::memcpy(patched_constants, constant_data, total_floats * sizeof(float));
    for (UINT i = 0; i < vector4f_count; ++i) {
        const UINT reg = start_register + i;
        if (reg < 10 || reg > 15) {
            continue;
        }

        float* v = patched_constants + (static_cast<size_t>(i) * 4);
        const bool scalar_distance =
            LooksLikeDistanceValue(v[0]) &&
            std::fabs(v[1]) < 0.0001f &&
            std::fabs(v[2]) < 0.0001f &&
            std::fabs(v[3]) < 0.0001f;
        if (!scalar_distance) {
            continue;
        }

        const float old_value = v[0];
        v[0] *= g_lod_distance_scale;
        patched_any = true;

        const LONG index = InterlockedIncrement(&g_lod_distance_scale_log_count);
        if (index <= 96) {
            Log(
                "LOD.Scale[%ld]: PS r%u %.3f -> %.3f scale=%.3f",
                static_cast<long>(index),
                reg,
                static_cast<double>(old_value),
                static_cast<double>(v[0]),
                static_cast<double>(g_lod_distance_scale)
            );
        }
    }

    return patched_any;
}

void __stdcall PatchMovieDrawArgs(std::uintptr_t entry_esp, void* movie_player)
{
    if (!g_ultrawide_enabled || !g_cinematic_draw_patch) return;
    const MovieDrawContext movie = ReadMovieDrawContext(entry_esp, movie_player);
    if (!movie.cinematic_movie || !movie.screen_rectangle || !movie.viewport_known) return;

    auto* args = reinterpret_cast<cinematic::Rect*>(entry_esp + 0x08);
    if (!IsCommittedWritableMemory(args, sizeof(*args))) return;
    const cinematic::Rect before = *args;
    cinematic::Rect fitted = before;
    const LONG index = InterlockedIncrement(&g_cinematic_movie_draw_count);
    if (!FitMovieDrawRectangle(movie, fitted)) return;
    *args = fitted;

    if (InterlockedCompareExchange(&g_cinematic_movie_draw_log_count, 1, 0) == 0) {
        Log(
            "CinematicMovieDraw[%ld]: handle=0x%p source=%lux%lu destination=%u,%u,%u,%u rect=%.6f,%.6f,%.6f,%.6f fitted=%.6f,%.6f,%.6f,%.6f",
            static_cast<long>(index), movie.handle,
            static_cast<unsigned long>(movie.movie_width), static_cast<unsigned long>(movie.movie_height),
            movie.viewport.x, movie.viewport.y, movie.viewport.width, movie.viewport.height,
            static_cast<double>(before.x), static_cast<double>(before.y),
            static_cast<double>(before.width), static_cast<double>(before.height),
            static_cast<double>(fitted.x), static_cast<double>(fitted.y),
            static_cast<double>(fitted.width), static_cast<double>(fitted.height));
    }
}

bool AutoAcknowledgeUbisoftServerLostPrompt(void* message_page)
{
    if (!g_suppress_ubisoft_server_lost_prompt || message_page == nullptr) {
        return false;
    }

    constexpr wchar_t message_id[] = L"ID:249127";
    auto* base = static_cast<std::uint8_t*>(message_page);
    auto* message = reinterpret_cast<const wchar_t*>(base + 0x218);
    if (!IsReadableMemory(message, sizeof(message_id)) || std::wcscmp(message, message_id) != 0) {
        return false;
    }

    using ResponseCallbackFn = void (__cdecl*)(void*, void*, int, void*);
    if (!IsReadableMemory(base + 0x04, 0x10)) {
        return false;
    }

    auto callback = *reinterpret_cast<ResponseCallbackFn*>(base + 0x10);
    if (callback == nullptr) {
        Log("Startup: matched obsolete Ubisoft server disconnect prompt but response callback was null");
        return false;
    }

    if (InterlockedIncrement(&g_ubisoft_server_lost_prompt_log_count) <= 4) {
        Log("Startup: auto-acknowledging obsolete Ubisoft server disconnect prompt ID:249127");
    }
    void* context = *reinterpret_cast<void**>(base + 0x04);
    callback(context, base + 0x08, 1, message_page);
    return true;
}

__declspec(naked) void ProxyErrorMessageUpdate()
{
    __asm {
        push ecx
        push ecx
        call AutoAcknowledgeUbisoftServerLostPrompt
        add esp, 4
        pop ecx
        test al, al
        jz pass_through
        mov eax, 3
        ret 4

    pass_through:
        push ecx
        push ebx
        push esi
        mov esi, ecx
        jmp dword ptr [g_error_message_update_continue]
    }
}

__declspec(naked) void ProxyMovieDraw()
{
    __asm {
        mov eax, esp
        pushad
        push ecx
        push eax
        call PatchMovieDrawArgs
        popad
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        jmp dword ptr [g_movie_draw_continue]
    }
}

void ResetCinematicDiagnostics()
{
    InterlockedExchange(&g_cinematic_movie_draw_count, 0);
    InterlockedExchange(&g_cinematic_movie_draw_log_count, 0);
}

void LogCinematicDiagnostics(const char* source)
{
    Log(
        "%s: MovieDraw calls=%ld",
        source,
        static_cast<long>(g_cinematic_movie_draw_count)
    );
}

bool UntrackBinkHandle(void* handle)
{
    if (handle == nullptr) return false;
    bool cinematic = false;
    AcquireSRWLockExclusive(&g_bink_handle_lock);
    for (auto& record : g_bink_handles) {
        if (record.handle == handle) {
            cinematic = record.cinematic;
            record = {};
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_bink_handle_lock);
    return cinematic;
}

void* WINAPI ProxyBinkOpen(const char* file_name, UINT flags)
{
    void* handle = g_real_bink_open != nullptr ? g_real_bink_open(file_name, flags) : nullptr;
    const bool cinematic = IsCinematicBinkPath(file_name);
    if (handle != nullptr) {
        InterlockedIncrement(&g_active_bink_count);
        if (cinematic) {
            ResetCinematicDiagnostics();
            InterlockedIncrement(&g_active_cinematic_bink_count);
        }
        TrackBinkHandle(handle, cinematic);
    }

    DWORD width = 0;
    DWORD height = 0;
    if (IsReadableMemory(handle, sizeof(DWORD) * 2)) {
        width = *reinterpret_cast<DWORD*>(handle);
        height = *reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(handle) + sizeof(DWORD));
    }

    char source_buffer[128] = {};
    Log(
        "BinkOpen: handle=0x%p cinematic=%d active=%ld cinematic_active=%ld size=%lux%lu flags=0x%08lX file=%s",
        handle,
        cinematic ? 1 : 0,
        static_cast<long>(g_active_bink_count),
        static_cast<long>(g_active_cinematic_bink_count),
        static_cast<unsigned long>(width),
        static_cast<unsigned long>(height),
        static_cast<unsigned long>(flags),
        BinkSourceForLog(file_name, source_buffer, sizeof(source_buffer))
    );
    return handle;
}

void WINAPI ProxyBinkClose(void* handle)
{
    const bool cinematic = UntrackBinkHandle(handle);
    if (handle != nullptr) {
        InterlockedDecrement(&g_active_bink_count);
        if (cinematic) {
            InterlockedDecrement(&g_active_cinematic_bink_count);
        }
    }

    Log(
        "BinkClose: handle=0x%p cinematic=%d active=%ld cinematic_active=%ld",
        handle,
        cinematic ? 1 : 0,
        static_cast<long>(g_active_bink_count),
        static_cast<long>(g_active_cinematic_bink_count)
    );
    if (cinematic) {
        LogCinematicDiagnostics("BinkClose");
        RehookTrackedD3DDevice("BinkClose");
    }

    if (g_real_bink_close != nullptr) {
        g_real_bink_close(handle);
    }
}

HRESULT STDMETHODCALLTYPE ProxyPresent(
    IDirect3DDevice9* device,
    const RECT* source_rect,
    const RECT* dest_rect,
    HWND dest_window,
    const RGNDATA* dirty_region
)
{
    InterlockedIncrement(&g_present_count);
    return g_real_present(device, source_rect, dest_rect, dest_window, dirty_region);
}

HRESULT STDMETHODCALLTYPE ProxySetPixelShaderConstantF(
    IDirect3DDevice9* device,
    UINT start_register,
    const float* constant_data,
    UINT vector4f_count
)
{
    if (g_real_set_pixel_shader_constant_f == nullptr) {
        return D3DERR_INVALIDCALL;
    }

    float patched_constants[256] = {};
    if (ScalePixelShaderDistanceConstants(
            start_register,
            constant_data,
            vector4f_count,
            patched_constants,
            sizeof(patched_constants) / sizeof(patched_constants[0]))) {
        return g_real_set_pixel_shader_constant_f(device, start_register, patched_constants, vector4f_count);
    }

    return g_real_set_pixel_shader_constant_f(device, start_register, constant_data, vector4f_count);
}

HWND GetPresentationWindow(D3DPRESENT_PARAMETERS* presentation_parameters, HWND fallback)
{
    if (presentation_parameters != nullptr && presentation_parameters->hDeviceWindow != nullptr) {
        return presentation_parameters->hDeviceWindow;
    }
    if (fallback != nullptr) {
        return fallback;
    }
    return GetActiveWindow();
}

void ApplyBorderlessWindow(HWND window, D3DPRESENT_PARAMETERS* presentation_parameters)
{
    if (!g_borderless_windowed || window == nullptr || presentation_parameters == nullptr) {
        return;
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor == nullptr || !GetMonitorInfoA(monitor, &monitor_info)) {
        monitor_info.rcMonitor.left = 0;
        monitor_info.rcMonitor.top = 0;
        monitor_info.rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
        monitor_info.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const RECT rect = monitor_info.rcMonitor;
    const UINT width = static_cast<UINT>(rect.right - rect.left);
    const UINT height = static_cast<UINT>(rect.bottom - rect.top);

    presentation_parameters->Windowed = TRUE;
    presentation_parameters->FullScreen_RefreshRateInHz = 0;
    presentation_parameters->BackBufferWidth = width;
    presentation_parameters->BackBufferHeight = height;
    if (presentation_parameters->hDeviceWindow == nullptr) {
        presentation_parameters->hDeviceWindow = window;
    }

    SetMenu(window, nullptr);
    SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongA(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(
        window,
        HWND_TOP,
        rect.left,
        rect.top,
        static_cast<int>(width),
        static_cast<int>(height),
        SWP_FRAMECHANGED | SWP_SHOWWINDOW
    );
    ShowWindow(window, SW_SHOW);

    Log("BorderlessWindowed: applied %ux%u at (%ld,%ld)", width, height, rect.left, rect.top);
}

void ApplyPresentationTweaks(D3DPRESENT_PARAMETERS* presentation_parameters, HWND window, const char* source)
{
    if (presentation_parameters == nullptr) {
        return;
    }

    if (g_borderless_windowed) {
        ApplyBorderlessWindow(window, presentation_parameters);
    }

    if (g_remove_vsync) {
        const UINT old_interval = presentation_parameters->PresentationInterval;
        presentation_parameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        Log("%s: PresentationInterval %u -> %u", source, old_interval, presentation_parameters->PresentationInterval);
    }

    UpdateBackbuffer(presentation_parameters->BackBufferWidth, presentation_parameters->BackBufferHeight, source);
}

void ApplyUltrawideInstrumentation()
{
    if (!g_ultrawide_enabled) {
        Log("Ultrawide: disabled");
        return;
    }

    Log(
        "Ultrawide: instrumentation active FOVScaling=%d target_aspect=%.6f current_aspect=%.6f",
        g_fov_scaling ? 1 : 0,
        static_cast<double>(g_target_aspect),
        static_cast<double>(g_current_aspect)
    );

    const ModuleRange exe = GetExeRange();
    ApplyAspectConstantPatch(exe, "Ultrawide");
    ApplyGlobalFovScalarPatch(exe, "Ultrawide");
}

DWORD CallerRva(void* caller)
{
    return RvaFromAddress(caller, GetExeRange());
}


void TrackLastD3DDevice(IDirect3DDevice9* device)
{
    if (device != nullptr) {
        device->AddRef();
    }

    auto* old_device = static_cast<IDirect3DDevice9*>(
        InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(&g_last_d3d_device),
            device
        )
    );
    if (old_device != nullptr) {
        old_device->Release();
    }
}

void RehookTrackedD3DDevice(const char* source)
{
    auto* device = static_cast<IDirect3DDevice9*>(g_last_d3d_device);
    if (device == nullptr) {
        Log("%s: no tracked D3D device to rehook", source != nullptr ? source : "D3D");
        return;
    }

    device->AddRef();
    HookDeviceMethods(device);
    device->Release();
}

void ApplySamplerQuality(IDirect3DDevice9* device, const char* source)
{
    if (device == nullptr || g_real_set_sampler_state == nullptr) {
        return;
    }

    const DWORD anisotropy = static_cast<DWORD>(g_anisotropic_filtering);
    for (DWORD sampler = 0; sampler < 16; ++sampler) {
        if (g_anisotropic_filtering > 1) {
            g_real_set_sampler_state(device, sampler, D3DSAMP_MAXANISOTROPY, anisotropy);
        }

        if (g_force_trilinear_filtering) {
            if (g_anisotropic_filtering > 1) {
                g_real_set_sampler_state(device, sampler, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
                g_real_set_sampler_state(device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
            } else {
                g_real_set_sampler_state(device, sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                g_real_set_sampler_state(device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            }
            g_real_set_sampler_state(device, sampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        }

        if (g_force_highest_mip) {
            g_real_set_sampler_state(device, sampler, D3DSAMP_MAXMIPLEVEL, 0);
        }

        if (g_texture_lod_bias != 0.0f) {
            g_real_set_sampler_state(device, sampler, D3DSAMP_MIPMAPLODBIAS, FloatBits(g_texture_lod_bias));
        }
    }

    const LONG count = InterlockedIncrement(&g_sampler_quality_log_count);
    if (count <= 8) {
        Log(
            "%s: applied sampler quality anisotropy=%lu trilinear=%d highest_mip=%d lod_bias=%.3f to samplers 0-15",
            source,
            static_cast<unsigned long>(anisotropy),
            g_force_trilinear_filtering ? 1 : 0,
            g_force_highest_mip ? 1 : 0,
            static_cast<double>(g_texture_lod_bias)
        );
    }
}

HRESULT STDMETHODCALLTYPE ProxySetSamplerState(
    IDirect3DDevice9* device,
    DWORD sampler,
    D3DSAMPLERSTATETYPE type,
    DWORD value
)
{
    if (g_real_set_sampler_state == nullptr) {
        return D3DERR_INVALIDCALL;
    }

    if (sampler < 16) {
        const DWORD anisotropy = static_cast<DWORD>(g_anisotropic_filtering);
        if (g_anisotropic_filtering > 1) {
            if (type == D3DSAMP_MAXANISOTROPY && value < anisotropy) {
                value = anisotropy;
            } else if (type == D3DSAMP_MINFILTER) {
                if (value == D3DTEXF_LINEAR || value == D3DTEXF_ANISOTROPIC ||
                    (g_force_trilinear_filtering && value == D3DTEXF_POINT)) {
                    value = D3DTEXF_ANISOTROPIC;
                }
            } else if (g_force_trilinear_filtering && type == D3DSAMP_MAGFILTER &&
                (value == D3DTEXF_POINT || value == D3DTEXF_LINEAR || value == D3DTEXF_ANISOTROPIC)) {
                value = D3DTEXF_ANISOTROPIC;
            }
        } else if (g_force_trilinear_filtering &&
            (type == D3DSAMP_MINFILTER || type == D3DSAMP_MAGFILTER) &&
            value == D3DTEXF_POINT) {
            value = D3DTEXF_LINEAR;
        }

        if (g_force_trilinear_filtering && type == D3DSAMP_MIPFILTER &&
            (value == D3DTEXF_NONE || value == D3DTEXF_POINT ||
             value == D3DTEXF_LINEAR || value == D3DTEXF_ANISOTROPIC)) {
            value = D3DTEXF_LINEAR;
        }

        if (g_force_highest_mip && type == D3DSAMP_MAXMIPLEVEL && value != 0) {
            value = 0;
        }

        if (g_texture_lod_bias != 0.0f && type == D3DSAMP_MIPMAPLODBIAS) {
            value = FloatBits(g_texture_lod_bias);
        }
    }

    return g_real_set_sampler_state(device, sampler, type, value);
}

HRESULT STDMETHODCALLTYPE ProxyReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentation_parameters)
{
    TrackLastD3DDevice(device);
    ApplyPresentationTweaks(presentation_parameters, GetPresentationWindow(presentation_parameters, nullptr), "Reset");
    ApplyUltrawideInstrumentation();
    const HRESULT result = g_real_reset(device, presentation_parameters);
    if (SUCCEEDED(result)) {
        ApplySamplerQuality(device, "Reset");
    }
    return result;
}

bool HookDeviceVTable(void** vtable, size_t index, void* replacement, void** original, const char* name)
{
    if (vtable == nullptr || replacement == nullptr || original == nullptr) {
        return false;
    }

    if (vtable[index] == replacement) {
        Log("D3D: IDirect3DDevice9::%s already hooked", name);
        return false;
    }

    *original = vtable[index];
    if (!WriteMemory(&vtable[index], &replacement, sizeof(replacement))) {
        *original = nullptr;
        Log("D3D: failed to hook IDirect3DDevice9::%s", name);
        return false;
    }

    Log("D3D: hooked IDirect3DDevice9::%s original=0x%p", name, *original);
    return true;
}

void HookDeviceMethods(IDirect3DDevice9* device)
{
    if (device == nullptr) {
        return;
    }

    auto*** object = reinterpret_cast<void***>(device);
    void** vtable = *object;

    void* reset_replacement = reinterpret_cast<void*>(&ProxyReset);
    void* set_sampler_state_replacement = reinterpret_cast<void*>(&ProxySetSamplerState);
    void* present_replacement = reinterpret_cast<void*>(&ProxyPresent);
    void* set_pixel_shader_constant_f_replacement = reinterpret_cast<void*>(&ProxySetPixelShaderConstantF);

    if (vtable != nullptr && vtable[16] == reset_replacement) {
        Log("D3D: device hooks already installed");
        return;
    }

    void* original_reset = nullptr;
    if (!HookDeviceVTable(vtable, 16, reset_replacement, &original_reset, "Reset")) {
        return;
    }
    g_real_reset = reinterpret_cast<ResetFn>(original_reset);
    InterlockedIncrement(&g_device_hooks_installed);

    void* original_set_sampler_state = nullptr;
    if (HookDeviceVTable(vtable, 69, set_sampler_state_replacement, &original_set_sampler_state, "SetSamplerState")) {
        g_real_set_sampler_state = reinterpret_cast<SetSamplerStateFn>(original_set_sampler_state);
        ApplySamplerQuality(device, "CreateDevice");
    }

    // Present only counts frames for TrafficDiagnostics.
    void* original_present = nullptr;
    if (g_traffic_diagnostics &&
        HookDeviceVTable(vtable, 17, present_replacement, &original_present, "Present")) {
        g_real_present = reinterpret_cast<PresentFn>(original_present);
    }

    void* original_set_pixel_shader_constant_f = nullptr;
    if (HookDeviceVTable(
            vtable,
            109,
            set_pixel_shader_constant_f_replacement,
            &original_set_pixel_shader_constant_f,
            "SetPixelShaderConstantF")) {
        g_real_set_pixel_shader_constant_f = reinterpret_cast<SetPixelShaderConstantFFn>(original_set_pixel_shader_constant_f);
    }
}

class Direct3D9Proxy final : public IDirect3D9
{
public:
    explicit Direct3D9Proxy(IDirect3D9* real) : m_real(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (ppvObj == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
            *ppvObj = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        m_real->AddRef();
        return InterlockedIncrement(&m_refs);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        m_real->Release();
        const ULONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return m_real->RegisterSoftwareDevice(pInitializeFunction); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return m_real->GetAdapterModeCount(Adapter, Format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return m_real->GetAdapterDisplayMode(Adapter, pMode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return m_real->GetAdapterMonitor(Adapter); }

    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface
    ) override
    {
        ApplyPresentationTweaks(pPresentationParameters, GetPresentationWindow(pPresentationParameters, hFocusWindow), "CreateDevice");
        ApplyUltrawideInstrumentation();

        const HRESULT result = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
        if (SUCCEEDED(result) && ppReturnedDeviceInterface != nullptr) {
            TrackLastD3DDevice(*ppReturnedDeviceInterface);
            HookDeviceMethods(*ppReturnedDeviceInterface);
        } else {
            Log("CreateDevice: failed hr=0x%08lX", static_cast<unsigned long>(result));
        }
        return result;
    }

private:
    IDirect3D9* m_real = nullptr;
    volatile LONG m_refs = 1;
};

IDirect3D9* WINAPI ProxyDirect3DCreate9(UINT sdk_version)
{
    if (g_real_direct3d_create9 == nullptr) {
        return nullptr;
    }

    IDirect3D9* real = g_real_direct3d_create9(sdk_version);
    if (real == nullptr) {
        return nullptr;
    }

    Log("D3D: wrapping IDirect3D9 from Direct3DCreate9(%u)", sdk_version);
    return new Direct3D9Proxy(real);
}

bool PatchDirect3DCreate9Import(const ModuleRange& range)
{
    void* original = nullptr;
    if (!PatchImportByName(range, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&ProxyDirect3DCreate9), &original)) {
        Log("D3D: failed to patch Direct3DCreate9 import");
        return false;
    }

    g_real_direct3d_create9 = reinterpret_cast<Direct3DCreate9Fn>(original);
    Log("D3D: patched Direct3DCreate9 import original=0x%p", original);
    return true;
}

bool ApplyMovieDrawPatch(const ModuleRange& range)
{
    if (!g_ultrawide_enabled || !g_cinematic_draw_patch) {
        return false;
    }

    constexpr size_t patch_size = 6;
    if (!IsRvaInRange(range, kMovieDrawRva, patch_size)) {
        Log("MovieDraw: RVA 0x%08lX is outside the loaded image", static_cast<unsigned long>(kMovieDrawRva));
        return false;
    }

    auto* target = range.base + kMovieDrawRva;
    const std::uint8_t expected[patch_size] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    if (std::memcmp(target, expected, patch_size) != 0) {
        Log(
            "MovieDraw: prologue mismatch at RVA 0x%08lX bytes=%02X %02X %02X %02X %02X %02X",
            static_cast<unsigned long>(kMovieDrawRva),
            target[0],
            target[1],
            target[2],
            target[3],
            target[4],
            target[5]
        );
        return false;
    }

    g_movie_draw_continue = target + patch_size;
    std::uint8_t patch[patch_size] = {};
    patch[0] = 0xE9;
    const auto relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uint8_t*>(&ProxyMovieDraw) - (target + 5)
    );
    std::memcpy(&patch[1], &relative, sizeof(relative));
    patch[5] = 0x90;

    if (!WriteMemory(target, patch, sizeof(patch))) {
        Log("MovieDraw: failed to patch RVA 0x%08lX", static_cast<unsigned long>(kMovieDrawRva));
        return false;
    }

    Log(
        "MovieDraw: patched RVA 0x%08lX target=0x%p hook=0x%p continue=0x%p",
        static_cast<unsigned long>(kMovieDrawRva),
        target,
        &ProxyMovieDraw,
        g_movie_draw_continue
    );
    return true;
}

// The resource hash arena's 45,000 stock nodes stay in use; once the bump pointer reaches their
// end, nodes come from an extension block instead. Nodes are linked by absolute pointers (hash
// chains and the free list at +0x127AC0), so they can live anywhere. A reset of the arena
// (0xBAEE50) moves the stock bump pointer back to the start; the next stock acquire then rewinds
// the extension as well.
void __stdcall ResourceArenaExtensionFull()
{
    Log("ResourceArena: extension full (%d nodes); raise [Limits] ResourceArenaExtraNodes", g_resource_arena_extra_nodes);
    FatalAppExitA(0, "DSF Enhanced: resource arena full. Raise [Limits] ResourceArenaExtraNodes in dsf_enhanced.ini.");
}

__declspec(naked) void ProxyResourceHashBump()
{
    __asm {
        mov eax, dword ptr [ecx + 0x127AC4]
        mov ebx, ecx
        add ebx, dword ptr [g_arena_stock_end_offset]
        cmp eax, ebx
        jae extension
        mov ebx, dword ptr [g_arena_extension_begin]
        cmp dword ptr [g_arena_extension_next], ebx
        je stock
        mov dword ptr [g_arena_extension_next], ebx

    stock:
        lea ebx, [eax + 0x18]
        mov dword ptr [ecx + 0x127AC4], ebx
        jmp dword ptr [g_arena_bump_continue]

    extension:
        mov eax, dword ptr [g_arena_extension_next]
        lea ebx, [eax + 0x18]
        cmp ebx, dword ptr [g_arena_extension_end]
        ja full
        mov dword ptr [g_arena_extension_next], ebx
        jmp dword ptr [g_arena_bump_continue]

    full:
        call ResourceArenaExtensionFull
    }
}

void ApplyResourceArenaPatch(const ModuleRange& range)
{
    if (g_resource_arena_extra_nodes <= 0) {
        return;
    }
    constexpr size_t patch_size = 17;
    constexpr std::uint8_t expected[patch_size] = {
        0x8B, 0x81, 0xC4, 0x7A, 0x12, 0x00,  // mov eax, [ecx+0x127AC4]
        0x8D, 0x58, 0x18,                    // lea ebx, [eax+0x18]
        0x89, 0x99, 0xC4, 0x7A, 0x12, 0x00,  // mov [ecx+0x127AC4], ebx
        0xEB, 0x09                           // jmp 0xBAEDD0
    };
    if (!IsRvaInRange(range, kResourceHashBumpRva, patch_size)) {
        Log("ResourceArena: patch site outside the loaded image");
        return;
    }
    auto* target = range.base + kResourceHashBumpRva;
    if (std::memcmp(target, expected, patch_size) != 0) {
        Log("ResourceArena: unexpected bytes at RVA 0x%08lX; not patched", static_cast<unsigned long>(kResourceHashBumpRva));
        return;
    }
    const size_t bytes = static_cast<size_t>(g_resource_arena_extra_nodes) * 0x18;
    auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (block == nullptr) {
        Log("ResourceArena: could not allocate %lu bytes for the extension", static_cast<unsigned long>(bytes));
        return;
    }
    g_arena_extension_begin = block;
    g_arena_extension_next = block;
    g_arena_extension_end = block + bytes;
    g_arena_bump_continue = range.base + kResourceHashBumpContinueRva;
    g_arena_stock_end_offset = 0x20000 + static_cast<DWORD>(g_resource_arena_stock_nodes) * 0x18;

    std::uint8_t patch[patch_size];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const auto relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uint8_t*>(&ProxyResourceHashBump) - (target + 5)
    );
    std::memcpy(&patch[1], &relative, sizeof(relative));
    if (!WriteMemory(target, patch, sizeof(patch))) {
        Log("ResourceArena: failed to patch RVA 0x%08lX", static_cast<unsigned long>(kResourceHashBumpRva));
        return;
    }
    Log(
        "ResourceArena: %d stock nodes, then %d extension nodes at 0x%p (%lu KB)",
        g_resource_arena_stock_nodes,
        g_resource_arena_extra_nodes,
        block,
        static_cast<unsigned long>(bytes / 1024)
    );
}

void LogResourceArenaUse()
{
    if (g_arena_extension_begin == nullptr) {
        return;
    }
    const auto used = static_cast<DWORD>((g_arena_extension_next - g_arena_extension_begin) / 0x18);
    if (used > g_arena_extension_logged_nodes) {
        g_arena_extension_logged_nodes = used;
        Log("ResourceArena: stock nodes full; extension high water %lu of %d", static_cast<unsigned long>(used), g_resource_arena_extra_nodes);
    }
}

// When the legal screen ends, state 4 calls the loading screen's begin function 0x4A20C0, which
// ends by clearing the resource manager's list of unresolved resource references (count at
// 0x188A6B8: mov dword [0x188A6B8],0 at 0x4A2153). After the stock 5 s wait the startup packages
// have finished loading and the list is empty. Without the wait the call comes a frame after the
// legal screen appears, while startup packages are still being fixed up, and the clear drops up
// to a few hundred references that are still waiting for their resources. The engine never
// resolves those slots, and the world load later faults on the corrupted reference lists
// (0xBAE85A, 0xBAE8BE, 0xBAEBB2). So the skip keeps the list across that one call; every other
// loading screen still clears it.
volatile LONG g_keep_pending_resource_references = 0;
DWORD* g_pending_resource_reference_count = nullptr;
DWORD g_kept_pending_resource_references = 0;
using LoadingScreenBeginFn = void (__thiscall*)(void*);
LoadingScreenBeginFn g_real_loading_screen_begin = nullptr;

__declspec(naked) void ProxyClearPendingResourceReferences()
{
    __asm {
        push eax
        mov eax, dword ptr [g_pending_resource_reference_count]
        cmp dword ptr [g_keep_pending_resource_references], 0
        jne keep
        mov dword ptr [eax], 0
        pop eax
        ret

    keep:
        mov eax, dword ptr [eax]
        mov dword ptr [g_kept_pending_resource_references], eax
        pop eax
        ret
    }
}

void __fastcall ProxyLegalScreenBeginLoading(void* loading_screen)
{
    g_kept_pending_resource_references = 0;
    InterlockedExchange(&g_keep_pending_resource_references, 1);
    g_real_loading_screen_begin(loading_screen);
    InterlockedExchange(&g_keep_pending_resource_references, 0);
    Log(
        "Startup: legal screen skipped; kept %lu pending resource references",
        static_cast<unsigned long>(g_kept_pending_resource_references)
    );
}

void ApplyStartupPatches(const ModuleRange& range)
{
    if (!g_skip_legal_screen) {
        return;
    }

    // State 4 computes elapsed time and uses this branch to keep waiting while
    // elapsed < 5.0f. Removing only that branch preserves the legal screen's
    // setup and teardown without rewriting the widely shared 5.0f constant.
    constexpr std::uint8_t expected[] = { 0x0F, 0x82, 0x89, 0x00, 0x00, 0x00 };
    constexpr std::uint8_t patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    // call 0x4A20C0 (begin loading) at the end of state 4, and the list clear inside it.
    std::uint8_t expected_call[5] = { 0xE8 };
    const std::int32_t call_relative = static_cast<std::int32_t>(kLoadingScreenBeginRva - (kLegalScreenBeginLoadingCallRva + 5));
    std::memcpy(&expected_call[1], &call_relative, sizeof(call_relative));
    std::uint8_t expected_clear[10] = { 0xC7, 0x05 };
    const DWORD count_address = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(range.base) + kPendingResourceReferenceCountRva);
    std::memcpy(&expected_clear[2], &count_address, sizeof(count_address));

    if (!IsRvaInRange(range, kLegalScreenWaitBranchRva, sizeof(patch)) ||
        !IsRvaInRange(range, kLegalScreenBeginLoadingCallRva, sizeof(expected_call)) ||
        !IsRvaInRange(range, kPendingResourceReferenceClearRva, sizeof(expected_clear))) {
        Log("Startup: legal screen patch sites are outside the loaded image");
        return;
    }

    auto* target = range.base + kLegalScreenWaitBranchRva;
    if (std::memcmp(target, patch, sizeof(patch)) == 0) {
        return;
    }

    auto* call_site = range.base + kLegalScreenBeginLoadingCallRva;
    auto* clear_site = range.base + kPendingResourceReferenceClearRva;
    if (std::memcmp(target, expected, sizeof(expected)) != 0 ||
        std::memcmp(call_site, expected_call, sizeof(expected_call)) != 0 ||
        std::memcmp(clear_site, expected_clear, sizeof(expected_clear)) != 0) {
        Log(
            "Startup: legal screen code differs (wait branch %02X %02X, call %02X, clear %02X %02X); keeping the 5 s legal screen",
            target[0], target[1], call_site[0], clear_site[0], clear_site[1]
        );
        return;
    }

    // The guard first, the skip last: without the guard the skip must not run.
    g_pending_resource_reference_count = reinterpret_cast<DWORD*>(range.base + kPendingResourceReferenceCountRva);
    g_real_loading_screen_begin = reinterpret_cast<LoadingScreenBeginFn>(range.base + kLoadingScreenBeginRva);
    std::uint8_t clear_patch[10] = { 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x44, 0x00, 0x00 };   // call stub; nop5
    const auto clear_relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uint8_t*>(&ProxyClearPendingResourceReferences) - (clear_site + 5));
    std::memcpy(&clear_patch[1], &clear_relative, sizeof(clear_relative));
    std::uint8_t call_patch[5] = { 0xE8 };
    const auto proxy_relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uint8_t*>(&ProxyLegalScreenBeginLoading) - (call_site + 5));
    std::memcpy(&call_patch[1], &proxy_relative, sizeof(proxy_relative));

    if (!WriteMemory(clear_site, clear_patch, sizeof(clear_patch))) {
        Log("Startup: failed to patch the pending resource reference clear; keeping the 5 s legal screen");
        return;
    }
    if (!WriteMemory(call_site, call_patch, sizeof(call_patch))) {
        WriteMemory(clear_site, expected_clear, sizeof(expected_clear));
        Log("Startup: failed to patch the legal screen's begin-loading call; keeping the 5 s legal screen");
        return;
    }

    if (WriteMemory(target, patch, sizeof(patch))) {
        Log(
            "Startup: bypassed legal screen wait branch at RVA 0x%08lX (pending resource references kept)",
            static_cast<unsigned long>(kLegalScreenWaitBranchRva)
        );
    } else {
        WriteMemory(call_site, expected_call, sizeof(expected_call));
        WriteMemory(clear_site, expected_clear, sizeof(expected_clear));
        Log("Startup: failed to patch legal screen wait branch");
    }
}

void ApplyUbisoftServerLostPromptPatch(const ModuleRange& range)
{
    if (!g_suppress_ubisoft_server_lost_prompt) {
        return;
    }

    constexpr size_t patch_size = 5;
    constexpr std::uint8_t expected[patch_size] = { 0x51, 0x53, 0x56, 0x8B, 0xF1 };
    if (!IsRvaInRange(range, kErrorMessageUpdateRva, patch_size)) {
        Log("Startup: error-message update RVA 0x%08lX is outside the loaded image", static_cast<unsigned long>(kErrorMessageUpdateRva));
        return;
    }

    auto* target = range.base + kErrorMessageUpdateRva;
    if (target[0] == 0xE9) {
        return;
    }

    if (std::memcmp(target, expected, patch_size) != 0) {
        Log(
            "Startup: error-message update mismatch at RVA 0x%08lX bytes=%02X %02X %02X %02X %02X",
            static_cast<unsigned long>(kErrorMessageUpdateRva),
            target[0], target[1], target[2], target[3], target[4]
        );
        return;
    }

    g_error_message_update_continue = target + patch_size;
    std::uint8_t patch[patch_size] = { 0xE9, 0, 0, 0, 0 };
    const auto relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uint8_t*>(&ProxyErrorMessageUpdate) - (target + patch_size)
    );
    std::memcpy(&patch[1], &relative, sizeof(relative));

    if (WriteMemory(target, patch, sizeof(patch))) {
        Log(
            "Startup: installed frontend update hook for the server-prompt patch at RVA 0x%08lX",
            static_cast<unsigned long>(kErrorMessageUpdateRva)
        );
    } else {
        Log("Startup: failed to install frontend update hook");
        g_error_message_update_continue = nullptr;
    }
}

void PatchBinkImports(const ModuleRange& range)
{
    void* original_open = nullptr;
    if (PatchImportByName(range, "binkw32.dll", "_BinkOpen@8", reinterpret_cast<void*>(&ProxyBinkOpen), &original_open)) {
        g_real_bink_open = reinterpret_cast<BinkOpenFn>(original_open);
        Log("Bink: patched BinkOpen original=0x%p", original_open);
    } else {
        Log("Bink: failed to patch BinkOpen");
    }

    void* original_close = nullptr;
    if (PatchImportByName(range, "binkw32.dll", "_BinkClose@4", reinterpret_cast<void*>(&ProxyBinkClose), &original_close)) {
        g_real_bink_close = reinterpret_cast<BinkCloseFn>(original_close);
        Log("Bink: patched BinkClose original=0x%p", original_close);
    } else {
        Log("Bink: failed to patch BinkClose");
    }

}

// Plan A: the radius loads read plugin-owned floats (the shared 350/256 constants stay), and
// 0x7723A0's cell list moves to a buffer large enough for the bigger square. Without the buffer
// a radius above ~360 m overflows the 21-entry stack array (see kTrafficCellWriteRva).
alignas(16) std::int32_t g_traffic_cell_buffer[kTrafficCellCapacity * 2] = {};
float g_traffic_radius_value = kTrafficStockRadius;
float g_traffic_split_radius_value = kTrafficStockSplitRadius;

void ApplyTrafficRadiusPatch(const ModuleRange& range)
{
    if (g_traffic_spawn_radius <= kTrafficStockRadius + 0.5f) {
        return;
    }

    struct Site
    {
        DWORD rva;
        std::uint8_t expected[8];
        size_t size;
    };
    const DWORD radius_constant = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(range.base) + kTrafficRadiusConstantRva);
    const DWORD split_constant = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(range.base) + kTrafficRadiusSplitConstantRva);
    Site sites[] = {
        {kTrafficCellWriteRva, {0x8D, 0x84, 0xC4, 0xBC, 0x00, 0x00, 0x00}, 7},
        {kTrafficCellSortEndRva, {0x8D, 0x84, 0xC4, 0xC4, 0x00, 0x00, 0x00}, 7},
        {kTrafficCellSortBeginRva, {0x8D, 0x94, 0x24, 0xC4, 0x00, 0x00, 0x00}, 7},
        {kTrafficCellLoopBeginRva, {0x8D, 0xBC, 0x24, 0xD0, 0x00, 0x00, 0x00}, 7},
        {kTrafficCellLoopEndRva, {0x8D, 0x84, 0xC4, 0xD0, 0x00, 0x00, 0x00}, 7},
        {kTrafficRadiusLoadRva, {0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0}, 8},
        {kTrafficRadiusSplitLoadRva, {0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0}, 8},
    };
    std::memcpy(&sites[5].expected[4], &radius_constant, 4);
    std::memcpy(&sites[6].expected[4], &split_constant, 4);

    for (const Site& site : sites) {
        if (!IsRvaInRange(range, site.rva, site.size) ||
            std::memcmp(range.base + site.rva, site.expected, site.size) != 0) {
            const std::uint8_t* bytes = IsRvaInRange(range, site.rva, 8) ? range.base + site.rva : nullptr;
            Log(
                "TrafficRadius: unexpected bytes at RVA 0x%08lX (%02X %02X %02X %02X %02X %02X %02X %02X); radius stays %.0f m",
                static_cast<unsigned long>(site.rva),
                bytes ? bytes[0] : 0, bytes ? bytes[1] : 0, bytes ? bytes[2] : 0, bytes ? bytes[3] : 0,
                bytes ? bytes[4] : 0, bytes ? bytes[5] : 0, bytes ? bytes[6] : 0, bytes ? bytes[7] : 0,
                static_cast<double>(kTrafficStockRadius)
            );
            return;
        }
    }

    g_traffic_radius_value = g_traffic_spawn_radius;
    g_traffic_split_radius_value = g_traffic_spawn_radius * (kTrafficStockSplitRadius / kTrafficStockRadius);
    const DWORD buffer = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(g_traffic_cell_buffer));
    const DWORD radius = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(&g_traffic_radius_value));
    const DWORD split_radius = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(&g_traffic_split_radius_value));

    // Cell list first, so the larger radius never runs with the stack array.
    std::uint8_t lea_scaled[7] = {0x8D, 0x04, 0xC5, 0, 0, 0, 0};   // lea eax,[eax*8+buffer]
    std::memcpy(&lea_scaled[3], &buffer, 4);
    std::uint8_t mov_edx[7] = {0xBA, 0, 0, 0, 0, 0x66, 0x90};       // mov edx,buffer
    std::memcpy(&mov_edx[1], &buffer, 4);
    std::uint8_t mov_edi[7] = {0xBF, 0, 0, 0, 0, 0x66, 0x90};       // mov edi,buffer
    std::memcpy(&mov_edi[1], &buffer, 4);
    bool ok =
        WriteMemory(range.base + kTrafficCellWriteRva, lea_scaled, sizeof(lea_scaled)) &&
        WriteMemory(range.base + kTrafficCellSortEndRva, lea_scaled, sizeof(lea_scaled)) &&
        WriteMemory(range.base + kTrafficCellSortBeginRva, mov_edx, sizeof(mov_edx)) &&
        WriteMemory(range.base + kTrafficCellLoopBeginRva, mov_edi, sizeof(mov_edi)) &&
        WriteMemory(range.base + kTrafficCellLoopEndRva, lea_scaled, sizeof(lea_scaled));
    ok = ok &&
        WriteMemory(range.base + kTrafficRadiusLoadRva + 4, &radius, sizeof(radius)) &&
        WriteMemory(range.base + kTrafficRadiusSplitLoadRva + 4, &split_radius, sizeof(split_radius));
    g_traffic_radius_patch_applied = ok;
    Log(
        "TrafficRadius: %s road radius %.0f m (split %.0f m), cell list at 0x%p for %d cells",
        ok ? "patched" : "FAILED to patch",
        static_cast<double>(g_traffic_radius_value),
        static_cast<double>(g_traffic_split_radius_value),
        static_cast<void*>(g_traffic_cell_buffer),
        kTrafficCellCapacity
    );
}

void ApplyDrlFlareCurves(const ModuleRange& range)
{
    bool any = false;
    for (const FlareCurve& curve : g_drl_flare_curves) {
        any = any || curve.enabled;
    }
    if (!any || !IsRvaInRange(range, kDrlFlareTableRva, 0x60)) {
        return;
    }

    auto* table = reinterpret_cast<float*>(range.base + kDrlFlareTableRva);
    if (!IsReadableMemory(table, 0x60)) {
        return;
    }
    // Zero until the game's static initialiser has filled it.
    bool initialised = false;
    for (int i = 0; i < 24; ++i) {
        initialised = initialised || table[i] != 0.0f;
    }
    if (!initialised) {
        return;
    }

    for (int attr = 0; attr < 3; ++attr) {
        const FlareCurve& curve = g_drl_flare_curves[attr];
        if (!curve.enabled) {
            continue;
        }
        float before_d[3];
        float before_v[3];
        bool differs = false;
        for (int point = 0; point < 3; ++point) {
            const float* d = table + point * 4 + attr;
            const float* v = table + (3 + point) * 4 + attr;
            before_d[point] = *d;
            before_v[point] = *v;
            differs = differs || *d != curve.distances[point] || *v != curve.values[point];
        }
        if (!differs) {
            continue;
        }
        for (int point = 0; point < 3; ++point) {
            WriteMemory(table + point * 4 + attr, &curve.distances[point], sizeof(float));
            WriteMemory(table + (3 + point) * 4 + attr, &curve.values[point], sizeof(float));
        }
        Log(
            "Lights: %s {%.4g, %.4g, %.4g}:{%.4g, %.4g, %.4g} -> {%.4g, %.4g, %.4g}:{%.4g, %.4g, %.4g}%s",
            kFlareAttributeKeys[attr],
            static_cast<double>(before_d[0]), static_cast<double>(before_d[1]), static_cast<double>(before_d[2]),
            static_cast<double>(before_v[0]), static_cast<double>(before_v[1]), static_cast<double>(before_v[2]),
            static_cast<double>(curve.distances[0]), static_cast<double>(curve.distances[1]), static_cast<double>(curve.distances[2]),
            static_cast<double>(curve.values[0]), static_cast<double>(curve.values[1]), static_cast<double>(curve.values[2]),
            g_drl_flares_written ? " (re-applied)" : ""
        );
    }
    g_drl_flares_written = true;
}

void LogTrafficDiagnostics(const ModuleRange& range)
{
    static DWORD last_tick = 0;
    static LONG last_presents = 0;
    if (!g_traffic_diagnostics) {
        return;
    }
    const DWORD now = GetTickCount();
    if (last_tick != 0 && now - last_tick < 2000) {
        return;
    }
    const LONG presents = g_present_count;
    const double fps = last_tick != 0 ? (presents - last_presents) * 1000.0 / static_cast<double>(now - last_tick) : 0.0;
    last_tick = now;
    last_presents = presents;

    std::uint8_t* engine = nullptr;
    if (!ResolveEngineInstance(range, &engine) || !IsReadableMemory(engine + 0x758, sizeof(void*))) {
        return;
    }
    auto* manager = *reinterpret_cast<std::uint8_t**>(engine + 0x758);
    if (manager == nullptr || !IsReadableMemory(manager + kTrafficVehicleCountOffset, sizeof(DWORD)) ||
        !IsReadableMemory(manager + kTrafficActiveNodeCountOffset, sizeof(DWORD)) ||
        !IsReadableMemory(manager + kTrafficActiveLaneCountOffset, sizeof(DWORD)) ||
        !IsReadableMemory(manager + kTrafficWantedCountOffset, sizeof(DWORD)) ||
        !IsReadableMemory(manager + 0x5ca84c, 1)) {
        return;
    }
    Log(
        "TrafficDiag: fps=%.1f radius=%.0f enabled=%u wantedRoads=%lu activeRoads=%lu activeLanes=%lu vehicles=%lu",
        fps,
        static_cast<double>(g_traffic_radius_patch_applied ? g_traffic_radius_value : kTrafficStockRadius),
        static_cast<unsigned>(manager[0x5ca84c]),
        static_cast<unsigned long>(*reinterpret_cast<DWORD*>(manager + kTrafficWantedCountOffset)),
        static_cast<unsigned long>(*reinterpret_cast<DWORD*>(manager + kTrafficActiveNodeCountOffset)),
        static_cast<unsigned long>(*reinterpret_cast<DWORD*>(manager + kTrafficActiveLaneCountOffset)),
        static_cast<unsigned long>(*reinterpret_cast<DWORD*>(manager + kTrafficVehicleCountOffset))
    );
}

// Garage vehicles screen: Up/Down switch makers while a maker's cars are open.
//
// The vehicles screen lists makers top to bottom (Up/Down); Enter opens the
// maker's cars as a row of cards that Left/Right scroll, and there Up/Down only
// move the focus between the card row and the BACK button; Esc goes back to
// the makers. With UpDownSwitchesMaker=1, Down in the card row opens the next
// maker's cars and Up the previous maker's, at their first card, so all makers
// can be browsed with Up/Down and one maker's cars with Left/Right.
//
// The switch drives the menu's own actions, so the script keeps its state:
// Cancel (back to the maker list), Down or Up there, Select (open). Only the
// key entries the menu reads are changed; the maker list, the card row, the
// side panel, the "x/y OWNED" texts and Select all come from the script and
// the game's car list as when a player presses those keys.
//
// Menu input (reverse_engineering/driver_sf/garage_vertical_car_list.md): the
// game's menu-input object (vtable 0xE280D0) holds per player (2, stride 0x158,
// from +0x10) one {float value, int state} entry per MmDk menu key, filled once a
// frame (FUN_00719940: state 0xFF the first frame down, 0xFE still down, 1 the
// release frame, 0 idle). Its slot 3 (FUN_00719B50, thiscall menu, arg; ret 8)
// copies them into one menu's key table and runs the key handlers. The keys'
// input actions are in the table at 0xE28000: 0..3 Up, Down, Right, Left
// (EInputAction_Menu_Up/Down/Right/Left), 4 Select, 5 Cancel. The hook changes
// the entries for that one call and puts them back afterwards, so other menus
// updated in the same frame see the real keys.
constexpr DWORD kMenuInputUpdateSlotRva = 0x00A280DC;  // vtable 0x00E280D0 + 0x0C
constexpr DWORD kMenuInputUpdateRva = 0x00319B50;      // FUN_00719B50
constexpr DWORD kMenuFindVariableRva = 0x006CBE20;     // FUN_00ACBE20, MmDk menu script variable lookup
constexpr DWORD kMenuFindListRva = 0x006B5E70;         // FUN_00AB5E70, MmDk menu list lookup (thiscall menu, name, -1)
constexpr DWORD kMenuInputKeysOffset = 0x10;
constexpr DWORD kMenuInputPlayerStride = 0x158;
constexpr int kMenuKeyCount = 43;
constexpr int kMenuButtonKeys = 16;  // keys 0..15: actions 6..22, the buttons (directions to L2/R2); 16..42: actions 44..70 (mouse and text, going by the enum's names)
constexpr int kMenuKeyUp = 0, kMenuKeyDown = 1, kMenuKeySelect = 4, kMenuKeyCancel = 5;
constexpr int kMenuKeyPressed = 0xFF, kMenuKeyReleased = 1;
constexpr char kCarBrowserPage[] = "ItemBrowser";               // the card browser page of menusmaster.sp
constexpr char kMakerListPage[] = "CategorySelect_Vehicles";    // the maker list page

struct MenuKeyInput
{
    float value;
    int state;
};

using MenuInputUpdateFn = char (__fastcall*)(void* input, void* edx, void* menu, int arg);
MenuInputUpdateFn g_menu_input_update = nullptr;
std::uint8_t* g_exe_base = nullptr;
volatile LONG g_garage_log_count = 0;

// Name of the page on top of an MmDk menu's page stack (entries of 8 bytes at
// +0x1C0, count at +0x1CC, FUN_00AB9440; an entry's first word is the page, a
// page's first word its name), or "".
const char* MenuTopPageName(std::uint8_t* menu)
{
    if (!IsReadableMemory(menu + 0x1C0, 0x10)) {
        return "";
    }
    const int depth = *reinterpret_cast<int*>(menu + 0x1CC);
    auto* stack = *reinterpret_cast<std::uint8_t**>(menu + 0x1C0);
    if (depth <= 0 || depth > 64 || !IsReadableMemory(stack + (depth - 1) * 8, 4)) {
        return "";
    }
    auto* page = *reinterpret_cast<std::uint8_t**>(stack + (depth - 1) * 8);
    if (!IsReadableMemory(page, 4)) {
        return "";
    }
    const char* name = *reinterpret_cast<const char**>(page);
    return IsReadableMemory(name, sizeof(kMakerListPage)) ? name : "";
}

// Script variables of the vehicles screen (FUN_00ACBE20, thiscall on menu + 0x50,
// (name, info out) -> int*). startPageSelected is 2 or 3 while the screen shows
// vehicles (as the garage hub FUN_0070E7D0 reads it); the same card browser page
// shows the abilities (1) and challenges (4). categorySelected is the maker,
// 1-based, as the car list FUN_007113A0 reads it.
struct VehicleScreenVars
{
    std::uint8_t* menu = nullptr;
    int* page = nullptr;
    int* maker = nullptr;
};
VehicleScreenVars g_screen_vars;

const VehicleScreenVars& ScreenVars(std::uint8_t* menu)
{
    if (g_screen_vars.menu != menu) {
        using FindVarFn = int* (__fastcall*)(void* vars, void* edx, const char* name, int* info);
        auto find_var = reinterpret_cast<FindVarFn>(g_exe_base + kMenuFindVariableRva);
        g_screen_vars.menu = menu;
        g_screen_vars.page = find_var(menu + 0x50, nullptr, "startPageSelected", nullptr);
        g_screen_vars.maker = find_var(menu + 0x50, nullptr, "categorySelected", nullptr);
    }
    return g_screen_vars;
}

bool ShowsVehicles(std::uint8_t* menu)
{
    const VehicleScreenVars& vars = ScreenVars(menu);
    return vars.page != nullptr && (*vars.page == 2 || *vars.page == 3);
}

int CurrentMaker(std::uint8_t* menu)
{
    const VehicleScreenVars& vars = ScreenVars(menu);
    return vars.maker != nullptr && IsReadableMemory(vars.maker, sizeof(int)) ? *vars.maker : 0;
}

// Makers in the maker list: the length of the script list ManufacturerNumList
// (MmDk list: int16 length at +0x0C), which the garage's maker fill
// (FUN_006C6940) sets to the makers it lists; dsfforza.asi's fill keeps that.
int MakerCount(std::uint8_t* menu)
{
    using FindListFn = std::uint8_t* (__fastcall*)(void* menu, void* edx, const char* name, int index);
    auto find_list = reinterpret_cast<FindListFn>(g_exe_base + kMenuFindListRva);
    std::uint8_t* list = find_list(menu, nullptr, "ManufacturerNumList", -1);
    if (list == nullptr || !IsReadableMemory(list + 0x0C, 2)) {
        return 0;
    }
    return *reinterpret_cast<std::int16_t*>(list + 0x0C);
}

MenuKeyInput* PlayerKeys(std::uint8_t* input, int player)
{
    return reinterpret_cast<MenuKeyInput*>(input + kMenuInputKeysOffset + player * kMenuInputPlayerStride);
}

// The maker switch, one step per menu update of the vehicles menu.
enum class SwitchStep
{
    Idle,
    Back,       // Cancel pressed and released: the card row closes
    ToMakers,   // until the maker list is on top and has settled
    Move,       // Down or Up pressed and released
    Moved,      // until categorySelected is the next maker and the list has settled
    Open,       // Select pressed and released
    ToBrowser,  // until the card row is on top again
};

struct MakerSwitch
{
    SwitchStep step = SwitchStep::Idle;
    std::uint8_t* menu = nullptr;
    int dir = 0;          // +1 next maker, -1 previous
    int target = 0;       // categorySelected to reach
    int queued = 0;       // presses that came in meanwhile (+ Down, - Up)
    int frames = 0;       // menu updates in this step
    int settled = 0;      // menu updates the wait condition has held
    DWORD started = 0;    // GetTickCount of this step
};
MakerSwitch g_switch;

constexpr int kSettleUpdates = 2;          // menu updates a page or maker must hold before the next key
constexpr int kStepTimeoutUpdates = 300;   // a step that takes more menu updates (about 5 s) ends the switch
constexpr DWORD kStepTimeoutMs = 30000;    // so does one that takes longer, however slow the frames (stalls)

const char* StepName(SwitchStep step)
{
    switch (step) {
    case SwitchStep::Idle: return "idle";
    case SwitchStep::Back: return "back";
    case SwitchStep::ToMakers: return "to-makers";
    case SwitchStep::Move: return "move";
    case SwitchStep::Moved: return "moved";
    case SwitchStep::Open: return "open";
    case SwitchStep::ToBrowser: return "to-browser";
    }
    return "?";
}

void SetStep(SwitchStep step)
{
    g_switch.step = step;
    g_switch.frames = 0;
    g_switch.settled = 0;
    g_switch.started = GetTickCount();
}

// Up/Down pressed this frame by either player: +1 Down, -1 Up, 0 none.
int VerticalPress(std::uint8_t* input)
{
    int dir = 0;
    for (int player = 0; player < 2; ++player) {
        MenuKeyInput* keys = PlayerKeys(input, player);
        if (keys[kMenuKeyDown].state == kMenuKeyPressed) {
            dir += 1;
        }
        if (keys[kMenuKeyUp].state == kMenuKeyPressed) {
            dir -= 1;
        }
    }
    return dir;
}

// Starts a switch towards maker + dir, if there is one (the switch stops at the
// first and the last maker, as the maker list does).
bool BeginSwitch(std::uint8_t* menu, int dir)
{
    const int maker = CurrentMaker(menu);
    const int count = MakerCount(menu);
    const int target = maker + dir;
    if (maker < 1 || target < 1 || target > count) {
        return false;
    }
    g_switch.menu = menu;
    g_switch.dir = dir;
    g_switch.target = target;
    g_switch.queued = 0;
    g_switch.step = SwitchStep::Idle;
    if (InterlockedIncrement(&g_garage_log_count) <= 8) {
        Log("Garage: %s in the vehicles card row: maker %d -> %d of %d", dir > 0 ? "Down" : "Up", maker, target, count);
    }
    SetStep(SwitchStep::Back);
    return true;
}

// Writes one injected key for player 0: press on the step's first update,
// release on the second. Returns true once both are sent.
bool Tap(MenuKeyInput* keys, int key)
{
    if (g_switch.frames == 0) {
        keys[key] = MenuKeyInput{1.0f, kMenuKeyPressed};
        return false;
    }
    keys[key] = MenuKeyInput{0.0f, kMenuKeyReleased};
    return true;
}

// Runs one menu update of the switch; the key entries are already saved.
void StepSwitch(std::uint8_t* input, std::uint8_t* menu, const char* page)
{
    // Meanwhile the player's own button presses don't reach the menu (mouse and
    // text entries are left alone); further Up/Down presses are kept and run
    // after this switch.
    g_switch.queued += VerticalPress(input);
    for (int player = 0; player < 2; ++player) {
        std::memset(PlayerKeys(input, player), 0, sizeof(MenuKeyInput) * kMenuButtonKeys);
    }
    MenuKeyInput* keys = PlayerKeys(input, 0);

    if (g_switch.frames > kStepTimeoutUpdates || GetTickCount() - g_switch.started > kStepTimeoutMs) {
        Log("Garage: maker switch gave up in step %s (page %s, maker %d, target %d)", StepName(g_switch.step), page,
            CurrentMaker(menu), g_switch.target);
        g_switch.step = SwitchStep::Idle;
        g_switch.queued = 0;
        return;
    }

    switch (g_switch.step) {
    case SwitchStep::Back:
        if (Tap(keys, kMenuKeyCancel)) {
            SetStep(SwitchStep::ToMakers);
            return;
        }
        break;
    case SwitchStep::ToMakers:
        g_switch.settled = std::strcmp(page, kMakerListPage) == 0 ? g_switch.settled + 1 : 0;
        if (g_switch.settled > kSettleUpdates) {
            SetStep(SwitchStep::Move);
            return;
        }
        break;
    case SwitchStep::Move:
        if (Tap(keys, g_switch.dir > 0 ? kMenuKeyDown : kMenuKeyUp)) {
            SetStep(SwitchStep::Moved);
            return;
        }
        break;
    case SwitchStep::Moved:
        g_switch.settled = CurrentMaker(menu) == g_switch.target ? g_switch.settled + 1 : 0;
        if (g_switch.settled > kSettleUpdates) {
            // Presses that came in meanwhile move on in the list before it opens
            // (and stop at its ends).
            if (g_switch.queued != 0) {
                const int dir = g_switch.queued > 0 ? 1 : -1;
                const int next = g_switch.target + dir;
                g_switch.queued -= dir;
                if (next >= 1 && next <= MakerCount(menu)) {
                    g_switch.dir = dir;
                    g_switch.target = next;
                    SetStep(SwitchStep::Move);
                    return;
                }
                g_switch.queued = 0;
            }
            SetStep(SwitchStep::Open);
            return;
        }
        break;
    case SwitchStep::Open:
        if (Tap(keys, kMenuKeySelect)) {
            SetStep(SwitchStep::ToBrowser);
            return;
        }
        break;
    case SwitchStep::ToBrowser:
        g_switch.settled = std::strcmp(page, kCarBrowserPage) == 0 ? g_switch.settled + 1 : 0;
        if (g_switch.settled > kSettleUpdates) {
            SetStep(SwitchStep::Idle);
            // Presses that came in while the row opened start the next switch.
            const int queued = g_switch.queued;
            if (queued != 0 && BeginSwitch(menu, queued > 0 ? 1 : -1)) {
                g_switch.queued = queued - g_switch.dir;
            }
            return;
        }
        break;
    case SwitchStep::Idle:
        break;
    }
    ++g_switch.frames;
}

char __fastcall ProxyMenuInputUpdate(std::uint8_t* input, void* edx, std::uint8_t* menu, int arg)
{
    if (!g_updown_switches_maker || menu == nullptr || input == nullptr) {
        return g_menu_input_update(input, edx, menu, arg);
    }
    const bool switching = g_switch.step != SwitchStep::Idle;
    if (switching && menu != g_switch.menu) {
        return g_menu_input_update(input, edx, menu, arg);
    }
    const char* page = MenuTopPageName(menu);
    const bool browser = std::strcmp(page, kCarBrowserPage) == 0;
    if (!switching && !browser) {
        return g_menu_input_update(input, edx, menu, arg);
    }
    if (!switching && !ShowsVehicles(menu)) {
        return g_menu_input_update(input, edx, menu, arg);
    }

    MenuKeyInput saved[2][kMenuKeyCount];
    for (int player = 0; player < 2; ++player) {
        std::memcpy(saved[player], PlayerKeys(input, player), sizeof(saved[player]));
    }
    if (switching) {
        StepSwitch(input, menu, page);
    } else if (browser) {
        // In the card row Up/Down only switch makers; they never take the focus to BACK.
        const int dir = VerticalPress(input);
        for (int player = 0; player < 2; ++player) {
            MenuKeyInput* keys = PlayerKeys(input, player);
            keys[kMenuKeyUp] = MenuKeyInput{};
            keys[kMenuKeyDown] = MenuKeyInput{};
        }
        if (dir != 0 && BeginSwitch(menu, dir > 0 ? 1 : -1)) {
            StepSwitch(input, menu, page);
        }
    }

    const char result = g_menu_input_update(input, edx, menu, arg);
    for (int player = 0; player < 2; ++player) {
        std::memcpy(PlayerKeys(input, player), saved[player], sizeof(saved[player]));
    }
    return result;
}

void ApplyGarageMakerSwitchPatch(const ModuleRange& range)
{
    if (!g_updown_switches_maker) {
        return;
    }
    if (!IsRvaInRange(range, kMenuInputUpdateSlotRva, 4) || !IsRvaInRange(range, kMenuFindVariableRva, 16) ||
        !IsRvaInRange(range, kMenuFindListRva, 16)) {
        Log("Garage: menu input vtable slot RVA 0x%08lX is outside the loaded image",
            static_cast<unsigned long>(kMenuInputUpdateSlotRva));
        return;
    }

    auto* slot = reinterpret_cast<std::uint8_t**>(range.base + kMenuInputUpdateSlotRva);
    auto* expected = range.base + kMenuInputUpdateRva;
    if (*slot == reinterpret_cast<std::uint8_t*>(&ProxyMenuInputUpdate)) {
        return;
    }
    if (*slot != expected) {
        Log("Garage: menu input update slot holds 0x%p, expected 0x%p; Up/Down maker switch not installed", *slot,
            expected);
        return;
    }

    g_exe_base = range.base;
    g_menu_input_update = reinterpret_cast<MenuInputUpdateFn>(expected);
    void* replacement = reinterpret_cast<void*>(&ProxyMenuInputUpdate);
    if (WriteMemory(slot, &replacement, sizeof(replacement))) {
        Log("Garage: Up/Down switch makers in the vehicles card row (menu input slot RVA 0x%08lX)",
            static_cast<unsigned long>(kMenuInputUpdateSlotRva));
    } else {
        Log("Garage: failed to install the Up/Down maker switch hook");
        g_menu_input_update = nullptr;
    }
}

void ApplyPatches()
{
    const ModuleRange exe = GetExeRange();
    Log("DSF Enhanced attached: exe=0x%p size=0x%lX", exe.base, static_cast<unsigned long>(exe.size));
    Log(
        "Config: Log=%d RemoveVSync=%d BorderlessWindowed=%d SkipLegalScreen=%d SuppressUbisoftServerLostPrompt=%d Ultrawide=%d TargetAspect=%.6f FOVScaling=%d FOVScalar=%.6f CinematicDrawPatch=%d AnisotropicFiltering=%d ForceTrilinearFiltering=%d ForceHighestMip=%d TextureLODBias=%.3f LODDistanceScale=%.3f LODEngineThresholdScale=%.3f ImposterDrawDistance=%.3f InterestingVehicleMinDistanceScale=%.3f InterestingVehicleMaxDistanceScale=%.3f InterestingVehicleSlotCount=%d PreloadAllTrafficVehiclePools=%d TrafficCandidateWindowScale=%.3f TrafficSpawnIntervalScale=%.3f",
        g_log_enabled ? 1 : 0,
        g_remove_vsync ? 1 : 0,
        g_borderless_windowed ? 1 : 0,
        g_skip_legal_screen ? 1 : 0,
        g_suppress_ubisoft_server_lost_prompt ? 1 : 0,
        g_ultrawide_enabled ? 1 : 0,
        static_cast<double>(g_target_aspect),
        g_fov_scaling ? 1 : 0,
        static_cast<double>(g_fov_scalar),
        g_cinematic_draw_patch ? 1 : 0,
        g_anisotropic_filtering,
        g_force_trilinear_filtering ? 1 : 0,
        g_force_highest_mip ? 1 : 0,
        static_cast<double>(g_texture_lod_bias),
        static_cast<double>(g_lod_distance_scale),
        static_cast<double>(g_lod_engine_threshold_scale),
        static_cast<double>(g_imposter_draw_distance),
        static_cast<double>(g_interesting_vehicle_min_distance_scale),
        static_cast<double>(g_interesting_vehicle_max_distance_scale),
        g_interesting_vehicle_slot_count,
        g_preload_all_traffic_vehicle_pools ? 1 : 0,
        static_cast<double>(g_traffic_candidate_window_scale),
        static_cast<double>(g_traffic_spawn_interval_scale)
    );

    PatchDirect3DCreate9Import(exe);
    PatchBinkImports(exe);
    ApplyResourceArenaPatch(exe);
    ApplyStartupPatches(exe);
    ApplyUbisoftServerLostPromptPatch(exe);
    ApplyGarageMakerSwitchPatch(exe);
    ApplyMovieDrawPatch(exe);
    ApplyAspectConstantPatch(exe, "Init");
    ApplyGlobalFovScalarPatch(exe, "Init");
    ApplyTrafficRadiusPatch(exe);
    Log(
        "Config: MapDetailLODThresholdScale=%.3f VehicleLODThresholdScale=%.3f VehicleFullDetail=%d VehicleDrawDistance=%.0f TrafficSpawnRadius=%.0f TrafficDiagnostics=%d DRLFlares=%d/%d/%d",
        static_cast<double>(g_map_detail_lod_threshold_scale),
        static_cast<double>(g_vehicle_lod_threshold_scale),
        g_vehicle_full_detail ? 1 : 0,
        static_cast<double>(g_vehicle_draw_distance),
        static_cast<double>(g_traffic_spawn_radius),
        g_traffic_diagnostics ? 1 : 0,
        g_drl_flare_curves[0].enabled ? 1 : 0,
        g_drl_flare_curves[1].enabled ? 1 : 0,
        g_drl_flare_curves[2].enabled ? 1 : 0
    );
}

DWORD WINAPI InitThread(LPVOID)
{
    LoadConfig();
    ApplyPatches();
    for (unsigned tick = 0;; ++tick) {
        const ModuleRange exe = GetExeRange();
        // LOD thresholds and flare curves are re-checked every 250 ms, so a graphics-settings
        // change (which restores the preset) is undone at once; the rest runs once a second.
        ApplyLodThresholds(exe, "Loop");
        ApplyDrlFlareCurves(exe);
        LogTrafficDiagnostics(exe);
        if (tick % 4 == 0) {
            LogResourceArenaUse();
        }
        if (tick % 4 == 0) {
            ApplyAspectConstantPatch(exe, "Loop");
            ApplyGlobalFovScalarPatch(exe, "Loop");
            ApplyImposterDrawDistancePatch(exe, "Loop");
            ApplyInterestingVehiclePatch(exe, "Loop");
            ApplyTrafficVehiclePoolPreload(exe, "Loop");
            ApplyTrafficRuntimePatches(exe, "Loop");
        }
        Sleep(250);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
