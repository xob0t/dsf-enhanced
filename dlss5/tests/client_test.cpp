// SPDX-License-Identifier: MIT
//
// dsf-dlss5-client-test (x86): stands in for the 32-bit game side. It starts the helper,
// shares D3D11 textures and fences with it, feeds the synthetic scene through the real
// protocol and writes the helper's output as PNG. No window, no game.
//
//   dsf-dlss5-client-test.exe --helper <dsf-dlss5-helper.exe> --runtime <dir> --out <dir>
//       [--frames N] [--width W --height H] [--host-creates] [--bypass] [--style 0..2]
//       [--feature nr|dlaa|both] [--robustness]
//
// --feature picks what runs (protocol version 2): DLSS 5 (nr, the default, needs
// nvngx_dlssnr.dll), DLAA (needs nvngx_dlss.dll; it gets the scene's linear values in f16), or
// both over the one pipe, each with its own textures, fences and frame numbers, frame by frame.
//
// --robustness then checks the failure paths: a repeated frame number, a frame whose
// fence_in never arrives (the helper must time out, reach fence_out and ask for a new
// CONFIGURE), a frame while unconfigured, a fresh CONFIGURE with frames after it, an unknown
// feature in FRAME and CONFIGURE, and a version 1 header. With one feature it also sends a
// FRAME for the other, unconfigured one.
//
// Exit code 0 means every frame and check came back as expected and the output was written.

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "dsf_dlss5_pipe.h"
#include "../src/image_io.h"
#include "synthetic_scene.h"

using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::XMConvertFloatToHalf;
using namespace dsf_dlss5;

namespace {

// Pipe timeouts on the game side. HELLO includes NGX start-up, which loads every feature.
constexpr DWORD kHelloTimeoutMs = 30000;
constexpr DWORD kConfigureTimeoutMs = 15000;
constexpr DWORD kFrameTimeoutMs = 5000;

struct Options {
  std::wstring helper = L"dlss5\\build\\x64\\dsf-dlss5-helper.exe";
  std::wstring runtime = L"out\\ngx";
  std::wstring outDir = L"out\\dlss5-client-test";
  int frames = 24;
  uint32_t width = 1280;
  uint32_t height = 720;
  bool hostCreates = false;
  bool bypass = false;
  bool robustness = false;
  uint32_t style = 0;
  bool features[DSF_DLSS5_FEATURE_COUNT] = {true, false};
};

const char* FeatureName(uint32_t feature) { return feature == DSF_DLSS5_FEATURE_DLAA ? "DLAA" : "DLSS 5"; }

uint8_t ToByte(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

std::wstring FullPath(const std::wstring& path) {
  wchar_t buffer[MAX_PATH] = {};
  GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
  return buffer;
}

void Say(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vprintf(format, args);
  va_end(args);
  std::printf("\n");
  std::fflush(stdout);
}

HANDLE AsHandle(uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); }

HANDLE StartHelper(const Options& o, PROCESS_INFORMATION* pi) {
  // Hand the helper a handle to this process, so it needs no OpenProcess rights on us.
  HANDLE self = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, PROCESS_DUP_HANDLE | SYNCHRONIZE, TRUE, 0);
  std::wstring cmd = L"\"" + o.helper + L"\" --game-pid " + std::to_wstring(GetCurrentProcessId()) + L" --game-process " +
                     std::to_wstring(reinterpret_cast<uintptr_t>(self)) + L" --runtime \"" + o.runtime + L"\" --log \"" +
                     o.outDir + L"\\helper.log\"";

  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<char> attrBuffer(attrSize);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
  InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self), nullptr, nullptr);
  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = attrs;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si.StartupInfo, pi);
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(self);
  if (!ok) {
    Say("could not start the helper (error %lu): %ls", GetLastError(), cmd.c_str());
    return INVALID_HANDLE_VALUE;
  }
  wchar_t name[128];
  swprintf_s(name, DSF_DLSS5_PIPE_FMT, GetCurrentProcessId());
  for (int attempt = 0; attempt < 200; ++attempt) {
    // Overlapped, so the pipe calls can time out.
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;
    if (WaitForSingleObject(pi->hProcess, 0) == WAIT_OBJECT_0) break;
    Sleep(50);
  }
  Say("could not connect to the helper pipe");
  return INVALID_HANDLE_VALUE;
}

