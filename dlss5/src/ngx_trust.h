// SPDX-License-Identifier: MIT
//
// Checks on the user-supplied nvngx_dlssnr.dll before NGX is allowed near it:
//  - the file is opened once, with write and delete sharing denied, and stays open for the
//    lifetime of the VerifiedRuntime, so the checked bytes are the bytes NGX loads;
//  - its SHA-256 is a known public release (kKnownRuntimes) or one the user allowed
//    explicitly (--allow-runtime-sha256);
//  - it has a valid Authenticode signature from NVIDIA Corporation and is at least the
//    first public version.
// After NGX creates the feature, IsSameFile confirms the module NGX mapped is this file.
// That check runs after the module is loaded and its DllMain has run: it detects a
// different copy and stops the helper from using it, but cannot prevent the load.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dsf_dlss5 {

struct KnownRuntime {
  const char* sha256;
  const char* description;
};

// Public releases of nvngx_dlssnr.dll this project has tested.
constexpr KnownRuntime kKnownRuntimes[] = {
    {"2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f", "310.9.16.0 from RTX Remix 1.6.1"},
};

// The first version NVIDIA published openly (RTX Remix 1.6.1, October 2026).
constexpr uint16_t kMinRuntimeVersion[4] = {310, 9, 16, 0};

class VerifiedRuntime {
 public:
  VerifiedRuntime() = default;
  ~VerifiedRuntime();
  VerifiedRuntime(const VerifiedRuntime&) = delete;
  VerifiedRuntime& operator=(const VerifiedRuntime&) = delete;

  // extraSha256 lists additional allowed hashes (lowercase hex) for newer runtimes.
  bool Verify(const std::wstring& path, const std::vector<std::string>& extraSha256, std::string* error);
  // True when path names the same file (volume and file id) as the verified one.
  bool IsSameFile(const std::wstring& path) const;

  const std::wstring& Path() const { return path_; }
  std::wstring Directory() const { return path_.substr(0, path_.find_last_of(L"\\/")); }
  const std::wstring& Signer() const { return signer_; }
  const uint16_t* Version() const { return version_; }
  const std::string& Sha256() const { return sha256_; }
  const char* Description() const { return description_.c_str(); }
  uint64_t Size() const { return size_; }

 private:
  HANDLE file_ = INVALID_HANDLE_VALUE;
  std::wstring path_;
  std::wstring signer_;
  uint16_t version_[4] = {};
  std::string sha256_;
  std::string description_;
  uint64_t size_ = 0;
};

std::string Narrow(const std::wstring& text);

}  // namespace dsf_dlss5
