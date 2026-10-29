# DLAA for Driver SF: research and design

Status 2026-10-29: research only, no code yet. DLAA here is NVIDIA DLSS Super Resolution (NGX feature
`NVSDK_NGX_Feature_SuperSampling`) at a render scale of 1.0, run by the existing 64-bit helper on the
game's HDR scene target, the same point the parallel `feat/dlss5-hdr-input` work uses for DLSS 5.

Evidence levels as in [dlss5-renderer.md](dlss5-renderer.md): *runtime* (seen in a log or capture),
*static* (disassembly, shader bytecode, headers), *inferred*, *estimate*. No VM or game run was made
for this note (the PC was needed for other work). The checks that need one are listed under "Follow-up
checks".

## Summary

1. **The game's AA.** `AntiAliasing` is a boolean: any non-zero value is "on", so 2 is the same as 1. It
   only sets the swap chain's `MultiSampleType` to 4x MSAA (2x if 4x is unsupported). The 3D scene is
   never multisampled: it renders into an fp16 texture with non-MSAA depth, and the swap chain only
   receives a `StretchRect` of the finished frame. So that MSAA does nothing visible. The anti-aliasing
   you can see is **MLAA** (Jimenez-style morphological AA) in the post chain: the two unidentified
   passes `T#28`/`T#29` from the renderer notes, plus the neighbourhood blend inside the composite. It
   runs once per frame on the tone-mapped 8-bit image, after the film grain. It has no sub-pixel or
   temporal information, and it only searches 16 px along an edge. That is why thin geometry,
   foliage, specular highlights and long shallow edges stay aliased and crawl in motion.
