// SPDX-License-Identifier: MIT
//
// dsf-dlss5-offscreen: runs the DLSS 5 (DLSSNR) pass on synthetic or saved inputs with no
// window and no game, and writes the result as PNG.
//
//   dsf-dlss5-offscreen.exe --runtime <dir with nvngx_dlssnr.dll> --out <dir>
//       [--color in.png [--depth depth.pfm] [--mv motion.pfm]]   still image instead of the scene
//       [--frames N] [--width W --height H] [--style 0..2] [--intensity f] [--tone f]
//       [--structure f] [--input-format f16|u8] [--no-jitter] [--save-every] [--verbose-ngx]
//       [--static-camera] [--jitter-scale f] [--stability] [--zero-mv] [--flat-depth]
//       [--allow-runtime-sha256 <hash>]
//
// --static-camera keeps the synthetic camera still (only the jitter moves), --jitter-scale
// multiplies the jitter reported to NGX (-1 tests the opposite sign convention), and
// --stability reads back every output and reports the mean frame-to-frame change, which is
// how the jitter convention in research/dlss5-helper.md was checked.
//
// Exit code 0 means every evaluate succeeded and the output was written.

#include <windows.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/d3d12_util.h"
#include "../src/image_io.h"
#include "../src/log.h"
#include "../src/ngx_trust.h"
#include "../src/nr_d3d12.h"
#include "synthetic_scene.h"

using namespace dsf_dlss5;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

namespace {

struct Options {
  std::wstring runtimeDir = L"out\\ngx";
  std::wstring outDir = L"out\\dlss5-test";
  std::wstring colorPath, depthPath, mvPath;
  int frames = 24;
  uint32_t width = 1280;
  uint32_t height = 720;
  bool f16Input = true;
  bool jitter = true;
  bool saveEvery = false;
  bool verboseNgx = false;
  bool staticCamera = false;
  float jitterScale = 1.0f;
  bool stability = false;
  bool zeroMotion = false;
  bool flatDepth = false;
  std::vector<std::string> allowedSha256;
  NrSettings nr;
};

bool ParseArgs(int argc, wchar_t** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--runtime") o->runtimeDir = next();
    else if (a == L"--out") o->outDir = next();
    else if (a == L"--color") o->colorPath = next();
    else if (a == L"--depth") o->depthPath = next();
    else if (a == L"--mv") o->mvPath = next();
    else if (a == L"--frames") o->frames = _wtoi(next());
    else if (a == L"--width") o->width = static_cast<uint32_t>(_wtoi(next()));
    else if (a == L"--height") o->height = static_cast<uint32_t>(_wtoi(next()));
    else if (a == L"--style") o->nr.style = static_cast<uint32_t>(_wtoi(next()));
    else if (a == L"--intensity") o->nr.intensity = static_cast<float>(_wtof(next()));
    else if (a == L"--tone") o->nr.toneStrength = static_cast<float>(_wtof(next()));
    else if (a == L"--structure") o->nr.structureStrength = static_cast<float>(_wtof(next()));
    else if (a == L"--input-format") o->f16Input = std::wstring(next()) != L"u8";
    else if (a == L"--no-jitter") o->jitter = false;
    else if (a == L"--save-every") o->saveEvery = true;
    else if (a == L"--verbose-ngx") o->verboseNgx = true;
    else if (a == L"--static-camera") o->staticCamera = true;
    else if (a == L"--jitter-scale") o->jitterScale = static_cast<float>(_wtof(next()));
    else if (a == L"--stability") o->stability = true;
    else if (a == L"--zero-mv") o->zeroMotion = true;
    else if (a == L"--flat-depth") o->flatDepth = true;
    else if (a == L"--allow-runtime-sha256") o->allowedSha256.push_back(Narrow(next()));
    else {
      std::fwprintf(stderr, L"unknown argument: %ls\n", a.c_str());
      return false;
    }
  }
  return o->frames > 0 && o->width > 0 && o->height > 0;
}

std::wstring FullPath(const std::wstring& path) {
  wchar_t buffer[MAX_PATH] = {};
  GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
  return buffer;
}

