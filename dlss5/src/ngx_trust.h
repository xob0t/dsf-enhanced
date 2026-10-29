// SPDX-License-Identifier: MIT
//
// Checks on the user-supplied nvngx_dlssnr.dll before NGX is allowed near it: a valid
// Authenticode signature whose signer is NVIDIA Corporation, and a file version at or
// above the first public release (310.9.16, shipped in RTX Remix 1.6.1). Patched builds
// fail the signature check and pre-release builds fail the version check.

#pragma once

#include <cstdint>
#include <string>

namespace dsf_dlss5 {

struct RuntimeFileInfo {
  std::wstring path;
  std::wstring signer;     // signer certificate subject, e.g. "NVIDIA Corporation"
  uint16_t version[4] = {};  // FileVersion, major..build
  std::string sha256;      // lowercase hex
  uint64_t size = 0;
};

// The first version NVIDIA published openly (RTX Remix 1.6.1, October 2026).
constexpr uint16_t kMinRuntimeVersion[4] = {310, 9, 16, 0};

// Fills info and returns true when the file exists, has a valid NVIDIA signature and is
// at least kMinRuntimeVersion. On failure error says which check failed.
bool VerifyNgxRuntime(const std::wstring& path, RuntimeFileInfo* info, std::string* error);

std::string Narrow(const std::wstring& text);

}  // namespace dsf_dlss5
