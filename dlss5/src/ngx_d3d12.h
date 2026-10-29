// SPDX-License-Identifier: MIT
//
// One NGX initialization on a D3D12 device, shared by the helper's features (DLSS 5 in
// nr_d3d12, DLAA in sr_d3d12). NGX is pointed at a single folder (PathListInfo), where it
// looks for every feature DLL: the folder the verified runtimes are in.

#pragma once

#include <d3d12.h>

#include <string>

struct NVSDK_NGX_Parameter;

namespace dsf_dlss5 {

class NgxD3D12 {
 public:
  NgxD3D12() = default;
  ~NgxD3D12();
  NgxD3D12(const NgxD3D12&) = delete;
  NgxD3D12& operator=(const NgxD3D12&) = delete;

  // runtimeDir holds the feature DLLs; dataDir receives NGX logs.
  bool Initialize(ID3D12Device* device, const std::wstring& runtimeDir, const std::wstring& dataDir, bool verboseLog);
  // Release every feature first.
  void Shutdown();

  bool Initialized() const { return initialized_; }
  ID3D12Device* Device() const { return device_; }
  // NGX's capability parameters (feature support queries; DLSS 5 also creates and evaluates
  // with them, as in protocol version 1).
  NVSDK_NGX_Parameter* Capabilities() const { return caps_; }
  const std::string& LastError() const { return error_; }

 private:
  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* caps_ = nullptr;
  bool initialized_ = false;
  std::string error_;
};

// "<what> failed: 0x... (NGX's text)", logged.
std::string NgxFailure(const char* what, unsigned int result);

}  // namespace dsf_dlss5