uint8_t ToByte(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

Rgba8Image ToRgba8(const std::vector<float>& rgba, uint32_t width, uint32_t height) {
  Rgba8Image image{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
  for (size_t i = 0; i < image.pixels.size(); ++i) image.pixels[i] = (i % 4 == 3) ? 255 : ToByte(rgba[i]);
  return image;
}

// Side by side: input | output | |difference| x4.
Rgba8Image Compare(const Rgba8Image& in, const Rgba8Image& out) {
  const uint32_t w = in.width;
  const uint32_t h = in.height;
  Rgba8Image c{w * 3, h, std::vector<uint8_t>(static_cast<size_t>(w) * 3 * h * 4, 255)};
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const size_t s = (static_cast<size_t>(y) * w + x) * 4;
      for (int k = 0; k < 3; ++k) {
        const int diff = std::abs(static_cast<int>(out.pixels[s + k]) - static_cast<int>(in.pixels[s + k]));
        c.pixels[(static_cast<size_t>(y) * w * 3 + x) * 4 + k] = in.pixels[s + k];
        c.pixels[(static_cast<size_t>(y) * w * 3 + w + x) * 4 + k] = out.pixels[s + k];
        c.pixels[(static_cast<size_t>(y) * w * 3 + 2 * w + x) * 4 + k] = static_cast<uint8_t>(std::min(255, diff * 4));
      }
    }
  }
  return c;
}

bool Save(const std::wstring& path, const Rgba8Image& image) {
  std::string error;
  if (!SavePngRgba8(path, image, &error)) {
    Log("could not write %ls: %s", path.c_str(), error.c_str());
    return false;
  }
  Log("wrote %ls", path.c_str());
  return true;
}

struct Inputs {
  std::vector<float> color;   // RGBA
  std::vector<float> depth;   // 1 channel
  std::vector<float> motion;  // RG
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Same DLL search policy as the helper: System32 only for loads by name.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
  Options o;
  if (!ParseArgs(argc, argv, &o)) {
    std::fprintf(stderr, "usage: see the comment at the top of offscreen_test.cpp\n");
    return 2;
  }
  o.runtimeDir = FullPath(o.runtimeDir);
  o.outDir = FullPath(o.outDir);
  CreateDirectoryW(o.outDir.c_str(), nullptr);
  OpenLogFile((o.outDir + L"\\offscreen_test.log").c_str());

  // 1. The runtime the user supplied must be a known (or explicitly allowed) NVIDIA-signed
  // public release. It stays locked until NGX has shut down.
  VerifiedRuntime runtime;
  std::string error;
  if (!runtime.Verify(o.runtimeDir + L"\\nvngx_dlssnr.dll", o.allowedSha256, &error)) {
    Log("runtime check failed: %s", error.c_str());
    return 3;
  }
  Log("runtime %ls: version %u.%u.%u.%u, signer \"%ls\", %llu bytes, sha256 %s (%s)", runtime.Path().c_str(),
      runtime.Version()[0], runtime.Version()[1], runtime.Version()[2], runtime.Version()[3], runtime.Signer().c_str(),
      static_cast<unsigned long long>(runtime.Size()), runtime.Sha256().c_str(), runtime.Description());

  // 2. Inputs: a still image, or the synthetic scene.
  uint32_t width = o.width;
  uint32_t height = o.height;
  const bool still = !o.colorPath.empty();
  Inputs stillInputs;
  if (still) {
    Rgba8Image color;
    if (!LoadImageRgba8(o.colorPath, &color, &error)) {
      Log("%ls: %s", o.colorPath.c_str(), error.c_str());
      return 4;
    }
    width = color.width;
    height = color.height;
    const size_t n = static_cast<size_t>(width) * height;
    stillInputs.color.resize(n * 4);
    for (size_t i = 0; i < n * 4; ++i) stillInputs.color[i] = color.pixels[i] / 255.0f;
    stillInputs.depth.assign(n, 0.5f);
    stillInputs.motion.assign(n * 2, 0.0f);
    if (!o.depthPath.empty()) {
      FloatImage depth;
      if (!LoadPfm(o.depthPath, &depth, &error) || depth.width != width || depth.height != height || depth.channels != 1) {
        Log("%ls: %s (needs a 1-channel PFM the size of the colour)", o.depthPath.c_str(), error.c_str());
        return 4;
      }
      stillInputs.depth = depth.pixels;
    }
    if (!o.mvPath.empty()) {
      FloatImage mv;
      if (!LoadPfm(o.mvPath, &mv, &error) || mv.width != width || mv.height != height || mv.channels != 3) {
        Log("%ls: %s (needs a 3-channel PFM the size of the colour)", o.mvPath.c_str(), error.c_str());
        return 4;
      }
      for (size_t i = 0; i < n; ++i) {
        stillInputs.motion[i * 2] = mv.pixels[i * 3];
        stillInputs.motion[i * 2 + 1] = mv.pixels[i * 3 + 1];
      }
    }
    o.jitter = false;
    Log("still input %ls, %ux%u", o.colorPath.c_str(), width, height);
  } else {
    Log("synthetic street scene, %ux%u, %d frames", width, height, o.frames);
  }

