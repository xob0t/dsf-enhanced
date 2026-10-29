// SPDX-License-Identifier: MIT
//
// dsf-dlss5-helper: the 64-bit process that runs DLSS 5 for the 32-bit game.
//
//   dsf-dlss5-helper.exe --game-pid <pid> [--game-process <inherited handle>]
//                        [--runtime <dir with nvngx_dlssnr.dll>] [--log <file>] [--verbose-ngx]
//
// Protocol and conventions: include/dsf_dlss5_protocol.h. The runtime folder defaults to
// the folder of this executable.

#include <windows.h>

#include <cstdio>
#include <string>

#include "dsf_dlss5_pipe.h"
#include "d3d12_util.h"
#include "log.h"
#include "ngx_trust.h"
#include "nr_d3d12.h"

using namespace dsf_dlss5;

namespace {

struct Args {
  DWORD gamePid = 0;
  HANDLE gameProcess = nullptr;
  std::wstring runtimeDir;
  std::wstring logPath;
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
  explicit Session(const Args& args) : args_(args) {}

  void Hello(const DsfDlss5Hello& in, DsfDlss5HelloAck* out) {
    *out = {};
    out->helper_pid = GetCurrentProcessId();
    out->ngx_result = 1;
    std::string error;
    RuntimeFileInfo runtime;
    if (!VerifyNgxRuntime(args_.runtimeDir + L"\\nvngx_dlssnr.dll", &runtime, &error)) {
      Reply(out, DSF_DLSS5_ERR_RUNTIME, "nvngx_dlssnr.dll: " + error);
      return;
    }
    for (int i = 0; i < 4; ++i) out->runtime_version[i] = runtime.version[i];
    CopyMessage(out->runtime_sha256, sizeof(out->runtime_sha256), runtime.sha256.c_str());
    Log("runtime %ls %u.%u.%u.%u sha256 %s", runtime.path.c_str(), runtime.version[0], runtime.version[1],
        runtime.version[2], runtime.version[3], runtime.sha256.c_str());

    LUID luid = {in.adapter_luid_low, in.adapter_luid_high};
    if (!gpu_.Create(luid, &error)) {
      Reply(out, DSF_DLSS5_ERR_DEVICE, "D3D12: " + error);
      return;
    }
    Log("adapter %ls (client api %u)", gpu_.AdapterName().c_str(), in.client_api);
    if (!nr_.Initialize(gpu_.Device(), args_.runtimeDir, ExeDir(), args_.verboseNgx)) {
      Reply(out, DSF_DLSS5_ERR_NGX, nr_.LastError());
      return;
    }
    ready_ = true;
    Reply(out, DSF_DLSS5_OK, "ready");
  }

