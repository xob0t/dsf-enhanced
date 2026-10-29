// SPDX-License-Identifier: MIT
//
// DLSS 5 (NGX DLSSNR) on D3D12 through the official NGX path: the public DLSS SDK's
// static loader finds the driver's NGX core, and the core loads and verifies the signed
// nvngx_dlssnr.dll from the runtime folder passed to Initialize.

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace dsf_dlss5 {

struct NrSettings {
  // Remix defaults (rtx.dlssNeuralRendering.*).
  float intensity = 1.0f;
  float toneStrength = 0.3f;
  float structureStrength = 0.7f;
  float skinStructureStrength = 0.5f;
  uint32_t style = 0;  // model A/B/C = 0/1/2
  bool useAutoMask = true;
  bool depthInverted = false;
  bool reset = false;
  float jitter[2] = {0.0f, 0.0f};     // pixels, see research/dlss5-helper.md
  float mvScale[2] = {1.0f, 1.0f};    // multiplies MVec into pixels
};

// Every resource must be in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE except output,
// which must be in D3D12_RESOURCE_STATE_UNORDERED_ACCESS and allow UAV access.
struct NrResources {
  ID3D12Resource* color = nullptr;        // gamma-encoded SDR, [0,1]
  ID3D12Resource* output = nullptr;       // same size as color
  ID3D12Resource* motionVectors = nullptr;  // RG, previous minus current position
  ID3D12Resource* depth = nullptr;        // hardware depth, 0 = near unless depthInverted
  ID3D12Resource* controlMask = nullptr;  // RGBA, 1 = apply NR; optional
};

class NrD3D12 {
 public:
  NrD3D12() = default;
  ~NrD3D12();
  NrD3D12(const NrD3D12&) = delete;
  NrD3D12& operator=(const NrD3D12&) = delete;

  // runtimeDir holds nvngx_dlssnr.dll; dataDir receives NGX logs.
  bool Initialize(ID3D12Device* device, const std::wstring& runtimeDir, const std::wstring& dataDir, bool verboseLog);
  // Records feature creation into list. The list must be executed before Evaluate.
  bool CreateFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, int preset);
  bool Evaluate(ID3D12GraphicsCommandList* list, const NrResources& resources, const NrSettings& settings);
  void ReleaseFeature();
  void Shutdown();

  bool HasFeature() const { return feature_ != nullptr; }
  uint32_t Width() const { return width_; }
  uint32_t Height() const { return height_; }
  const std::string& LastError() const { return error_; }
  // Path of the nvngx_dlssnr.dll NGX actually mapped, known after CreateFeature.
  std::wstring LoadedRuntimePath() const;

 private:
  bool Fail(const char* what, unsigned int result);

  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;
  std::string error_;
};

}  // namespace dsf_dlss5