2. **Jitter.** Jitter only the draws in the scene pass (render-queue target set 35, `T#3`). The scene's
   vertex shaders use at least five constant layouts (table below), including separate view and
   projection matrices for particles and some character shaders. Recommended method: patch the vertex
   shader bytecode at creation, so that `oPos.xy += J.xy * oPos.w` with `J` in a free constant that is
   set per draw. This covers every layout without touching the constants the game uploads, so the
   view-projection solve and the motion vectors stay unjittered. Use Halton (2,3) − 0.5, at least 8
   phases at DLAA (NVIDIA's table), in render pixels, in the motion vectors' convention (+x right, +y
   down).
3. **NGX.** Create SuperSampling with input = output = scene size, `PerfQualityValue_DLAA`, and flags
   `IsHDR | MVLowRes | AutoExposure` (`MVJittered = 0`, `DepthInverted = 0`). Set
   `DLSS.Hint.Render.Preset.DLAA` = preset K (11), which the 310.9.16 headers make the default for
   DLAA; preset L is an option. Use the SDK's `NGX_D3D12_CREATE_DLSS_EXT` and
   `NGX_D3D12_EVALUATE_DLSS_EXT`. Protocol v2 adds a feature field to CONFIGURE and FRAME and reuses the
   same five slots. DLAA and DLSS 5 can run in sequence (DLAA first) and should. DLSS 5 ignores jitter,
   so it must only ever see a resolved image.
4. **Runtime.** `nvngx_dlss.dll` 310.9.16.0 from github.com/NVIDIA/DLSS (release v310.9.16, the commit
   the build already pins): 58,956,912 bytes, SHA-256 `d00b5b0a…6123e`, signed by NVIDIA Corporation.
   The licence lets an application distribute it, but not on its own. We keep our rule: users download
   it themselves, and the helper checks a SHA-256 allowlist, the signature and a version floor. The
   driver store on this PC carries no SR fallback (only `nvngx_dlssg.dll`).
5. **Costs and risks.** Estimate: preset K at 3440x1440 on the RTX 5070 Ti takes about 1.3-2 ms of GPU
   time. The synchronous round trip costs another ~1.5-2 ms of render-thread time, measured from the
   DLSS 5 bridge at that size. Ghosting is likely on wheel spin, pedestrians, sparks and smoke, scrolling
   textures and paint reflections. The game's MLAA must be skipped when DLAA is on, and the useless MSAA
   swap chain can go too. Split screen should fall back to MLAA at first. Cutscenes reuse the camera-cut
   resets.
6. **Plan.** Three PRs stacked on `feat/dlss5-hdr-input`, plus an optional fourth:
   - PR 1: game-side jitter and the AA switches, which the VM can test without NGX;
   - PR 2: helper protocol v2, the DLSS SR pass and the offscreen DLAA test;
   - PR 3: game-side DLAA at the scene end, with DLSS 5 chained after it;
   - PR 4 (optional): one FRAME for both features, and a frame-lagged mode.

   One prerequisite fix is needed first. The scene-target detector currently picks the 3072² shadow map
   in the user's session, and that map must never be jittered.

**Recommendation:** do it, and offer DLAA on its own too. It works on every RTX GPU, while DLSS 5 needs
an RTX 50. Start with PR 1 and the offline sign test, because the jitter patch and the jitter sign are
where integrations usually go wrong.

## 1. What the game's AntiAliasing setting does

### The setting (static)

- `graphics.ini` is read at `0xBAA7E0` (strings `AntiAliasing` `0xE98174`, written back with
  `AntiAliasing = %d` at `0xBAA8C1`). The value goes through `setne` into the byte at settings `+0x1E`
  (`0xBAAB7F-0xBAAB89`). Any non-zero value is stored as 1, so 0/1/2 give only off/on.
- The pause-menu option `PC_Anti_Aliasing` (`0x6FE67F`, `0x6FEAE4`) is a two-state toggle on the live
  copy at `0x124909E` (settings struct `0x1249080`).
- `0xBB2F80` applies the settings to the render device object `0x19B0CD0`. When the flag changes it
  calls `0xBAFB80`:
  - on: `[obj+0x20] = 4` if 4x MSAA is supported (else 2), and `[obj+0x24] = quality levels − 1`;
  - off: both 0;
  - either way it marks the device for a reset (`[obj+0x969] = 1`).
- The capability comes from `0xBAFC30`, which loops `IDirect3D9::CheckDeviceMultiSampleType` (vtable
  `+0x2C`) over the back-buffer format, records 2x, then 4x, and stops at 4x.
- `obj+0x10` is the device's `D3DPRESENT_PARAMETERS`. The offsets line up: `+0x20` = `MultiSampleType`,
  `+0x24` = `MultiSampleQuality`, and `+0x44` = `PresentationInterval`, which the same function sets to
  `0x80000000` (`D3DPRESENT_INTERVAL_IMMEDIATE`) when VSync is off.
- No other reader of the flag was found. Its copy at `+0x1D6` is read only by the change check
  `0xBB2FBA`. That was a targeted search of the renderer range `0xBA0000-0xBC0000`, not of the whole
  binary.

### The scene is never multisampled (runtime)

- The scene colour target `T#3` is a texture (`A16B16G16R16F`). D3D9 textures cannot be multisampled.
- DSF Enhanced swaps INTZ in only for depth requests with `MultiSampleType == NONE`
  (`dlss5_feed.inl`, `D5CreateDepthStencil`). The user's session logged both 3440x1440 depth
  surfaces as "created as INTZ" with `AntiAliasing = 1` (`dsf_enhanced.log`, 2026-10-29 23:01:00), so
  the game asks for non-MSAA depth.
- The HUD draws into `T#1`, also a texture. Only the swap chain is multisampled, and it holds nothing
  but the `StretchRect` of `T#1` (`0xBB293F`).
- The cost of that MSAA (inferred): 3440 × 1440 × 4 samples × 4 B ≈ 79 MB, plus a resolve at every
  present, for no visible change.
- Not yet seen at runtime: DXVK's log doesn't print the sample count (follow-up V5).

### The visible AA is MLAA in the post chain (static and runtime)

The renderer notes left `T#28`/`T#29` unidentified. Their pixel shaders are in `media\SfxMisc.fchunk`,
with the constant tables and line info of `R:\Resources\Shaders\Post Process\PostProcessCombinerPixel.hlsl`.
Hashes are DSF Enhanced's FNV-1a over the bytecode. Each shader is found at four offsets in that file
(four variants of the material, for example 3405C219 at `0xBA4A8`). Disassembled with `D3DDisassemble`.

| Set | Target | Pixel shader | What it does |
| --- | --- | --- | --- |
| 26 | `T#30` A8R8G8B8 | `2F0359F4` (`PostProcessing.fx`) | Tone map. It also applies bloom, colour grading, saturation, vignette and **film grain** (`g_samNoise`, `g_NoiseParams2/3`) |
| 24 | `T#28` R5G6B5 | `3405C219` | **Edge detection.** Rec. 709 luma of the saturated LDR colour, square-rooted, compared with the left, top, right and bottom neighbours against a threshold of 0.1. Writes the edge flags |
| 25 | `T#29` A8R8G8B8 | `AECA6459` | **Blending weights.** Searches each edge in 8 steps of 2 px each way (at most 16 px; it stops at an edge value below 0.9, the bilinear trick). It looks up the "Area Map" texture (33-pixel pattern stride) and checks for U shapes (`IsUShape`) |
| 0, draw 1 | `T#1` | `F5ACBC8D` | **Neighbourhood blending** inside the composite. If a pixel's four weights sum to 0, the shader copies `T#30` unchanged; otherwise it blends the neighbours |

This is the three-pass MLAA of Jimenez et al., "Practical Morphological Antialiasing" (GPU Pro 2,
2011). It ran in every traced frame, for example `host-20261029-103023` with its passes 23-26, and
every one of those runs used `AntiAliasing = 1`. Static analysis did not show whether
`AntiAliasing = 0` turns MLAA off (follow-up V1). The plan below doesn't depend on the answer.

### Why it looks bad

- **Single-sample post AA (inferred from the shader).**
  - It smooths edges that are visible in the final 8-bit image. It cannot rebuild what fell between
    pixels: thin poles, wires, railings, distant buildings, alpha-tested foliage and fences.
  - It does nothing for specular aliasing.
  - Nothing is temporal, so edges crawl and shimmer when the camera moves.
- **It runs after the film grain and vignette (static, inferred effect).** The edge detector sees an
  image with animated noise, so the detected edges and their blend weights can flicker from frame to
  frame.
- **Fixed threshold.** 0.1 on square-rooted luma misses low-contrast edges: dark scenes, fog, distant
  geometry.
- **16 px search.** At 3440 px wide, long shallow edges (roof lines, kerbs, lane markings) are longer
  than the 32 px the search can span. Only their ends get the right coverage, so steps stay visible
  along them.
- The `AntiAliasing` MSAA has no effect on the scene (above).

## 2. Jitter injection

### Which draws

| Pass (renderer notes) | Target / set | Jitter |
| --- | --- | --- |
| Scene: world, props, imposters, sky, cars, characters, particles, decals, transparents | `T#3` / 35 | **yes**, every draw that has a vertex shader |
| Sun shadow map | `T#65` 3072² / 60 | no |
| Minimap | `S#37` / 63 | no |
| Distant-scene strip (rear-view research) | `T#48` / 41, 42 | no |
| Bright pass, adaptation, bloom, tone map, MLAA | sets 2-26 | no (post) |
| Composite and HUD, including the 3D HUD meshes and the Shift edge glow | `T#1` / 0 | no |

Pre-transformed draws with no vertex shader can't be jittered. If any reach the scene pass, they stay
unjittered, which is harmless for screen-space overlays.

**Prerequisite: the scene target.** In the user's session (`dsf_enhanced.log`, 23:04:40 to 23:11:19)
the detected scene target keeps switching between `T#3` 3440x1440 and the shadow map `T#65` 3072x3072.
`D5NoteBusiestPass` accepts any target with `Width * 2 >= render width && Height * 2 >= render height`,
and at this resolution the shadow pass (~390 draws) is as busy as the scene. Jittering the shadow map
would make every shadow shimmer. Jitter has to key on render-queue set 35 (read from `0x1D4AD10`, as
the pre-HUD point does) with a target of exactly the render size. Whoever owns `dlss5_feed.inl` on
`feat/dlss5-hdr-input` should fix this. DLSS 5's own input selection has the same problem.

### The scene shaders' constant layouts (static)

Read from the vertex shaders' constant tables, in the game's shader packages and
[dlss5-renderer.md](dlss5-renderer.md):

| Shaders (package) | Clip transform | Count |
| --- | --- | --- |
| World tiles, props, gadgets (`NonCityGadgets.fchunk`), sky (`skies.sp`) | `g_ObjectToScreen` `c0-c3` (World·View·Proj rows), `g_ObjectToWorld` `c4-c6` | 13 of 28 and 36 of 72 VS in those files |
| Characters, hands (`character_global.fchunk`) | `g_boneMatrixArray` `c0-c182`, then `g_ObjectToScreen` at `c183` | 8 of 26 |
| Characters, other variants | `g_WorldToView_Column0-3` `c3-c6` and `g_ViewToProj_Column0-3` `c7-c10`: **separate view and projection, as columns** | 1 of 26 |
| Particles (`SfxParticles.fchunk`) | `g_WorldToCam` `c10-c13` and `g_CameraToScreen` `c14-c17` (offsets vary by 1-2 per shader): **separate view and projection** | 5 of 10 |
| Vehicles (`Uber2_0\Vehicles`) | `g_PanelTransforms` `c0…`, `g_mtxWorldRotationToScreen` `c96` or `c160`, `g_WorldToCam` `c103` | |
| Some terrain (`8DCC1218`) | a plain VP at `c16-c19` | |

That explains the earlier test (`host-20261029-103256`): adding `j·row3` to `c0`/`c1` moved only the
world draws, and the skinned and cockpit draws stayed put.

### Method: patch the vertex shaders' position output (recommended)

When the game calls `CreateVertexShader` (DSF Enhanced already hooks it, in `D5CreateVertexShader`),
create a patched copy of the bytecode:

1. Walk the tokens, skipping comment blocks. Find the position output: `dcl_position oN` in vs_3_0,
   or the `oPos` rasterizer output in vs_1_1/2_0. Pick a temp register `rT` that the shader never uses
   (vs_3_0 has 32, vs_2_0 has 12).
2. Pick a constant `cJ` the shader cannot reach. That rules out the constant table's ranges (including
   whole arrays used with relative addressing, such as `g_boneMatrixArray`), every `def` and every
   `c` operand. Prefer `c255`; vs_1_1 has only `c0-c95`.
