// SPDX-License-Identifier: MIT
//
// DSF Enhanced <-> dsf-dlss5-helper protocol, version 2.
//
// The 32-bit game side (DSF Enhanced, running the game under DXVK or dgVoodoo2) cannot
// load NGX, which is 64-bit only, so the NGX passes run in dsf-dlss5-helper.exe. The two
// processes share GPU textures and two fences per feature. A named pipe carries only the
// fixed-size messages below; all pixel data stays on the GPU.
//
// Features (version 2)
//   DSF_DLSS5_FEATURE_NR   DLSS 5 (NGX DLSSNR, nvngx_dlssnr.dll), as in version 1.
//   DSF_DLSS5_FEATURE_DLAA DLAA: DLSS Super Resolution at render size = output size
//                          (NGX SuperSampling, nvngx_dlss.dll), research/dlaa.md.
//   CONFIGURE and FRAME name their feature; FRAME_ACK echoes it. Each feature has its own
//   five slots, its own two fences and its own frame numbers, so everything under "Shared
//   resources" and "Per frame n" below holds per feature. Both share one D3D12 device, one
//   NGX initialization (both DLLs in the --runtime folder) and the pipe. HELLO_ACK says which
//   features the runtime folder and the GPU allow: DLAA runs on every RTX GPU, DLSS 5 needs an
//   RTX 50, and either DLL may be missing. Version 1 is version 2 with every message naming
//   DSF_DLSS5_FEATURE_NR; a version 1 peer is refused (bad version).
//
// Lifetime
//   1. The game starts the helper: dsf-dlss5-helper.exe --game-pid <pid>
//      [--game-process <inherited handle value>] [--runtime <dir>] [--log <file>].
//   2. The helper creates the pipe DSF_DLSS5_PIPE_FMT and the game connects to it.
//   3. HELLO / HELLO_ACK: the game names its adapter LUID; the helper creates its D3D12
//      device on that adapter, verifies nvngx_dlssnr.dll and nvngx_dlss.dll (whichever are
//      there) and initializes NGX.
//   4. CONFIGURE / CONFIGURE_ACK on every size or format change of a feature: that
//      feature's shared textures and NGX feature are (re)created.
//   5. FRAME / FRAME_ACK once per frame.
//   6. SHUTDOWN, a broken pipe, or the game process exiting ends the helper.
//   A message the helper cannot parse (bad magic, version, type or size) is answered with
//   DSF_DLSS5_MSG_ERROR and ends the connection. Only the game process may connect: the
//   helper checks the pipe client's process id against --game-pid.
//
// Shared resources
//   Five 2D textures (DSF_DLSS5_SLOT_*) and two fences. Either side may create the
//   textures:
//    - DSF_DLSS5_CONFIG_CLIENT_CREATES: a D3D11 client creates them with
//      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE and sends its own
//      NT handle values; the helper duplicates them out of the game process.
//    - otherwise the helper creates them on D3D12 (D3D12_HEAP_FLAG_SHARED) and returns NT
//      handle values already duplicated into the game process. A Vulkan client (DXVK)
//      imports them with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT.
//   The helper always creates both fences (D3D12_FENCE_FLAG_SHARED) and duplicates them
//   into the game. D3D11 opens them with ID3D11Device5::OpenSharedFence, Vulkan imports
//   them as timeline semaphores (VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT).
//   The game owns the handles it receives and closes them on reconfigure or shutdown.
//
// Per frame n
//   n is the fence value for the frame. It starts at 1 after every CONFIGURE (the fences
//   are new) and must increase with each FRAME; the helper rejects 0 or a value that is
//   not greater than the last one with DSF_DLSS5_ERR_PROTOCOL, because a fence (and a
//   Vulkan timeline semaphore) can never go backwards. Skipping values is allowed.
//
//   game:   write Color, Depth, Motion (and Mask if used) for frame n, GPU-signal
//           fence_in = n, and FLUSH that signal to the GPU (D3D11 Flush, vkQueueSubmit)
//           before sending FRAME{n}. Otherwise the helper waits for a signal that is
//           still sitting in the game's command buffer.
//   helper: GPU-wait fence_in >= n, run DLSS 5 into Output, GPU-signal fence_out = n,
//           and send FRAME_ACK{n} once its GPU work for n has finished. The ACK therefore
//           means fence_out >= n has been reached.
//   game:   read FRAME_ACK and check its status BEFORE queueing a GPU wait on fence_out.
//           On every error the helper still moves fence_out to n (from the CPU if no GPU
//           work will), as long as the fences exist: they live from a successful
//           CONFIGURE until the next CONFIGURE. A rejected frame number (ERR_PROTOCOL)
//           is not signalled. After ERR_TIMEOUT, ERR_NOT_CONFIGURED or ERR_DEVICE the
//           game drops the frame and sends CONFIGURE again.
//
//   Timeout: if fence_in = n does not arrive within about 2 s, the helper signals
//   fence_in = n itself so its queue drains, answers DSF_DLSS5_ERR_TIMEOUT and becomes
//   unconfigured until the next CONFIGURE. From then on the game must treat fence_in = n
//   as consumed: it must not signal n (or anything <= n) on that fence again, because a
//   late signal would not be greater than the fence's value, which is invalid for a
//   Vulkan timeline semaphore. In practice: drop both fences, send CONFIGURE, and use the
//   new fences from 1. The game should likewise give up on a missing ACK after a few
//   seconds (see dsf_dlss5_pipe.h for reads with a timeout).
//
//   Every slot is single-buffered. The helper reads the inputs and writes Output until
//   fence_out = n, so the game must not write the inputs for n+1, or read Output for n,
//   before fence_out >= n. In practice: the writes for n+1 are queued after the GPU wait
//   on fence_out >= n. A frame-lagged game that wants to render n+1 while the helper
//   works on n must copy its inputs into the shared slots only after that wait.
//
// Input conventions (research/dlss5-helper.md has the sources)
//   Color   display-encoded (sRGB) SDR in [0,1], taken before the HUD is drawn. Or, as RTX
//           Remix and DSF Enhanced's Input=1 do it, the HDR scene before post-processing,
//           tone-mapped (exposure, ACES fit, 1/2.2 gamma) into R16G16B16A16_FLOAT; Output in the
//           same format then goes back through the inverse. Any colour format CONFIGURE names.
//   Depth   hardware depth z/w, 0 = near and 1 = far (set DSF_DLSS5_CONFIG_DEPTH_INVERTED
//           for reversed Z). R32_FLOAT.
//   Motion  previous minus current screen position of the surface, in pixels, +x right,
//           +y down, without jitter. Multiplied by mv_scale (so 1,1 for pixel units, or
//           width,height for UV units). R16G16_FLOAT.
//   Mask    optional. R = how much DLSS 5 applies (1 everywhere = full effect). DLAA has none.
//   Jitter  the sub-pixel offset applied to the projection this frame, in pixels: pixel
//           (x, y) was sampled at (x + 0.5 - jitter_x, y + 0.5 - jitter_y). 0,0 when the
//           game does not jitter. DLSS 5 ignores it; DLAA needs it (Halton 2,3 - 0.5).
//   DLAA    Color and Output linear HDR, R16G16B16A16_FLOAT by default; Depth and Motion as
//           above (Motion without jitter). The feature is created with IsHDR, MVLowRes and
//           AutoExposure, and without MVJittered and DepthInverted.
//
// All structs are naturally aligned with explicit padding so the x86 and x64 layouts
// match; the static_asserts below pin the sizes.

