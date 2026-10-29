// SPDX-License-Identifier: MIT

#include "d3d12_util.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace dsf_dlss5 {

namespace {

constexpr UINT kNvidiaVendorId = 0x10DE;

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES props = {};
  props.Type = type;
  return props;
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const D3D12_HEAP_PROPERTIES props = HeapProps(type);
  ComPtr<ID3D12Resource> buffer;
  device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
  return buffer;
}

}  // namespace

bool Gpu::Create(LUID adapterLuid, std::string* error) {
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    *error = "CreateDXGIFactory2 failed";
    return false;
  }
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    const bool wantLuid = adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0;
    const bool matches = wantLuid ? (desc.AdapterLuid.LowPart == adapterLuid.LowPart && desc.AdapterLuid.HighPart == adapterLuid.HighPart)
                                  : desc.VendorId == kNvidiaVendorId;
    if (matches) {
      adapter_ = adapter;
      luid_ = desc.AdapterLuid;
      adapterName_ = desc.Description;
      break;
    }
  }
  if (!adapter_) {
    *error = "no NVIDIA adapter found";
    return false;
  }
  if (FAILED(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
    *error = "D3D12CreateDevice failed";
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_))) ||
      FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
      FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_))) ||
      FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    *error = "could not create the queue, list or fence";
    return false;
  }
  list_->Close();
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  D3D12_QUERY_HEAP_DESC queryDesc = {};
  queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  queryDesc.Count = 2;
  if (SUCCEEDED(device_->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&timestamps_)))) {
    timestampReadback_ = CreateBuffer(device_.Get(), 2 * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  return true;
}

ID3D12GraphicsCommandList* Gpu::Begin() {
  allocator_->Reset();
  list_->Reset(allocator_.Get(), nullptr);
  if (timestamps_) list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  return list_.Get();
}

bool Gpu::SubmitAndWait(double* gpuMs, ID3D12Fence* waitFence, uint64_t waitValue, ID3D12Fence* signalFence,
                        uint64_t signalValue) {
  if (timestamps_) {
    list_->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list_->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestampReadback_.Get(), 0);
  }
  if (FAILED(list_->Close())) return false;
  if (waitFence != nullptr) queue_->Wait(waitFence, waitValue);
  ID3D12CommandList* lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  if (signalFence != nullptr) queue_->Signal(signalFence, signalValue);
  queue_->Signal(fence_.Get(), ++fenceValue_);
  if (fence_->GetCompletedValue() < fenceValue_) {
    fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
    WaitForSingleObject(fenceEvent_, INFINITE);
  }
  staging_.clear();
  if (gpuMs != nullptr) {
    *gpuMs = -1.0;
    uint64_t frequency = 0;
    uint64_t* ticks = nullptr;
    D3D12_RANGE range = {0, 2 * sizeof(uint64_t)};
    if (timestamps_ && SUCCEEDED(queue_->GetTimestampFrequency(&frequency)) &&
        SUCCEEDED(timestampReadback_->Map(0, &range, reinterpret_cast<void**>(&ticks)))) {
      *gpuMs = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(frequency);
      D3D12_RANGE none = {0, 0};
      timestampReadback_->Unmap(0, &none);
    }
  }
  return device_->GetDeviceRemovedReason() == S_OK;
}

ComPtr<ID3D12Resource> Gpu::CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                          D3D12_HEAP_FLAGS heapFlags) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  const D3D12_HEAP_PROPERTIES props = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  device_->CreateCommittedResource(&props, heapFlags, &desc, state, nullptr, IID_PPV_ARGS(&texture));
  return texture;
}

void Gpu::Upload(ID3D12Resource* texture, const void* pixels, uint32_t bytesPerPixel) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 rowSize = 0;
  UINT64 total = 0;
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
  ComPtr<ID3D12Resource> staging = CreateBuffer(device_.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t* mapped = nullptr;
  staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  const size_t packedRow = static_cast<size_t>(desc.Width) * bytesPerPixel;
  for (UINT y = 0; y < rows; ++y) {
    std::memcpy(mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                static_cast<const uint8_t*>(pixels) + y * packedRow, packedRow);
  }
  staging->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = staging.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  staging_.push_back(staging);
}

bool Gpu::Readback(ID3D12Resource* texture, uint32_t bytesPerPixel, std::vector<uint8_t>* pixels) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 rowSize = 0;
  UINT64 total = 0;
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
  ComPtr<ID3D12Resource> staging = CreateBuffer(device_.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList* list = Begin();
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = texture;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = staging.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (!SubmitAndWait()) return false;
  const size_t packedRow = static_cast<size_t>(desc.Width) * bytesPerPixel;
  pixels->resize(packedRow * rows);
  uint8_t* mapped = nullptr;
  if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
  for (UINT y = 0; y < rows; ++y) {
    std::memcpy(pixels->data() + y * packedRow,
                mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch, packedRow);
  }
  D3D12_RANGE none = {0, 0};
  staging->Unmap(0, &none);
  return true;
}

void Gpu::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                     D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (before == after) return;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  list->ResourceBarrier(1, &barrier);
}

}  // namespace dsf_dlss5