3. Redirect every write to the position output into `rT`. Before the end token, append:
   `mad oPos.xy, rT.wwww, cJ.xyyy, rT.xyyy` and `mov oPos.zw, rT.zwzw`.
4. If any of that fails (no free register, relative addressing on the output, an unknown token), keep
   the original shader, log it, and count it. That draw stays unjittered.

Per draw, in `D5BeforeDraw`: if the bound vertex shader is patched, compute
`J = (2·jx/W, −2·jy/H, 0, 0)` when the draw is in set 35 on the scene target and jitter is active, and
`(0, 0, 0, 0)` otherwise. Upload `cJ` only when its value differs from the shadow copy.
`D5SetVsConstantF` already sees every constant upload, so a game upload to that register from another
shader invalidates the shadow copy.

Why this method rather than editing the matrices:

- One mechanism covers every layout, including the column-wise view/projection pairs and shaders
  nobody has traced yet.
- The constants stay exactly as the game uploaded them. The view-projection solve, the per-car
  matrices of the object motion and the traces all keep reading unjittered values with no extra
  bookkeeping.
- The cost is at most one 16-byte constant upload per draw, and nothing per pixel.
- Shader hashes, traces and `D5NoteShader` keep working on the original bytecode.

The fallback, also usable as a quick prototype for world draws only, is to edit constants per draw. The
constant table names the clip matrix of each shader (`g_ObjectToScreen`, `g_CameraToScreen`,
`g_mtxWorldRotationToScreen`, `g_ViewToProj_Column*`). Add `jx_clip·row3` and `jy_clip·row3` to its x
and y rows at draw time, and put the game's values back afterwards. That means one code path per layout.

