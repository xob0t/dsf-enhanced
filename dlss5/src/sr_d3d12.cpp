// SPDX-License-Identifier: MIT

#include "sr_d3d12.h"

#include <windows.h>

#include <cstdio>

#include "nvsdk_ngx_helpers_d3d.h"
#include "log.h"
#include "ngx_d3d12.h"
#include "ngx_trust.h"

namespace dsf_dlss5 {

SrD3D12::~SrD3D12() { Shutdown(); }

bool SrD3D12::Fail(const char* what, unsigned int result) {
  error_ = NgxFailure(what, result);
  return false;
}

bool SrD3D12::Initialize(NgxD3D12& ngx, const VerifiedRuntime& runtime) {
  runtime_ = &runtime;
  NVSDK_NGX_Parameter* caps = ngx.Capabilities();
  if (caps == nullptr) {
    error_ = "NGX is not initialized";
    return false;
  }
  int needsUpdatedDriver = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
  if (NVSDK_NGX_FAILED(result)) return Fail("SuperSampling support query", result);
  if (needsUpdatedDriver) {
    unsigned int major = 0;
    unsigned int minor = 0;
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "DLSS Super Resolution needs a newer driver (minimum %u.%u)", major, minor);
    error_ = buffer;
    Log("%s", buffer);
    return false;
  }
  int available = 0;
  result = NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (NVSDK_NGX_FAILED(result) || !available) {
    int initResult = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
    return Fail("DLSS Super Resolution not available on this GPU/driver; feature init result", static_cast<unsigned int>(initResult));
  }
  result = NVSDK_NGX_D3D12_AllocateParameters(&params_);
  if (NVSDK_NGX_FAILED(result)) return Fail("NVSDK_NGX_D3D12_AllocateParameters", result);
  Log("DLSS Super Resolution (DLAA) is available");
  return true;
}

bool SrD3D12::CreateFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, int preset) {
  ReleaseFeature();
  if (params_ == nullptr) {
    error_ = "CreateFeature before Initialize";
    return false;
  }
  if (preset == 0) preset = kPresetK;   // never Default (0), which NVIDIA may remap
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = create.Feature.InTargetWidth = width;
  create.Feature.InHeight = create.Feature.InTargetHeight = height;
  create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
  create.InFeatureCreateFlags =
      NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, static_cast<unsigned int>(preset));
  NVSDK_NGX_Parameter_SetI(params_, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
  const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &feature_, params_, &create);
  if (NVSDK_NGX_FAILED(result)) {
    feature_ = nullptr;
    return Fail("NGX_D3D12_CREATE_DLSS_EXT (DLAA)", result);
  }
  // As for DLSSNR (nr_d3d12.cpp): only the verified file is acceptable.
  const std::wstring loaded = LoadedRuntimePath();
  if (!runtime_->IsSameFile(loaded)) {
    ReleaseFeature();
    error_ = "NGX loaded " + Narrow(loaded) + ", not the verified " + Narrow(runtime_->Path());
    Log("%s", error_.c_str());
    return false;
  }
  width_ = width;
  height_ = height;
  Log("DLAA feature created at %ux%u, preset %d; runtime %ls", width, height, preset, loaded.c_str());
  return true;
}

bool SrD3D12::Evaluate(ID3D12GraphicsCommandList* list, const SrResources& r, const SrSettings& s) {
  if (feature_ == nullptr) {
    error_ = "Evaluate without a feature";
    return false;
  }
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = r.color;
  eval.Feature.pInOutput = r.output;
  eval.pInDepth = r.depth;
  eval.pInMotionVectors = r.motionVectors;
  eval.InJitterOffsetX = s.jitter[0];
  eval.InJitterOffsetY = s.jitter[1];
  eval.InMVScaleX = s.mvScale[0];
  eval.InMVScaleY = s.mvScale[1];
  eval.InReset = s.reset ? 1 : 0;
  eval.InRenderSubrectDimensions = {width_, height_};
  const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature_, params_, &eval);
  if (NVSDK_NGX_FAILED(result)) return Fail("NGX_D3D12_EVALUATE_DLSS_EXT (DLAA)", result);
  return true;
}

void SrD3D12::ReleaseFeature() {
  if (feature_ != nullptr) {
    NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    feature_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
}

void SrD3D12::Shutdown() {
  ReleaseFeature();
  if (params_ != nullptr) {
    NVSDK_NGX_D3D12_DestroyParameters(params_);
    params_ = nullptr;
  }
}

std::wstring SrD3D12::LoadedRuntimePath() const {
  HMODULE module = GetModuleHandleW(L"nvngx_dlss.dll");
  if (module == nullptr) return L"(nvngx_dlss.dll not mapped)";
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return path;
}

}  // namespace dsf_dlss5