#pragma once

#include <stdint.h>

#define DSF_DLSS5_MAGIC 0x35465344u  // "DSF5"
#define DSF_DLSS5_VERSION 2u
#define DSF_DLSS5_PIPE_FMT L"\\\\.\\pipe\\dsf-dlss5.%lu"  // %lu = game process id

enum DsfDlss5MessageType {
  DSF_DLSS5_MSG_HELLO = 1,
  DSF_DLSS5_MSG_HELLO_ACK = 2,
  DSF_DLSS5_MSG_CONFIGURE = 3,
  DSF_DLSS5_MSG_CONFIGURE_ACK = 4,
  DSF_DLSS5_MSG_FRAME = 5,
  DSF_DLSS5_MSG_FRAME_ACK = 6,
  DSF_DLSS5_MSG_SHUTDOWN = 7,
  DSF_DLSS5_MSG_ERROR = 8,  // helper -> game, DsfDlss5Error, then the helper disconnects
};

enum DsfDlss5Slot {
  DSF_DLSS5_SLOT_COLOR = 0,
  DSF_DLSS5_SLOT_OUTPUT = 1,
  DSF_DLSS5_SLOT_DEPTH = 2,
  DSF_DLSS5_SLOT_MOTION = 3,
  DSF_DLSS5_SLOT_MASK = 4,
  DSF_DLSS5_SLOT_COUNT = 5,
};