class Client {
 public:
  Client(const Options& o, ID3D11Device5* device, ID3D11DeviceContext4* context, HANDLE pipe, uint32_t feature)
      : o_(o), device_(device), context_(context), pipe_(pipe), feature_(feature) {}

  uint32_t Feature() const { return feature_; }
  bool Dlaa() const { return feature_ == DSF_DLSS5_FEATURE_DLAA; }

  // Sends CONFIGURE and opens what the helper returns. Frame numbers restart at 1.
  bool Configure() {
    for (auto& t : textures_) t.Reset();
    fenceIn_.Reset();
    fenceOut_.Reset();
    // DLAA: linear HDR in and out.
    const DXGI_FORMAT colorFormat = Dlaa() ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT formats[DSF_DLSS5_SLOT_COUNT] = {colorFormat, colorFormat, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                                      DXGI_FORMAT_UNKNOWN};
    DsfDlss5Configure configure = {};
    configure.width = o_.width;
    configure.height = o_.height;
    configure.preset = Dlaa() ? 11 : 2;   // K for DLAA, B (Remix) for DLSS 5
    configure.feature = feature_;
    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) configure.formats[slot] = formats[slot];
    if (!o_.hostCreates) {
      configure.flags |= DSF_DLSS5_CONFIG_CLIENT_CREATES;
      for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
        const UINT bind = D3D11_BIND_SHADER_RESOURCE | (slot == DSF_DLSS5_SLOT_OUTPUT ? D3D11_BIND_UNORDERED_ACCESS : 0);
        textures_[slot] = CreateShared(formats[slot], bind);
        configure.textures[slot] = textures_[slot] ? NtHandleOf(textures_[slot].Get()) : 0;
        if (configure.textures[slot] == 0) {
          Say("could not create shared texture %d", slot);
          return false;
        }
      }
    }
    DsfDlss5ConfigureAck ack;
    DsfDlss5Error error = {};
    const bool sent = PipeSend(pipe_, DSF_DLSS5_MSG_CONFIGURE, configure, kConfigureTimeoutMs) &&
                      PipeReceive(pipe_, DSF_DLSS5_MSG_CONFIGURE_ACK, &ack, kConfigureTimeoutMs, &error);
    // The helper duplicated our handles; ours are no longer needed.
    for (uint64_t handle : configure.textures) {
      if (handle != 0) CloseHandle(AsHandle(handle));
    }
    if (!sent) {
      Say("CONFIGURE failed on the pipe (error message: status %d, %s)", error.status, error.message);
      return false;
    }
    Say("configure %s: status %d, %s", FeatureName(feature_), ack.status, ack.message);
    if (ack.status != DSF_DLSS5_OK) return false;
    if (o_.hostCreates) {
      for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
        const HRESULT hr = device_->OpenSharedResource1(AsHandle(ack.textures[slot]), IID_PPV_ARGS(&textures_[slot]));
        CloseHandle(AsHandle(ack.textures[slot]));
        if (FAILED(hr)) {
          Say("OpenSharedResource1 failed for helper-created slot %d: 0x%08lX", slot, static_cast<unsigned long>(hr));
          return false;
        }
      }
    }
    const bool fences = SUCCEEDED(device_->OpenSharedFence(AsHandle(ack.fence_in), IID_PPV_ARGS(&fenceIn_))) &&
                        SUCCEEDED(device_->OpenSharedFence(AsHandle(ack.fence_out), IID_PPV_ARGS(&fenceOut_)));
    CloseHandle(AsHandle(ack.fence_in));
    CloseHandle(AsHandle(ack.fence_out));
    if (!fences) {
      Say("OpenSharedFence failed");
      return false;
    }
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    textures_[DSF_DLSS5_SLOT_OUTPUT]->GetDesc(&stagingDesc);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_.Reset();
    device_->CreateTexture2D(&stagingDesc, nullptr, &staging_);
    return staging_ != nullptr;
  }

  // Converts a rendered frame to the texture formats (CPU work a game would not have).
  void Prepare(const synthetic::Frame& s) {
    colorBytes_.resize(s.color.size());
    for (size_t i = 0; i < colorBytes_.size(); ++i) colorBytes_[i] = ToByte(s.color[i]);
    if (Dlaa()) {
      colorHalves_.resize(s.linear.size());
      for (size_t i = 0; i < colorHalves_.size(); ++i) colorHalves_[i] = XMConvertFloatToHalf(s.linear[i]);
    }
    depth_ = s.depth;
    motionHalves_.resize(s.motion.size());
    for (size_t i = 0; i < motionHalves_.size(); ++i) motionHalves_[i] = XMConvertFloatToHalf(s.motion[i]);
  }

  // Uploads the prepared inputs. signalIn=false leaves fence_in alone (a stalled game).
  void Upload(uint64_t n, bool signalIn) {
    const UINT pitch = o_.width * 4;
    if (Dlaa()) {
      context_->UpdateSubresource(textures_[DSF_DLSS5_SLOT_COLOR].Get(), 0, nullptr, colorHalves_.data(), pitch * 2, 0);
    } else {
      context_->UpdateSubresource(textures_[DSF_DLSS5_SLOT_COLOR].Get(), 0, nullptr, colorBytes_.data(), pitch, 0);
    }
    context_->UpdateSubresource(textures_[DSF_DLSS5_SLOT_DEPTH].Get(), 0, nullptr, depth_.data(), pitch, 0);
    context_->UpdateSubresource(textures_[DSF_DLSS5_SLOT_MOTION].Get(), 0, nullptr, motionHalves_.data(), pitch, 0);
    if (signalIn) context_->Signal(fenceIn_.Get(), n);
    // The signal must reach the GPU before FRAME is sent (protocol: flush before FRAME).
    context_->Flush();
  }

  // Sends FRAME n and reads the ACK. False only when the pipe failed.
  bool Frame(uint64_t n, uint32_t extraFlags, float jx, float jy, DsfDlss5FrameAck* ack) {
    DsfDlss5Frame frame = {};
    frame.frame = n;
    frame.flags = DSF_DLSS5_FRAME_AUTO_MASK | DSF_DLSS5_FRAME_GPU_TIME | extraFlags | (o_.bypass ? DSF_DLSS5_FRAME_BYPASS : 0);
    frame.style = o_.style;
    frame.jitter[0] = jx;
    frame.jitter[1] = jy;
    frame.mv_scale[0] = frame.mv_scale[1] = 1.0f;
    frame.intensity = 1.0f;
    frame.tone_strength = 0.3f;
    frame.structure_strength = 0.7f;
    frame.skin_strength = 0.5f;
    frame.feature = feature_;
    DsfDlss5Error error = {};
    if (!PipeSend(pipe_, DSF_DLSS5_MSG_FRAME, frame, kFrameTimeoutMs) ||
        !PipeReceive(pipe_, DSF_DLSS5_MSG_FRAME_ACK, ack, kFrameTimeoutMs, &error)) {
      Say("FRAME %llu failed on the pipe (error %lu; message status %d, %s)", static_cast<unsigned long long>(n), GetLastError(),
          error.status, error.message);
      return false;
    }
    return true;
  }

  // After an OK ACK: wait on fence_out on the GPU and read Output back.
  bool ReadOutput(uint64_t n, Rgba8Image* out) {
    context_->Wait(fenceOut_.Get(), n);
    context_->CopyResource(staging_.Get(), textures_[DSF_DLSS5_SLOT_OUTPUT].Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    if (out != nullptr) {
      *out = {o_.width, o_.height, std::vector<uint8_t>(static_cast<size_t>(o_.width) * o_.height * 4)};
      for (uint32_t y = 0; y < o_.height; ++y) {
        const uint8_t* row = static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        uint8_t* dst = out->pixels.data() + static_cast<size_t>(y) * o_.width * 4;
        if (Dlaa()) {
          // Linear HDR: through the scene's tone map, to compare with the colour input.
          const uint16_t* halves = reinterpret_cast<const uint16_t*>(row);
          for (uint32_t i = 0; i < o_.width * 4; ++i) {
            dst[i] = ToByte(synthetic::ToneMapValue(DirectX::PackedVector::XMConvertHalfToFloat(halves[i])));
          }
        } else {
          std::memcpy(dst, row, o_.width * 4);
        }
      }
      for (size_t i = 3; i < out->pixels.size(); i += 4) out->pixels[i] = 255;
    }
    context_->Unmap(staging_.Get(), 0);
    return true;
  }

  uint64_t FenceOutValue() const { return fenceOut_ ? fenceOut_->GetCompletedValue() : 0; }
  const std::vector<uint8_t>& LastColor() const { return colorBytes_; }

 private:
  ComPtr<ID3D11Texture2D> CreateShared(DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = o_.width;
    desc.Height = o_.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> texture;
    device_->CreateTexture2D(&desc, nullptr, &texture);
    return texture;
  }

  static uint64_t NtHandleOf(ID3D11Texture2D* texture) {
    ComPtr<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) ||
        FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
      return 0;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  }

  const Options& o_;
  ID3D11Device5* device_;
  ID3D11DeviceContext4* context_;
  HANDLE pipe_;
  uint32_t feature_;
  ComPtr<ID3D11Texture2D> textures_[DSF_DLSS5_SLOT_COUNT];
  ComPtr<ID3D11Texture2D> staging_;
  ComPtr<ID3D11Fence> fenceIn_, fenceOut_;
  std::vector<uint8_t> colorBytes_;
  std::vector<uint16_t> colorHalves_;
  std::vector<float> depth_;
  std::vector<uint16_t> motionHalves_;
};

