# DLSS 5 for Driver: San Francisco: the helper (phase 0)

Date: 2026-10-29. Hardware for every test below: RTX 5070 Ti, driver 621.01, Windows 11.

## Summary

- DLSS 5 is NGX feature 18, "DLSSNR". It can be created and evaluated through the normal, documented NGX path. The public DLSS SDK's static loader finds the driver's NGX core, and the core loads and signature-checks the user's `nvngx_dlssnr.dll`. No caller-identity tricks or direct loading of the feature DLL are needed.
- The signed `nvngx_dlssnr.dll` 310.9.16.0 from RTX Remix 1.6.1 runs on the RTX 5070 Ti under a project ID of our own. A 1280×720 evaluate takes about 1.9 ms of GPU time.
- We built our own helper and protocol rather than plugging into DLSS5-Feeder or adopting DLSS5-ReShade-AIO or RenoDX's add-on. The reasons are in "Architecture decision".
- The whole cross-process chain works offscreen: a 32-bit D3D11 client, a named pipe, shared textures and fences, the 64-bit D3D12 helper, DLSS 5, and back. 48/48 frames came back, and the output matches the in-process run.
- Measured on this runtime: it uses motion vectors, but it never reads jitter, and depth had no effect on the test scene.

## Sources

| Source | Licence | How it was used |
| --- | --- | --- |
| dxvk-remix, commit `0ba9973f` (2026-10-28): `src/dxvk/rtx_render/rtx_ngx_wrapper.{h,cpp}`, `rtx_dlss_neural_rendering.{h,cpp}`, `rtx_resources.cpp`, `rtx_context.cpp`, `rtx_camera.cpp`, `shaders/rtx/...` | zlib (DXVK) / MIT (NVIDIA files) | The reference integration: parameters, values, formats, conventions, pipeline position. Read, not copied. |
| dxvk-remix `packman-external.xml` → package `rtx-remix-ngx_sdk_dlnr@2` (public CloudFront) | NVIDIA RTX SDKs license, headers marked `LicenseRef-NvidiaProprietary` | Contains `nvsdk_ngx_defs_dlssnr.h` and `nvsdk_ngx_helpers_dlssnr_vk.h`. Read only to cross-check names and types. Nothing from it is in this repository. |
| NVIDIA DLSS SDK 310.9.16 (github.com/NVIDIA/DLSS, commit `9981c635`) | NVIDIA RTX SDKs license | `nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h`, `nvsdk_ngx_s.lib`, `LICENSE.txt`, downloaded at build time by `dlss5/fetch-sdk.ps1` from that pinned commit, each checked against a SHA-256, and gitignored. |
| RTX Remix 1.6.1 release zip | NVIDIA | Source of the signed `nvngx_dlssnr.dll`. Kept in a scratch folder, never committed. |
| DLSS5-Feeder, commit `16cb1946` | MIT | Studied its protocol and host. |
| DLSS5-ReShade-AIO (kibblerz), commit `09301f55` | Apache-2.0 (LICENSE, NOTICE confirmed) | Studied. |
| FoveatedDLSS5 (maximedgoulet), commit `4fcb312d`, `src/addons/dlss5` = RenoDX DLSS 5 add-on v7.5.0-rc5 | MIT | Studied. |
| OptiScaler forks with DLSS-NR | GPL-3.0 | Not read for code. Nothing taken. |
| NIGos/dlss5-bridge | MIT, per the Feeder's notice | Not reviewed separately. The Feeder includes parts of it. |

## The NGX DLSS 5 interface

### Feature, creation, evaluation

The calls are the generic NGX ones. Remix uses the Vulkan variants and our helper uses D3D12.

