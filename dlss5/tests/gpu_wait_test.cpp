// SPDX-License-Identifier: MIT
//
// dsf-dlss5-gpu-wait-test (x64): regression test for Gpu::Submit/WaitIdle after a timeout,
// the helper's ERR_TIMEOUT path. Needs a D3D12 device but no NGX runtime.
//
// A submission whose queue waits on a fence nobody signals times out; the gate is then
// signalled from the CPU and the queue drained, as the helper does. A second submission
// blocked on the gate again must not report completion before the gate allows it to run:
// a stale wake-up left by the first wait used to make it return kOk at once.
//
// Exit code 0 means both checks passed.

#include <windows.h>

#include <cstdio>
#include <thread>

#include "../src/d3d12_util.h"

using namespace dsf_dlss5;

namespace {

const char* Name(Gpu::SubmitResult r) {
  return r == Gpu::SubmitResult::kOk ? "kOk" : r == Gpu::SubmitResult::kTimeout ? "kTimeout" : "kFailed";
}

bool Check(bool condition, const char* what) {
  std::printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
  return condition;
}

}  // namespace

int wmain() {
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
  Gpu gpu;
  std::string error;
  if (!gpu.Create(LUID{}, &error)) {
    std::printf("D3D12: %s\n", error.c_str());
    return 2;
  }
  ComPtr<ID3D12Fence> gate;
  if (FAILED(gpu.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) return 2;
  bool ok = true;

  // 1. A missing signal: the submission times out, then the helper's recovery drains it.
  gpu.Begin();
  Gpu::Sync stalled;
  stalled.waitFence = gate.Get();
  stalled.waitValue = 1;
  stalled.timeoutMs = 200;
  const Gpu::SubmitResult first = gpu.Submit(stalled);
  ok &= Check(first == Gpu::SubmitResult::kTimeout, "a submission waiting on an unsignalled fence times out");
  gate->Signal(1);
  ok &= Check(gpu.WaitIdle(5000), "signalling the gate from the CPU drains the queue");
  Sleep(100);

  // 2. Blocked again; the gate opens after 500 ms. Submit must return kOk only after that,
  // and with the gate open. Before the fix it returned kOk immediately.
  gpu.Begin();
  Gpu::Sync blocked = stalled;
  blocked.waitValue = 2;
  blocked.timeoutMs = 5000;
  std::thread opener([&] {
    Sleep(500);
    gate->Signal(2);
  });
  const ULONGLONG start = GetTickCount64();
  const Gpu::SubmitResult second = gpu.Submit(blocked);
  const ULONGLONG elapsed = GetTickCount64() - start;
  opener.join();
  std::printf("second submission: %s after %llu ms\n", Name(second), elapsed);
  ok &= Check(second == Gpu::SubmitResult::kOk && elapsed >= 450,
              "the next submission completes only after its fence wait was satisfied");

  // 3. And a plain submission still completes normally.
  gpu.Begin();
  ok &= Check(gpu.SubmitAndWait(), "an ordinary submission completes");
  return ok ? 0 : 1;
}
