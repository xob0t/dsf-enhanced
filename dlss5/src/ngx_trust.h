// SPDX-License-Identifier: MIT
//
// Checks on the user-supplied NGX feature DLLs (nvngx_dlssnr.dll for DLSS 5, nvngx_dlss.dll for
// DLAA) before NGX is allowed near them:
//  - the file is opened once, with write and delete sharing denied, and stays open for the
//    lifetime of the VerifiedRuntime, so the checked bytes are the bytes NGX loads;
//  - its SHA-256 is a known public release of that file (RuntimeSpec::known) or one the user
//    allowed explicitly (--allow-runtime-sha256);
//  - it has a valid Authenticode signature from NVIDIA Corporation and is at least the
//    first version this project accepts for that file.
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

// What one NGX feature DLL must be.
struct RuntimeSpec {
  const wchar_t* file;          // the name NGX loads it by
  const KnownRuntime* known;    // public releases this project has tested
  size_t knownCount;
  uint16_t minVersion[4];
  const char* where;            // where users get it, for the error message
};

// DLSS 5: nvngx_dlssnr.dll. The first version NVIDIA published openly (RTX Remix 1.6.1,
// October 2026) is also the floor.
constexpr KnownRuntime kKnownDlssnr[] = {
    {"2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f", "310.9.16.0 from RTX Remix 1.6.1"},
};
constexpr RuntimeSpec kDlssnrRuntime = {L"nvngx_dlssnr.dll", kKnownDlssnr, 1, {310, 9, 16, 0}, "the nvngx_dlssnr.dll from RTX Remix 1.6.1"};

// DLAA: nvngx_dlss.dll (DLSS Super Resolution), research/dlaa.md section 4. The floor is the
// release of the SDK commit the build pins (fetch-sdk.ps1).
constexpr KnownRuntime kKnownDlss[] = {
    {"d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e", "310.9.16.0 from NVIDIA/DLSS v310.9.16"},
};
constexpr RuntimeSpec kDlssRuntime = {L"nvngx_dlss.dll", kKnownDlss, 1, {310, 9, 16, 0},
                                      "the nvngx_dlss.dll of NVIDIA/DLSS release v310.9.16 (lib/Windows_x86_64/rel)"};

class VerifiedRuntime {
 public:
  VerifiedRuntime() = default;
  ~VerifiedRuntime();
  VerifiedRuntime(const VerifiedRuntime&) = delete;
  VerifiedRuntime& operator=(const VerifiedRuntime&) = delete;

  // path names spec.file. extraSha256 lists additional allowed hashes (lowercase hex) for
  // newer runtimes.
  bool Verify(const std::wstring& path, const RuntimeSpec& spec, const std::vector<std::string>& extraSha256,
              std::string* error);
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