1. `NVSDK_NGX_<API>_Init` (Remix) or `NVSDK_NGX_D3D12_Init_with_ProjectID` (ours). Ours passes project ID `9cad904d-8216-4482-b2e7-7c2ccba2973e`, engine type `CUSTOM`, engine version `1.0`, and a `NVSDK_NGX_FeatureCommonInfo` whose `PathListInfo` names the folder containing `nvngx_dlssnr.dll`. The NGX log confirms the core then loads the feature from there: `feature dlssnr snippet: ...\out\ngx/nvngx_dlssnr.dll version: 310.9.16`.
2. `NVSDK_NGX_<API>_GetCapabilityParameters`, then check `DLSSNR.NeedsUpdatedDriver` (int; if set, `DLSSNR.MinDriverVersionMajor/Minor`), `DLSSNR.Available` (int) and `DLSSNR.FeatureInitResult`. This mirrors Remix's `checkDlssNeuralRenderingSupport`.
3. Create the feature with id 18 (`NVSDK_NGX_Feature_Reserved18` in the public SDK, aliased `NVSDK_NGX_Feature_DLSSNR` in Remix's header). Set these parameters first:
   - `DLSSNR.Width` and `DLSSNR.Height` (unsigned int). The feature works at one size; NR is not an upscaler.
   - `DLSSNR.Hint.Render.Preset` (int): Default 0, A 1, B 2, C 3, … G 7. Remix always uses B (2), "the lightweight network, independent of the appearance model".
   - Optional `DLSSNR.ScalingRatio` (float).
4. Each frame, set the parameters below and call `NVSDK_NGX_<API>_EvaluateFeature(cmdList, handle, params)`. The work is recorded into the caller's command list.
5. Tear down with `ReleaseFeature`, `DestroyParameters` and `Shutdown1`.

The runtime also exposes two parameter callbacks: `DLSSNRGetStatsCallback` (VRAM use, which Remix shows) and `DLSSNRComputeScalingRatioCallback`.

Evaluate parameters (names from the runtime's string table and the Remix header; types from Remix's helper):

| Parameter | Type | Remix value / meaning |
| --- | --- | --- |
| `DLSSNR.Color` | resource | Input colour |
| `DLSSNR.Output` | resource (UAV) | Output, same size |
| `DLSSNR.MVec` | resource | Motion vectors |
| `DLSSNR.Depth` | resource | Depth |
| `DLSSNR.ControlMask` | resource | Per-pixel control. Remix's header comment: R = intensity, G = local tone strength, B = local structure strength, A unused. Remix clears it to 1 and lowers it under fog, volumetrics and alpha-blended surfaces. |
| `DLSSNR.<Color/Output/MVec/Depth/ControlMask>Subrect{BaseX,BaseY,Width,Height}` | unsigned int | Full texture in Remix |
| `DLSSNR.Reset` | int | 1 on history reset |
| `DLSSNR.DepthInverted` | int | 0 |
| `DLSSNR.Enabled` | int | 1 |
| `DLSSNR.UICorrection` | int | Not set by Remix (0) |
| `DLSSNR.Intensity` | float 0..1 | 1.0 |
| `DLSSNR.LocalToneStrength` | float 0..1 | 0.3 |
| `DLSSNR.LocalStructureStrength` | float 0..1 | 0.7 |
| `DLSSNR.Style` | unsigned int | Model A/B/C = 0/1/2 |
| `DLSSNR.UseAutoMask` | int | 1 (the runtime's automatic character mask) |
| `DLSSNR.MaskBlendMode` | int | None 0, Mean 1, Min 2, Max 3. Remix uses Min with the auto mask and None without it. |
| `DLSSNR.SkinStructureStrength` | float 0..1 | 0.5 with the auto mask, else 0 |
| `DLSSNR.MVecScaleX/Y` | float | 1, 1 (motion in pixels) |
| `Jitter.Offset.X/Y` | float | Remix sets these, but see the findings: this runtime never reads them. |
| Optional: `DLSSNR.Backbuffer`, `DLSSNR.UI`, `DLSSNR.UIAlpha`, `DLSSNR.BidirectionalDistortionField` (+ subrects) | resource | For end-of-pipeline use (after the UI). Remix passes none. |

D3D12 resources are passed as `ID3D12Resource*`: inputs in `NON_PIXEL_SHADER_RESOURCE`, output in `UNORDERED_ACCESS`. Vulkan passes `NVSDK_NGX_Resource_VK` (image view, image, subresource range, format, extent, UAV flag).

### API variants

The signed 310.9.16 DLL exports `NVSDK_NGX_{D3D11,D3D12,VULKAN,CUDA}_{Init,Init_Ext,CreateFeature,EvaluateFeature,ReleaseFeature,Shutdown1,PopulateParameters_Impl,...}`, and the driver's `_nvngx.dll` (32.0.16.2101) exports the matching core API. Remix only exercises Vulkan. We use D3D12 and it works. D3D11 is exported but untested.

### Inputs as Remix produces them

- **Where:** after upscaling (DLSS/DLAA/TAA), before bloom, motion blur and the final tone mapping. Remix tone-maps to SDR first (`dispatchFastToneMapping`: exposure, ACES fit, `linearToGamma`), runs NR, then inverse-tone-maps back to HDR (`rtx_context.cpp`, "DLSS-NR operates in SDR space").
- **Colour:** `R16G16B16A16_SFLOAT` at output resolution, holding display-encoded SDR in [0,1]. We also tested `R8G8B8A8_UNORM` input. It works, and the output differs from the f16-input run by at most 5/255 (mean 0.31/255), which is the 8-bit quantization.
- **Output:** `R16G16B16A16_SFLOAT` at output resolution. `R8G8B8A8_UNORM` also works (cross-process test).
- **Motion vectors:** `R16G16_SFLOAT` at render resolution: previous minus current position in pixels, +x right, +y down (`cameraCalcMotionVectorPixel`: `(prevNDC - currNDC) * (0.5, -0.5) * resolution`), computed from the unjittered pixel centre. Sky pixels reproject as a direction. Scale 1, 1.
- **Depth:** z/w of the jittered projection, 0 near, 1 far, `DepthInverted = 0` (`projectVirtualHit`).
- **Control mask:** `R8G8B8A8_UNORM` at render resolution, cleared to 1.
- **Jitter:** Halton (2,3) − 0.5 in pixels (`RtCamera::calcPixelJitter`). It is applied to the projection so that the image shifts by +jitter pixels (y down), meaning pixel (x, y) is sampled at (x + 0.5 − jx, y + 0.5 − jy).

## Findings from our runs

All runs used `dsf-dlss5-offscreen` and `dsf-dlss5-client-test` in this branch, with `nvngx_dlssnr.dll` 310.9.16.0 (SHA-256 `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f`, Authenticode valid, signer "NVIDIA Corporation", DigiCert CA). The copy in the Remix packman package is byte-identical.

| Test | Result |
| --- | --- |
| Synthetic street, 1280×720, 48 frames, moving camera, f16 input | 48/48 evaluates OK. Median 1.88 ms GPU, max 2.68 ms. Visible but small change on flat-shaded content (mean max-channel difference 2.4/255). Images: `research/images/dlss5/synthetic-{input,output}.png`. |
| Driver SF screenshot (split-screen free drive, 1286×749), still, zero MV, flat depth, styles 0/1/2 | 16/16 each. Median ≈2.0–2.15 ms. 24 %, 35 % and 25 % of pixels changed by more than 8/255: clear relighting of the car body, ground and building. The HUD was processed too, which is why the game must hand over a pre-HUD image. Kept locally, not committed (game imagery): `out\dlss5-game-style{0,1,2}\compare_last.png`. |
| Cross-process: x86 D3D11 client → helper, game-created textures | 48/48. Helper GPU time 1.98 ms. Round trip including CPU uploads and readback averages 7.7 ms. |
| Cross-process, helper-created textures | 48/48. Output bit-identical to the game-created run. |
| Cross-process vs in-process (same u8 input) | Max difference 6/255, mean 0.21/255 (output format rounding). |
| Jitter passed as +1×, −1× and 0× on a static jittered camera | Bit-identical outputs. The runtime has no "jitter" string anywhere, so it never reads `Jitter.Offset`. With jitter, the output's frame-to-frame change was 1.41/255, against 0.29/255 without jitter. **NR does not remove jitter; feed it an unjittered image.** |
| Motion vectors zeroed (moving camera) | Output changes (max 9/255, mean 0.42/255), so MVs are used. |
| Depth replaced by a constant 0.5 | Bit-identical output on this scene. Depth had no measurable effect here. Remix still passes it and so do we. Whether the model uses it on other content (characters, for example) is open. |
| D3D11 opening a D3D12-created shared texture | `E_INVALIDARG` unless the D3D12 texture has `ALLOW_RENDER_TARGET \| ALLOW_SIMULTANEOUS_ACCESS`. With both flags it opens, NGX accepts it, and the output is bit-identical. |
| Failure paths (`dsf-dlss5-client-test --robustness`) | A repeated frame number and frame 0 get `ERR_PROTOCOL`. A FRAME whose `fence_in` is never signalled gets `ERR_TIMEOUT` after about 2.0 s, with `fence_out` already at that frame. The next FRAME gets `ERR_NOT_CONFIGURED` and `fence_out` still advances. A new CONFIGURE then works again. A header with the wrong version gets `MSG_ERROR(ERR_PROTOCOL)` before the helper exits. |
| Runtime checks | A file with an unknown SHA-256 is refused. An NVIDIA-signed but different DLL (the driver's `nvngx_dlssd.dll` renamed) is refused by the hash allowlist. With its hash allowed it is still refused by the version floor (310.9.0 < 310.9.16). The verified file stays open with write and delete sharing denied until NGX shuts down, and after CreateFeature the helper checks that the module NGX mapped is that same file (volume and file id). |

## Architecture decision

Options considered:

1. **A drop-in consumer for DLSS5-Feeder.** The Feeder links the public NGX SDK the normal way, but its 64-bit host only evaluates DLSS Super Resolution/DLAA (feature 1). DLSS 5 then comes from a separate "neural consumer" add-on (closed ones, or RenoDX's) that hooks that DLSS evaluate inside the host's ReShade and injects feature 18. A drop-in consumer would therefore be a hook on a DLSS SR evaluate in someone else's process. It needs `nvngx_dlss.dll` as a carrier, a 64-bit ReShade in the host, and the Feeder's IPC, which is at v11 and still changing. It would also inherit motion estimated from colour rather than the game's real motion vectors. All of that is overhead for a game where we control both ends.
2. **DLSS5-ReShade-AIO** (Apache-2.0; the README claims checked against LICENSE/NOTICE/source). It is open and covers NR + SR/DLAA + FG, a 32-bit D3D9 path through ReShade's D3D10.1 bridge with an x64 carrier, NVOF motion, and no shipped NVIDIA DLLs, all as claimed. However, its NGX code loads `nvngx_dlssnr.dll` directly and calls the feature's own `NVSDK_NGX_D3D12_Init_Ext`. It routes that call through its own `nvngx.dll` because, as its comment says, the feature "verifies that Init_Ext was called from a module whose filename contains 'nvngx.dll'". That works around an NVIDIA caller check (the SDK license forbids circumventing such mechanisms), and the code was written against a pre-release 310.8 runtime. Its pipeline takes ReShade frames and NVOF motion, has no input for external motion vectors or jitter (a VORT shader feed is the closest), and deliberately leaves the ControlMask unused.
3. **RenoDX's DLSS 5 add-on** (the `src/addons/dlss5` copy in FoveatedDLSS5). It is MIT and open, and it does contain working NGX DLSS 5 call code, so the earlier "closed" label was wrong for v7.5.0-rc5. But it runs NR "around a game's existing DLSS evaluation" by hooking NGX, so Driver SF (no DLSS) gives it nothing to hook. It also loads the 310.8 runtime directly and fakes its caller identity by patching the runtime's `GetModuleFileNameW` import to report `nvngx.dll`.
4. **Our own helper and protocol (chosen).** The game side (DSF Enhanced) owns the inputs: real camera motion vectors, a pre-HUD colour capture, and depth. The helper creates feature 18 itself through the official NGX path with a signed, publicly released runtime. That makes the whole chain open source (MIT) without bypassing anything. It is also small: a fixed-size protocol and about 2,700 lines including the tests. Nothing from the projects above was copied. From the Feeder we kept the general shape that it showed works across processes: a named pipe for control, shared textures plus two shared fences for data, and the D3D11 → D3D12 texture direction.

## Protocol and what the game side must provide

`dlss5/include/dsf_dlss5_protocol.h` is the contract. In short:

- DSF Enhanced starts `dsf-dlss5-helper.exe --game-pid <pid> --game-process <inherited handle>` (the inherited process handle avoids needing `OpenProcess` rights). It then connects to `\\.\pipe\dsf-dlss5.<pid>` and sends HELLO with the adapter LUID.
- CONFIGURE with the size and formats. A D3D11 game side (dgVoodoo2) may create the textures itself with `SHARED | SHARED_NTHANDLE` and send its handle values. Otherwise the helper creates them and returns NT handles duplicated into the game. That is the path for DXVK, which imports them with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`. Fences always come from the helper: D3D11 `OpenSharedFence`, or a Vulkan timeline semaphore imported as `D3D12_FENCE`.
- Per frame n: write the inputs, GPU-signal `fence_in = n` and **flush it to the GPU**, send FRAME (flags, model, strengths, jitter, MV scale), read FRAME_ACK and **check its status before** queueing the GPU wait on `fence_out = n`, then composite Output back before the HUD draws.
- Frame numbers restart at 1 after every CONFIGURE and must increase (gaps are fine). 0 or a non-increasing value is refused with `ERR_PROTOCOL`, because the fences, and the Vulkan timeline semaphores on the DXVK side, cannot go backwards.
- Every slot is single-buffered. The helper reads the inputs and writes Output until `fence_out = n`. So the inputs for n+1 may only be written, and Output for n only read, after `fence_out >= n`. A frame-lagged game copies its next inputs into the shared slots after that wait.
- Failures: the helper waits about 2 s for `fence_in`. If the signal never comes, it signals `fence_in` itself so its queue drains, answers `ERR_TIMEOUT`, and needs a new CONFIGURE. On every error path where the fence still exists, the helper moves `fence_out` to n from the CPU, so a GPU wait the game already queued cannot hang. The game should also give up on a missing ACK after a few seconds: `dsf_dlss5_pipe.h` reads and writes with a timeout on an overlapped pipe handle. A malformed message gets `MSG_ERROR` and ends the connection. Only the game's process id may connect to the pipe.

Inputs DSF Enhanced has to produce:

| Slot | Format (default) | Content |
| --- | --- | --- |
| Color | `R8G8B8A8_UNORM` (any UNORM/float colour format works) | The frame **before the HUD**, display-encoded SDR as the game presents it |
| Depth | `R32_FLOAT` | Hardware depth (INTZ or a resolve), 0 = near unless `CONFIG_DEPTH_INVERTED` |
| Motion | `R16G16_FLOAT` | Previous minus current screen position in pixels (+x right, +y down), unjittered: from current depth and the current and previous view-projection matrices for the static world, plus per-object motion for cars where available |
| Mask (optional) | `R8G8B8A8_UNORM` | R = how much DLSS 5 applies, for example 0 on HUD-like world elements or overlays the game cannot separate. Without it the helper uses a mask of ones with the runtime's auto mask (Remix's default). |
| Jitter | FRAME field | Pass the projection jitter if the game ever jitters. It is forwarded but **this runtime ignores it**, so do not add jitter for DLSS 5's sake. |

Timing. A synchronous mid-frame round trip costs one helper evaluate (about 2 ms at 720p on the 5070 Ti, more at higher resolutions) plus the copies, and that time lands on the game's frame. The alternative, compositing frame n−1's result, adds a frame of latency between the world and the HUD. The protocol supports both; DSF Enhanced chooses.

Under DXVK, the D3D9 interop interface (`ID3D9VkInteropDevice`: Vulkan handles of D3D9 resources and access to DXVK's submission queue) is the likely way to copy into and out of the imported images and to wait on and signal the timeline semaphores. Under dgVoodoo2 (D3D9 → D3D11), the D3D11 path here is already tested. Getting at dgVoodoo's D3D11 device and resources from a D3D9 hook is the open part.

## Licensing notes

- Everything committed is MIT. That includes `dlss5/src/dlssnr_defs.h`, which states interface facts: feature id 18; the parameter names, which the runtime itself contains as strings and which the MIT RenoDX source also uses (Remix's MIT-licensed NVIDIA files show the identifiers but not the strings); and the numbers behind the preset and mask-blend names. No open source states those numbers, so they were taken as bare values from the proprietary `nvsdk_ngx_defs_dlssnr.h` in Remix's public packman package. The preset numbering matches the public SDK's DLSS presets. No NVIDIA header text is included.
- The build downloads the NVIDIA DLSS SDK headers and `nvsdk_ngx_s.lib` at build time (RTX SDKs license) into a gitignored folder, from a pinned commit with a SHA-256 check per file. A built helper contains NVIDIA's static loader in object form, which section 1(c)/2 of that license allows inside an application with material additional functionality. Any binary release must carry that notice.
- No NVIDIA DLL is committed, packaged or downloaded by our scripts. Users fetch `nvngx_dlssnr.dll` from the RTX Remix release themselves (steps in `dlss5/README.md`). The helper only accepts a runtime whose SHA-256 is on its allowlist (today only the Remix 1.6.1 file), or one the user allowed with `--allow-runtime-sha256`. Either way the file must also be NVIDIA-signed and at least 310.9.16.
- Unlike AIO and RenoDX's add-on, we do not load the feature DLL directly or alter what NVIDIA's checks see. The driver's NGX core loads and verifies it.
- The NVIDIA RTX SDKs license supplement asks for notification before a *commercial* release. This project is not commercial.

## Host runs

None. The offscreen tests ran on the host GPU with no window and no game. No in-game run was needed for phase 0, because DSF Enhanced does not yet feed the helper.

## Open

- Game side (the DSF Enhanced owner): pre-HUD capture point, depth access, MV generation from the renderer's matrices, and the DXVK interop path. Then the first in-game run on the host through the driversf-forza harness (`-OnHost`), with the helper and runtime placed via `-Override`.
- Whether depth matters on other content, the effect of the ControlMask channels, and the optional UI/Backbuffer inputs, measured on real game captures. Add `--depth`/`--mv` PFM dumps from the game side to the offscreen test.
- Performance at 1080p/1440p/4K and with the frame-lagged composite.
- Run the helper's `ReleaseFeature`/`Shutdown` paths under a resolution-change stress test.
- If NVIDIA starts shipping DLSSNR through NGX's own update channel (the `C:\ProgramData\NVIDIA\NGX\models` cache), NGX may prefer that copy. The helper will then refuse to run, because the mapped module is not the verified file. Supporting that case would mean verifying the cached file the same way.
- The D3D11 NGX path (exported, untested) would let a 64-bit D3D11 helper skip D3D12. Not needed now.
