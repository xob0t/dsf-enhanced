// SPDX-License-Identifier: MIT

#include "ngx_trust.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <cctype>
#include <cstdio>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace dsf_dlss5 {

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

namespace {

bool CheckSignature(HANDLE file, const std::wstring& path, std::wstring* signer, std::string* error) {
  WINTRUST_FILE_INFO fileInfo = {};
  fileInfo.cbStruct = sizeof(fileInfo);
  fileInfo.pcwszFilePath = path.c_str();
  fileInfo.hFile = file;  // verify the open handle, not whatever the path names later

  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA data = {};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &fileInfo;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

  const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  bool ok = status == ERROR_SUCCESS;
  if (!ok) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "Authenticode signature is not valid (WinVerifyTrust 0x%08lX)",
                  static_cast<unsigned long>(status));
    *error = buffer;
  } else {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signerData = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    CRYPT_PROVIDER_CERT* cert = signerData ? WTHelperGetProvCertFromChain(signerData, 0) : nullptr;
    if (cert == nullptr || cert->pCert == nullptr) {
      *error = "could not read the signer certificate";
      ok = false;
    } else {
      wchar_t name[256] = {};
      CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_ORGANIZATION_NAME), name, 256);
      *signer = name;
      if (signer->compare(L"NVIDIA Corporation") != 0) {
        *error = "signed, but not by NVIDIA Corporation (signer: " + Narrow(*signer) + ")";
        ok = false;
      }
    }
  }

  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  return ok;
}

// The file is locked against writes, so reading the version through the path sees the
// same bytes as the handle.
bool ReadVersion(const std::wstring& path, uint16_t version[4]) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (size == 0) return false;
  std::vector<unsigned char> block(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return false;
  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || fixed == nullptr) return false;
  version[0] = HIWORD(fixed->dwFileVersionMS);
  version[1] = LOWORD(fixed->dwFileVersionMS);
  version[2] = HIWORD(fixed->dwFileVersionLS);
  version[3] = LOWORD(fixed->dwFileVersionLS);
  return true;
}

bool HashFile(HANDLE file, std::string* hex, uint64_t* size) {
  LARGE_INTEGER zero = {};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
            BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
  std::vector<unsigned char> buffer(1 << 20);
  *size = 0;
  DWORD read = 0;
  while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0) {
    ok = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    *size += read;
  }
  unsigned char digest[32] = {};
  ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  if (ok) {
    static const char kDigits[] = "0123456789abcdef";
    hex->clear();
    for (unsigned char byte : digest) {
      hex->push_back(kDigits[byte >> 4]);
      hex->push_back(kDigits[byte & 15]);
    }
  }
  return ok;
}

bool FileId(HANDLE file, BY_HANDLE_FILE_INFORMATION* info) {
  return GetFileInformationByHandle(file, info) != FALSE;
}

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

}  // namespace

VerifiedRuntime::~VerifiedRuntime() {
  if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool VerifiedRuntime::Verify(const std::wstring& path, const RuntimeSpec& spec, const std::vector<std::string>& extraSha256,
                             std::string* error) {
  if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  path_ = path;
  // Readers (NGX, the loader) may open it; nobody may change, rename or delete it.
  file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    *error = "cannot open " + Narrow(path) + " (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  if (!HashFile(file_, &sha256_, &size_)) {
    *error = "could not hash the file";
    return false;
  }
  description_.clear();
  for (size_t i = 0; i < spec.knownCount; ++i) {
    if (sha256_ == spec.known[i].sha256) description_ = spec.known[i].description;
  }
  for (const std::string& extra : extraSha256) {
    if (description_.empty() && sha256_ == Lower(extra)) description_ = "allowed by --allow-runtime-sha256";
  }
  if (description_.empty()) {
    *error = "unknown runtime (sha256 " + sha256_ + "). Use " + spec.where +
             ", or allow a newer NVIDIA release with --allow-runtime-sha256 <hash>";
    return false;
  }
  if (!CheckSignature(file_, path, &signer_, error)) return false;
  if (!ReadVersion(path, version_)) {
    *error = "no version resource";
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (version_[i] == spec.minVersion[i]) continue;
    if (version_[i] < spec.minVersion[i]) {
      char buffer[160];
      std::snprintf(buffer, sizeof(buffer), "version %u.%u.%u.%u is older than %u.%u.%u, the first this helper accepts", version_[0],
                    version_[1], version_[2], version_[3], spec.minVersion[0], spec.minVersion[1], spec.minVersion[2]);
      *error = buffer;
      return false;
    }
    break;
  }
  return true;
}

bool VerifiedRuntime::IsSameFile(const std::wstring& path) const {
  if (file_ == INVALID_HANDLE_VALUE) return false;
  HANDLE other = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (other == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION mine = {};
  BY_HANDLE_FILE_INFORMATION theirs = {};
  const bool same = FileId(file_, &mine) && FileId(other, &theirs) && mine.dwVolumeSerialNumber == theirs.dwVolumeSerialNumber &&
                    mine.nFileIndexHigh == theirs.nFileIndexHigh && mine.nFileIndexLow == theirs.nFileIndexLow;
  CloseHandle(other);
  return same;
}

}  // namespace dsf_dlss5
