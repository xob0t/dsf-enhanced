// DLAA jitter's vertex shader patch (src/vs_jitter_patch.h). Built and run by build.ps1: the
// self-test compiles a few shaders with d3dcompiler_47, patches them and checks the disassembly.
// With file arguments it also patches every vertex shader found in them, for example the game's
// shader packages (research/dlaa.md, "The free constant register"):
//   build\vs_jitter_test.exe "D:\Games\Driver San Francisco\media\SfxParticles.fchunk" ...
// It prints per-version and per-result counts, every shader it refuses, and the highest float
// constant any shader can reach. --dump DIR writes the original and patched disassembly of each
// shader to DIR. --gpu draws a patched shader on a D3D9 device and measures how far J moves the
// picture (the jitter's sign and scale). Nothing it reads or writes is committed. (Not called *patch*: Windows asks to
// elevate unmanifested 32-bit programs whose names say patch, setup or install.)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>
#include "../src/vs_jitter_patch.h"

using D3DCompileFn = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT,
    ID3DBlob**, ID3DBlob**);
using D3DDisassembleFn = HRESULT (WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
D3DCompileFn g_compile = nullptr;
D3DDisassembleFn g_disassemble = nullptr;
int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

bool Disassemble(const std::vector<std::uint32_t>& code, std::string* text)
{
    ID3DBlob* blob = nullptr;
    if (FAILED(g_disassemble(code.data(), code.size() * 4, 0, nullptr, &blob)) || blob == nullptr) {
        return false;
    }
    text->assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    blob->Release();
    return true;
}

std::vector<std::uint32_t> Compile(const char* source, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    std::vector<std::uint32_t> out;
    if (SUCCEEDED(g_compile(source, std::strlen(source), "test", nullptr, nullptr, "main", target, 0, 0, &code, &errors)) && code) {
        const auto* words = static_cast<const std::uint32_t*>(code->GetBufferPointer());
        out.assign(words, words + code->GetBufferSize() / 4);
    } else if (errors != nullptr) {
        std::printf("compile %s: %s\n", target, static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return out;
}

int Count(const std::string& text, const char* needle)
{
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

vsjitter::Info PatchVector(const std::vector<std::uint32_t>& code, std::vector<std::uint32_t>* out)
{
    return vsjitter::Patch(code.data(), vsjitter::TokenCount(code.data(), code.size()), out);
}

// The position output keeps exactly the two writes the patch appends, and the shader is otherwise
// unchanged in its declarations.
void CheckPatched(const std::vector<std::uint32_t>& code, const char* position, int jitter, const char* name)
{
    std::vector<std::uint32_t> patched;
    const vsjitter::Info info = PatchVector(code, &patched);
    std::printf("%s: %s, r%d, c%d, %d position writes, max constant c%d\n", name, vsjitter::ResultName(info.result), info.temp,
        info.jitter_register, info.position_writes, info.max_constant);
    CHECK(info.result == vsjitter::kPatched);
    CHECK(info.jitter_register == jitter);
    CHECK(patched.size() == code.size() + 8);
    std::string before, after;
    CHECK(Disassemble(code, &before));
    CHECK(Disassemble(patched, &after));
    char mad[96], mov[64];
    std::snprintf(mad, sizeof(mad), "mad %s.xy, r%d.w, c%d.xyyy, r%d.xyyy", position, info.temp, jitter, info.temp);
    std::snprintf(mov, sizeof(mov), "mov %s.zw, r%d", position, info.temp);
    CHECK(Count(after, mad) == 1);
    CHECK(Count(after, mov) == 1);
    char write[32];
    std::snprintf(write, sizeof(write), " %s", position);
    CHECK(Count(after, write) == 2 + Count(before, "dcl_position o"));
    CHECK(Count(after, "dcl_") == Count(before, "dcl_"));
}

void SelfTest()
{
    // World draws: World*View*Proj rows in c0-c3 (vs_2_0 writes oPos, vs_3_0 a declared output).
    const char* world =
        "float4x4 g_ObjectToScreen : register(c0); float4x3 g_ObjectToWorld : register(c4);\n"
        "void main(float4 p : POSITION, float2 uv : TEXCOORD0, out float4 o : POSITION, out float2 ouv : TEXCOORD0,\n"
        "          out float3 w : TEXCOORD1) { o = mul(p, g_ObjectToScreen); w = mul(p, g_ObjectToWorld); ouv = uv; }\n";
    const std::vector<std::uint32_t> world2 = Compile(world, "vs_2_0");
    const std::vector<std::uint32_t> world3 = Compile(world, "vs_3_0");
    CHECK(!world2.empty() && !world3.empty());
    CheckPatched(world2, "oPos", 255, "world vs_2_0");
    CheckPatched(world3, "o0", 255, "world vs_3_0");

    // Skinned characters: the bone palette c0-c182 indexed through a0, then the clip matrix at c183.
    const char* skinned =
        "float4x3 g_boneMatrixArray[61] : register(c0); float4x4 g_ObjectToScreen : register(c183);\n"
        "float4 main(float4 p : POSITION, float4 bi : BLENDINDICES) : POSITION {\n"
        "  float3 s = mul(p, g_boneMatrixArray[(int)bi.x]); return mul(float4(s, 1), g_ObjectToScreen); }\n";
    const std::vector<std::uint32_t> skinned3 = Compile(skinned, "vs_3_0");
    CheckPatched(skinned3, "o0", 255, "skinned vs_3_0");
    CheckPatched(Compile(skinned, "vs_2_0"), "oPos", 255, "skinned vs_2_0");

    // Particles: separate view and projection, column-wise for some characters.
    const char* particles =
        "float4x4 g_WorldToCam : register(c10); float4x4 g_CameraToScreen : register(c14);\n"
        "float4 main(float4 p : POSITION) : POSITION { return mul(mul(p, g_WorldToCam), g_CameraToScreen); }\n";
    CheckPatched(Compile(particles, "vs_3_0"), "o0", 255, "particles vs_3_0");

    // A shader that reads c255 itself is left alone.
    const char* uses_j = "float4 k : register(c255); float4 main(float4 p : POSITION) : POSITION { return p * k; }\n";
    std::vector<std::uint32_t> patched;
    CHECK(PatchVector(Compile(uses_j, "vs_3_0"), &patched).result == vsjitter::kJitterRegisterUsed && patched.empty());
    // ... and so is one whose constant table range covers it (an array to c255).
    const char* array_to_j =
        "float4 a[56] : register(c200); float4 main(float4 p : POSITION, float4 i : BLENDINDICES) : POSITION { return p * a[(int)i.x]; }\n";
    CHECK(PatchVector(Compile(array_to_j, "vs_3_0"), &patched).result == vsjitter::kJitterRegisterUsed);

    // Relative addressing with the constant table stripped: its reach is unknown.
    std::vector<std::uint32_t> stripped;
    for (size_t i = 0; i < skinned3.size();) {
        const std::uint32_t t = skinned3[i];
        if ((t & 0xFFFF) == 0xFFFE) {
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        stripped.push_back(t);
        ++i;
    }
    CHECK(PatchVector(stripped, &patched).result == vsjitter::kUnknownRelative);

    // vs_1_1 (no instruction lengths): dcl_position v0; dp4 oPos.x-w, v0, c0-c3.
    std::vector<std::uint32_t> sm1 = {0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000};
    for (std::uint32_t c = 0; c < 4; ++c) {
        sm1.insert(sm1.end(), {0x00000009, vsjitter::Param(vsjitter::kRegRastOut, 0, 1u << c), 0x90E40000,
            vsjitter::Param(vsjitter::kRegConst, c, 0xE4)});
    }
    sm1.push_back(0x0000FFFF);
    CheckPatched(sm1, "oPos", 255, "vs_1_1");
    // Writing only oPos.xy (z and w would come from an unwritten temp).
    std::vector<std::uint32_t> partial(sm1.begin(), sm1.begin() + 12);
    partial.push_back(0x0000FFFF);
    CHECK(PatchVector(partial, &patched).result == vsjitter::kPartialPosition);
    // Truncated: no end token.
    std::vector<std::uint32_t> truncated(world3.begin(), world3.end() - 1);
    CHECK(vsjitter::TokenCount(truncated.data(), truncated.size()) == 0);
    CHECK(vsjitter::Patch(truncated.data(), truncated.size(), &patched).result == vsjitter::kMalformed);
    // A pixel shader is not touched.
    CHECK(PatchVector(Compile("float4 main() : COLOR { return 1; }", "ps_3_0"), &patched).result == vsjitter::kNotVertexShader);
}

// --gpu: a rectangle drawn at w = 2 through the patched pass-through shader, on a 64x64 target, with
// J = (2 jx / W, -2 jy / H) for jx = 4, jy = 3 pixels (the plugin's mapping). The patched picture must
// move 4 px right and 3 px down; J = 0 must not move it.
bool Centroid(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* staging, double* x, double* y)
{
    if (FAILED(device->GetRenderTargetData(target, staging))) return false;
    D3DLOCKED_RECT locked = {};
    if (FAILED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return false;
    double sx = 0, sy = 0, n = 0;
    for (int row = 0; row < 64; ++row) {
        const auto* line = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(locked.pBits) + row * locked.Pitch);
        for (int col = 0; col < 64; ++col) {
            const double v = (line[col] & 0xFF) / 255.0;
            sx += v * (col + 0.5);
            sy += v * (row + 0.5);
            n += v;
        }
    }
    staging->UnlockRect();
    *x = n > 0 ? sx / n : -1;
    *y = n > 0 ? sy / n : -1;
    return n > 0;
}

void GpuTest()
{
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND window = CreateWindowA("STATIC", "vs_jitter_test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    if (d3d == nullptr || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device))) {
        std::printf("gpu: no D3D9 device\n");
        ++g_failures;
        return;
    }
    const std::vector<std::uint32_t> vs = Compile("float4 main(float4 p : POSITION) : POSITION { return p; }", "vs_3_0");
    const std::vector<std::uint32_t> ps = Compile("float4 main() : COLOR { return 1; }", "ps_3_0");
    std::vector<std::uint32_t> patched;
    CHECK(PatchVector(vs, &patched).result == vsjitter::kPatched);
    IDirect3DVertexShader9* plain = nullptr;
    IDirect3DVertexShader9* jittered = nullptr;
    IDirect3DPixelShader9* white = nullptr;
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* staging = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    CHECK(SUCCEEDED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.data()), &plain)));
    CHECK(SUCCEEDED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(patched.data()), &jittered)));
    CHECK(SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &white)));
    CHECK(SUCCEEDED(device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)));
    CHECK(SUCCEEDED(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr)));
    CHECK(SUCCEEDED(device->CreateVertexDeclaration(elements, &decl)));
    if (plain && jittered && white && target && staging && decl) {
        // NDC x in [-0.25, 0.25), y in [-0.125, 0.25): clip coordinates times w = 2.
        const float w = 2.0f;
        const float quad[4][4] = {{-0.25f * w, 0.25f * w, 0.5f, w}, {0.25f * w, 0.25f * w, 0.5f, w},
                                  {-0.25f * w, -0.125f * w, 0.5f, w}, {0.25f * w, -0.125f * w, 0.5f, w}};
        device->SetRenderTarget(0, target);
        device->SetVertexDeclaration(decl);
        device->SetPixelShader(white);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        const float jx = 4.0f, jy = 3.0f;
        const float j[4] = {2.0f * jx / 64.0f, -2.0f * jy / 64.0f, 0, 0};
        const float zero[4] = {};
        double cx[3] = {}, cy[3] = {};
        for (int pass = 0; pass < 3; ++pass) {
            device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            device->SetVertexShader(pass == 0 ? plain : jittered);
            device->SetVertexShaderConstantF(255, pass == 2 ? j : zero, 1);
            device->BeginScene();
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
            device->EndScene();
            CHECK(Centroid(device, target, staging, &cx[pass], &cy[pass]));
        }
        std::printf("gpu: centre %.3f,%.3f plain, %.3f,%.3f patched with J = 0, %.3f,%.3f with j = (%g, %g) px\n", cx[0], cy[0],
            cx[1], cy[1], cx[2], cy[2], jx, jy);
        CHECK(cx[1] == cx[0] && cy[1] == cy[0]);
        CHECK(std::fabs(cx[2] - cx[0] - jx) < 0.01 && std::fabs(cy[2] - cy[0] - jy) < 0.01);
    }
    for (IUnknown* object : std::initializer_list<IUnknown*>{plain, jittered, white, target, staging, decl, device, d3d}) {
        if (object) object->Release();
    }
    DestroyWindow(window);
}

