// SPDX-License-Identifier: MIT
//
// dsf-dlss5-client-test (x86): stands in for the 32-bit game side. It starts the helper,
// shares D3D11 textures and fences with it, feeds the synthetic scene through the real
// protocol and writes the helper's output as PNG. No window, no game.
//
//   dsf-dlss5-client-test.exe --helper <dsf-dlss5-helper.exe> --runtime <dir> --out <dir>
//       [--frames N] [--width W --height H] [--host-creates] [--bypass] [--style 0..2]
//
// Exit code 0 means every frame came back DSF_DLSS5_OK and the output was written.

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsf_dlss5_pipe.h"
#include "../src/image_io.h"
#include "synthetic_scene.h"

using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::XMConvertFloatToHalf;
using namespace dsf_dlss5;

namespace {

struct Options {
  std::wstring helper = L"dlss5\\build\\x64\\dsf-dlss5-helper.exe";
  std::wstring runtime = L"out\\ngx";
  std::wstring outDir = L"out\\dlss5-client-test";
  int frames = 24;
  uint32_t width = 1280;
  uint32_t height = 720;
  bool hostCreates = false;
  bool bypass = false;
  uint32_t style = 0;
};

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

HANDLE StartHelper(const Options& o, PROCESS_INFORMATION* pi) {
  // Hand the helper a handle to this process, so it needs no OpenProcess rights on us.
  HANDLE self = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                  PROCESS_DUP_HANDLE | SYNCHRONIZE, TRUE, 0);
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
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;
    if (WaitForSingleObject(pi->hProcess, 0) == WAIT_OBJECT_0) break;
    Sleep(50);
  }
  Say("could not connect to the helper pipe");
  return INVALID_HANDLE_VALUE;
}

ComPtr<ID3D11Texture2D> CreateShared(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bind) {
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
  ComPtr<ID3D11Texture2D> texture;
  device->CreateTexture2D(&desc, nullptr, &texture);
  return texture;
}

uint64_t NtHandleOf(ID3D11Texture2D* texture) {
  ComPtr<IDXGIResource1> resource;
  HANDLE handle = nullptr;
  if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) ||
      FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
    return 0;
  }
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

HANDLE AsHandle(uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); }

}  // namespace

