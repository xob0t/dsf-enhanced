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
//       [--hdr in.pfm [--exposure f] [--no-highlight-recovery] [--bypass]]
//   dsf-dlss5-offscreen.exe --feature dlaa --runtime <dir with nvngx_dlss.dll> --out <dir>
//       [--frames N (16+)] [--width W --height H] [--phases N] [--preset K|L]
//       [--jitter-sign sx,sy] [--allow-runtime-sha256 <hash>]
//
// --feature dlaa runs DLAA (DLSS Super Resolution at 1.0, research/dlaa.md) instead: the jitter
// sign and scale test described at RunDlaa below. It runs every sign and scale variant unless
// --jitter-sign names one, and exits with 10 if the expected one (+x +y, 1x) is not the best.
//
// --static-camera keeps the synthetic camera still (only the jitter moves), --jitter-scale
// multiplies the jitter reported to NGX (-1 tests the opposite sign convention), and
// --stability reads back every output and reports the mean frame-to-frame change, which is
// how the jitter convention in research/dlss5-helper.md was checked.
//
// --hdr takes a linear HDR frame (an RGB PFM, as DSF Enhanced's TraceDump writes the scene
// target) and runs it the way DSF Enhanced's Input=1 does in game: the fast tone map (exposure,
// ACES approximation, 1/2.2 gamma) into the f16 colour input, DLSS 5, then the inverse tone map
// with highlight recovery back to HDR (hdr_output.pfm). The maths here mirrors the D3D9 shaders
// in src/dlss5_bridge.inl (HdrTonemapPS, HdrInversePS) and Remix's fast and inverse tone map.
// --bypass skips DLSS 5 (output = input) to check that the round trip gives the frame back.
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
#include "../src/ngx_d3d12.h"
#include "../src/ngx_trust.h"
#include "../src/nr_d3d12.h"
#include "../src/sr_d3d12.h"
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
  std::wstring hdrPath;
  float exposure = 1.0f;
  bool highlightRecovery = true;
  bool bypass = false;
  std::vector<std::string> allowedSha256;
  NrSettings nr;
  // --feature dlaa
  bool dlaa = false;
  bool sizeGiven = false;
  int phases = 16;
  int dlaaPreset = SrD3D12::kPresetK;
  bool signGiven = false;
  float signX = 1.0f, signY = 1.0f;
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
    else if (a == L"--width") o->width = static_cast<uint32_t>(_wtoi(next())), o->sizeGiven = true;
    else if (a == L"--height") o->height = static_cast<uint32_t>(_wtoi(next())), o->sizeGiven = true;
    else if (a == L"--feature") o->dlaa = std::wstring(next()) == L"dlaa";
    else if (a == L"--phases") o->phases = std::max(1, _wtoi(next()));
    else if (a == L"--preset") o->dlaaPreset = std::wstring(next()) == L"L" ? SrD3D12::kPresetL : SrD3D12::kPresetK;
    else if (a == L"--jitter-sign") {
      o->signGiven = true;
      if (swscanf_s(next(), L"%f,%f", &o->signX, &o->signY) == 1) o->signY = o->signX;
    }
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
    else if (a == L"--hdr") o->hdrPath = next();
    else if (a == L"--exposure") o->exposure = static_cast<float>(_wtof(next()));
    else if (a == L"--no-highlight-recovery") o->highlightRecovery = false;
    else if (a == L"--bypass") o->bypass = true;
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

// Remix's fast tone map and its inverse with highlight recovery (thresholds 1, 8, 0.75, 0.99),
// per channel, as HdrTonemapPS and HdrInversePS do it on the GPU.
float Aces(float x) {
  return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

float Sanitize(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 65504.0f) : 0.0f; }