// The plugin's shader hash (D5HashBytecode): FNV-1a over the tokens up to the first end-token value.
std::uint32_t PluginHash(const std::uint32_t* code)
{
    std::uint32_t hash = 2166136261u;
    for (size_t i = 0; i < 65536; ++i) {
        for (int b = 0; b < 4; ++b) {
            hash ^= (code[i] >> (b * 8)) & 0xFF;
            hash *= 16777619u;
        }
        if (code[i] == 0x0000FFFF) {
            break;
        }
    }
    return hash;
}

struct Totals
{
    std::map<std::uint32_t, int> versions;
    int results[vsjitter::kResultCount] = {};
    int max_constant = -1;
    std::uint32_t max_constant_hash = 0;
    int not_disassembled = 0;
    int patched_not_disassembled = 0;
    std::unordered_set<std::uint32_t> seen;
};

void ScanFile(const char* path, Totals* totals, const char* dump_dir)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("%s: cannot open\n", path);
        ++g_failures;
        return;
    }
    // Shaders are a few KB; read in 32 MB pieces that overlap by 1 MB.
    const size_t chunk = 32u << 20, overlap = 1u << 20;
    std::vector<std::uint8_t> data(chunk + overlap);
    size_t have = 0;
    long long base = 0;
    int found = 0, fresh = 0;
    for (bool eof = false; !eof;) {
        DWORD got = 0;
        ReadFile(file, data.data() + have, static_cast<DWORD>(data.size() - have), &got, nullptr);
        eof = got == 0 || have + got < data.size();
        have += got;
        const size_t scan_end = eof ? have : have - overlap;
        for (size_t at = 2; at + 2 <= scan_end; ++at) {
            if (data[at] != 0xFE || data[at + 1] != 0xFF) {
                continue;
            }
            const std::uint8_t minor = data[at - 2], major = data[at - 1];
            if (!((major == 1 && minor == 1) || (major == 2 && (minor == 0 || minor == 1)) || (major == 3 && minor == 0))) {
                continue;
            }
            const size_t start = at - 2;
            const auto* code = reinterpret_cast<const std::uint32_t*>(data.data() + start);   // x86: unaligned reads are fine
            const size_t limit = std::min<size_t>((have - start) / 4, 65536);
            const size_t count = vsjitter::TokenCount(code, limit);
            if (count < 4) {
                continue;
            }
            std::vector<std::uint32_t> shader(code, code + count);
            std::string text;
            // Not a shader, only bytes that look like a version token: D3DDisassemble accepts many of
            // them, printing ??? for opcodes it does not know, or finds no instruction at all.
            if (!Disassemble(shader, &text) || text.find("???") != std::string::npos ||
                text.find("approximately 0 instruction") != std::string::npos) {
                continue;
            }
            ++found;
            at = start + count * 4 - 1;
            const std::uint32_t hash = PluginHash(shader.data());
            if (!totals->seen.insert(hash).second) {
                continue;
            }
            ++fresh;
            std::vector<std::uint32_t> patched;
            const vsjitter::Info info = vsjitter::Patch(shader.data(), count, &patched);
            totals->versions[shader[0] & 0xFFFF] += 1;
            totals->results[info.result] += 1;
            if (info.max_constant > totals->max_constant) {
                totals->max_constant = info.max_constant;
                totals->max_constant_hash = hash;
            }
            std::string after;
            if (info.result == vsjitter::kPatched && !Disassemble(patched, &after)) {
                ++totals->patched_not_disassembled;
                ++g_failures;
                std::printf("  vs_%08X: patched but does not disassemble\n", hash);
            }
            if (info.result != vsjitter::kPatched) {
                std::printf("  vs_%08X %X: %s (max constant c%d, table %d) in %s at 0x%llX\n", hash, shader[0] & 0xFFFF,
                    vsjitter::ResultName(info.result), info.max_constant, info.constant_table ? 1 : 0, path,
                    static_cast<unsigned long long>(base + static_cast<long long>(start)));
            }
            if (dump_dir != nullptr) {
                char name[MAX_PATH];
                std::snprintf(name, sizeof(name), "%s\\vs_%08X.asm", dump_dir, hash);
                if (FILE* f = std::fopen(name, "wb")) {
                    std::fwrite(text.data(), 1, text.size() > 0 ? text.size() - 1 : 0, f);
                    std::fclose(f);
                }
                if (!after.empty()) {
                    std::snprintf(name, sizeof(name), "%s\\vs_%08X.patched.asm", dump_dir, hash);
                    if (FILE* f = std::fopen(name, "wb")) {
                        std::fwrite(after.data(), 1, after.size() - 1, f);
                        std::fclose(f);
                    }
                }
            }
        }
        if (!eof) {
            std::memmove(data.data(), data.data() + scan_end, have - scan_end);
            base += static_cast<long long>(scan_end);
            have -= scan_end;
        }
    }
    CloseHandle(file);
    std::printf("%s: %d vertex shaders, %d not seen before\n", path, found, fresh);
}

