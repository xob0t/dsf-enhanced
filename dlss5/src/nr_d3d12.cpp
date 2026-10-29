// SPDX-License-Identifier: MIT

#include "nr_d3d12.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "nvsdk_ngx.h"
#include "dlssnr_defs.h"
#include "log.h"
#include "ngx_d3d12.h"
#include "ngx_trust.h"

namespace dsf_dlss5 {

namespace {

void SetSubrect(NVSDK_NGX_Parameter* params, const char* prefix, ID3D12Resource* resource) {
  unsigned int width = 0;
  unsigned int height = 0;
  if (resource != nullptr) {
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    width = static_cast<unsigned int>(desc.Width);
    height = desc.Height;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "%sBaseX", prefix);
  NVSDK_NGX_Parameter_SetUI(params, name, 0);
  std::snprintf(name, sizeof(name), "%sBaseY", prefix);
  NVSDK_NGX_Parameter_SetUI(params, name, 0);
  std::snprintf(name, sizeof(name), "%sWidth", prefix);
  NVSDK_NGX_Parameter_SetUI(params, name, width);
  std::snprintf(name, sizeof(name), "%sHeight", prefix);
  NVSDK_NGX_Parameter_SetUI(params, name, height);
}

}  // namespace

NrD3D12::~NrD3D12() { Shutdown(); }

bool NrD3D12::Fail(const char* what, unsigned int result) {
  error_ = NgxFailure(what, result);
  return false;
}

bool NrD3D12::Initialize(NgxD3D12& ngx, const VerifiedRuntime& runtime) {
  runtime_ = &runtime;
  params_ = ngx.Capabilities();
  if (params_ == nullptr) {
    error_ = "NGX is not initialized";
    return false;
  }
  int needsUpdatedDriver = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Parameter_GetI(params_, dlssnr::kNeedsUpdatedDriver, &needsUpdatedDriver);
  if (NVSDK_NGX_FAILED(result)) return Fail("DLSSNR support query", result);
  if (needsUpdatedDriver) {
    unsigned int major = 0;
    unsigned int minor = 0;
    NVSDK_NGX_Parameter_GetUI(params_, dlssnr::kMinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(params_, dlssnr::kMinDriverVersionMinor, &minor);
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "DLSSNR needs a newer driver (minimum %u.%u)", major, minor);
    error_ = buffer;
    Log("%s", buffer);
    return false;
  }
  int available = 0;
  result = NVSDK_NGX_Parameter_GetI(params_, dlssnr::kAvailable, &available);
  if (NVSDK_NGX_FAILED(result) || !available) {
    int initResult = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Parameter_GetI(params_, dlssnr::kFeatureInitResult, &initResult);
    return Fail("DLSSNR not available on this GPU/driver; feature init result", static_cast<unsigned int>(initResult));
  }
  Log("DLSSNR is available");
  return true;
}

bool NrD3D12::CreateFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, int preset) {
  ReleaseFeature();
  NVSDK_NGX_Parameter_SetUI(params_, dlssnr::kWidth, width);
  NVSDK_NGX_Parameter_SetUI(params_, dlssnr::kHeight, height);
  NVSDK_NGX_Parameter_SetI(params_, dlssnr::kHintRenderPreset, preset);
  NVSDK_NGX_Parameter_SetI(params_, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
  const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_CreateFeature(list, dlssnr::kFeature, params_, &feature_);
  if (NVSDK_NGX_FAILED(result)) {
    feature_ = nullptr;
    return Fail("NVSDK_NGX_D3D12_CreateFeature(DLSSNR)", result);
  }
  // NGX could pick another copy (its model cache, the driver store); only the file we
  // verified is acceptable. This runs after NGX has already loaded the module and its
  // DllMain has run, so it detects a substitution and refuses to use it, but cannot
  // prevent the load. What prevents substitution of our copy is the lock VerifiedRuntime
  // holds; another copy is NGX's own choice and has passed NGX's own signature check.
  const std::wstring loaded = LoadedRuntimePath();
  if (!runtime_->IsSameFile(loaded)) {
    ReleaseFeature();
    error_ = "NGX loaded " + Narrow(loaded) + ", not the verified " + Narrow(runtime_->Path());
    Log("%s", error_.c_str());
    return false;
  }
  width_ = width;
  height_ = height;
  Log("DLSSNR feature created at %ux%u, preset %d; runtime %ls", width, height, preset, LoadedRuntimePath().c_str());
  return true;
}

bool NrD3D12::Evaluate(ID3D12GraphicsCommandList* list, const NrResources& r, const NrSettings& s) {
  if (feature_ == nullptr) {
    error_ = "Evaluate without a feature";
    return false;
  }
  NVSDK_NGX_Parameter* p = params_;
  NVSDK_NGX_Parameter_SetD3d12Resource(p, dlssnr::kColor, r.color);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, dlssnr::kOutput, r.output);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, dlssnr::kMVec, r.motionVectors);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, dlssnr::kDepth, r.depth);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, dlssnr::kControlMask, r.controlMask);
  SetSubrect(p, dlssnr::kColorSubrect, r.color);
  SetSubrect(p, dlssnr::kOutputSubrect, r.output);
  SetSubrect(p, dlssnr::kMVecSubrect, r.motionVectors);
  SetSubrect(p, dlssnr::kDepthSubrect, r.depth);
  SetSubrect(p, dlssnr::kControlMaskSubrect, r.controlMask);

  NVSDK_NGX_Parameter_SetI(p, dlssnr::kReset, s.reset ? 1 : 0);
  NVSDK_NGX_Parameter_SetI(p, dlssnr::kDepthInverted, s.depthInverted ? 1 : 0);
  NVSDK_NGX_Parameter_SetI(p, dlssnr::kEnabled, 1);
  NVSDK_NGX_Parameter_SetI(p, dlssnr::kUICorrection, 0);
  NVSDK_NGX_Parameter_SetF(p, dlssnr::kIntensity, s.intensity);
  NVSDK_NGX_Parameter_SetF(p, dlssnr::kLocalToneStrength, s.toneStrength);
  NVSDK_NGX_Parameter_SetF(p, dlssnr::kLocalStructureStrength, s.structureStrength);
  NVSDK_NGX_Parameter_SetUI(p, dlssnr::kStyle, s.style);
  NVSDK_NGX_Parameter_SetI(p, dlssnr::kUseAutoMask, s.useAutoMask ? 1 : 0);
  NVSDK_NGX_Parameter_SetI(p, dlssnr::kMaskBlendMode, s.useAutoMask ? dlssnr::kMaskBlendMin : dlssnr::kMaskBlendNone);
  NVSDK_NGX_Parameter_SetF(p, dlssnr::kSkinStructureStrength, s.useAutoMask ? s.skinStructureStrength : 0.0f);
  NVSDK_NGX_Parameter_SetF(p, dlssnr::kMVecScaleX, s.mvScale[0]);
  NVSDK_NGX_Parameter_SetF(p, dlssnr::kMVecScaleY, s.mvScale[1]);
  NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Jitter_Offset_X, s.jitter[0]);
  NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, s.jitter[1]);

  const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_EvaluateFeature(list, feature_, p, nullptr);
  if (NVSDK_NGX_FAILED(result)) return Fail("NVSDK_NGX_D3D12_EvaluateFeature(DLSSNR)", result);
  return true;
}

void NrD3D12::ReleaseFeature() {
  if (feature_ != nullptr) {
    NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    feature_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
}

void NrD3D12::Shutdown() {
  ReleaseFeature();
  params_ = nullptr;   // NgxD3D12 owns them
}

std::wstring NrD3D12::LoadedRuntimePath() const {
  HMODULE module = GetModuleHandleW(L"nvngx_dlssnr.dll");
  if (module == nullptr) return L"(nvngx_dlssnr.dll not mapped)";
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return path;
}

}  // namespace dsf_dlss5
