// SPDX-License-Identifier: MIT
//
// DLAA: NGX DLSS Super Resolution (NVSDK_NGX_Feature_SuperSampling) at render size = output
// size, on D3D12, through the public SDK's helpers (nvsdk_ngx_helpers_d3d.h). NGX loads the
// signed nvngx_dlss.dll from NgxD3D12's folder, where the VerifiedRuntime passed to
// Initialize is; CreateFeature fails if NGX mapped any other file. research/dlaa.md, section 3.

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace dsf_dlss5 {

class NgxD3D12;
class VerifiedRuntime;

struct SrSettings {
  bool reset = false;
  float jitter[2] = {0.0f, 0.0f};   // render pixels, -0.5..0.5, the motion vectors' convention
  float mvScale[2] = {1.0f, 1.0f};  // multiplies the motion vectors into pixels
};

// Inputs in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output in
// D3D12_RESOURCE_STATE_UNORDERED_ACCESS (and allowing UAV access), all the feature's size.
struct SrResources {
  ID3D12Resource* color = nullptr;          // linear HDR
  ID3D12Resource* output = nullptr;
  ID3D12Resource* motionVectors = nullptr;  // RG, previous minus current position, without jitter
  ID3D12Resource* depth = nullptr;          // hardware depth, 0 = near
};

class SrD3D12 {
 public:
  // DLSS.Hint.Render.Preset.DLAA values (NVSDK_NGX_DLSS_Hint_Render_Preset_K, _L).
  static constexpr int kPresetK = 11;
  static constexpr int kPresetL = 12;

  SrD3D12() = default;
  ~SrD3D12();
  SrD3D12(const SrD3D12&) = delete;
  SrD3D12& operator=(const SrD3D12&) = delete;

  // Checks that SuperSampling is available. ngx and runtime must stay alive until Shutdown.
  bool Initialize(NgxD3D12& ngx, const VerifiedRuntime& runtime);
  // Records feature creation into list (input = output = width x height, the DLAA quality
  // mode, IsHDR | MVLowRes | AutoExposure). preset 0 means K. Execute the list before Evaluate.
  bool CreateFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, int preset);
  bool Evaluate(ID3D12GraphicsCommandList* list, const SrResources& resources, const SrSettings& settings);
  void ReleaseFeature();
  void Shutdown();

  bool HasFeature() const { return feature_ != nullptr; }
  const std::string& LastError() const { return error_; }
  // Path of the nvngx_dlss.dll NGX actually mapped, known after CreateFeature.
  std::wstring LoadedRuntimePath() const;

 private:
  bool Fail(const char* what, unsigned int result);

  const VerifiedRuntime* runtime_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;   // our own, not NgxD3D12's capability parameters
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::string error_;
};

}  // namespace dsf_dlss5