int wmain(int argc, wchar_t** argv) {
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
    else if (a == L"--style") o.style = static_cast<uint32_t>(_wtoi(next()));
    else {
      Say("unknown argument: %ls", a.c_str());
      return 2;
    }
  }
  o.helper = FullPath(o.helper);
  o.runtime = FullPath(o.runtime);
  o.outDir = FullPath(o.outDir);
  CreateDirectoryW(o.outDir.c_str(), nullptr);
  Say("client: %u-bit process, %ux%u, %d frames, %s-created textures%s", static_cast<unsigned>(sizeof(void*) * 8), o.width,
      o.height, o.frames, o.hostCreates ? "helper" : "client", o.bypass ? ", bypass" : "");

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
  if (!PipeSend(pipe, DSF_DLSS5_MSG_HELLO, hello) || !PipeReceive(pipe, DSF_DLSS5_MSG_HELLO_ACK, &helloAck)) {
    Say("HELLO failed on the pipe");
    return 5;
  }
  Say("helper pid %u: status %d, %s; runtime %u.%u.%u.%u", helloAck.helper_pid, helloAck.status, helloAck.message,
      helloAck.runtime_version[0], helloAck.runtime_version[1], helloAck.runtime_version[2], helloAck.runtime_version[3]);
  if (helloAck.status != DSF_DLSS5_OK) return 5;

  const uint32_t w = o.width;
  const uint32_t h = o.height;
  const DXGI_FORMAT formats[DSF_DLSS5_SLOT_COUNT] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
                                                    DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN};
  ComPtr<ID3D11Texture2D> textures[DSF_DLSS5_SLOT_COUNT];
  DsfDlss5Configure configure = {};
  configure.width = w;
  configure.height = h;
  configure.preset = 2;
  for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) configure.formats[slot] = formats[slot];
  if (!o.hostCreates) {
    configure.flags |= DSF_DLSS5_CONFIG_CLIENT_CREATES;
    for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
      const UINT bind = D3D11_BIND_SHADER_RESOURCE | (slot == DSF_DLSS5_SLOT_OUTPUT ? D3D11_BIND_UNORDERED_ACCESS : 0);
      textures[slot] = CreateShared(device.Get(), w, h, formats[slot], bind);
      configure.textures[slot] = textures[slot] ? NtHandleOf(textures[slot].Get()) : 0;
      if (configure.textures[slot] == 0) {
        Say("could not create shared texture %d", slot);
        return 6;
      }
    }
  }
  DsfDlss5ConfigureAck configureAck;
  if (!PipeSend(pipe, DSF_DLSS5_MSG_CONFIGURE, configure) || !PipeReceive(pipe, DSF_DLSS5_MSG_CONFIGURE_ACK, &configureAck)) {
    Say("CONFIGURE failed on the pipe");
    return 6;
  }
  Say("configure: status %d, %s", configureAck.status, configureAck.message);
  if (configureAck.status != DSF_DLSS5_OK) return 6;
  if (o.hostCreates) {
    for (int slot = 0; slot < DSF_DLSS5_SLOT_MASK; ++slot) {
      const HRESULT hr = device->OpenSharedResource1(AsHandle(configureAck.textures[slot]), IID_PPV_ARGS(&textures[slot]));
      CloseHandle(AsHandle(configureAck.textures[slot]));
      if (FAILED(hr)) {
        Say("OpenSharedResource1 failed for helper-created slot %d: 0x%08lX", slot, static_cast<unsigned long>(hr));
        return 6;
      }
    }
  }
  ComPtr<ID3D11Fence> fenceIn, fenceOut;
  if (FAILED(device->OpenSharedFence(AsHandle(configureAck.fence_in), IID_PPV_ARGS(&fenceIn))) ||
      FAILED(device->OpenSharedFence(AsHandle(configureAck.fence_out), IID_PPV_ARGS(&fenceOut)))) {
    Say("OpenSharedFence failed");
    return 6;
  }
  CloseHandle(AsHandle(configureAck.fence_in));
  CloseHandle(AsHandle(configureAck.fence_out));

  D3D11_TEXTURE2D_DESC stagingDesc = {};
  textures[DSF_DLSS5_SLOT_OUTPUT]->GetDesc(&stagingDesc);
  stagingDesc.Usage = D3D11_USAGE_STAGING;
  stagingDesc.BindFlags = 0;
  stagingDesc.MiscFlags = 0;
  stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&stagingDesc, nullptr, &staging);

  std::vector<uint8_t> colorBytes(static_cast<size_t>(w) * h * 4);
  std::vector<uint16_t> motionHalves(static_cast<size_t>(w) * h * 2);
  Rgba8Image lastInput{w, h, {}};
  double totalMs = 0.0, maxMs = 0.0, gpuMsSum = 0.0;
  int failures = 0;
  for (int f = 0; f < o.frames; ++f) {
    float jx = 0.0f, jy = 0.0f;
    synthetic::HaltonJitter(f % 32, &jx, &jy);
    synthetic::Frame s;
    synthetic::Render(w, h, f, jx, jy, &s);
    for (size_t i = 0; i < colorBytes.size(); ++i) {
      colorBytes[i] = static_cast<uint8_t>(std::lround(std::clamp(s.color[i], 0.0f, 1.0f) * 255.0f));
    }
    for (size_t i = 0; i < motionHalves.size(); ++i) motionHalves[i] = XMConvertFloatToHalf(s.motion[i]);

    const auto start = std::chrono::steady_clock::now();
    const uint64_t n = static_cast<uint64_t>(f) + 1;
    context->UpdateSubresource(textures[DSF_DLSS5_SLOT_COLOR].Get(), 0, nullptr, colorBytes.data(), w * 4, 0);
    context->UpdateSubresource(textures[DSF_DLSS5_SLOT_DEPTH].Get(), 0, nullptr, s.depth.data(), w * 4, 0);
    context->UpdateSubresource(textures[DSF_DLSS5_SLOT_MOTION].Get(), 0, nullptr, motionHalves.data(), w * 4, 0);
    context->Signal(fenceIn.Get(), n);
    context->Flush();

    DsfDlss5Frame frame = {};
    frame.frame = n;
    frame.flags = DSF_DLSS5_FRAME_AUTO_MASK | DSF_DLSS5_FRAME_ACK_AFTER_GPU | (f == 0 ? DSF_DLSS5_FRAME_RESET : 0) |
                  (o.bypass ? DSF_DLSS5_FRAME_BYPASS : 0);
    frame.style = o.style;
    frame.jitter[0] = jx;
    frame.jitter[1] = jy;
    frame.mv_scale[0] = frame.mv_scale[1] = 1.0f;
    frame.intensity = 1.0f;
    frame.tone_strength = 0.3f;
    frame.structure_strength = 0.7f;
    frame.skin_strength = 0.5f;
    DsfDlss5FrameAck ack;
    if (!PipeSend(pipe, DSF_DLSS5_MSG_FRAME, frame) || !PipeReceive(pipe, DSF_DLSS5_MSG_FRAME_ACK, &ack)) {
      Say("FRAME %llu failed on the pipe", static_cast<unsigned long long>(n));
      return 7;
    }
    context->Wait(fenceOut.Get(), n);
    context->CopyResource(staging.Get(), textures[DSF_DLSS5_SLOT_OUTPUT].Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    // Mapping waits for the copy, which waited for the helper: this is the full round trip.
    const bool last = f == o.frames - 1;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      Say("Map failed");
      return 7;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    totalMs += ms;
    maxMs = std::max(maxMs, ms);
    gpuMsSum += ack.gpu_ms;
    if (ack.status != DSF_DLSS5_OK || ack.frame != n) {
      ++failures;
      Say("frame %llu: status %d", static_cast<unsigned long long>(n), ack.status);
    }
    if (last) {
      Rgba8Image out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
      for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(out.pixels.data() + static_cast<size_t>(y) * w * 4, static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, w * 4);
      }
      for (size_t i = 3; i < out.pixels.size(); i += 4) out.pixels[i] = 255;
      lastInput.pixels = colorBytes;
      std::string error;
      SavePngRgba8(o.outDir + L"\\input_last.png", lastInput, &error);
      SavePngRgba8(o.outDir + L"\\output_last.png", out, &error);
      double sum = 0.0;
      for (size_t i = 0; i < out.pixels.size(); i += 4) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(out.pixels[i + k]) - int(lastInput.pixels[i + k])));
        sum += d;
      }
      Say("last frame: mean max-channel difference input vs output %.2f/255", sum / (static_cast<double>(w) * h));
      Say("wrote %ls\\output_last.png", o.outDir.c_str());
    }
    context->Unmap(staging.Get(), 0);
  }
  Say("round trip (upload + helper DLSS 5 + copy back) per frame: mean %.2f ms, max %.2f ms; helper GPU time mean %.2f ms",
      totalMs / o.frames, maxMs, gpuMsSum / o.frames);
  PipeSendEmpty(pipe, DSF_DLSS5_MSG_SHUTDOWN);
  CloseHandle(pipe);
  WaitForSingleObject(pi.hProcess, 10000);
  DWORD helperExit = 0;
  GetExitCodeProcess(pi.hProcess, &helperExit);
  Say("%d/%d frames OK; helper exit code %lu", o.frames - failures, o.frames, helperExit);
  return failures == 0 ? 0 : 8;
}