float Smooth(float e0, float e1, float v) {
  const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void FastTonemap(const float* hdr, float exposure, float* ldr) {
  for (int c = 0; c < 3; ++c) ldr[c] = std::pow(Aces(Sanitize(hdr[c]) * exposure), 1.0f / 2.2f);
  ldr[3] = 1.0f;
}

void InverseTonemap(const float* nr, const float* hdr, float exposure, bool recovery, float* out) {
  float x[3], t[3], original[3];
  float hdrMax = 0.0f, ldrMax = 0.0f;
  for (int c = 0; c < 3; ++c) {
    original[c] = Sanitize(hdr[c]);
    x[c] = original[c] * exposure;
    t[c] = Aces(x[c]);
    const float linear = std::pow(std::clamp(std::isfinite(nr[c]) ? nr[c] : 0.0f, 0.0f, 1.0f), 2.2f);
    out[c] = linear * (x[c] / std::max(t[c], 1e-6f)) / exposure;
    hdrMax = std::max(hdrMax, x[c]);
    ldrMax = std::max(ldrMax, std::pow(t[c], 1.0f / 2.2f));
  }
  const float mask = recovery ? Smooth(1.0f, 8.0f, hdrMax) * Smooth(0.75f, 0.99f, ldrMax) : 0.0f;
  for (int c = 0; c < 3; ++c) out[c] = std::clamp(out[c] + (original[c] - out[c]) * mask, 0.0f, 65504.0f);
}

// Mean |a - b| per channel, in 1/255 of the [0,1] range.
void LogChannelDiff(const char* what, const std::vector<float>& a, const std::vector<float>& b, size_t stride) {
  double sum[3] = {};
  const size_t n = a.size() / stride;
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c) sum[c] += std::fabs(std::clamp(a[i * stride + c], 0.0f, 1.0f) - std::clamp(b[i * stride + c], 0.0f, 1.0f));
  }
  Log("%s: mean |difference| per channel R %.2f G %.2f B %.2f /255", what, 255.0 * sum[0] / n, 255.0 * sum[1] / n, 255.0 * sum[2] / n);
}

struct Inputs {
  std::vector<float> color;   // RGBA
  std::vector<float> depth;   // 1 channel
  std::vector<float> motion;  // RG
};

// --feature dlaa: DLAA (DLSS Super Resolution at 1.0) on the synthetic scene against a
// supersampled reference, to check the jitter's sign and scale (research/dlaa.md, "Sequence,
// scale and sign"). Each frame is rendered at the pixel centres minus the Halton (2,3) jitter,
// the jitter DSF Enhanced's patched vertex shaders apply, and the jitter reported to NGX is that
// one times a variant's factors. The reference averages an 8x8 grid of sub-pixel samples with no
// jitter. Both are compared after the scene's tone map, in 1/255.
//   static camera: the error against the reference after 1, 8, 16 and the last frame, the mean
//                  frame-to-frame change over the last 16 frames (shimmer), and the sharpness
//                  (mean gradient of the last frame over the reference's; below 1 is blurrier);
//   moving camera: the error after 8, 16 and the last frame.
// With the right sign the error falls over the first frames and the still image settles; with a
// wrong sign or scale the samples land in the wrong places, so the image blurs or crawls.
struct DlaaVariant {
  float sx, sy;  // factors on the jitter reported to NGX
  bool jitter;   // render with jitter at all
  const char* name;
};

std::vector<float> ToneMapped(const std::vector<float>& linear) {
  std::vector<float> out(linear.size());
  for (size_t i = 0; i < linear.size(); ++i) out[i] = i % 4 == 3 ? 1.0f : synthetic::ToneMapValue(linear[i]);
  return out;
}

// Mean |horizontal and vertical difference| over RGB, of tone-mapped values.
double Gradient(const std::vector<float>& a, uint32_t width, uint32_t height) {
  double sum = 0.0;
  for (uint32_t y = 0; y + 1 < height; ++y) {
    for (uint32_t x = 0; x + 1 < width; ++x) {
      const size_t i = (static_cast<size_t>(y) * width + x) * 4;
      for (int c = 0; c < 3; ++c) sum += std::fabs(a[i + c] - a[i + 4 + c]) + std::fabs(a[i + c] - a[i + width * 4 + c]);
    }
  }
  return sum;
}

