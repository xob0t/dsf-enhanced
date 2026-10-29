// SPDX-License-Identifier: MIT
//
// dsf-dlss5-helper: the 64-bit process that runs DLSS 5 and DLAA for the 32-bit game.
//
//   dsf-dlss5-helper.exe --game-pid <pid> [--game-process <inherited handle>]
//                        [--runtime <dir with nvngx_dlssnr.dll and/or nvngx_dlss.dll>] [--log <file>]
//                        [--verbose-ngx] [--allow-runtime-sha256 <hash>]...
//
// Protocol and conventions: include/dsf_dlss5_protocol.h. The runtime folder defaults to
// the folder of this executable.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "dsf_dlss5_pipe.h"
#include "d3d12_util.h"
#include "log.h"
#include "ngx_d3d12.h"
#include "ngx_trust.h"
#include "nr_d3d12.h"
#include "sr_d3d12.h"

using namespace dsf_dlss5;

namespace {

// How long the helper waits for the game's fence_in signal before giving up on a frame.
constexpr DWORD kFrameTimeoutMs = 2000;
// How long it then waits for its own queue to drain.
constexpr DWORD kDrainTimeoutMs = 5000;
// Writes to the game: it should be reading, so a full pipe means it is stuck.
constexpr DWORD kWriteTimeoutMs = 5000;

struct Args {
  DWORD gamePid = 0;
  HANDLE gameProcess = nullptr;
  std::wstring runtimeDir;
  std::wstring logPath;
  std::vector<std::string> allowedSha256;
  bool verboseNgx = false;
};

std::wstring ExeDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir = path;
  return dir.substr(0, dir.find_last_of(L"\\/"));
}

bool ParseArgs(int argc, wchar_t** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : L""; };
    if (arg == L"--game-pid") a->gamePid = static_cast<DWORD>(_wtoi64(next()));
    else if (arg == L"--game-process") a->gameProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wtoi64(next())));
    else if (arg == L"--runtime") a->runtimeDir = next();
    else if (arg == L"--log") a->logPath = next();
    else if (arg == L"--verbose-ngx") a->verboseNgx = true;
    else if (arg == L"--allow-runtime-sha256") a->allowedSha256.push_back(Narrow(next()));
    else return false;
  }
  if (a->runtimeDir.empty()) a->runtimeDir = ExeDir();
  if (a->logPath.empty()) a->logPath = ExeDir() + L"\\dsf-dlss5-helper.log";
  return a->gamePid != 0;
}

const char* FeatureName(uint32_t feature) { return feature == DSF_DLSS5_FEATURE_DLAA ? "DLAA" : "DLSS 5"; }