### Motion vectors stay unjittered

- The view-projection matrix is solved from the constants as the game uploaded them. With the shader
  patch, jitter never enters them, so `VP`, `VP_prev` and the per-car `D = W_prev · W_cur⁻¹` are
  unjittered as before.
- The motion pass reprojects each pixel centre through those unjittered matrices, using depth from the
  jittered scene. That is the usual approach. The depth is sampled at most 0.5 px from where the
  unjittered surface would be, which changes the motion only by the motion's own gradient over half a
  pixel (inferred: negligible). So `MVJittered = 0`, and the jitter goes to NGX separately.
- Depth stays jittered, which is what DLSS expects.
- Camera cuts keep sending RESET (the existing cut detection), which maps to the evaluate's `InReset`.

### Sequence, scale and sign

From the DLSS Super Resolution programming guide 310.6.0 (March 2026, `doc/` in the pinned SDK commit),
section 3.7, and the 310.9.16 headers:

- **Pattern.** Halton base 2 for x and base 3 for y, minus 0.5. NVIDIA trains on Halton (3.7.1).
- **Phases.** `Base (8) × (target / render)²`. The guide's table gives a minimum of 8 for DLAA
  (3.7.1.1). Use 16 (Halton indices 1-16) for better pixel coverage at the same cost.
- **Units and range.** Render-target pixels, between −0.5 and +0.5 (3.7.3).
- **Sign.** "The same co-ordinate and direction system as motion vectors" (3.7.3, item 4): pixels, +x
  right, +y down. The protocol header already defines jitter that way: pixel (x, y) was sampled at
  (x + 0.5 − jx, y + 0.5 − jy), so the image moves by +j. RTX Remix passes the same values to DLSS
  (research/dlss5-helper.md, "Inputs as Remix produces them"). In D3D9 clip space, where y is up,
  that means `Δx_clip = +2·jx/W · w` and `Δy_clip = −2·jy/H · w`, the `J` above.
- **Checking the sign.** A wrong sign leaves static scenes fuzzy or wobbling (3.7.4). Check it offline
  first (PR 2 test) and, in a debug build only, with the SDK's development DLL, whose `CTRL+ALT+F10`
  hotkey cycles negations of the jitter (guide 8.4).
- **Mip bias.** `NativeBias + log2(render / display) − 1` = **−1.0** at DLAA (3.5). DSF Enhanced's
  `[Quality] TextureLODBias` currently applies to all samplers. With DLAA on, apply −1 only during the
  scene pass. The post shaders sample with `texldl` at an explicit LOD, so they wouldn't change, but the
  HUD would. Make it an ini value, because high-frequency textures can shimmer (3.5.1).

## 3. The NGX side

### Feature creation

| Setting | Value | Why |
| --- | --- | --- |
| `InWidth/InHeight`, `InTargetWidth/InTargetHeight` | the scene size, e.g. 3440x1440, all four equal | DLAA: render size = output size |
| `InPerfQualityValue` | `NVSDK_NGX_PerfQuality_Value_DLAA` (5) | |
| `IsHDR` | 1 | `T#3` holds linear fp16 HDR before bloom and the tone map. LDR mode would need display-encoded [0,1] values (guide 3.1.2) |
| `MVLowRes` | 1 | Motion vectors at render resolution, which here equals output resolution (3.6.2) |
| `MVJittered` | 0 | See above |
| `DepthInverted` | 0 | Standard Z, 0 near |
| `AutoExposure` | 1 | The game's eye adaptation (a 1x1 `R32F` average luminance at the end of the bright-pass chain) is computed *after* the scene for this frame, and it is a luminance, not an exposure. The guide allows auto-exposure instead of `pInExposureTexture` (3.10), at about 0.02 ms. A later option: feed `MidGray / (lum · (1 − MidGray))` from the previous frame's 1x1 target (3.9) |
| `DLSS.Hint.Render.Preset.DLAA` | **K (11)** | `nvsdk_ngx_defs.h` 310.9.16: "Default preset for DLAA/Balanced/Quality modes that is transformer based". The guide calls preset L "sharper, more stable … less ghosting than J, K but more expensive". Offer `Preset = K` (default) or `L` in the ini. Don't send `Default` (0), which NVIDIA may remap. J is "slightly less ghosting … extra flickering"; A-F are removed or deprecated |
| `FreeMemOnReleaseFeature` | 1 | As the DLSS 5 pass does |

Capability check, before creating the feature: `SuperSampling.NeedsUpdatedDriver` (then
`SuperSampling.MinDriverVersionMajor/Minor`), `SuperSampling.Available` and
`SuperSampling.FeatureInitResult`. SR is supported on every RTX GPU (20-series and up), unlike DLSSNR.

Through the SDK's D3D12 helper (`nvsdk_ngx_helpers_d3d.h`):

```cpp
NVSDK_NGX_DLSS_Create_Params create = {};
create.Feature.InWidth = create.Feature.InTargetWidth = width;
create.Feature.InHeight = create.Feature.InTargetHeight = height;
create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
NGX_D3D12_CREATE_DLSS_EXT(cmdList, 1, 1, &handle, params, &create);
```

The helper sets `Width`, `Height`, `OutWidth`, `OutHeight`, `PerfQualityValue`,
`DLSS_Feature_Create_Flags` and `DLSS_Enable_Output_Subrects`, then creates
`NVSDK_NGX_Feature_SuperSampling`.

