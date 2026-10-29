// SPDX-License-Identifier: MIT

#include "ngx_d3d12.h"

#include <windows.h>

#include <cstdio>

#include "nvsdk_ngx.h"
#include "log.h"

namespace dsf_dlss5 {

namespace {

// NGX project id for DSF Enhanced. Projects without an NVIDIA-assigned application id
// identify themselves with a GUID and NVSDK_NGX_ENGINE_TYPE_CUSTOM.
constexpr char kProjectId[] = "9cad904d-8216-4482-b2e7-7c2ccba2973e";
constexpr char kEngineVersion[] = "1.0";

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  LogRaw("[ngx] ", message, false);
}

}  // namespace

std::string NgxFailure(const char* what, unsigned int result) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08X (%ls)", what, result,
                GetNGXResultAsString(static_cast<NVSDK_NGX_Result>(result)));
  Log("%s", buffer);
  return buffer;
}

NgxD3D12::~NgxD3D12() { Shutdown(); }

bool NgxD3D12::Initialize(ID3D12Device* device, const std::wstring& runtimeDir, const std::wstring& dataDir, bool verboseLog) {
  device_ = device;
  const wchar_t* paths[] = {runtimeDir.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  info.LoggingInfo.LoggingCallback = &NgxLog;
  info.LoggingInfo.MinimumLoggingLevel = verboseLog ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
  info.LoggingInfo.DisableOtherLoggingSinks = true;

  NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, dataDir.c_str(), device, &info, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(result)) {
    error_ = NgxFailure("NVSDK_NGX_D3D12_Init_with_ProjectID", result);
    return false;
  }
  initialized_ = true;
  result = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps_);
  if (NVSDK_NGX_FAILED(result)) {
    error_ = NgxFailure("NVSDK_NGX_D3D12_GetCapabilityParameters", result);
    return false;
  }
  return true;
}

void NgxD3D12::Shutdown() {
  if (caps_ != nullptr) {
    NVSDK_NGX_D3D12_DestroyParameters(caps_);
    caps_ = nullptr;
  }
  if (initialized_) {
    NVSDK_NGX_D3D12_Shutdown1(device_);
    initialized_ = false;
  }
}

}  // namespace dsf_dlss5