DXGI_FORMAT DefaultFormat(uint32_t feature, int slot) {
  switch (slot) {
    case DSF_DLSS5_SLOT_DEPTH: return DXGI_FORMAT_R32_FLOAT;
    case DSF_DLSS5_SLOT_MOTION: return DXGI_FORMAT_R16G16_FLOAT;
    // DLAA takes the linear HDR scene and gives it back in the same format.
    default: return feature == DSF_DLSS5_FEATURE_DLAA ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

// One feature's shared slots, fences and frame numbers (protocol version 2).
struct FeatureState {
  ComPtr<ID3D12Resource> textures[DSF_DLSS5_SLOT_COUNT];
  ComPtr<ID3D12Fence> fenceIn, fenceOut;
  uint32_t width = 0, height = 0, flags = 0;
  uint64_t lastFrame = 0;
  bool configured = false;
};

class Session {
 public:
  Session(const Args& args, HANDLE game) : args_(args), game_(game) {}

  void Hello(const DsfDlss5Hello& in, DsfDlss5HelloAck* out) {
    *out = {};
    out->helper_pid = GetCurrentProcessId();
    out->ngx_result = 1;
    if (helloSeen_) {
      Reply(out, DSF_DLSS5_ERR_PROTOCOL, "HELLO was already sent on this connection");
      return;
    }
    helloSeen_ = true;
    // Each feature needs its own DLL; either may be missing. Neither: nothing to run.
    std::string nrError, srError;
    const bool nrRuntime = VerifyRuntime(kDlssnrRuntime, &nrRuntime_, &nrError);
    const bool srRuntime = VerifyRuntime(kDlssRuntime, &srRuntime_, &srError);
    if (nrRuntime) {
      for (int i = 0; i < 4; ++i) out->runtime_version[i] = nrRuntime_.Version()[i];
      CopyMessage(out->runtime_sha256, sizeof(out->runtime_sha256), nrRuntime_.Sha256().c_str());
    }
    if (srRuntime) {
      for (int i = 0; i < 4; ++i) out->dlaa_runtime_version[i] = srRuntime_.Version()[i];
    }
    if (!nrRuntime && !srRuntime) {
      Reply(out, DSF_DLSS5_ERR_RUNTIME, "nvngx_dlssnr.dll: " + nrError + "; nvngx_dlss.dll: " + srError);
      return;
    }

    LUID luid = {in.adapter_luid_low, in.adapter_luid_high};
    std::string error;
    if (!gpu_.Create(luid, &error)) {
      Reply(out, DSF_DLSS5_ERR_DEVICE, "D3D12: " + error);
      return;
    }
    Log("adapter %ls (client api %u)", gpu_.AdapterName().c_str(), in.client_api);
    if (!ngx_.Initialize(gpu_.Device(), args_.runtimeDir, ExeDir(), args_.verboseNgx)) {
      Reply(out, DSF_DLSS5_ERR_NGX, ngx_.LastError());
      return;
    }
    if (nrRuntime) {
      available_[DSF_DLSS5_FEATURE_NR] = nr_.Initialize(ngx_, nrRuntime_);
      if (!available_[DSF_DLSS5_FEATURE_NR]) nrError = nr_.LastError();
    }
    if (srRuntime) {
      available_[DSF_DLSS5_FEATURE_DLAA] = sr_.Initialize(ngx_, srRuntime_);
      if (!available_[DSF_DLSS5_FEATURE_DLAA]) srError = sr_.LastError();
    }
    std::string message;
    for (uint32_t f = 0; f < DSF_DLSS5_FEATURE_COUNT; ++f) {
      if (available_[f]) out->features |= DSF_DLSS5_FEATURE_BIT(f);
      message += std::string(message.empty() ? "" : "; ") + FeatureName(f) + ": " +
                 (available_[f] ? "ready" : f == DSF_DLSS5_FEATURE_NR ? nrError : srError);
    }
    if (out->features == 0) {
      Reply(out, DSF_DLSS5_ERR_NGX, message);
      return;
    }
    ready_ = true;
    Log("features 0x%X: %s", out->features, message.c_str());
    Reply(out, DSF_DLSS5_OK, message);
  }

  void Configure(const DsfDlss5Configure& in, DsfDlss5ConfigureAck* out) {
    *out = {};
    out->ngx_result = 1;
    if (!ready_) {
      Reply(out, DSF_DLSS5_ERR_NOT_CONFIGURED, "the helper is not ready (HELLO failed or the device was lost)");
      return;
    }
    if (in.feature >= DSF_DLSS5_FEATURE_COUNT) {
      Reply(out, DSF_DLSS5_ERR_PROTOCOL, "unknown feature " + std::to_string(in.feature));
      return;
    }
    const uint32_t feature = in.feature;
    const bool dlaa = feature == DSF_DLSS5_FEATURE_DLAA;
    if (!available_[feature]) {
      Reply(out, DSF_DLSS5_ERR_RUNTIME, std::string(FeatureName(feature)) + " is not available (see HELLO_ACK)");
      return;
    }
    if (!Release(feature)) {
      Reply(out, DSF_DLSS5_ERR_DEVICE, "the previous frame's GPU work did not finish");
      return;
    }
    if (in.width == 0 || in.height == 0 || in.width > 16384 || in.height > 16384) {
      Reply(out, DSF_DLSS5_ERR_PROTOCOL, "bad size");
      return;
    }
    FeatureState& s = state_[feature];
    s.width = in.width;
    s.height = in.height;
    s.flags = in.flags;
    ID3D12Device* device = gpu_.Device();
    const bool clientCreates = (in.flags & DSF_DLSS5_CONFIG_CLIENT_CREATES) != 0;
    const bool useMask = !dlaa && (in.flags & DSF_DLSS5_CONFIG_USE_MASK) != 0;

    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) {
      if (slot == DSF_DLSS5_SLOT_MASK && !useMask) continue;
      const std::string name = std::to_string(slot);
      if (clientCreates) {
        HANDLE local = nullptr;
        if (!DuplicateHandle(game_, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(in.textures[slot])), GetCurrentProcess(),
                             &local, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
            FAILED(device->OpenSharedHandle(local, IID_PPV_ARGS(&s.textures[slot])))) {
          if (local) CloseHandle(local);
          return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES, "could not open the game's texture for slot " + name);
        }
        CloseHandle(local);
      } else {
        const DXGI_FORMAT format = in.formats[slot] ? static_cast<DXGI_FORMAT>(in.formats[slot]) : DefaultFormat(feature, slot);
        // D3D11 opens a D3D12-created texture only when it allows render-target and
        // simultaneous access; without them OpenSharedResource1 returns E_INVALIDARG (tested
        // on driver 621.01).
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (slot == DSF_DLSS5_SLOT_OUTPUT) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        s.textures[slot] = gpu_.CreateTexture(s.width, s.height, format, flags, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_SHARED);
        uint64_t remote = 0;
        if (!s.textures[slot] || !ShareInto(s.textures[slot].Get(), &remote)) {
          return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES, "could not create or share the texture for slot " + name);
        }
        out->textures[slot] = remote;
      }
      const D3D12_RESOURCE_DESC desc = s.textures[slot]->GetDesc();
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != s.width || desc.Height != s.height) {
        return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES, "slot " + name + " is not a 2D texture of the configured size");
      }
      if (slot == DSF_DLSS5_SLOT_OUTPUT && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES,
                             "the Output texture needs unordered access (D3D11_BIND_UNORDERED_ACCESS)");
      }
      if (slot != DSF_DLSS5_SLOT_OUTPUT && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES, "slot " + name + " needs shader-resource access");
      }
      out->formats[slot] = desc.Format;
    }
    if (!dlaa && !useMask) {
      // Remix's default: a mask of ones, combined with the runtime's automatic mask.
      s.textures[DSF_DLSS5_SLOT_MASK] = gpu_.CreateTexture(s.width, s.height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                                           D3D12_RESOURCE_STATE_COPY_DEST);
      if (!s.textures[DSF_DLSS5_SLOT_MASK]) {
        return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES, "could not create the control mask");
      }
      std::string ones(static_cast<size_t>(s.width) * s.height * 4, '\xff');
      ID3D12GraphicsCommandList* list = gpu_.Begin();
      gpu_.Upload(s.textures[DSF_DLSS5_SLOT_MASK].Get(), ones.data(), 4);
      Gpu::Transition(list, s.textures[DSF_DLSS5_SLOT_MASK].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      if (!gpu_.SubmitAndWait()) {
        return FailConfigure(feature, out, DSF_DLSS5_ERR_DEVICE, "could not initialize the control mask");
      }
      out->formats[DSF_DLSS5_SLOT_MASK] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    uint64_t remoteIn = 0, remoteOut = 0;
    const bool fences = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fenceIn))) &&
                        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fenceOut))) &&
                        ShareInto(s.fenceIn.Get(), &remoteIn);
    out->fence_in = remoteIn;
    if (!fences || !ShareInto(s.fenceOut.Get(), &remoteOut)) {
      return FailConfigure(feature, out, DSF_DLSS5_ERR_RESOURCES, "could not create or share the fences");
    }
    out->fence_out = remoteOut;

    ID3D12GraphicsCommandList* list = gpu_.Begin();
    const bool created = dlaa ? sr_.CreateFeature(list, s.width, s.height, in.preset) : nr_.CreateFeature(list, s.width, s.height, in.preset);
    if (!created || !gpu_.SubmitAndWait()) {
      out->ngx_result = 0;
      return FailConfigure(feature, out, DSF_DLSS5_ERR_NGX, dlaa ? sr_.LastError() : nr_.LastError());
    }
    s.configured = true;
    s.lastFrame = 0;
    Log("%s configured %ux%u, formats color %u output %u depth %u motion %u mask %u, %s-created textures", FeatureName(feature),
        s.width, s.height, out->formats[0], out->formats[1], out->formats[2], out->formats[3], out->formats[4],
        clientCreates ? "game" : "helper");
    Reply(out, DSF_DLSS5_OK, "configured");
  }

  void Frame(const DsfDlss5Frame& in, DsfDlss5FrameAck* out) {
    *out = {};
    out->frame = in.frame;
    out->ngx_result = 1;
    out->gpu_ms = -1.0f;
    out->feature = in.feature;
    if (in.feature >= DSF_DLSS5_FEATURE_COUNT) {
      out->status = DSF_DLSS5_ERR_PROTOCOL;
      Log("frame %llu rejected: unknown feature %u", static_cast<unsigned long long>(in.frame), in.feature);
      return;
    }
    const uint32_t feature = in.feature;
    const bool dlaa = feature == DSF_DLSS5_FEATURE_DLAA;
    FeatureState& st = state_[feature];
    if (in.frame == 0 || in.frame <= st.lastFrame) {
      // A fence cannot go backwards, and the inputs for an old n may be overwritten already.
      out->status = DSF_DLSS5_ERR_PROTOCOL;
      Log("%s frame %llu rejected: frame numbers must start at 1 and increase (last %llu)", FeatureName(feature),
          static_cast<unsigned long long>(in.frame), static_cast<unsigned long long>(st.lastFrame));
      return;
    }
    if (!st.configured) {
      out->status = DSF_DLSS5_ERR_NOT_CONFIGURED;
      SignalOut(st, in.frame);
      return;
    }
    ID3D12Resource* color = st.textures[DSF_DLSS5_SLOT_COLOR].Get();
    ID3D12Resource* output = st.textures[DSF_DLSS5_SLOT_OUTPUT].Get();
    ID3D12Resource* depth = st.textures[DSF_DLSS5_SLOT_DEPTH].Get();
    ID3D12Resource* motion = st.textures[DSF_DLSS5_SLOT_MOTION].Get();
    ID3D12Resource* mask = st.textures[DSF_DLSS5_SLOT_MASK].Get();   // none for DLAA
    const bool bypass = (in.flags & DSF_DLSS5_FRAME_BYPASS) != 0;
    if (bypass && color->GetDesc().Format != output->GetDesc().Format) {
      out->status = DSF_DLSS5_ERR_RESOURCES;
      Log("frame %llu: BYPASS needs Color and Output in the same format", static_cast<unsigned long long>(in.frame));
      // Nothing was queued, but a game that already queued Wait(fence_out, n) must not stall.
      SignalOut(st, in.frame);
      st.lastFrame = in.frame;
      return;
    }
    std::vector<ID3D12Resource*> inputs = {color, depth, motion};
    if (mask != nullptr) inputs.push_back(mask);
    const D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList* list = gpu_.Begin();
    bool evaluated = true;
    const bool reset = (in.flags & DSF_DLSS5_FRAME_RESET) != 0 || st.lastFrame == 0;
    if (bypass) {
      Gpu::Transition(list, color, kCommon, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Gpu::Transition(list, output, kCommon, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(output, color);
      Gpu::Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, kCommon);
      Gpu::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, kCommon);
    } else {
      for (ID3D12Resource* r : inputs) Gpu::Transition(list, r, kCommon, kRead);
      Gpu::Transition(list, output, kCommon, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (dlaa) {
        SrSettings s;
        s.reset = reset;
        s.jitter[0] = in.jitter[0];
        s.jitter[1] = in.jitter[1];
        s.mvScale[0] = in.mv_scale[0];
        s.mvScale[1] = in.mv_scale[1];
        evaluated = sr_.Evaluate(list, {color, output, motion, depth}, s);
      } else {
        NrSettings s;
        s.intensity = in.intensity;
        s.toneStrength = in.tone_strength;
        s.structureStrength = in.structure_strength;
        s.skinStructureStrength = in.skin_strength;
        s.style = in.style;
        s.useAutoMask = (in.flags & DSF_DLSS5_FRAME_AUTO_MASK) != 0;
        s.depthInverted = (st.flags & DSF_DLSS5_CONFIG_DEPTH_INVERTED) != 0;
        s.reset = reset;
        s.jitter[0] = in.jitter[0];
        s.jitter[1] = in.jitter[1];
        s.mvScale[0] = in.mv_scale[0];
        s.mvScale[1] = in.mv_scale[1];
        evaluated = nr_.Evaluate(list, {color, output, motion, depth, mask}, s);
      }
      for (ID3D12Resource* r : inputs) Gpu::Transition(list, r, kRead, kCommon);
      Gpu::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kCommon);
    }

    // The queue waits for the game's fence_in = n and signals fence_out = n after the work.
    // The helper serializes on its own queue: frame n+1 cannot start before n finished.
    Gpu::Sync sync;
    sync.waitFence = st.fenceIn.Get();
    sync.waitValue = in.frame;
    sync.signalFence = st.fenceOut.Get();
    sync.signalValue = in.frame;
    sync.timeoutMs = kFrameTimeoutMs;
    double gpuMs = -1.0;
    const Gpu::SubmitResult result = gpu_.Submit(sync, &gpuMs);
    st.lastFrame = in.frame;
    if (result == Gpu::SubmitResult::kTimeout) {
      // The game's fence_in = n never reached the GPU. Release the queue ourselves so it
      // drains, and make the game reconfigure (its fences may be in an unknown state).
      Log("%s frame %llu: fence_in was not signalled within %lu ms; draining and dropping the configuration", FeatureName(feature),
          static_cast<unsigned long long>(in.frame), kFrameTimeoutMs);
      st.fenceIn->Signal(in.frame);
      if (!gpu_.WaitIdle(kDrainTimeoutMs)) {
        Log("the helper's queue did not drain; the device is unusable");
        ready_ = false;
      }
      st.configured = false;
      SignalOut(st, in.frame);
      out->status = DSF_DLSS5_ERR_TIMEOUT;
      return;
    }
    if (result == Gpu::SubmitResult::kFailed) {
      const HRESULT removed = gpu_.Device()->GetDeviceRemovedReason();
      Log("%s frame %llu: submission failed (device removed reason 0x%08lX)", FeatureName(feature),
          static_cast<unsigned long long>(in.frame), static_cast<unsigned long>(removed));
      if (removed != S_OK) ready_ = false;
      st.configured = false;
      SignalOut(st, in.frame);
      out->status = DSF_DLSS5_ERR_DEVICE;
      return;
    }
    if (!evaluated) {
      // The list (barriers and the fence_out signal) still ran; only the evaluate is missing.
      out->status = DSF_DLSS5_ERR_NGX;
      out->ngx_result = 0;
      Log("%s frame %llu: %s", FeatureName(feature), static_cast<unsigned long long>(in.frame),
          (dlaa ? sr_.LastError() : nr_.LastError()).c_str());
      return;
    }
    if ((in.flags & DSF_DLSS5_FRAME_GPU_TIME) != 0) out->gpu_ms = static_cast<float>(gpuMs);
    if (in.frame == 1 || in.frame % 600 == 0) {
      Log("%s frame %llu evaluated, %.2f ms", FeatureName(feature), static_cast<unsigned long long>(in.frame), gpuMs);
    }
  }

  // Waits for the GPU and drops one feature's shared resources and NGX feature. False if the
  // GPU did not finish, in which case the session is no longer usable.
  bool Release(uint32_t feature) {
    FeatureState& s = state_[feature];
    s.configured = false;
    if (ready_ && !gpu_.WaitIdle(kDrainTimeoutMs)) {
      ready_ = false;
      return false;
    }
    if (feature == DSF_DLSS5_FEATURE_DLAA) {
      sr_.ReleaseFeature();
    } else {
      nr_.ReleaseFeature();
    }
    for (auto& t : s.textures) t.Reset();
    s.fenceIn.Reset();
    s.fenceOut.Reset();
    return true;
  }

  void Shutdown() {
    for (uint32_t f = 0; f < DSF_DLSS5_FEATURE_COUNT; ++f) Release(f);
    nr_.Shutdown();
    sr_.Shutdown();
    ngx_.Shutdown();
  }

 private:
  // A missing file is no error here (the feature is just not offered); a file that is there
  // and fails the checks is logged.
  bool VerifyRuntime(const RuntimeSpec& spec, VerifiedRuntime* runtime, std::string* error) {
    const std::wstring path = args_.runtimeDir + L"\\" + spec.file;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      *error = "not in " + Narrow(args_.runtimeDir);
      Log("%ls: %s", spec.file, error->c_str());
      return false;
    }
    if (!runtime->Verify(path, spec, args_.allowedSha256, error)) {
      Log("%ls refused: %s", spec.file, error->c_str());
      return false;
    }
    Log("runtime %ls %u.%u.%u.%u sha256 %s (%s)", runtime->Path().c_str(), runtime->Version()[0], runtime->Version()[1],
        runtime->Version()[2], runtime->Version()[3], runtime->Sha256().c_str(), runtime->Description());
    return true;
  }

  template <typename Ack>
  void Reply(Ack* out, int status, const std::string& message) {
    out->status = status;
    CopyMessage(out->message, sizeof(out->message), message.c_str());
    if (status != DSF_DLSS5_OK) Log("error: %s", message.c_str());
  }

  // Undoes a half-finished CONFIGURE: closes every handle already placed in the game
  // process and zeroes it in the ACK, then drops the feature's own objects.
  void FailConfigure(uint32_t feature, DsfDlss5ConfigureAck* out, int status, const std::string& message) {
    auto closeRemote = [this](uint64_t* value) {
      if (*value != 0) {
        DuplicateHandle(game_, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(*value)), nullptr, nullptr, 0, FALSE,
                        DUPLICATE_CLOSE_SOURCE);
        *value = 0;
      }
    };
    for (uint64_t& texture : out->textures) closeRemote(&texture);
    closeRemote(&out->fence_in);
    closeRemote(&out->fence_out);
    Release(feature);
    Reply(out, status, message);
  }

  // Lets a game that already queued Wait(fence_out, n) continue when no GPU work will
  // signal it. Never moves the fence backwards.
  static void SignalOut(FeatureState& s, uint64_t frame) {
    if (s.fenceOut && s.fenceOut->GetCompletedValue() < frame) s.fenceOut->Signal(frame);
  }

  // Creates an NT handle for a shared resource or fence and moves it into the game process.
  bool ShareInto(ID3D12DeviceChild* object, uint64_t* remote) {
    HANDLE local = nullptr;
    HANDLE theirs = nullptr;
    if (FAILED(gpu_.Device()->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &local))) return false;
    const BOOL ok = DuplicateHandle(GetCurrentProcess(), local, game_, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    if (!ok) CloseHandle(local);
    *remote = ok ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs)) : 0;
    return ok != FALSE;
  }

  const Args& args_;
  HANDLE game_ = nullptr;
  VerifiedRuntime nrRuntime_, srRuntime_;
  Gpu gpu_;
  NgxD3D12 ngx_;
  NrD3D12 nr_;
  SrD3D12 sr_;
  FeatureState state_[DSF_DLSS5_FEATURE_COUNT];
  bool available_[DSF_DLSS5_FEATURE_COUNT] = {};
  bool helloSeen_ = false;
  bool ready_ = false;
};