// Mean |a - b| over RGB, in 1/255, of tone-mapped values.
double MeanError(const std::vector<float>& a, const std::vector<float>& b) {
  double sum = 0.0;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (i % 4 == 3) continue;
    sum += std::fabs(a[i] - b[i]);
    ++n;
  }
  return 255.0 * sum / static_cast<double>(n);
}

int RunDlaa(const Options& o, uint32_t width, uint32_t height) {
  // The scene is ray traced on every core; stay out of the way of whatever else runs.
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  VerifiedRuntime runtime;
  std::string error;
  if (!runtime.Verify(o.runtimeDir + L"\\nvngx_dlss.dll", kDlssRuntime, o.allowedSha256, &error)) {
    Log("runtime check failed: %s", error.c_str());
    return 3;
  }
  Log("runtime %ls: version %u.%u.%u.%u, signer \"%ls\", %llu bytes, sha256 %s (%s)", runtime.Path().c_str(),
      runtime.Version()[0], runtime.Version()[1], runtime.Version()[2], runtime.Version()[3], runtime.Signer().c_str(),
      static_cast<unsigned long long>(runtime.Size()), runtime.Sha256().c_str(), runtime.Description());
  Gpu gpu;
  if (!gpu.Create(LUID{}, &error)) {
    Log("D3D12: %s", error.c_str());
    return 5;
  }
  Log("adapter: %ls; DLAA %ux%u, %d frames, %d jitter phases, preset %d", gpu.AdapterName().c_str(), width, height, o.frames,
      o.phases, o.dlaaPreset);
  NgxD3D12 ngx;
  SrD3D12 sr;
  if (!ngx.Initialize(gpu.Device(), runtime.Directory(), o.outDir, o.verboseNgx) || !sr.Initialize(ngx, runtime)) return 6;

  const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  ComPtr<ID3D12Resource> color = gpu.CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead);
  ComPtr<ID3D12Resource> output = gpu.CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComPtr<ID3D12Resource> depth = gpu.CreateTexture(width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead);
  ComPtr<ID3D12Resource> motion = gpu.CreateTexture(width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead);
  if (!color || !output || !depth || !motion) {
    Log("could not create the textures");
    return 5;
  }

  // The reference: an 8x8 grid of samples per pixel, no jitter, averaged in linear.
  const int grid = 8;
  auto reference = [&](int frame) {
    std::vector<float> sum(static_cast<size_t>(width) * height * 4, 0.0f);
    synthetic::Frame s;
    for (int gy = 0; gy < grid; ++gy) {
      for (int gx = 0; gx < grid; ++gx) {
        synthetic::Render(width, height, frame, (gx + 0.5f) / grid - 0.5f, (gy + 0.5f) / grid - 0.5f, &s);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += s.linear[i];
      }
    }
    for (float& v : sum) v /= static_cast<float>(grid * grid);
    return ToneMapped(sum);
  };

  std::vector<DlaaVariant> variants;
  if (o.signGiven) {
    variants.push_back({o.signX, o.signY, o.signX != 0.0f || o.signY != 0.0f, "given"});
  } else {
    variants = {{1, 1, true, "+x +y (expected)"}, {-1, -1, true, "-x -y"}, {1, -1, true, "+x -y"}, {-1, 1, true, "-x +y"},
                {2, 2, true, "2x scale"}, {0.5f, 0.5f, true, "0.5x scale"}, {0, 0, false, "no jitter"}};
  }
  const int last = o.frames;
  std::vector<uint16_t> halves;
  std::vector<uint16_t> motionHalves;
  bool ok = true;
  struct Result {
    double staticError[4] = {};
    double shimmer = 0.0;
    double sharpness = 0.0;
    double movingError[3] = {};
  };
  std::vector<Result> results(variants.size());

  for (int moving = 0; moving < 2 && ok; ++moving) {
    // References at the frames the errors are read at (the static camera has one).
    std::vector<int> checkpoints = moving ? std::vector<int>{8, 16, last} : std::vector<int>{1, 8, 16, last};
    std::vector<std::vector<float>> refs;
    for (size_t c = 0; c < (moving ? checkpoints.size() : 1); ++c) refs.push_back(reference(moving ? checkpoints[c] - 1 : 0));
    if (!moving) Save(o.outDir + L"\\dlaa_reference.png", ToRgba8(refs[0], width, height));

    for (size_t v = 0; v < variants.size() && ok; ++v) {
      const DlaaVariant& variant = variants[v];
      ID3D12GraphicsCommandList* list = gpu.Begin();
      if (!sr.CreateFeature(list, width, height, o.dlaaPreset) || !gpu.SubmitAndWait()) {
        Log("feature creation failed");
        return 7;
      }
      std::vector<float> previous;
      double shimmer = 0.0;
      int shimmerFrames = 0;
      size_t nextCheckpoint = 0;
      for (int f = 0; f < last && ok; ++f) {
        float jx = 0.0f, jy = 0.0f;
        if (variant.jitter) synthetic::HaltonJitter(f % o.phases, &jx, &jy);
        synthetic::Frame s;
        synthetic::Render(width, height, moving ? f : 0, jx, jy, &s);
        // Frame 0's motion is the move from frame -1; a still camera has none.
        if (!moving) std::fill(s.motion.begin(), s.motion.end(), 0.0f);

        list = gpu.Begin();
        for (ID3D12Resource* r : {color.Get(), depth.Get(), motion.Get()}) Gpu::Transition(list, r, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        halves.resize(s.linear.size());
        for (size_t i = 0; i < s.linear.size(); ++i) halves[i] = XMConvertFloatToHalf(s.linear[i]);
        gpu.Upload(color.Get(), halves.data(), 8);
        gpu.Upload(depth.Get(), s.depth.data(), 4);
        motionHalves.resize(s.motion.size());
        for (size_t i = 0; i < s.motion.size(); ++i) motionHalves[i] = XMConvertFloatToHalf(s.motion[i]);
        gpu.Upload(motion.Get(), motionHalves.data(), 4);
        for (ID3D12Resource* r : {color.Get(), depth.Get(), motion.Get()}) Gpu::Transition(list, r, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        if (!gpu.SubmitAndWait()) {
          Log("upload failed (device removed?)");
          return 8;
        }

        SrSettings settings;
        settings.reset = f == 0;
        settings.jitter[0] = jx * variant.sx;
        settings.jitter[1] = jy * variant.sy;
        list = gpu.Begin();
        const bool evaluated = sr.Evaluate(list, {color.Get(), output.Get(), motion.Get(), depth.Get()}, settings);
        Gpu::Transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        std::vector<uint8_t> raw;
        if (!gpu.SubmitAndWait() || !evaluated || !gpu.Readback(output.Get(), 8, &raw)) {
          Log("frame %d: evaluate or readback failed: %s", f, sr.LastError().c_str());
          ok = false;
          break;
        }
        list = gpu.Begin();
        Gpu::Transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.SubmitAndWait();
        const uint16_t* h = reinterpret_cast<const uint16_t*>(raw.data());
        std::vector<float> out(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < out.size(); ++i) out[i] = XMConvertHalfToFloat(h[i]);
        out = ToneMapped(out);

        if (!moving && f >= last - 16 && !previous.empty()) {
          shimmer += MeanError(out, previous);
          ++shimmerFrames;
        }
        previous = out;
        if (nextCheckpoint < checkpoints.size() && f + 1 == checkpoints[nextCheckpoint]) {
          const double e = MeanError(out, refs[moving ? nextCheckpoint : 0]);
          if (moving) {
            results[v].movingError[nextCheckpoint] = e;
          } else {
            results[v].staticError[nextCheckpoint] = e;
          }
          ++nextCheckpoint;
        }
        if (!moving && f == last - 1) results[v].sharpness = Gradient(out, width, height) / Gradient(refs[0], width, height);
        if (!moving && f == last - 1 && (v == 0 || v == 1 || v == variants.size() - 1)) {
          wchar_t name[64];
          swprintf_s(name, L"\\dlaa_output_%zu.png", v);
          Save(o.outDir + name, ToRgba8(out, width, height));
          if (v == 0) Save(o.outDir + L"\\dlaa_input_last.png", ToRgba8(ToneMapped(s.linear), width, height));
        }
      }
      if (!moving) results[v].shimmer = shimmerFrames ? shimmer / shimmerFrames : 0.0;
      sr.ReleaseFeature();
    }
  }
  if (!ok) return 9;

  // The input alone: one sample per pixel at the centre, against the reference.
  synthetic::Frame single;
  synthetic::Render(width, height, 0, 0.0f, 0.0f, &single);
  const std::vector<float> staticRef = reference(0);
  Log("input without DLAA (one sample per pixel), static camera: error %.3f/255", MeanError(ToneMapped(single.linear), staticRef));
  Log("%-18s | static: error after 1 / 8 / 16 / %d frames, shimmer, sharpness | moving: error after 8 / 16 / %d", "variant", last,
      last);
  for (size_t v = 0; v < variants.size(); ++v) {
    const Result& r = results[v];
    Log("%-18s | %6.3f %6.3f %6.3f %6.3f   %6.3f  %5.3f | %6.3f %6.3f %6.3f", variants[v].name, r.staticError[0], r.staticError[1],
        r.staticError[2], r.staticError[3], r.shimmer, r.sharpness, r.movingError[0], r.movingError[1], r.movingError[2]);
  }
  sr.Shutdown();
  ngx.Shutdown();
  if (o.signGiven) return 0;
  // The expected sign must beat the three other signs on the static error, the shimmer and the
  // moving error, and the other scales on the shimmer. (A smaller reported jitter blurs the
  // result a little, which a box-filtered reference can score as closer: the shimmer and the
  // sharpness tell the scales apart.)
  const Result& e = results[0];
  bool sign = true, scale = true;
  for (size_t v = 1; v <= 3; ++v) {
    sign = sign && e.staticError[3] < results[v].staticError[3] && e.shimmer < results[v].shimmer &&
           e.movingError[2] < results[v].movingError[2];
  }
  for (size_t v = 4; v <= 5; ++v) scale = scale && e.shimmer < results[v].shimmer;
  Log("jitter sign: +x +y %s; jitter scale: 1x %s", sign ? "is the best (as the protocol defines it)" : "is NOT the best",
      scale ? "shimmers least" : "does NOT shimmer least");
  return sign && scale ? 0 : 10;
}


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
  if (o.dlaa) {
    if (o.frames < 16) {
      Log("--feature dlaa needs --frames 16 or more");
      return 2;
    }
    // Small by default: the scene is ray traced on the CPU, 64 times per reference.
    return RunDlaa(o, o.sizeGiven ? o.width : 640, o.sizeGiven ? o.height : 360);
  }

  // 1. The runtime the user supplied must be a known (or explicitly allowed) NVIDIA-signed
  // public release. It stays locked until NGX has shut down.
  VerifiedRuntime runtime;
  std::string error;
  if (!runtime.Verify(o.runtimeDir + L"\\nvngx_dlssnr.dll", kDlssnrRuntime, o.allowedSha256, &error)) {
    Log("runtime check failed: %s", error.c_str());
    return 3;
  }
  Log("runtime %ls: version %u.%u.%u.%u, signer \"%ls\", %llu bytes, sha256 %s (%s)", runtime.Path().c_str(),
      runtime.Version()[0], runtime.Version()[1], runtime.Version()[2], runtime.Version()[3], runtime.Signer().c_str(),
      static_cast<unsigned long long>(runtime.Size()), runtime.Sha256().c_str(), runtime.Description());

  // 2. Inputs: a still image, or the synthetic scene.
  uint32_t width = o.width;
  uint32_t height = o.height;
  const bool hdrMode = !o.hdrPath.empty();
  const bool still = !o.colorPath.empty() || hdrMode;
  Inputs stillInputs;
  FloatImage hdr;
  if (hdrMode) {
    if (!LoadPfm(o.hdrPath, &hdr, &error) || hdr.channels != 3) {
      Log("%ls: %s (needs an RGB PFM)", o.hdrPath.c_str(), error.c_str());
      return 4;
    }
    width = hdr.width;
    height = hdr.height;
    const size_t n = static_cast<size_t>(width) * height;
    stillInputs.color.resize(n * 4);
    double sum[3] = {};
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      FastTonemap(&hdr.pixels[i * 3], o.exposure, &stillInputs.color[i * 4]);
      for (int c = 0; c < 3; ++c) {
        sum[c] += Sanitize(hdr.pixels[i * 3 + c]);
        peak = std::max(peak, Sanitize(hdr.pixels[i * 3 + c]));
      }
    }
    stillInputs.depth.assign(n, 0.5f);
    stillInputs.motion.assign(n * 2, 0.0f);
    o.jitter = false;
    Log("HDR input %ls, %ux%u, mean R %.4f G %.4f B %.4f, peak %.3f, exposure %.3f", o.hdrPath.c_str(), width, height, sum[0] / n,
        sum[1] / n, sum[2] / n, static_cast<double>(peak), static_cast<double>(o.exposure));
  } else if (still) {
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
  NgxD3D12 ngx;
  NrD3D12 nr;
  if (!ngx.Initialize(gpu.Device(), runtime.Directory(), o.outDir, o.verboseNgx) || !nr.Initialize(ngx, runtime)) return 6;

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
    bool ok = true;
    if (o.bypass) {
      // The round trip without DLSS 5: the colour input is copied to the output.
      Gpu::Transition(list, color.Get(), kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Gpu::Transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(output.Get(), color.Get());
      Gpu::Transition(list, color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
      Gpu::Transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
      ok = nr.Evaluate(list, {color.Get(), output.Get(), motion.Get(), depth.Get(), mask.Get()}, settings);
    }
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
        LogChannelDiff("last frame, DLSS 5 output against its input", out, frame.color, 4);
        if (hdrMode) {
          // Back to HDR, as the game's post chain would get it.
          const size_t n = static_cast<size_t>(width) * height;
          FloatImage back{width, height, 3, std::vector<float>(n * 3)};
          std::vector<float> again(n * 4);
          double relative = 0.0;
          for (size_t i = 0; i < n; ++i) {
            InverseTonemap(&out[i * 4], &hdr.pixels[i * 3], o.exposure, o.highlightRecovery, &back.pixels[i * 3]);
            FastTonemap(&back.pixels[i * 3], o.exposure, &again[i * 4]);
            for (int c = 0; c < 3; ++c) {
              const float h = Sanitize(hdr.pixels[i * 3 + c]);
              relative += std::fabs(back.pixels[i * 3 + c] - h) / std::max(h, 1e-3f);
            }
          }
          Log("HDR round trip: mean relative change of the HDR values %.4f", relative / (3.0 * n));
          LogChannelDiff("HDR round trip, tone-mapped again against the input", again, frame.color, 4);
          SavePfm(o.outDir + L"\\hdr_output.pfm", back, &error);
          Save(o.outDir + L"\\hdr_roundtrip.png", ToRgba8(again, width, height));
        }
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
  ngx.Shutdown();
  return failures == 0 ? 0 : 9;
}