bool Expect(bool condition, const char* what) {
  Say("%s: %s", condition ? "PASS" : "FAIL", what);
  return condition;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Same DLL search policy as the helper: System32 only for loads by name.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--helper") o.helper = next();
    else if (a == L"--runtime") o.runtime = next();
    else if (a == L"--out") o.outDir = next();
    else if (a == L"--frames") o.frames = _wtoi(next());
    else if (a == L"--width") o.width = static_cast<uint32_t>(_wtoi(next()));
    else if (a == L"--height") o.height = static_cast<uint32_t>(_wtoi(next()));
    else if (a == L"--host-creates") o.hostCreates = true;
    else if (a == L"--bypass") o.bypass = true;
    else if (a == L"--robustness") o.robustness = true;
    else if (a == L"--style") o.style = static_cast<uint32_t>(_wtoi(next()));
    else if (a == L"--feature") {
      const std::wstring f = next();
      o.features[DSF_DLSS5_FEATURE_NR] = f == L"nr" || f == L"both";
      o.features[DSF_DLSS5_FEATURE_DLAA] = f == L"dlaa" || f == L"both";
    }
    else {
      Say("unknown argument: %ls", a.c_str());
      return 2;
    }
  }
  if (o.frames < 1) o.frames = 1;
  o.helper = FullPath(o.helper);
  o.runtime = FullPath(o.runtime);
  o.outDir = FullPath(o.outDir);
  CreateDirectoryW(o.outDir.c_str(), nullptr);
  Say("client: %u-bit process, %ux%u, %d frames, %s-created textures%s%s", static_cast<unsigned>(sizeof(void*) * 8), o.width,
      o.height, o.frames, o.hostCreates ? "helper" : "client", o.bypass ? ", bypass" : "", o.robustness ? ", robustness checks" : "");

  // D3D11 on the NVIDIA adapter, as the game's DXVK/dgVoodoo device would be.
  ComPtr<IDXGIFactory1> factory;
  CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  ComPtr<IDXGIAdapter1> adapter;
  DXGI_ADAPTER_DESC1 adapterDesc = {};
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    adapter->GetDesc1(&adapterDesc);
    if (adapterDesc.VendorId == 0x10DE) break;
    adapter.Reset();
  }
  if (!adapter) {
    Say("no NVIDIA adapter");
    return 3;
  }
  ComPtr<ID3D11Device> device0;
  ComPtr<ID3D11DeviceContext> context0;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device0, nullptr,
                               &context0))) {
    Say("D3D11CreateDevice failed");
    return 3;
  }
  ComPtr<ID3D11Device5> device;
  ComPtr<ID3D11DeviceContext4> context;
  device0.As(&device);
  context0.As(&context);
  if (!device || !context) {
    Say("D3D11.4 fences are not available");
    return 3;
  }

  PROCESS_INFORMATION pi = {};
  HANDLE pipe = StartHelper(o, &pi);
  if (pipe == INVALID_HANDLE_VALUE) return 4;

  DsfDlss5Hello hello = {GetCurrentProcessId(), DSF_DLSS5_API_D3D11, adapterDesc.AdapterLuid.LowPart, adapterDesc.AdapterLuid.HighPart};
  DsfDlss5HelloAck helloAck;
  if (!PipeSend(pipe, DSF_DLSS5_MSG_HELLO, hello, kHelloTimeoutMs) ||
      !PipeReceive(pipe, DSF_DLSS5_MSG_HELLO_ACK, &helloAck, kHelloTimeoutMs)) {
    Say("HELLO failed on the pipe");
    return 5;
  }
  Say("helper pid %u: status %d, %s; features 0x%X; DLSS 5 runtime %u.%u.%u.%u, DLAA runtime %u.%u.%u.%u", helloAck.helper_pid,
      helloAck.status, helloAck.message, helloAck.features, helloAck.runtime_version[0], helloAck.runtime_version[1],
      helloAck.runtime_version[2], helloAck.runtime_version[3], helloAck.dlaa_runtime_version[0], helloAck.dlaa_runtime_version[1],
      helloAck.dlaa_runtime_version[2], helloAck.dlaa_runtime_version[3]);
  if (helloAck.status != DSF_DLSS5_OK) return 5;

  // One client per feature, over the one pipe.
  std::vector<std::unique_ptr<Client>> clients;
  for (uint32_t f = 0; f < DSF_DLSS5_FEATURE_COUNT; ++f) {
    if (!o.features[f]) continue;
    if ((helloAck.features & DSF_DLSS5_FEATURE_BIT(f)) == 0) {
      Say("%s is not available", FeatureName(f));
      return 5;
    }
    clients.push_back(std::make_unique<Client>(o, device.Get(), context.Get(), pipe, f));
    if (!clients.back()->Configure()) return 6;
  }

  const uint32_t w = o.width;
  const uint32_t h = o.height;
  double totalMs[DSF_DLSS5_FEATURE_COUNT] = {}, maxMs[DSF_DLSS5_FEATURE_COUNT] = {}, gpuMsSum[DSF_DLSS5_FEATURE_COUNT] = {};
  int failures = 0;
  for (int f = 0; f < o.frames; ++f) {
    float jx = 0.0f, jy = 0.0f;
    synthetic::HaltonJitter(f % 32, &jx, &jy);
    synthetic::Frame s;
    synthetic::Render(w, h, f, jx, jy, &s);

    for (const auto& c : clients) {
      Client& client = *c;
      const uint32_t feature = client.Feature();
      client.Prepare(s);
      const auto start = std::chrono::steady_clock::now();
      const uint64_t n = static_cast<uint64_t>(f) + 1;
      client.Upload(n, true);
      DsfDlss5FrameAck ack;
      if (!client.Frame(n, f == 0 ? DSF_DLSS5_FRAME_RESET : 0, jx, jy, &ack)) return 7;
      // Check the ACK before queueing the GPU wait on fence_out (protocol rule).
      if (ack.status != DSF_DLSS5_OK || ack.frame != n || ack.feature != feature) {
        ++failures;
        Say("%s frame %llu: status %d, feature %u", FeatureName(feature), static_cast<unsigned long long>(n), ack.status, ack.feature);
        continue;
      }
      const bool last = f == o.frames - 1;
      Rgba8Image out;
      if (!client.ReadOutput(n, last ? &out : nullptr)) {
        Say("Map failed");
        return 7;
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      totalMs[feature] += ms;
      maxMs[feature] = std::max(maxMs[feature], ms);
      gpuMsSum[feature] += ack.gpu_ms;
      if (last) {
        const std::wstring suffix = client.Dlaa() ? L"_dlaa" : L"";
        Rgba8Image input{w, h, client.LastColor()};
        std::string error;
        SavePngRgba8(o.outDir + L"\\input_last.png", input, &error);
        SavePngRgba8(o.outDir + L"\\output_last" + suffix + L".png", out, &error);
        double sum = 0.0;
        for (size_t i = 0; i < out.pixels.size(); i += 4) {
          int d = 0;
          for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(out.pixels[i + k]) - int(input.pixels[i + k])));
          sum += d;
        }
        Say("%s last frame: mean max-channel difference input vs output %.2f/255", FeatureName(feature), sum / (static_cast<double>(w) * h));
        Say("wrote %ls\\output_last%ls.png", o.outDir.c_str(), suffix.c_str());
      }
    }
  }
  for (const auto& c : clients) {
    const uint32_t feature = c->Feature();
    Say("%s round trip (upload + helper + copy back) per frame: mean %.2f ms, max %.2f ms; helper GPU time mean %.2f ms",
        FeatureName(feature), totalMs[feature] / o.frames, maxMs[feature], gpuMsSum[feature] / o.frames);
  }
  Say("%d/%d frames OK", o.frames * static_cast<int>(clients.size()) - failures, o.frames * static_cast<int>(clients.size()));

  bool checks = true;
  if (o.robustness) {
    Client& client = *clients.front();
    synthetic::Frame s;
    synthetic::Render(w, h, 0, 0.0f, 0.0f, &s);
    client.Prepare(s);
    const uint64_t last = static_cast<uint64_t>(o.frames);
    DsfDlss5FrameAck ack;

    // A repeated frame number must be refused without touching the fences.
    checks &= client.Frame(last, 0, 0, 0, &ack) && Expect(ack.status == DSF_DLSS5_ERR_PROTOCOL, "repeated frame number -> ERR_PROTOCOL");
    checks &= client.Frame(0, 0, 0, 0, &ack) && Expect(ack.status == DSF_DLSS5_ERR_PROTOCOL, "frame number 0 -> ERR_PROTOCOL");

    // A frame whose fence_in never arrives: the helper must time out, reach fence_out = n
    // itself and drop the configuration.
    const uint64_t stalled = last + 1;
    client.Upload(stalled, false);
    const auto start = std::chrono::steady_clock::now();
    checks &= client.Frame(stalled, 0, 0, 0, &ack) && Expect(ack.status == DSF_DLSS5_ERR_TIMEOUT, "missing fence_in -> ERR_TIMEOUT");
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Say("  (ACK after %.0f ms; fence_out is now %llu)", waited, static_cast<unsigned long long>(client.FenceOutValue()));
    checks &= Expect(client.FenceOutValue() >= stalled, "fence_out reached the stalled frame, so a queued GPU wait would not hang");

    // Unconfigured now: the frame is refused but fence_out still advances.
    checks &= client.Frame(stalled + 1, 0, 0, 0, &ack) &&
              Expect(ack.status == DSF_DLSS5_ERR_NOT_CONFIGURED, "frame after the timeout -> ERR_NOT_CONFIGURED");
    checks &= Expect(client.FenceOutValue() >= stalled + 1, "fence_out reached the refused frame");

    // A fresh CONFIGURE restarts the numbering at 1 and works again.
    checks &= Expect(client.Configure(), "CONFIGURE after the timeout");
    for (uint64_t n = 1; n <= 3 && checks; ++n) {
      client.Upload(n, true);
      checks &= client.Frame(n, n == 1 ? DSF_DLSS5_FRAME_RESET : 0, 0, 0, &ack) && ack.status == DSF_DLSS5_OK && client.ReadOutput(n, nullptr);
    }
    checks &= Expect(checks, "three frames after reconfiguring");

    // Protocol version 2: an unknown feature is refused in FRAME and in CONFIGURE.
    DsfDlss5Frame unknown = {};
    unknown.frame = 100;
    unknown.feature = DSF_DLSS5_FEATURE_COUNT;
    checks &= PipeSend(pipe, DSF_DLSS5_MSG_FRAME, unknown, kFrameTimeoutMs) &&
              PipeReceive(pipe, DSF_DLSS5_MSG_FRAME_ACK, &ack, kFrameTimeoutMs) &&
              Expect(ack.status == DSF_DLSS5_ERR_PROTOCOL && ack.feature == unknown.feature, "FRAME for an unknown feature -> ERR_PROTOCOL");
    DsfDlss5Configure unknownConfigure = {};
    unknownConfigure.width = w;
    unknownConfigure.height = h;
    unknownConfigure.feature = DSF_DLSS5_FEATURE_COUNT;
    DsfDlss5ConfigureAck configureAck;
    checks &= PipeSend(pipe, DSF_DLSS5_MSG_CONFIGURE, unknownConfigure, kConfigureTimeoutMs) &&
              PipeReceive(pipe, DSF_DLSS5_MSG_CONFIGURE_ACK, &configureAck, kConfigureTimeoutMs) &&
              Expect(configureAck.status == DSF_DLSS5_ERR_PROTOCOL, "CONFIGURE for an unknown feature -> ERR_PROTOCOL");
    // The other feature has its own state: never configured here, so its frames are refused.
    if (clients.size() == 1) {
      DsfDlss5Frame other = {};
      other.frame = 1;
      other.feature = client.Feature() == DSF_DLSS5_FEATURE_NR ? DSF_DLSS5_FEATURE_DLAA : DSF_DLSS5_FEATURE_NR;
      checks &= PipeSend(pipe, DSF_DLSS5_MSG_FRAME, other, kFrameTimeoutMs) &&
                PipeReceive(pipe, DSF_DLSS5_MSG_FRAME_ACK, &ack, kFrameTimeoutMs) &&
                Expect(ack.status == DSF_DLSS5_ERR_NOT_CONFIGURED && ack.feature == other.feature,
                       "FRAME for the other, unconfigured feature -> ERR_NOT_CONFIGURED");
    }
    // ... and the configured one still runs.
    client.Upload(4, true);
    checks &= client.Frame(4, 0, 0, 0, &ack) && Expect(ack.status == DSF_DLSS5_OK && client.ReadOutput(4, nullptr),
                                                       "the configured feature still runs after them");

    // A version 1 header is answered with MSG_ERROR, then the helper exits.
    DsfDlss5Header bad = MakeHeader(DSF_DLSS5_MSG_FRAME, 0);
    bad.version = 1;
    DsfDlss5Header reply = {};
    DsfDlss5Error error = {};
    const bool answered = PipeWriteAll(pipe, &bad, sizeof(bad), kFrameTimeoutMs) && PipeReadHeader(pipe, &reply, kFrameTimeoutMs) &&
                          reply.type == DSF_DLSS5_MSG_ERROR && PipeReadPayload(pipe, reply, &error, kFrameTimeoutMs);
    checks &= Expect(answered && error.status == DSF_DLSS5_ERR_PROTOCOL, "protocol version 1 header -> MSG_ERROR(ERR_PROTOCOL)");
  } else {
    PipeSendEmpty(pipe, DSF_DLSS5_MSG_SHUTDOWN, kFrameTimeoutMs);
  }
  CloseHandle(pipe);
  WaitForSingleObject(pi.hProcess, 10000);
  DWORD helperExit = 0;
  GetExitCodeProcess(pi.hProcess, &helperExit);
  Say("helper exit code %lu", helperExit);
  if (o.robustness) Say("robustness checks %s", checks ? "passed" : "FAILED");
  return failures == 0 && checks ? 0 : 8;
}
