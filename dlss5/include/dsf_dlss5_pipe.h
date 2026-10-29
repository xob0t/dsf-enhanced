// SPDX-License-Identifier: MIT
//
// Message framing over the helper's named pipe, shared by both sides (header-only, Win32).
//
// Timeouts need a pipe handle opened for overlapped I/O (FILE_FLAG_OVERLAPPED on
// CreateNamedPipe or CreateFile). On a handle without it the calls still work but block
// regardless of the timeout. A timeout or any other failure leaves the stream in an unknown
// position: treat it as the end of the connection.

#pragma once

#include <windows.h>

#include <string.h>

#include "dsf_dlss5_protocol.h"

namespace dsf_dlss5 {

// Reads or writes exactly size bytes within timeoutMs (INFINITE for no limit).
inline bool PipeTransfer(HANDLE pipe, bool write, void* data, DWORD size, DWORD timeoutMs) {
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) return false;
  const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;
  char* bytes = static_cast<char*>(data);
  bool ok = true;
  while (ok && size != 0) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;
    ResetEvent(event);
    const BOOL started = write ? WriteFile(pipe, bytes, size, nullptr, &overlapped)
                               : ReadFile(pipe, bytes, size, nullptr, &overlapped);
    if (!started && GetLastError() != ERROR_IO_PENDING) {
      ok = false;
      break;
    }
    DWORD wait = INFINITE;
    if (timeoutMs != INFINITE) {
      const ULONGLONG now = GetTickCount64();
      wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    }
    DWORD done = 0;
    if (WaitForSingleObject(event, wait) != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &overlapped);
      GetOverlappedResult(pipe, &overlapped, &done, TRUE);
      SetLastError(WAIT_TIMEOUT);
      ok = false;
      break;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &done, FALSE) || done == 0) {
      ok = false;
      break;
    }
    bytes += done;
    size -= done;
  }
  CloseHandle(event);
  return ok;
}

inline bool PipeWriteAll(HANDLE pipe, const void* data, DWORD size, DWORD timeoutMs = INFINITE) {
  return PipeTransfer(pipe, true, const_cast<void*>(data), size, timeoutMs);
}

inline bool PipeReadAll(HANDLE pipe, void* data, DWORD size, DWORD timeoutMs = INFINITE) {
  return PipeTransfer(pipe, false, data, size, timeoutMs);
}

inline DsfDlss5Header MakeHeader(DsfDlss5MessageType type, uint32_t size) {
  DsfDlss5Header header = {DSF_DLSS5_MAGIC, static_cast<uint16_t>(type), static_cast<uint16_t>(DSF_DLSS5_VERSION), size, 0};
  return header;
}

template <typename Payload>
bool PipeSend(HANDLE pipe, DsfDlss5MessageType type, const Payload& payload, DWORD timeoutMs = INFINITE) {
  // One write per message so a reader never sees half of it when the writer gives up.
  struct {
    DsfDlss5Header header;
    Payload payload;
  } message = {MakeHeader(type, static_cast<uint32_t>(sizeof(Payload))), payload};
  static_assert(sizeof(message) == sizeof(DsfDlss5Header) + sizeof(Payload), "no padding between header and payload");
  return PipeWriteAll(pipe, &message, sizeof(message), timeoutMs);
}

inline bool PipeSendEmpty(HANDLE pipe, DsfDlss5MessageType type, DWORD timeoutMs = INFINITE) {
  const DsfDlss5Header header = MakeHeader(type, 0);
  return PipeWriteAll(pipe, &header, sizeof(header), timeoutMs);
}

inline bool PipeSendError(HANDLE pipe, int32_t status, const char* text, DWORD timeoutMs = INFINITE) {
  DsfDlss5Error error = {};
  error.status = status;
  strncpy_s(error.message, sizeof(error.message), text, _TRUNCATE);
  return PipeSend(pipe, DSF_DLSS5_MSG_ERROR, error, timeoutMs);
}

// Reads one header without judging it. HeaderValid says whether this side understands it.
inline bool PipeReadHeader(HANDLE pipe, DsfDlss5Header* header, DWORD timeoutMs = INFINITE) {
  return PipeReadAll(pipe, header, sizeof(*header), timeoutMs);
}

inline bool HeaderValid(const DsfDlss5Header& header) {
  return header.magic == DSF_DLSS5_MAGIC && header.version == DSF_DLSS5_VERSION;
}

template <typename Payload>
bool PipeReadPayload(HANDLE pipe, const DsfDlss5Header& header, Payload* payload, DWORD timeoutMs = INFINITE) {
  if (header.size != sizeof(Payload)) return false;
  return PipeReadAll(pipe, payload, sizeof(Payload), timeoutMs);
}

// Reads a whole message of the expected type. If the other side sent DSF_DLSS5_MSG_ERROR
// instead, it is stored in *error (when given) and the call returns false.
template <typename Payload>
bool PipeReceive(HANDLE pipe, DsfDlss5MessageType type, Payload* payload, DWORD timeoutMs = INFINITE,
                 DsfDlss5Error* error = nullptr) {
  DsfDlss5Header header;
  if (!PipeReadHeader(pipe, &header, timeoutMs) || !HeaderValid(header)) return false;
  if (header.type == DSF_DLSS5_MSG_ERROR) {
    DsfDlss5Error received = {};
    if (PipeReadPayload(pipe, header, &received, timeoutMs) && error != nullptr) *error = received;
    return false;
  }
  return header.type == type && PipeReadPayload(pipe, header, payload, timeoutMs);
}

inline void CopyMessage(char* dst, size_t size, const char* text) {
  strncpy_s(dst, size, text, _TRUNCATE);
}

}  // namespace dsf_dlss5