### Evaluation

```cpp
NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
eval.Feature.pInColor = color;        // R16G16B16A16_FLOAT, linear HDR, NON_PIXEL_SHADER_RESOURCE
eval.Feature.pInOutput = output;      // R16G16B16A16_FLOAT, UNORDERED_ACCESS
eval.pInDepth = depth;                // R32_FLOAT z/w (the existing depth slot)
eval.pInMotionVectors = motion;       // R16G16_FLOAT, previous minus current, pixels, +y down
eval.InJitterOffsetX = jx;            // render pixels, -0.5..0.5, same convention as MVs
eval.InJitterOffsetY = jy;
eval.InMVScaleX = eval.InMVScaleY = 1.0f;
eval.InReset = cut ? 1 : 0;
eval.InRenderSubrectDimensions = {width, height};
NGX_D3D12_EVALUATE_DLSS_EXT(cmdList, handle, params, &eval);
```

The helper sets `Jitter.Offset.X/Y`, `MV.Scale.X/Y` (it replaces 0 with 1), `Reset`, the exposure
texture, `DLSS.Pre.Exposure` and `DLSS.Exposure.Scale` (both default to 1), the subrect bases and the
render subrect. The motion vectors already follow the guide's convention: "when the motion vector …
is added to the pixel's current location, the result is the location the pixel occupied in the
previous frame" (3.6). Pixels count from the top left (3.6.1).

**Headers.** `fetch-sdk.ps1` fetches only `nvsdk_ngx.h`, `nvsdk_ngx_defs.h` and `nvsdk_ngx_params.h`.
Add `include/nvsdk_ngx_helpers_d3d.h` from the same commit (SHA-256
`ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba`) and include it directly.
`nvsdk_ngx_helpers.h` (`5bcbadfe…44471`) also pulls in `nvsdk_ngx_helpers_cuda.h` (`f2851a76…05042`),
which needs CUDA's types.

### How it fits the helper and the protocol

The protocol is at version 1 (`dlss5/include/dsf_dlss5_protocol.h`). Version 2:

- `DsfDlss5Configure::reserved` becomes `feature`: `DSF_DLSS5_FEATURE_NR = 0` (today's behaviour) or
  `DSF_DLSS5_FEATURE_DLAA = 1`. `preset` applies to the named feature. The layout stays 80 bytes.
- `DsfDlss5Frame` gets `uint32_t feature` plus padding (56 bytes). Its `jitter`, `mv_scale`, `RESET`,
  `GPU_TIME` and `BYPASS` already cover DLAA. `style` and the strengths stay NR-only.
- `DsfDlss5HelloAck` gets a mask of available features, since a GPU or runtime folder can have SR
  without NR (RTX 20-40) or the other way round.
- The helper keeps state per feature: its own five slots, its two fences and its frame counter. Both
  features share one D3D12 device, one NGX initialization (both DLLs in the same `dlss5` folder, which
  is already the `PathListInfo`) and one pipe. Frame numbers stay per feature, so each pair of fences
  only moves forward.
- DLAA slots: `COLOR` `R16G16B16A16_FLOAT`, `OUTPUT` `R16G16B16A16_FLOAT` (UAV), `DEPTH` `R32_FLOAT`
  and `MOTION` `R16G16_FLOAT`, the latter two from the same game-side passes as DLSS 5. `MASK` is
  unused. The guide's `BiasCurrentColor` mask is not supported by the current models (3.15).
- New `dlss5/src/sr_d3d12.{h,cpp}` next to `nr_d3d12.*`, with the same Initialize, CreateFeature,
  Evaluate and Release shape. CreateFeature ends with the same mapped-module check, for
  `nvngx_dlss.dll`.

### DLAA, then DLSS 5

Yes, in that order. DLSS 5 ignores jitter and does no anti-aliasing (phase 0, "NR does not remove
jitter; feed it an unjittered image"), so with jitter on it must only see DLAA's resolved output. DLAA
wants the earliest, unprocessed HDR image (guide 3.1), which is the HDR input point. Two stages:

- **Stage A (PR 3), two round trips.** At the scene end, DLAA runs and its output is written back into
  `T#3`. Then the `Input=1` DLSS 5 path from `feat/dlss5-hdr-input` runs exactly as it does now: it
  reads `T#3`, tone-maps it, runs NR, tone-maps it back and writes it into `T#3`. Nothing in that path
  changes.
- **Stage B (PR 4), one round trip.** One FRAME runs DLAA, then Remix's fast tone map (as a helper
  compute pass), then NR, then the inverse, all in one command list. That saves the second synchronous
  wait (see the costs).

Rule: jitter is on only while DLAA is producing output. If DLAA fails, gets turned off, or the helper
drops out, jitter goes off in the same frame. Otherwise the frame shimmers, and DLSS 5 would pass the
shimmer through.

## 4. The runtime: nvngx_dlss.dll

| | |
| --- | --- |
| Source | https://github.com/NVIDIA/DLSS, release **v310.9.16** (2026-10-16). File `lib/Windows_x86_64/rel/nvngx_dlss.dll` at commit `9981c6350dc58c0ce21f024fd65e31e851c31689`, the commit `fetch-sdk.ps1` already pins for the headers. Direct link: `https://raw.githubusercontent.com/NVIDIA/DLSS/9981c6350dc58c0ce21f024fd65e31e851c31689/lib/Windows_x86_64/rel/nvngx_dlss.dll` |
| Size | 58,956,912 bytes (git blob `23c4672e4a31ef303e8f98ff4f0d146bd1718ccf`) |
| SHA-256 | `d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e` |
| Version | 310.9.16.0, "NVIDIA DLSS - DVS PRODUCTION" |
| Signature | Authenticode valid, `CN=NVIDIA Corporation` |

Checked on a copy downloaded to a scratch folder, which is not committed.

**Licence.** The NVIDIA RTX SDKs license (the SDK's `LICENSE.txt`):

- Section 1(c) allows distributing SDK material "as incorporated in object code format into a software
  application". Section 3(b) forbids distributing it "as a stand-alone product".
- The guide (4.2) expects the release DLL next to the executable, or in a folder passed in
  `PathListInfo`, and the development DLL from `lib/…/dev/` "MUST NOT be included or distributed in any
  public release".

Bundling it in our release ZIP would probably be allowed. We keep the project's rule anyway: no NVIDIA
binaries in the repository or the ZIP, and users download the file from NVIDIA. That is a single 59 MB
file from GitHub, simpler than DLSS 5's 557 MB Remix zip.

**Is there a fallback in the driver?** Not for SR:

- This PC's driver store carries only `nvngx_dlssg.dll` 310.2.1 (frame generation), in
  `DriverStore\FileRepository\nv_dispi.inf_amd64_20f49c6e…`.
- NGX's OTA cache (`C:\ProgramData\NVIDIA\NGX\models\dlss\versions\*\files\*.bin`) holds snippets for
  applications that opt in with an NVIDIA application id. We use a custom project id and never call
  `NVSDK_NGX_UpdateFeature`, so NGX should load our file.
- If it ever maps another copy, the helper's mapped-module check refuses it, the same open point as for
  DLSSNR in [dlss5-helper.md](dlss5-helper.md).

**Verification**, the same as for `nvngx_dlssnr.dll`. `ngx_trust` gets a per-file allowlist:

- `nvngx_dlss.dll`: `d00b5b0a…` (310.9.16.0, NVIDIA/DLSS v310.9.16), with a floor of 310.9.16.0;
- `nvngx_dlssnr.dll`: `2bc19bc0…` as now.

Both files must be signed by NVIDIA Corporation, held locked while in use, and checked against the
mapped module after CreateFeature. `--allow-runtime-sha256` admits newer releases, and the signature and
version checks still apply. Copies taken from games are refused unless allowed that way.

User steps for the README:

1. Download `nvngx_dlss.dll` from the link above.
2. Put it in the game's `dlss5` folder next to `nvngx_dlssnr.dll` (or on its own).
3. Check it with `Get-FileHash dlss5\nvngx_dlss.dll`.

## 5. Costs and risks

### GPU and frame cost

| | Value | Level |
| --- | --- | --- |
| NVIDIA's table (guide p. 6-7, preset J/K, **Performance mode**, ¼ input) | RTX 5070: 1.02 ms at 2560x1440, 2.11 ms at 3840x2160. RTX 5080: 0.68 / 1.31 ms | doc |
| Preset K, DLAA, 3440x1440, RTX 5070 Ti | ≈ 1.3-2.0 ms of GPU time: 1440p scaled by 1.34× the pixels, about 0.8× for the Ti, and 1.3-1.5× for full-resolution input | estimate |
| Preset L, same | ≈ 2.5-3.5 ms (the guide's L costs about 1.8× K) | estimate |
| Copies of colour, depth, motion and output (≈ 120 MB of traffic) | ≈ 0.1-0.2 ms | estimate |
| Synchronous round trip beyond the evaluate | ≈ 1.5-2 ms of render-thread time: the DLSS 5 bridge at 3440x1440 took 6.9 ms on average with a 4.9-6.0 ms evaluate (user's session, `dsf_enhanced.log` 23:03-23:04, Vulkan transport) | runtime (DLSS 5), applied to DLAA |
| VRAM | ≈ 150-200 MB (K is 143.5 MB at 2560x1440 per the guide) | doc, scaled |
| Saved | the MLAA edge and weight passes and the 4x MSAA swap chain (≈ 79 MB) | inferred |

DLAA alone therefore adds about 3-4 ms to a frame that currently takes 13 ms (70-80 fps in traffic in
the same log), unless a frame-lagged mode hides the wait (open since the DLSS 5 work). DLAA together
with DLSS 5 at Stage A: about 6.9 + 3.5 ms. Measure with `FRAME_GPU_TIME` and `FrameTimeLog=1`.

### Ghosting and smearing risks

| What | Motion vectors today | Expected | Mitigation |
| --- | --- | --- | --- |
| Wheel spin, suspension, loose panels | the car's, not the panel's | spokes smear and blur | per-panel motion (open in the renderer notes) |
| Pedestrians, driver, passengers, hands | camera only, zero nearer than 1.5 m | trails on fast limbs; hands are mostly static relative to the camera | character motion from the bone palette (later) |
| Sparks, smoke, dust, rain (blended, no depth write) | those of the surface behind | trails on fast sparks; smoke mostly fine | none in the current models (`BiasCurrentColor` is not supported, guide 3.15) |
| Reflections in car paint and glass (environment maps) | the surface's | reflections lag and soften slightly in turns | accept |
| Scrolling or animated textures (water, signs) | the surface's | smearing (guide 3.15) | accept; mip bias exceptions if they shimmer |
| Road revealed from under fast cars | car or camera | disocclusion noise for a frame or two | accept |
| Thin geometry (power lines, railings, fences) | correct | **the main improvement** | |

### Should the game's own AA be off when DLAA is on?

Yes:

- **MLAA.** Skip it while DLAA is on. It would re-detect edges in an image that is already
  anti-aliased, on top of grain, and blur fine detail.
  - The plugin can do this without patching the game: drop the draws of target sets 24 and 25 and
    clear `T#29` to 0. The composite then takes its "weights sum to 0" branch and copies `T#30`
    unchanged (static, from `F5ACBC8D`).
  - That works whatever `AntiAliasing` turns out to gate (V1).
- **The MSAA swap chain.** Force `MultiSampleType = NONE` and quality 0 in the `CreateDevice` and
  `Reset` hooks, for both the swap chain and its automatic depth. It costs memory and a resolve per
  present and shows nothing. This could be its own ini switch, on by default with DLAA, or the README
  can just tell users to set `AntiAliasing = 0`.

### Other cases

- **Cutscenes** (in-engine). Same renderer and scene pass. Camera cuts trigger the existing RESET.
  Letterbox bars are static and harmless. Bink movies have no scene pass, so no DLAA.
- **Shift zoom.** Already a cut when it starts. During the zoom, motion is from the camera.
- **Pause menu and loading screens.** No scene pass, or a frozen one, so jitter converges. The menus
  are in the HUD pass, which is never jittered.
- **Split screen.** Two viewports with two cameras in one target: the view-projection vote picks one
  camera, so the other half gets wrong motion. DLSS would need one feature per viewport, with subrects
  and per-viewport matrices. First version: detect two scene viewports and fall back to the game's MLAA.
- **Resolution change or device reset.** Reconfigure (existing path) and restart the Halton index.
- **GPUs.** SR runs on RTX 20-50. Only the DLSS 5 half needs RTX 50.

## 6. Implementation plan

The stack sits on `feat/dlss5-hdr-input`, which provides the HDR scene-end hook, the f16 colour input,
the PFM dumps and the `Input` key. Ini keys live in a new `[DLAA]` section (`Enable`, `Preset`,
`MipBias`, `JitterPhases`, `JitterDebugPixels`, `ForceGameAAOff`), which needs `[DLSS5] Export=1` and
the helper.

**PR 0 (prerequisite; part of `feat/dlss5-hdr-input` or the first commit of PR 1).** The scene target
is set 35 with a target of exactly the render size, not the busiest pass. Files: `src/dlss5_feed.inl`
(`D5NoteBusiestPass`, `D5FrameEnd`).

**PR 1: game-side jitter and AA switches (no NGX; testable in the VM).**

- New `src/dlaa_jitter.inl`: the SM1-3 token walker, the position-output patch, the free
  register/constant search, the Halton sequence, and per-draw `cJ` upload with its shadow copy.
- `src/dlss5_feed.inl` hooks:
  - `D5CreateVertexShader`: create the patched copy, keeping the hash of the original;
  - `D5BeforeDraw`: gate and upload `cJ`;
  - `D5SetVsConstantF`: invalidate the shadow copy;
  - the `SetRenderTarget` handling for sets 24 and 25: skip MLAA and clear `T#29`.
- `src/dsf_enhanced.cpp`: in `CreateDevice` and `Reset`, force non-MSAA present parameters and log the
  requested `MultiSampleType`; scope the scene-pass mip bias.
- `src/dsf_enhanced.ini`: the keys above, documented.
- `JitterDebugPixels=48`: a constant 48 px offset instead of Halton, to see in screenshots which draws
  move.
- `Trace=1` logs patched and rejected vertex shaders with their hashes.
- New `tests/vs_patch_test.cpp` (built by `build.ps1`): patches every vertex shader in shader blobs
  given on the command line. Each patched shader must disassemble with `D3DDisassemble`, contain
  exactly one final `mad`/`mov` pair to the position output, and leave the declared inputs and outputs
  unchanged. Run it locally on the shaders extracted from the game files and on `TraceShaders=1`
  dumps. Game shaders are not committed.

**PR 2: helper (DLSS SR at 1.0).**

- `dlss5/include/dsf_dlss5_protocol.h`: v2 as above.
- `dlss5/src/sr_d3d12.{h,cpp}`: new.
- `helper_main.cpp`: per-feature state, and the CONFIGURE/FRAME dispatch.
- `ngx_trust.{h,cpp}`: per-file allowlist and version floor.
- `fetch-sdk.ps1`: `nvsdk_ngx_helpers_d3d.h` with its hash.
- `dlss5/README.md`.
- Offscreen test: extend `dsf-dlss5-offscreen.exe` with `--feature dlaa [--jitter-sign 1|-1|0]
  [--phases N] [--preset K|L]`. The synthetic CPU-ray-traced scene already has exact depth and motion:
  - render each frame at pixel centre minus the Halton jitter, and a 64-sample reference without
    jitter;
  - report the mean error against the reference after 1, 8, 16 and 48 frames, and the frame-to-frame
    change with the camera still;
  - expected: with the right sign the error falls over the first 8-16 frames and the still image is
    stable; with the wrong sign or zero it doesn't.
  - Also `--hdr` with jittered PFM sequences dumped from the game (PR 3's `TraceDump`).
  - Runs a few seconds on the host GPU, no game needed.
- `client_test.cpp`: run both features over one pipe, with separate fences; `--robustness` per feature.

**PR 3: game-side DLAA.**

- `src/dlss5_bridge.inl`: at the scene end, before the `Input=1` DLSS 5 pass:
  - send the colour (a copy of `T#3`, sanitized as in the HDR branch), depth and motion;
  - send FRAME{feature DLAA, jitter, RESET} and composite the output back into `T#3`;
  - keep the original alpha.
- Jitter is tied to DLAA's success (on only after the first OK FRAME_ACK, off at once on any failure).
- Extend the trace dumps with jittered HDR PFM pairs and the jitter values.
- `README.md` gets a DLAA section: requirements, download, hash, ini, limitations.
- `research/dlaa.md`: results.

**PR 4 (optional).** One FRAME for DLAA, then the tone map, NR and the inverse in the helper. A
frame-lagged composite.

### How to test

Offline (host GPU, seconds):

1. `vs_patch_test` over every extracted vertex shader. The count of rejected shaders should be 0, or
   each one explained.
2. `dsf-dlss5-offscreen --feature dlaa` for the sign, phase count and preset, as described in PR 2.
3. `dsf-dlss5-client-test` with both features.

VM, through the driversf-forza harness. VMs have no NVIDIA GPU, so only the game side can be tested
there, on native D3D9 with `Helper=0`. These are the follow-up checks below.

Host A/B shots, when the user allows a host run:

- Setup: DXVK 3.1.1, 3440x1440, the 370Z probe (cockpit at 20 and 40 mph, then chase, with traffic),
  `FrameTimeLog=1`.
- Same frames for:
  1. the game's MLAA (`AntiAliasing=1`);
  2. DLAA preset K;
  3. DLAA preset L;
  4. DLSS 5 alone (`Input=1`);
  5. DLAA, then DLSS 5;
  6. jitter with no DLAA, as a negative control that should visibly shimmer.
- Crops: power lines and poles, fences and foliage, roof lines and kerbs at shallow angles, a wheel at
  speed, a pedestrian, sparks or smoke.
- Two consecutive frames of each, so crawling shows.
- Record the GPU time from FRAME_ACK and the frame times.
- Shots stay local and are not committed (game imagery).

### Follow-up checks (VM, game side, when the PC is free)

| # | Check | How |
| --- | --- | --- |
| V1 | Does `AntiAliasing = 0` turn MLAA off? | One VM run with the guest's `graphics.ini` set to `AntiAliasing = 0` (`setup-vm.ps1` rewrites only the window size today) and `[DLSS5] Trace=1`. Do target sets 24 and 25 (`3405C219`, `AECA6459`) disappear? |
| V2 | Which scene draws move with jitter | PR 1 build, `JitterDebugPixels=48`: cockpit, chase and Shift shots. Everything 3D in the scene should move (characters, particles, sky, cars); the HUD, shadows and minimap should not |
| V3 | The patcher over the real shader set | PR 1 build, `Trace=1`: count of patched and rejected vertex shaders over a drive and a cutscene; no D3D errors |
| V4 | MLAA skip and the forced non-MSAA swap chain | `ForceGameAAOff=1`: sets 24 and 25 skipped, `T#29` cleared, the composite unchanged otherwise; `CreateDevice` logs `MultiSampleType` 4 requested, 0 used |
| V5 | What the game requests today | the `MultiSampleType` log line from V4 with `AntiAliasing = 1`, confirming the static reading at runtime |
| V6 | Split screen | two viewports in set 35: does the plugin detect them and keep DLAA off? |

## Sources

- Driver.exe 1.04 (`D:\Games\Driver San Francisco`, read only): targeted disassembly with capstone of
  `0xBAA7E0` (ini), `0xBB2F80`, `0xBAFB80`, `0xBAFC30` (settings to present parameters),
  `0x6FE640`/`0x6FEAB0` (menu) and `0xBB2360` (depth surface creation), plus displacement searches in
  `0xBA0000-0xBC0000`.
- Shader bytecode from `media\SfxMisc.fchunk`, `Install\skies.sp`, `media\SfxParticles.fchunk`,
  `Install\character_global.fchunk` and `media\NonCityGadgets.fchunk`, disassembled with
  `d3dcompiler_47` `D3DDisassemble` and read through their constant tables. Nothing extracted is
  committed.
- `dsf_enhanced.log` and `Driver_d3d9.log` from the user's session of 2026-10-29 (3440x1440, DXVK, DLSS 5
  on). Harness run `host-20261029-103023` (pass list and shader hashes).
- NVIDIA DLSS SDK at `9981c635` (v310.9.16): `nvsdk_ngx_defs.h`, `nvsdk_ngx_params.h`,
  `nvsdk_ngx_helpers_d3d.h`, `LICENSE.txt`, and `doc/DLSS_Programming_Guide_Release.pdf` ("NVIDIA DLSS
  Super Resolution (version 310.6.0) Programming Guide", March 2026).
- [dlss5-renderer.md](dlss5-renderer.md), [dlss5-helper.md](dlss5-helper.md), and the uncommitted
  `feat/dlss5-hdr-input` worktree (read only, to match its `Input` key, its HDR tone map and inverse,
  and its PFM dumps).