  void Configure(const DsfDlss5Configure& in, DsfDlss5ConfigureAck* out) {
    *out = {};
    out->ngx_result = 1;
    Release();
    if (!ready_) {
      Reply(out, DSF_DLSS5_ERR_NOT_CONFIGURED, "HELLO failed");
      return;
    }
    width_ = in.width;
    height_ = in.height;
    flags_ = in.flags;
    ID3D12Device* device = gpu_.Device();
    const bool clientCreates = (in.flags & DSF_DLSS5_CONFIG_CLIENT_CREATES) != 0;
    const bool useMask = (in.flags & DSF_DLSS5_CONFIG_USE_MASK) != 0;

    for (int slot = 0; slot < DSF_DLSS5_SLOT_COUNT; ++slot) {
      const bool shared = slot != DSF_DLSS5_SLOT_MASK || useMask;
      if (!shared) continue;
      if (clientCreates) {
        HANDLE local = nullptr;
        if (!DuplicateHandle(game_, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(in.textures[slot])), GetCurrentProcess(),
                             &local, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
            FAILED(device->OpenSharedHandle(local, IID_PPV_ARGS(&textures_[slot])))) {
          if (local) CloseHandle(local);
          Reply(out, DSF_DLSS5_ERR_RESOURCES, "could not open the game's texture for slot " + std::to_string(slot));
          return;
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
          Reply(out, DSF_DLSS5_ERR_RESOURCES, "could not create or share the texture for slot " + std::to_string(slot));
          return;
        }
        out->textures[slot] = remote;
      }
      const D3D12_RESOURCE_DESC desc = textures_[slot]->GetDesc();
      if (desc.Width != width_ || desc.Height != height_) {
        Reply(out, DSF_DLSS5_ERR_RESOURCES, "texture size does not match the configured size, slot " + std::to_string(slot));
        return;
      }
      out->formats[slot] = desc.Format;
    }
    if (!useMask) {
      // Remix's default: a mask of ones, combined with the runtime's automatic mask.
      textures_[DSF_DLSS5_SLOT_MASK] = gpu_.CreateTexture(width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                                          D3D12_RESOURCE_STATE_COPY_DEST);
      std::string ones(static_cast<size_t>(width_) * height_ * 4, '\xff');
      ID3D12GraphicsCommandList* list = gpu_.Begin();
      gpu_.Upload(textures_[DSF_DLSS5_SLOT_MASK].Get(), ones.data(), 4);
      Gpu::Transition(list, textures_[DSF_DLSS5_SLOT_MASK].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      gpu_.SubmitAndWait();
      out->formats[DSF_DLSS5_SLOT_MASK] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    uint64_t remoteIn = 0, remoteOut = 0;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceIn_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fenceOut_))) ||
        !ShareInto(fenceIn_.Get(), &remoteIn) || !ShareInto(fenceOut_.Get(), &remoteOut)) {
      Reply(out, DSF_DLSS5_ERR_RESOURCES, "could not create or share the fences");
      return;
    }
    out->fence_in = remoteIn;
    out->fence_out = remoteOut;

    ID3D12GraphicsCommandList* list = gpu_.Begin();
    if (!nr_.CreateFeature(list, width_, height_, in.preset) || !gpu_.SubmitAndWait()) {
      out->ngx_result = 0;
      Reply(out, DSF_DLSS5_ERR_NGX, nr_.LastError());
      return;
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
    if (!configured_) {
      out->status = DSF_DLSS5_ERR_NOT_CONFIGURED;
      return;
    }
    ID3D12Resource* color = textures_[DSF_DLSS5_SLOT_COLOR].Get();
    ID3D12Resource* output = textures_[DSF_DLSS5_SLOT_OUTPUT].Get();
    ID3D12Resource* inputs[] = {color, textures_[DSF_DLSS5_SLOT_DEPTH].Get(), textures_[DSF_DLSS5_SLOT_MOTION].Get(),
                                textures_[DSF_DLSS5_SLOT_MASK].Get()};
    const D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList* list = gpu_.Begin();
    bool ok = true;
    if ((in.flags & DSF_DLSS5_FRAME_BYPASS) != 0) {
      Gpu::Transition(list, color, kCommon, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Gpu::Transition(list, output, kCommon, D3D12_RESOURCE_STATE_COPY_DEST);
      if (color->GetDesc().Format == output->GetDesc().Format) list->CopyResource(output, color);
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
      ok = nr_.Evaluate(list, {color, output, textures_[DSF_DLSS5_SLOT_MOTION].Get(), textures_[DSF_DLSS5_SLOT_DEPTH].Get(),
                               textures_[DSF_DLSS5_SLOT_MASK].Get()},
                        s);
      for (ID3D12Resource* r : inputs) Gpu::Transition(list, r, kRead, kCommon);
      Gpu::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kCommon);
    }
    double gpuMs = -1.0;
    // The helper serializes on its own queue: frame n+1 cannot start before n finished,
    // which matches the single Output slot.
    const bool submitted = gpu_.SubmitAndWait(&gpuMs, fenceIn_.Get(), in.frame, fenceOut_.Get(), in.frame);
    if (!ok || !submitted) {
      out->status = DSF_DLSS5_ERR_NGX;
      out->ngx_result = 0;
      Log("frame %llu failed: %s", static_cast<unsigned long long>(in.frame), ok ? "device removed" : nr_.LastError().c_str());
      return;
    }
    if ((in.flags & DSF_DLSS5_FRAME_ACK_AFTER_GPU) != 0) out->gpu_ms = static_cast<float>(gpuMs);
    if (lastFrame_ == 0 || in.frame % 600 == 0) {
      Log("frame %llu evaluated, %.2f ms", static_cast<unsigned long long>(in.frame), gpuMs);
    }
    lastFrame_ = in.frame;
  }