  // 3. D3D12 and NGX.
  Gpu gpu;
  if (!gpu.Create(LUID{}, &error)) {
    Log("D3D12: %s", error.c_str());
    return 5;
  }
  Log("adapter: %ls", gpu.AdapterName().c_str());
  NrD3D12 nr;
  if (!nr.Initialize(gpu.Device(), runtime, o.outDir, o.verboseNgx)) return 6;

  const DXGI_FORMAT colorFormat = o.f16Input ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  ComPtr<ID3D12Resource> color = gpu.CreateTexture(width, height, colorFormat, D3D12_RESOURCE_FLAG_NONE, kRead);
  ComPtr<ID3D12Resource> output = gpu.CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComPtr<ID3D12Resource> depth = gpu.CreateTexture(width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead);
  ComPtr<ID3D12Resource> motion = gpu.CreateTexture(width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead);
  ComPtr<ID3D12Resource> mask = gpu.CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, kRead);
  if (!color || !output || !depth || !motion || !mask) {
    Log("could not create the textures");
    return 5;
  }

  {
    ID3D12GraphicsCommandList* list = gpu.Begin();
    // The control mask is all ones (apply NR everywhere), as Remix clears it.
    std::vector<uint8_t> ones(static_cast<size_t>(width) * height * 4, 255);
    Gpu::Transition(list, mask.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.Upload(mask.Get(), ones.data(), 4);
    Gpu::Transition(list, mask.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    if (!nr.CreateFeature(list, width, height, 2 /* preset B, as Remix */) || !gpu.SubmitAndWait()) {
      Log("feature creation failed");
      return 7;
    }
  }

  Inputs frame;
  std::vector<uint8_t> colorBytes;
  std::vector<uint16_t> colorHalves, motionHalves;
  Rgba8Image firstInput, lastInput;
  std::vector<double> gpuTimes;
  std::vector<float> previousOutput;
  double stabilitySum = 0.0;
  int stabilityFrames = 0;
  int failures = 0;
  for (int f = 0; f < o.frames; ++f) {
    float jx = 0.0f, jy = 0.0f;
    if (o.jitter) synthetic::HaltonJitter(f % 32, &jx, &jy);
    if (still) {
      frame = stillInputs;
    } else {
      synthetic::Frame s;
      synthetic::Render(width, height, o.staticCamera ? 0 : f, jx, jy, &s);
      frame.color = std::move(s.color);
      frame.depth = std::move(s.depth);
      frame.motion = std::move(s.motion);
    }
    // Ablations: does the runtime use the guides at all?
    if (o.zeroMotion) std::fill(frame.motion.begin(), frame.motion.end(), 0.0f);
    if (o.flatDepth) std::fill(frame.depth.begin(), frame.depth.end(), 0.5f);
    if (f == 0) firstInput = ToRgba8(frame.color, width, height);
    if (f == o.frames - 1) lastInput = ToRgba8(frame.color, width, height);

    ID3D12GraphicsCommandList* list = gpu.Begin();
    for (ID3D12Resource* r : {color.Get(), depth.Get(), motion.Get()}) Gpu::Transition(list, r, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    if (o.f16Input) {
      colorHalves.resize(frame.color.size());
      for (size_t i = 0; i < frame.color.size(); ++i) colorHalves[i] = XMConvertFloatToHalf(frame.color[i]);
      gpu.Upload(color.Get(), colorHalves.data(), 8);
    } else {
      colorBytes.resize(frame.color.size());
      for (size_t i = 0; i < frame.color.size(); ++i) colorBytes[i] = ToByte(frame.color[i]);
      gpu.Upload(color.Get(), colorBytes.data(), 4);
    }
    gpu.Upload(depth.Get(), frame.depth.data(), 4);
    motionHalves.resize(frame.motion.size());
    for (size_t i = 0; i < frame.motion.size(); ++i) motionHalves[i] = XMConvertFloatToHalf(frame.motion[i]);
    gpu.Upload(motion.Get(), motionHalves.data(), 4);
    for (ID3D12Resource* r : {color.Get(), depth.Get(), motion.Get()}) Gpu::Transition(list, r, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    if (!gpu.SubmitAndWait()) {
      Log("upload failed (device removed?)");
      return 8;
    }

    NrSettings settings = o.nr;
    settings.reset = f == 0;
    settings.jitter[0] = jx * o.jitterScale;
    settings.jitter[1] = jy * o.jitterScale;
    list = gpu.Begin();
    const bool ok = nr.Evaluate(list, {color.Get(), output.Get(), motion.Get(), depth.Get(), mask.Get()}, settings);
    double gpuMs = -1.0;
    if (!gpu.SubmitAndWait(&gpuMs) || !ok) {
      ++failures;
      Log("frame %d: evaluate failed: %s", f, nr.LastError().c_str());
      continue;
    }
    gpuTimes.push_back(gpuMs);

    if (o.saveEvery || o.stability || f == o.frames - 1) {
      list = gpu.Begin();
      Gpu::Transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.SubmitAndWait();
      std::vector<uint8_t> raw;
      if (!gpu.Readback(output.Get(), 8, &raw)) {
        Log("readback failed");
        return 8;
      }
      list = gpu.Begin();
      Gpu::Transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.SubmitAndWait();
      const uint16_t* halves = reinterpret_cast<const uint16_t*>(raw.data());
      std::vector<float> out(static_cast<size_t>(width) * height * 4);
      for (size_t i = 0; i < out.size(); ++i) out[i] = XMConvertHalfToFloat(halves[i]);
      if (o.stability && !previousOutput.empty() && f >= 8) {
        double sum = 0.0;
        for (size_t i = 0; i < out.size(); ++i) {
          if (i % 4 != 3) sum += std::fabs(out[i] - previousOutput[i]);
        }
        stabilitySum += sum / (static_cast<double>(width) * height * 3);
        ++stabilityFrames;
      }
      previousOutput = out;
      const Rgba8Image outImage = ToRgba8(out, width, height);
      const Rgba8Image inImage = ToRgba8(frame.color, width, height);
      wchar_t name[64];
      if (o.saveEvery) {
        swprintf_s(name, L"\\frame_%03d_output.png", f);
        Save(o.outDir + name, outImage);
      }
      if (f == o.frames - 1) {
        Save(o.outDir + L"\\input_last.png", inImage);
        Save(o.outDir + L"\\output_last.png", outImage);
        Save(o.outDir + L"\\compare_last.png", Compare(inImage, outImage));
        double sum = 0.0;
        int changed = 0;
        for (size_t i = 0; i < inImage.pixels.size(); i += 4) {
          int d = 0;
          for (int k = 0; k < 3; ++k) d = std::max(d, std::abs(int(outImage.pixels[i + k]) - int(inImage.pixels[i + k])));
          sum += d;
          changed += d > 8;
        }
        const double pixels = static_cast<double>(width) * height;
        Log("last frame: mean max-channel difference %.2f/255, %.1f%% of pixels changed by more than 8/255", sum / pixels,
            100.0 * changed / pixels);
      }
    }
  }
  Save(o.outDir + L"\\input_first.png", firstInput);

  if (!gpuTimes.empty()) {
    std::vector<double> sorted = gpuTimes;
    std::sort(sorted.begin(), sorted.end());
    Log("evaluate GPU time over %zu frames: median %.2f ms, max %.2f ms", sorted.size(), sorted[sorted.size() / 2], sorted.back());
  }
  if (stabilityFrames != 0) {
    Log("output stability: mean frame-to-frame change %.3f/255 over %d frames (jitter scale %.1f)",
        255.0 * stabilitySum / stabilityFrames, stabilityFrames, o.jitterScale);
  }
  Log("NGX mapped %ls", nr.LoadedRuntimePath().c_str());
  Log("%d/%d evaluates succeeded", o.frames - failures, o.frames);
  nr.Shutdown();
  return failures == 0 ? 0 : 9;
}
