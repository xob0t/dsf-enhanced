// SPDX-License-Identifier: MIT
//
// The small amount of D3D12 plumbing the helper and the offscreen test share: a device on
// the NVIDIA adapter, one direct queue with a fence, and texture upload/readback.

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dsf_dlss5 {

using Microsoft::WRL::ComPtr;

class Gpu {
 public:
  // Picks the first NVIDIA adapter (or the one with the given LUID, when not zero).
  bool Create(LUID adapterLuid, std::string* error);

  ID3D12Device* Device() const { return device_.Get(); }
  ID3D12CommandQueue* Queue() const { return queue_.Get(); }
  LUID AdapterLuid() const { return luid_; }
  const std::wstring& AdapterName() const { return adapterName_; }

  // Resets the shared allocator and list and returns the list ready for recording. The
  // previous submission must have completed (Submit returned kOk, or WaitIdle succeeded).
  ID3D12GraphicsCommandList* Begin();

  struct Sync {
    ID3D12Fence* waitFence = nullptr;    // the queue waits for waitFence >= waitValue first
    uint64_t waitValue = 0;
    ID3D12Fence* signalFence = nullptr;  // and signals signalFence = signalValue after the list
    uint64_t signalValue = 0;
    DWORD timeoutMs = INFINITE;          // CPU wait for the list to finish
  };
  enum class SubmitResult { kOk, kTimeout, kFailed };
  // Closes and executes the list returned by Begin, then waits up to sync.timeoutMs for it.
  // kTimeout leaves the work queued: call WaitIdle before the next Begin. kFailed means
  // the list could not be closed (nothing was executed) or the device was removed. gpuMs
  // receives the GPU time of the list on kOk when timestamps are available, otherwise -1.
  SubmitResult Submit(const Sync& sync, double* gpuMs = nullptr);
  bool SubmitAndWait(double* gpuMs = nullptr) { return Submit(Sync{}, gpuMs) == SubmitResult::kOk; }
  // Waits for everything submitted so far. False on timeout or device removal.
  bool WaitIdle(DWORD timeoutMs);

  ComPtr<ID3D12Resource> CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                       D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE);

  // Copies tightly packed rows (bytesPerPixel * width per row) into texture, which must be
  // in COPY_DEST. Records into the current list; the staging buffer lives until the next
  // SubmitAndWait.
  void Upload(ID3D12Resource* texture, const void* pixels, uint32_t bytesPerPixel);
  // Reads texture (in COPY_SOURCE) back into tightly packed rows. Submits and waits.
  bool Readback(ID3D12Resource* texture, uint32_t bytesPerPixel, std::vector<uint8_t>* pixels);

  static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                         D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

 private:
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12QueryHeap> timestamps_;
  ComPtr<ID3D12Resource> timestampReadback_;
  uint64_t fenceValue_ = 0;
  HANDLE fenceEvent_ = nullptr;
  LUID luid_ = {};
  std::wstring adapterName_;
  std::vector<ComPtr<ID3D12Resource>> staging_;
};

}  // namespace dsf_dlss5
