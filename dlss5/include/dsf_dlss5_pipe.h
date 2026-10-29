// SPDX-License-Identifier: MIT
//
// Message framing over the helper's named pipe, shared by both sides (header-only, Win32).

#pragma once

#include <windows.h>

#include <string.h>

#include "dsf_dlss5_protocol.h"

namespace dsf_dlss5 {

inline bool PipeWriteAll(HANDLE pipe, const void* data, DWORD size) {
  const char* bytes = static_cast<const char*>(data);
  while (size != 0) {
    DWORD written = 0;
    if (!WriteFile(pipe, bytes, size, &written, nullptr) || written == 0) return false;
    bytes += written;
    size -= written;
  }
  return true;
}

inline bool PipeReadAll(HANDLE pipe, void* data, DWORD size) {
  char* bytes = static_cast<char*>(data);
  while (size != 0) {
    DWORD read = 0;
    if (!ReadFile(pipe, bytes, size, &read, nullptr) || read == 0) return false;
    bytes += read;
    size -= read;
  }
  return true;
}

template <typename Payload>
bool PipeSend(HANDLE pipe, DsfDlss5MessageType type, const Payload& payload) {
  DsfDlss5Header header = {DSF_DLSS5_MAGIC, static_cast<uint16_t>(type), static_cast<uint16_t>(DSF_DLSS5_VERSION),
                           static_cast<uint32_t>(sizeof(Payload)), 0};
  return PipeWriteAll(pipe, &header, sizeof(header)) && PipeWriteAll(pipe, &payload, sizeof(Payload));
}

inline bool PipeSendEmpty(HANDLE pipe, DsfDlss5MessageType type) {
  DsfDlss5Header header = {DSF_DLSS5_MAGIC, static_cast<uint16_t>(type), static_cast<uint16_t>(DSF_DLSS5_VERSION), 0, 0};
  return PipeWriteAll(pipe, &header, sizeof(header));
}

// Reads one header. The caller then reads header.size payload bytes with PipeReadPayload.
inline bool PipeReadHeader(HANDLE pipe, DsfDlss5Header* header) {
  return PipeReadAll(pipe, header, sizeof(*header)) && header->magic == DSF_DLSS5_MAGIC &&
         header->version == DSF_DLSS5_VERSION;
}

template <typename Payload>
bool PipeReadPayload(HANDLE pipe, const DsfDlss5Header& header, Payload* payload) {
  if (header.size != sizeof(Payload)) return false;
  return PipeReadAll(pipe, payload, sizeof(Payload));
}

// Reads a whole message of an expected type.
template <typename Payload>
bool PipeReceive(HANDLE pipe, DsfDlss5MessageType type, Payload* payload) {
  DsfDlss5Header header;
  return PipeReadHeader(pipe, &header) && header.type == type && PipeReadPayload(pipe, header, payload);
}

inline void CopyMessage(char* dst, size_t size, const char* text) {
  strncpy_s(dst, size, text, _TRUNCATE);
}

}  // namespace dsf_dlss5