enum DsfDlss5Feature {
  DSF_DLSS5_FEATURE_NR = 0,    // DLSS 5 (DLSSNR)
  DSF_DLSS5_FEATURE_DLAA = 1,  // DLSS Super Resolution at 1.0
  DSF_DLSS5_FEATURE_COUNT = 2,
};
#define DSF_DLSS5_FEATURE_BIT(feature) (1u << (feature))

enum DsfDlss5ClientApi {
  DSF_DLSS5_API_D3D11 = 1,
  DSF_DLSS5_API_VULKAN = 2,
  DSF_DLSS5_API_D3D12 = 3,
};

enum DsfDlss5Status {
  DSF_DLSS5_OK = 0,
  DSF_DLSS5_ERR_PROTOCOL = 1,      // bad magic, version, size or message order; bad frame number
  DSF_DLSS5_ERR_RUNTIME = 2,       // the feature's DLL missing, unsigned or too old; or no such feature
  DSF_DLSS5_ERR_DEVICE = 3,        // no D3D12 device on the requested adapter
  DSF_DLSS5_ERR_NGX = 4,           // NGX init, feature support (this GPU), create or evaluate failed
  DSF_DLSS5_ERR_RESOURCES = 5,     // shared texture or fence creation/open failed
  DSF_DLSS5_ERR_NOT_CONFIGURED = 6,
  DSF_DLSS5_ERR_TIMEOUT = 7,       // fence_in = n never arrived; send CONFIGURE again
};

// CONFIGURE flags
#define DSF_DLSS5_CONFIG_CLIENT_CREATES 0x1u  // textures[] hold the game's own NT handles
#define DSF_DLSS5_CONFIG_DEPTH_INVERTED 0x2u  // reversed Z: 1 = near
#define DSF_DLSS5_CONFIG_USE_MASK 0x4u        // the game writes the ControlMask slot

// FRAME flags
#define DSF_DLSS5_FRAME_RESET 0x1u          // camera cut, discard history
#define DSF_DLSS5_FRAME_AUTO_MASK 0x2u      // let the runtime's character mask combine (Remix default)
#define DSF_DLSS5_FRAME_GPU_TIME 0x4u       // report the evaluate's GPU time in FRAME_ACK::gpu_ms
#define DSF_DLSS5_FRAME_BYPASS 0x8u         // copy Color to Output without the feature (A/B)

typedef struct DsfDlss5Header {
  uint32_t magic;
  uint16_t type;     // DsfDlss5MessageType
  uint16_t version;  // DSF_DLSS5_VERSION
  uint32_t size;     // payload bytes that follow
  uint32_t reserved;
} DsfDlss5Header;

typedef struct DsfDlss5Hello {
  uint32_t client_pid;
  uint32_t client_api;     // DsfDlss5ClientApi
  uint32_t adapter_luid_low;
  int32_t adapter_luid_high;  // 0,0 = any NVIDIA adapter
} DsfDlss5Hello;

