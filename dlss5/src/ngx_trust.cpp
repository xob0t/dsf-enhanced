// SPDX-License-Identifier: MIT

#include "ngx_trust.h"

#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <cstdio>
#include <vector>

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

bool CheckSignature(const std::wstring& path, std::wstring* signer, std::string* error) {
  WINTRUST_FILE_INFO file = {};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path.c_str();

  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA data = {};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
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

bool HashFile(const std::wstring& path, std::string* hex, uint64_t* size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
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
  CloseHandle(file);
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

}  // namespace

bool VerifyNgxRuntime(const std::wstring& path, RuntimeFileInfo* info, std::string* error) {
  info->path = path;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = "not found: " + Narrow(path);
    return false;
  }
  if (!CheckSignature(path, &info->signer, error)) return false;
  if (!ReadVersion(path, info->version)) {
    *error = "no version resource";
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (info->version[i] != kMinRuntimeVersion[i]) {
      if (info->version[i] < kMinRuntimeVersion[i]) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "version %u.%u.%u.%u is older than the first public release %u.%u.%u",
                      info->version[0], info->version[1], info->version[2], info->version[3],
                      kMinRuntimeVersion[0], kMinRuntimeVersion[1], kMinRuntimeVersion[2]);
        *error = buffer;
        return false;
      }
      break;
    }
  }
  if (!HashFile(path, &info->sha256, &info->size)) {
    *error = "could not hash the file";
    return false;
  }
  return true;
}

}  // namespace dsf_dlss5