  void SetGame(HANDLE process) { game_ = process; }

  void Release() {
    configured_ = false;
    nr_.ReleaseFeature();
    for (auto& t : textures_) t.Reset();
    fenceIn_.Reset();
    fenceOut_.Reset();
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

  // Creates an NT handle for a shared resource or fence and moves it into the game process.
  bool ShareInto(ID3D12DeviceChild* object, uint64_t* remote) {
    HANDLE local = nullptr;
    HANDLE theirs = nullptr;
    if (FAILED(gpu_.Device()->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &local))) return false;
    const BOOL ok = DuplicateHandle(GetCurrentProcess(), local, game_, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    if (!ok) CloseHandle(local);
    *remote = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs));
    return ok != FALSE;
  }

  const Args& args_;
  HANDLE game_ = nullptr;
  Gpu gpu_;
  NrD3D12 nr_;
  ComPtr<ID3D12Resource> textures_[DSF_DLSS5_SLOT_COUNT];
  ComPtr<ID3D12Fence> fenceIn_, fenceOut_;
  uint32_t width_ = 0, height_ = 0, flags_ = 0;
  uint64_t lastFrame_ = 0;
  bool ready_ = false;
  bool configured_ = false;
};

DWORD WINAPI WatchGame(void* process) {
  WaitForSingleObject(static_cast<HANDLE>(process), INFINITE);
  Log("game process exited; helper exiting");
  ExitProcess(0);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::fprintf(stderr, "usage: dsf-dlss5-helper.exe --game-pid <pid> [--game-process <handle>] [--runtime <dir>] [--log <file>]\n");
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
  HANDLE pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    Log("could not create %ls (error %lu)", pipeName, GetLastError());
    return 4;
  }
  if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
    Log("pipe connect failed (error %lu)", GetLastError());
    return 4;
  }
  Log("game connected on %ls", pipeName);

  Session session(args);
  session.SetGame(game);
  for (;;) {
    DsfDlss5Header header;
    if (!PipeReadHeader(pipe, &header)) {
      Log("pipe closed or protocol mismatch; helper exiting");
      break;
    }
    bool ok = true;
    if (header.type == DSF_DLSS5_MSG_HELLO) {
      DsfDlss5Hello in;
      DsfDlss5HelloAck out;
      ok = PipeReadPayload(pipe, header, &in);
      if (ok) {
        session.Hello(in, &out);
        ok = PipeSend(pipe, DSF_DLSS5_MSG_HELLO_ACK, out);
      }
    } else if (header.type == DSF_DLSS5_MSG_CONFIGURE) {
      DsfDlss5Configure in;
      DsfDlss5ConfigureAck out;
      ok = PipeReadPayload(pipe, header, &in);
      if (ok) {
        session.Configure(in, &out);
        ok = PipeSend(pipe, DSF_DLSS5_MSG_CONFIGURE_ACK, out);
      }
    } else if (header.type == DSF_DLSS5_MSG_FRAME) {
      DsfDlss5Frame in;
      DsfDlss5FrameAck out;
      ok = PipeReadPayload(pipe, header, &in);
      if (ok) {
        session.Frame(in, &out);
        ok = PipeSend(pipe, DSF_DLSS5_MSG_FRAME_ACK, out);
      }
    } else if (header.type == DSF_DLSS5_MSG_SHUTDOWN) {
      Log("shutdown requested");
      break;
    } else {
      ok = false;
    }
    if (!ok) {
      Log("bad message (type %u, size %u); helper exiting", header.type, header.size);
      break;
    }
  }
  session.Shutdown();
  CloseHandle(pipe);
  return 0;
}