DWORD WINAPI WatchGame(void* process) {
  WaitForSingleObject(static_cast<HANDLE>(process), INFINITE);
  Log("game process exited; helper exiting");
  ExitProcess(0);
}

// Waits for a client and accepts it only if it is the game process.
bool AcceptGame(HANDLE pipe, DWORD gamePid) {
  for (;;) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
    if (!connected) {
      const DWORD error = GetLastError();
      DWORD unused = 0;
      if (error == ERROR_PIPE_CONNECTED) connected = true;
      else if (error == ERROR_IO_PENDING) connected = GetOverlappedResult(pipe, &overlapped, &unused, TRUE) != FALSE;
    }
    CloseHandle(overlapped.hEvent);
    if (!connected) {
      Log("pipe connect failed (error %lu)", GetLastError());
      return false;
    }
    ULONG clientPid = 0;
    if (GetNamedPipeClientProcessId(pipe, &clientPid) && clientPid == gamePid) return true;
    Log("rejected a pipe client from process %lu (expected the game, %lu)", clientPid, gamePid);
    DisconnectNamedPipe(pipe);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // /DEPENDENTLOADFLAG only covers static imports. Make every later LoadLibrary by name
  // search System32 (and folders added with AddDllDirectory) too, never the application
  // or current folder. NGX loads its core and nvngx_dlssnr.dll by full path, which this
  // does not affect.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::fprintf(stderr,
                 "usage: dsf-dlss5-helper.exe --game-pid <pid> [--game-process <handle>] [--runtime <dir>] [--log <file>]"
                 " [--allow-runtime-sha256 <hash>]\n");
    return 2;
  }
  OpenLogFile(args.logPath.c_str());
  Log("dsf-dlss5-helper protocol %u, game pid %lu", DSF_DLSS5_VERSION, args.gamePid);

  HANDLE game = args.gameProcess;
  if (game == nullptr) game = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, args.gamePid);
  if (game == nullptr) {
    Log("could not open the game process (error %lu)", GetLastError());
    return 3;
  }
  CloseHandle(CreateThread(nullptr, 0, WatchGame, game, 0, nullptr));

  wchar_t pipeName[128];
  swprintf_s(pipeName, DSF_DLSS5_PIPE_FMT, args.gamePid);
  HANDLE pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    Log("could not create %ls (error %lu)", pipeName, GetLastError());
    return 4;
  }
  if (!AcceptGame(pipe, args.gamePid)) return 4;
  Log("game connected on %ls", pipeName);

  Session session(args, game);
  for (;;) {
    // The game may idle between frames (menus, alt-tab), so reads have no timeout; the
    // game exiting ends the helper through WatchGame.
    DsfDlss5Header header;
    if (!PipeReadHeader(pipe, &header)) {
      Log("pipe closed; helper exiting");
      break;
    }
    if (!HeaderValid(header)) {
      Log("bad header (magic 0x%08X, version %u); helper exiting", header.magic, header.version);
      PipeSendError(pipe, DSF_DLSS5_ERR_PROTOCOL, "bad magic or protocol version", kWriteTimeoutMs);
      break;
    }
    bool ok = true;
    if (header.type == DSF_DLSS5_MSG_HELLO) {
      DsfDlss5Hello in;
      DsfDlss5HelloAck out;
      ok = PipeReadPayload(pipe, header, &in);
      if (ok) {
        session.Hello(in, &out);
        ok = PipeSend(pipe, DSF_DLSS5_MSG_HELLO_ACK, out, kWriteTimeoutMs);
      }
    } else if (header.type == DSF_DLSS5_MSG_CONFIGURE) {
      DsfDlss5Configure in;
      DsfDlss5ConfigureAck out;
      ok = PipeReadPayload(pipe, header, &in);
      if (ok) {
        session.Configure(in, &out);
        ok = PipeSend(pipe, DSF_DLSS5_MSG_CONFIGURE_ACK, out, kWriteTimeoutMs);
      }
    } else if (header.type == DSF_DLSS5_MSG_FRAME) {
      DsfDlss5Frame in;
      DsfDlss5FrameAck out;
      ok = PipeReadPayload(pipe, header, &in);
      if (ok) {
        session.Frame(in, &out);
        ok = PipeSend(pipe, DSF_DLSS5_MSG_FRAME_ACK, out, kWriteTimeoutMs);
      }
    } else if (header.type == DSF_DLSS5_MSG_SHUTDOWN && header.size == 0) {
      Log("shutdown requested");
      break;
    } else {
      ok = false;
    }
    if (!ok) {
      Log("bad message or broken pipe (type %u, size %u); helper exiting", header.type, header.size);
      PipeSendError(pipe, DSF_DLSS5_ERR_PROTOCOL, "unexpected message type or size", kWriteTimeoutMs);
      break;
    }
  }
  session.Shutdown();
  CloseHandle(pipe);
  return 0;
}
