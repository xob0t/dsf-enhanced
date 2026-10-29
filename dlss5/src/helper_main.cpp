// SPDX-License-Identifier: MIT
//
// dsf-dlss5-helper: the 64-bit process that runs DLSS 5 for the 32-bit game.
//
//   dsf-dlss5-helper.exe --game-pid <pid> [--game-process <inherited handle>]
//                        [--runtime <dir with nvngx_dlssnr.dll>] [--log <file>] [--verbose-ngx]
//                        [--allow-runtime-sha256 <hash>]...
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
#include "ngx_trust.h"
#include "nr_d3d12.h"

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

DXGI_FORMAT DefaultFormat(int slot) {
  switch (slot) {
    case DSF_DLSS5_SLOT_DEPTH: return DXGI_FORMAT_R32_FLOAT;
    case DSF_DLSS5_SLOT_MOTION: return DXGI_FORMAT_R16G16_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

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
    std::string error;
    if (!runtime_.Verify(args_.runtimeDir + L"\\nvngx_dlssnr.dll", args_.allowedSha256, &error)) {
      Reply(out, DSF_DLSS5_ERR_RUNTIME, "nvngx_dlssnr.dll: " + error);
      return;
    }
    for (int i = 0; i < 4; ++i) out->runtime_version[i] = runtime_.Version()[i];
    CopyMessage(out->runtime_sha256, sizeof(out->runtime_sha256), runtime_.Sha256().c_str());
    Log("runtime %ls %u.%u.%u.%u sha256 %s (%s)", runtime_.Path().c_str(), runtime_.Version()[0], runtime_.Version()[1],
        runtime_.Version()[2], runtime_.Version()[3], runtime_.Sha256().c_str(), runtime_.Description());

    LUID luid = {in.adapter_luid_low, in.adapter_luid_high};
    if (!gpu_.Create(luid, &error)) {
      Reply(out, DSF_DLSS5_ERR_DEVICE, "D3D12: " + error);
      return;
    }
    Log("adapter %ls (client api %u)", gpu_.AdapterName().c_str(), in.client_api);
    if (!nr_.Initialize(gpu_.Device(), runtime_, ExeDir(), args_.verboseNgx)) {
      Reply(out, DSF_DLSS5_ERR_NGX, nr_.LastError());
      return;
    }
    ready_ = true;
    Reply(out, DSF_DLSS5_OK, "ready");
  }

  void Configure(const DsfDlss5Configure& in, DsfDlss5ConfigureAck* out) {
    *out = {};
    out->ngx_result = 1;
    if (!ready_) {
      Reply(out, DSF_DLSS5_ERR_NOT_CONFIGURED, "the helper is not ready (HELLO failed or the device was lost)");
      return;
    }
    if (!Release()) {
      Reply(out, DSF_DLSS5_ERR_DEVICE, "the previous frame's GPU work did not finish");
      return;
    }
    if (in.width == 0 || in.height == 0 || in.width > 16384 || in.height > 16384) {
      Reply(out, DSF_DLSS5_ERR_PROTOCOL, "bad size");
      return;
    }
    width_ = in.width;
    height_ = in.height;
    flags_ = in.flags;
    ID3D12Device* device = gpu_.Device();
    const bool clientCreates = (in.flags & DSF_DLSS5_CONFIG_CLIENT_CREATES) != 0;
    const bool useMask = (in.flags & DSF_DLSS5_CONFIG_USE_MASK) != 0;

    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) {
      if (slot == DSF_DLSS5_SLOT_MASK && !useMask) continue;
      const std::string name = std::to_string(slot);
      if (clientCreates) {
        HANDLE local = nullptr;
        if (!DuplicateHandle(game_, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(in.textures[slot])), GetCurrentProcess(),
                             &local, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
            FAILED(device->OpenSharedHandle(local, IID_PPV_ARGS(&textures_[slot])))) {
          if (local) CloseHandle(local);
          return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "could not open the game's texture for slot " + name);
        }
        CloseHandle(local);
      } else {
        const DXGI_FORMAT format = in.formats[slot] ? static_cast<DXGI_FORMAT>(in.formats[slot]) : DefaultFormat(slot);
        // D3D11 opens a D3D12-created texture only when it allows render-target and
        // simultaneous access; without them OpenSharedResource1 returns E_INVALIDARG (tested
        // on driver 621.01).
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (slot == DSF_DLSS5_SLOT_OUTPUT) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        textures_[slot] = gpu_.CreateTexture(width_, height_, format, flags, D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_SHARED);
        uint64_t remote = 0;
        if (!textures_[slot] || !ShareInto(textures_[slot].Get(), &remote)) {
          return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "could not create or share the texture for slot " + name);
        }
        out->textures[slot] = remote;
      }
      const D3D12_RESOURCE_DESC desc = textures_[slot]->GetDesc();
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != width_ || desc.Height != height_) {
        return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "slot " + name + " is not a 2D texture of the configured size");
      }
      if (slot == DSF_DLSS5_SLOT_OUTPUT && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "the Output texture needs unordered access (D3D11_BIND_UNORDERED_ACCESS)");
      }
      if (slot != DSF_DLSS5_SLOT_OUTPUT && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "slot " + name + " needs shader-resource access");
      }
      out->formats[slot] = desc.Format;
    }
    if (!useMask) {
      // Remix's default: a mask of ones, combined with the runtime's automatic mask.
      textures_[DSF_DLSS5_SLOT_MASK] = gpu_.CreateTexture(width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                                          D3D12_RESOURCE_STATE_COPY_DEST);
      if (!textures_[DSF_DLSS5_SLOT_MASK]) {
        return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "could not create the control mask");
      }
      std::string ones(static_cast<size_t>(width_) * height_ * 4, '\xff');
      ID3D12GraphicsCommandList* list = gpu_.Begin();
      gpu_.Upload(textures_[DSF_DLSS5_SLOT_MASK].Get(), ones.data(), 4);
      Gpu::Transition(list, textures_[DSF_DLSS5_SLOT_MASK].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      if (!gpu_.SubmitAndWait()) {
        return FailConfigure(out, DSF_DLSS5_ERR_DEVICE, "could not initialize the control mask");
      }
      out->formats[DSF_DLSS5_SLOT_MASK] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    uint64_t remoteIn = 0, remoteOut = 0;
    const bool fences = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceIn_))) &&
                        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceOut_))) &&
                        ShareInto(fenceIn_.Get(), &remoteIn);
    out->fence_in = remoteIn;
    if (!fences || !ShareInto(fenceOut_.Get(), &remoteOut)) {
      return FailConfigure(out, DSF_DLSS5_ERR_RESOURCES, "could not create or share the fences");
    }
    out->fence_out = remoteOut;

    ID3D12GraphicsCommandList* list = gpu_.Begin();
    if (!nr_.CreateFeature(list, width_, height_, in.preset) || !gpu_.SubmitAndWait()) {
      out->ngx_result = 0;
      return FailConfigure(out, DSF_DLSS5_ERR_NGX, nr_.LastError());
    }
    configured_ = true;
    lastFrame_ = 0;
    Log("configured %ux%u, formats color %u output %u depth %u motion %u mask %u, %s-created textures", width_, height_,
        out->formats[0], out->formats[1], out->formats[2], out->formats[3], out->formats[4], clientCreates ? "game" : "helper");
    Reply(out, DSF_DLSS5_OK, "configured");
  }

  void Frame(const DsfDlss5Frame& in, DsfDlss5FrameAck* out) {
    *out = {};
    out->frame = in.frame;
    out->ngx_result = 1;
    out->gpu_ms = -1.0f;
    if (in.frame == 0 || in.frame <= lastFrame_) {
      // A fence cannot go backwards, and the inputs for an old n may be overwritten already.
      out->status = DSF_DLSS5_ERR_PROTOCOL;
      Log("frame %llu rejected: frame numbers must start at 1 and increase (last %llu)",
          static_cast<unsigned long long>(in.frame), static_cast<unsigned long long>(lastFrame_));
      return;
    }
    if (!configured_) {
      out->status = DSF_DLSS5_ERR_NOT_CONFIGURED;
      SignalOut(in.frame);
      return;
    }
    ID3D12Resource* color = textures_[DSF_DLSS5_SLOT_COLOR].Get();
    ID3D12Resource* output = textures_[DSF_DLSS5_SLOT_OUTPUT].Get();
    const bool bypass = (in.flags & DSF_DLSS5_FRAME_BYPASS) != 0;
    if (bypass && color->GetDesc().Format != output->GetDesc().Format) {
      out->status = DSF_DLSS5_ERR_RESOURCES;
      Log("frame %llu: BYPASS needs Color and Output in the same format", static_cast<unsigned long long>(in.frame));
      // Nothing was queued, but a game that already queued Wait(fence_out, n) must not stall.
      SignalOut(in.frame);
      lastFrame_ = in.frame;
      return;
    }
    ID3D12Resource* inputs[] = {color, textures_[DSF_DLSS5_SLOT_DEPTH].Get(), textures_[DSF_DLSS5_SLOT_MOTION].Get(),
                                textures_[DSF_DLSS5_SLOT_MASK].Get()};
    const D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList* list = gpu_.Begin();
    bool evaluated = true;
    if (bypass) {
      Gpu::Transition(list, color, kCommon, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Gpu::Transition(list, output, kCommon, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(output, color);
      Gpu::Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, kCommon);
      Gpu::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, kCommon);
    } else {
      for (ID3D12Resource* r : inputs) Gpu::Transition(list, r, kCommon, kRead);
      Gpu::Transition(list, output, kCommon, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      NrSettings s;
      s.intensity = in.intensity;
      s.toneStrength = in.tone_strength;
      s.structureStrength = in.structure_strength;
      s.skinStructureStrength = in.skin_strength;
      s.style = in.style;
      s.useAutoMask = (in.flags & DSF_DLSS5_FRAME_AUTO_MASK) != 0;
      s.depthInverted = (flags_ & DSF_DLSS5_CONFIG_DEPTH_INVERTED) != 0;
      s.reset = (in.flags & DSF_DLSS5_FRAME_RESET) != 0 || lastFrame_ == 0;
      s.jitter[0] = in.jitter[0];
      s.jitter[1] = in.jitter[1];
      s.mvScale[0] = in.mv_scale[0];
      s.mvScale[1] = in.mv_scale[1];
      evaluated = nr_.Evaluate(list, {color, output, textures_[DSF_DLSS5_SLOT_MOTION].Get(), textures_[DSF_DLSS5_SLOT_DEPTH].Get(),
                                      textures_[DSF_DLSS5_SLOT_MASK].Get()},
                               s);
      for (ID3D12Resource* r : inputs) Gpu::Transition(list, r, kRead, kCommon);
      Gpu::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kCommon);
    }

    // The queue waits for the game's fence_in = n and signals fence_out = n after the work.
    // The helper serializes on its own queue: frame n+1 cannot start before n finished.
    Gpu::Sync sync;
    sync.waitFence = fenceIn_.Get();
    sync.waitValue = in.frame;
    sync.signalFence = fenceOut_.Get();
    sync.signalValue = in.frame;
    sync.timeoutMs = kFrameTimeoutMs;
    double gpuMs = -1.0;
    const Gpu::SubmitResult result = gpu_.Submit(sync, &gpuMs);
    lastFrame_ = in.frame;
    if (result == Gpu::SubmitResult::kTimeout) {
      // The game's fence_in = n never reached the GPU. Release the queue ourselves so it
      // drains, and make the game reconfigure (its fences may be in an unknown state).
      Log("frame %llu: fence_in was not signalled within %lu ms; draining and dropping the configuration",
          static_cast<unsigned long long>(in.frame), kFrameTimeoutMs);
      fenceIn_->Signal(in.frame);
      if (!gpu_.WaitIdle(kDrainTimeoutMs)) {
        Log("the helper's queue did not drain; the device is unusable");
        ready_ = false;
      }
      configured_ = false;
      SignalOut(in.frame);
      out->status = DSF_DLSS5_ERR_TIMEOUT;
      return;
    }
    if (result == Gpu::SubmitResult::kFailed) {
      const HRESULT removed = gpu_.Device()->GetDeviceRemovedReason();
      Log("frame %llu: submission failed (device removed reason 0x%08lX)", static_cast<unsigned long long>(in.frame),
          static_cast<unsigned long>(removed));
      if (removed != S_OK) ready_ = false;
      configured_ = false;
      SignalOut(in.frame);
      out->status = DSF_DLSS5_ERR_DEVICE;
      return;
    }
    if (!evaluated) {
      // The list (barriers and the fence_out signal) still ran; only the evaluate is missing.
      out->status = DSF_DLSS5_ERR_NGX;
      out->ngx_result = 0;
      Log("frame %llu: %s", static_cast<unsigned long long>(in.frame), nr_.LastError().c_str());
      return;
    }
    if ((in.flags & DSF_DLSS5_FRAME_GPU_TIME) != 0) out->gpu_ms = static_cast<float>(gpuMs);
    if (in.frame == 1 || in.frame % 600 == 0) Log("frame %llu evaluated, %.2f ms", static_cast<unsigned long long>(in.frame), gpuMs);
  }

  // Waits for the GPU and drops the shared resources and the feature. False if the GPU did
  // not finish, in which case the session is no longer usable.
  bool Release() {
    configured_ = false;
    if (ready_ && !gpu_.WaitIdle(kDrainTimeoutMs)) {
      ready_ = false;
      return false;
    }
    nr_.ReleaseFeature();
    for (auto& t : textures_) t.Reset();
    fenceIn_.Reset();
    fenceOut_.Reset();
    return true;
  }

  void Shutdown() {
    Release();
    nr_.Shutdown();
  }

 private:
  template <typename Ack>
  void Reply(Ack* out, int status, const std::string& message) {
    out->status = status;
    CopyMessage(out->message, sizeof(out->message), message.c_str());
    if (status != DSF_DLSS5_OK) Log("error: %s", message.c_str());
  }

  // Undoes a half-finished CONFIGURE: closes every handle already placed in the game
  // process and zeroes it in the ACK, then drops the helper's own objects.
  void FailConfigure(DsfDlss5ConfigureAck* out, int status, const std::string& message) {
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
    Release();
    Reply(out, status, message);
  }

  // Lets a game that already queued Wait(fence_out, n) continue when no GPU work will
  // signal it. Never moves the fence backwards.
  void SignalOut(uint64_t frame) {
    if (fenceOut_ && fenceOut_->GetCompletedValue() < frame) fenceOut_->Signal(frame);
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
  VerifiedRuntime runtime_;
  Gpu gpu_;
  NrD3D12 nr_;
  ComPtr<ID3D12Resource> textures_[DSF_DLSS5_SLOT_COUNT];
  ComPtr<ID3D12Fence> fenceIn_, fenceOut_;
  uint32_t width_ = 0, height_ = 0, flags_ = 0;
  uint64_t lastFrame_ = 0;
  bool helloSeen_ = false;
  bool ready_ = false;
  bool configured_ = false;
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