int main(int argc, char** argv)
{
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    g_disassemble = compiler ? reinterpret_cast<D3DDisassembleFn>(GetProcAddress(compiler, "D3DDisassemble")) : nullptr;
    if (g_compile == nullptr || g_disassemble == nullptr) {
        std::printf("d3dcompiler_47.dll not found\n");
        return 1;
    }
    SelfTest();

    const char* dump_dir = nullptr;
    Totals totals;
    int files = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") == 0) {
            GpuTest();
            continue;
        }
        if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
            CreateDirectoryA(dump_dir, nullptr);
            continue;
        }
        ScanFile(argv[i], &totals, dump_dir);
        ++files;
    }
    if (files > 0) {
        std::printf("unique vertex shaders: %zu\n", totals.seen.size());
        for (const auto& v : totals.versions) {
            std::printf("  vs_%u_%u: %d\n", (v.first >> 8) & 0xFF, v.first & 0xFF, v.second);
        }
        for (int r = 0; r < vsjitter::kResultCount; ++r) {
            if (totals.results[r] != 0) {
                std::printf("  %s: %d\n", vsjitter::ResultName(static_cast<vsjitter::Result>(r)), totals.results[r]);
            }
        }
        std::printf("highest float constant reachable: c%d (vs_%08X)\n", totals.max_constant, totals.max_constant_hash);
    }
    std::printf(g_failures == 0 ? "vs_jitter_test: ok\n" : "vs_jitter_test: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