typedef struct DsfDlss5HelloAck {
  int32_t status;  // DsfDlss5Status: OK when at least one feature is available
  uint32_t helper_pid;
  uint32_t ngx_result;  // NVSDK_NGX_Result of the failing call, 1 = success
  uint16_t runtime_version[4];  // nvngx_dlssnr.dll (DLSS 5); zero when it is not there
  char runtime_sha256[68];      // lowercase hex, NUL-terminated
  char message[256];
  uint32_t features;                 // DSF_DLSS5_FEATURE_BIT() of each feature ready to CONFIGURE
  uint16_t dlaa_runtime_version[4];  // nvngx_dlss.dll; zero when it is not there
  uint32_t reserved;
} DsfDlss5HelloAck;

typedef struct DsfDlss5Configure {
  uint32_t width;
  uint32_t height;
  uint32_t formats[DSF_DLSS5_SLOT_COUNT];  // DXGI_FORMAT per slot; 0 = default
  uint32_t flags;                          // DSF_DLSS5_CONFIG_*
  int32_t preset;                          // NR: DLSSNR.Hint.Render.Preset, 2 (B) like Remix.
                                           // DLAA: DLSS.Hint.Render.Preset.DLAA, 11 (K) or 12 (L); 0 = K
  uint32_t feature;                        // DsfDlss5Feature (version 1: reserved, 0)
  uint64_t textures[DSF_DLSS5_SLOT_COUNT];  // CLIENT_CREATES: NT handle values in the game
} DsfDlss5Configure;

typedef struct DsfDlss5ConfigureAck {
  int32_t status;
  uint32_t ngx_result;
  uint32_t formats[DSF_DLSS5_SLOT_COUNT];   // the formats actually used
  uint32_t reserved;
  uint64_t textures[DSF_DLSS5_SLOT_COUNT];  // helper-created: NT handle values in the game
  uint64_t fence_in;                        // NT handle values in the game
  uint64_t fence_out;
  char message[256];
} DsfDlss5ConfigureAck;

typedef struct DsfDlss5Frame {
  uint64_t frame;  // fence value of the feature's fences, 1, 2, 3, ...
  uint32_t flags;  // DSF_DLSS5_FRAME_*
  uint32_t style;  // DLSS 5 model 0..2
  float jitter[2];
  float mv_scale[2];
  float intensity;           // 0..1, Remix default 1.0 (this and the three below: NR only)
  float tone_strength;       // 0..1, Remix default 0.3
  float structure_strength;  // 0..1, Remix default 0.7
  float skin_strength;       // 0..1, Remix default 0.5 (only with AUTO_MASK)
  uint32_t feature;          // DsfDlss5Feature
  uint32_t reserved;
} DsfDlss5Frame;

typedef struct DsfDlss5Error {
  int32_t status;
  char message[124];
} DsfDlss5Error;

typedef struct DsfDlss5FrameAck {
  uint64_t frame;
  int32_t status;
  uint32_t ngx_result;
  float gpu_ms;  // GPU time of the evaluate with DSF_DLSS5_FRAME_GPU_TIME, else -1
  uint32_t feature;  // the FRAME's feature
} DsfDlss5FrameAck;

#ifdef __cplusplus
static_assert(sizeof(DsfDlss5Header) == 16, "protocol layout");
static_assert(sizeof(DsfDlss5Hello) == 16, "protocol layout");
static_assert(sizeof(DsfDlss5HelloAck) == 360, "protocol layout");
static_assert(sizeof(DsfDlss5Configure) == 80, "protocol layout");
static_assert(sizeof(DsfDlss5ConfigureAck) == 344, "protocol layout");
static_assert(sizeof(DsfDlss5Frame) == 56, "protocol layout");
static_assert(sizeof(DsfDlss5FrameAck) == 24, "protocol layout");
static_assert(sizeof(DsfDlss5Error) == 128, "protocol layout");
#endif
