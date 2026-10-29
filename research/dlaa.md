# DLAA for Driver SF: research and design

Status 2026-10-30: PR 1 (game-side jitter and the AA switches, `feat/dlaa-jitter`) is built and tested
offline; see "PR 1 as built". PR 2 (the helper: protocol v2 and the DLAA pass, `feat/dlaa-helper`) is
built and tested offline, including the jitter sign test; see "PR 2 as built". PR 3 (game-side DLAA,
`feat/dlaa-game`) is built; only its offline parts are tested, see "PR 3 as built". The design below is
from 2026-10-29. DLAA here is NVIDIA DLSS Super Resolution (NGX feature
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

### The free constant register (static)

`J` is **c255** in every patched shader. Proof, from `tests/vs_jitter_test.cpp` run over the game's
files (2026-10-30):

- **Which files.** Every package in `media` and `Install` that is not audio (`*.fchunk`, `*.sp`, the
  3.1 GB `dngvehicles.sp` and the 795 MB `san_francisco.dngc`), plus `Driver.exe` and `DFEngine.dll`.
- **How shaders are found.** Every byte offset that holds a vertex shader version token (vs_1_1, 2_0,
  2_x, 3_0) is walked to its end token and disassembled with `D3DDisassemble`. Blobs whose disassembly
  has unknown opcodes (`???`) or no instruction are not shaders. Duplicates are removed by the
  plugin's shader hash.
- **Result.** 383 unique vertex shaders, all vs_3_0, all with a constant table.
- **What each shader can reach.** For each shader the patcher takes the highest of:
  - every `c` operand;
  - every `def`;
  - every float4 range in the constant table, which covers arrays read through `a0`/`aL`, such as
    the 183-register bone palette or the 208-register `g_litterToWorld`.

  Any relative read whose base lies outside every table range would make the shader's reach unknown.
  That shader would be left unpatched. None of the 383 has one.
- **Highest register any shader reaches: c215.** That is `vs_7F46FBDC` (litter): `g_litterToWorld`
  `c0-c207`, `s_WorldToScreen` `c208-c211`, `s_FogParams` `c214`, and `def c215`. The next highest
  are the vehicle shaders (`g_mtxWorldRotationToScreen` up to `c195`, `g_WorldToCam` `c199-c202`,
  `g_TexUVScale` `c204`). c216-c255 are unused by every shader.
- **Runtime guard.** The patcher checks each shader again when the game creates it, and leaves it
  unpatched if it reaches c255. The plugin also logs once if the game ever writes c255.
- **Layouts covered** (shaders by the clip transform in their constant table; one shader can have
  more than one):

  | Clip transform | Shaders |
  | --- | --- |
  | `g_ObjectToScreen` | 226 |
  | `g_mtxWorldRotationToScreen` (vehicles) | 76 |
  | `g_ViewToProj_Column0-3` | 29 |
  | `g_CameraToScreen` (particles) | 21 |
  | `g_boneMatrixArray` (skinned) | 12 |
  | `s_WorldToScreen` | 8 |

- **Flow control.** 16 shaders use `if`, `loop` or `rep`. None uses `call`, `label` or `ret`. The
  patcher would still insert its two instructions before main's first top-level `ret`.
- **The patch holds up.** All 383 patch. Each patched shader disassembles, has exactly one
  `mad o0.xy, rN.w, c255.xyyy, rN.xyyy`, and keeps its declarations.

Run it again with:

```powershell
build\vs_jitter_test.exe [--dump <dir>] "D:\Games\Driver San Francisco\media\SfxMisc.fchunk" ...
```

The test's `--gpu` mode draws through a patched pass-through shader on a D3D9 HAL device. The draw
is a 64x64 target at w = 2, with `J = (2·4/64, −2·3/64)`. Result on the RTX 5070 Ti:

- the picture's centroid moved from (32.000, 30.000) to (36.000, 33.000), +4 px right and +3 px down,
  as the sign convention says;
- with J = 0 the patched shader draws exactly what the original draws.

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
- **Checked (PR 2, offline).** The offscreen test confirms the protocol's convention: +x +y at 1x is the
  only variant that settles to a sharp, still image. The numbers are under "PR 2 as built".
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

**PR 1 as built** (`feat/dlaa-jitter`, 2026-10-30):

- **Files.**
  - `src/vs_jitter_patch.h`: the token walker and the patch. It has no Windows dependency.
  - `src/dlaa.inl`: the ini, the Halton sequence, the per-draw J, the scene's LOD bias, and the MLAA
    and MSAA switches.
  - Hooks in `src/dlss5_feed.inl`, `src/dsf_enhanced.cpp` (`ProxySetSamplerState`,
    `ApplyPresentationTweaks`) and `Dlss5OnReset`.
- **Ini keys** (`[DLAA]`, all off by default): `Jitter`, `JitterPhases` (16), `JitterDebugPixels`,
  `MipBias` (−1), `DisableMLAA`, `DisableSwapChainMSAA`. These replace the planned `ForceGameAAOff`,
  so the two parts can be tested on their own. `Enable` and `Preset` come with PR 3. With every key 0
  nothing is hooked: `Jitter` and `DisableMLAA` install the existing detours, without `Export`.
- **Shader model 1.** vs_1_x has no instruction length field, so the walker uses an opcode table, and
  any opcode outside it leaves the shader unpatched. J is c255 there too. vs_1_x only guarantees 96
  constants, and a device that refuses the patched copy gets the original. The game has no vs_1_x
  shaders.
- **Scene gate.** J is non-zero only on draws that meet all three conditions:
  - the target is the scene target (`D5IsScenePass`);
  - the render queue's set is 35 or 30;
  - the scene end has not been seen yet this frame.

  Draws after the scene end (a re-entered scene target), the shadow map, the minimap, post and the
  HUD get J = 0. J is uploaded only when the value differs from what c255 already holds.
- **LOD bias.** The scene's LOD bias goes on at the first jittered draw. It comes off at the scene end
  (and at Present as a fallback), back to what the game and `TextureLODBias` set.
- **MLAA skip.** It checks the targets before skipping:
  - set 24 must draw to `R5G6B5` at the render size, and set 25 to `A8R8G8B8` at the render size;
  - anything else keeps MLAA and logs once.

  The first skipped draw of set 25 clears its target to 0. The log names the skipped passes' pixel
  shaders (`3405C219`, `AECA6459` expected).
- **Swap-chain MSAA.** `DisableSwapChainMSAA` sets `MultiSampleType` and `MultiSampleQuality` to 0 in
  `CreateDevice` and `Reset`. It logs what the game asked for, which also answers V5.
- **Logging.** `Trace=1` logs each patched shader. A shader left unpatched is always logged once,
  with its hash and the reason.
- **Test name.** The test is `vs_jitter_test`, not `vs_patch_test` as planned. Windows' installer
  detection asks to elevate unmanifested 32-bit programs whose names contain "patch".
- **Offline results.**
  - the self-test passes (vs_1_1, vs_2_0 and vs_3_0 world, skinned and particle layouts; c255 in use;
    a table range over c255; relative reads with the table stripped; partial position writes;
    truncated bytecode; pixel shaders);
  - the game scan and `--gpu` gave the results above.
- **Not yet run in a VM or on the host.** The checks are V1-V5 below.

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

**PR 2 as built** (`feat/dlaa-helper`, 2026-10-30):

- **Protocol v2** (`dsf_dlss5_protocol.h`, `DSF_DLSS5_VERSION 2`).
  - `DsfDlss5Configure::reserved` became `feature`, and `DsfDlss5Frame` gained `feature` plus padding
    (56 bytes).
  - `DsfDlss5FrameAck::reserved` echoes the feature.
  - `DsfDlss5HelloAck` gained `features` (a bitmask), `dlaa_runtime_version` and padding (360 bytes).
  - HELLO succeeds if either DLL is there and its feature runs on the GPU. Each missing feature is
    named in the message.
  - A version 1 header is refused (MSG_ERROR). The game side speaks version 2 automatically. For DLSS
    5 it now checks that HELLO_ACK offers DLSS 5 (`D5StartHelper`); otherwise nothing changes.
- **Helper.**
  - `src/ngx_d3d12.*`: one NGX initialization, with the `--runtime` folder as the path list.
  - `src/nr_d3d12.*`: DLSS 5 uses it unchanged, still on NGX's capability parameters.
  - `src/sr_d3d12.*`: DLAA, on its own allocated parameter block, through `NGX_D3D12_CREATE_DLSS_EXT`
    and `NGX_D3D12_EVALUATE_DLSS_EXT`.
    - Input = output size, `PerfQualityValue_DLAA`, `IsHDR | MVLowRes | AutoExposure`.
    - `DLSS.Hint.Render.Preset.DLAA` = K (11) when CONFIGURE's `preset` is 0 or 11, L (12) on request.
    - `FreeMemOnReleaseFeature`.
  - `helper_main.cpp` keeps per-feature state: slots, fences, frame numbers, the configured flag and
    the timeout handling. DLAA's colour and output default to `R16G16B16A16_FLOAT`; it has no mask
    slot.
- **Trust.** `ngx_trust` takes a `RuntimeSpec` per file: name, allowlist, version floor and where to
  get it. `nvngx_dlss.dll` must be:
  - SHA-256 `d00b5b0a…6123e`;
  - signed by NVIDIA Corporation;
  - version 310.9.16.0 or later;
  - locked while in use, and the copy NGX mapped (`IsSameFile` after CreateFeature).

  `fetch-sdk.ps1` adds `nvsdk_ngx_helpers_d3d.h`, pinned by SHA-256. The downloaded copy used for the
  tests matched the size, hash, version and signature above. It is not committed.
- **Synthetic scene fix.** The motion vectors were measured from the pixel centre to the jittered
  sample's previous position, so they carried −jitter. The header says they are without jitter, and
  DLAA (`MVJittered = 0`) needs that. They are now the sampled point's own motion. DLSS 5's tests barely
  depend on it. The static-camera DLAA runs also zero the motion: the scene renders frame 0 every time,
  but its motion still describes a move from frame −1.
- **Sign and scale test** (`dsf-dlss5-offscreen --feature dlaa`, RTX 5070 Ti, driver as installed).
  - Setup: 640x360, preset K, 16 Halton phases, 24 frames per variant.
  - Reference: 8x8 supersampled, no jitter.
  - Errors are mean |difference| over RGB after the scene's tone map, in 1/255.
  - Shimmer is the mean frame-to-frame change over the last 16 frames with the camera still.
  - Sharpness is the mean gradient over the reference's.
  - The input alone (one sample at each pixel centre) has error 1.134.

  | Variant (factor on the reported jitter) | Static error after 1 / 8 / 16 / 24 | Shimmer | Sharpness | Moving error after 8 / 16 / 24 |
  | --- | --- | --- | --- | --- |
  | **+x +y, 1x (the protocol's convention)** | 1.075 / 0.440 / 0.372 / **0.354** | **0.075** | 0.982 | 1.176 / 1.043 / **1.058** |
  | −x −y | 1.321 / 1.105 / 0.894 / 0.922 | 0.278 | 0.918 | 2.174 / 1.905 / 2.063 |
  | +x −y | 1.321 / 0.745 / 0.581 / 0.646 | 0.219 | 0.951 | 1.545 / 1.155 / 1.393 |
  | −x +y | 1.075 / 0.777 / 0.651 / 0.588 | 0.122 | 0.953 | 1.681 / 1.714 / 1.609 |
  | 2x scale | 1.234 / 0.761 / 0.709 / 0.714 | 0.110 | 0.929 | 1.394 / 1.189 / 1.256 |
  | 0.5x scale | 1.109 / 0.377 / 0.321 / 0.318 | 0.092 | 0.998 | 1.205 / 1.091 / 1.098 |
  | no jitter | 1.005 / 0.988 / 1.001 / 1.006 | 0.006 | 1.070 | 1.121 / 0.997 / 1.020 |

  - **Sign.** The right sign converges to a third of the input's error. Flipping both axes gives 2.6x
    the error, 3.7x the shimmer and a visibly softer image (sharpness 0.92). Flipping one axis falls
    in between. The right sign also has the lowest moving error of the jittered variants.
  - **Scale.**
    - 2x is clearly worse.
    - 0.5x scores a slightly lower static error against the box-filtered reference: a smaller
      reported jitter averages a little more. But it shimmers more (0.092 against 0.075) and has a
      higher moving error.
    - The test therefore judges the scale on shimmer and the sign on all three measures. It exits 0
      when they hold, 10 otherwise.
  - **No jitter.** It is the reference point. The image is still and stays aliased (static error
    1.006). With the camera moving at this speed, DLAA's 24-frame error stays near the aliased input
    either way (1.06 jittered, 1.02 not). The gain is in the still and slow shots.
- **Protocol test** (`dsf-dlss5-client-test`, x86 client → helper over the pipe, D3D11 shared
  textures):
  - `--feature dlaa --robustness` with only `nvngx_dlss.dll`: HELLO_ACK has features 0x2 and says
    "DLSS 5: not in …". 12/12 frames OK, and every check passes:
    - the old checks: repeated and zero frame numbers, the fence_in timeout and its fence_out,
      unconfigured frames, reconfigure;
    - FRAME and CONFIGURE with an unknown feature → `ERR_PROTOCOL`;
    - FRAME for the other, never-configured feature → `ERR_NOT_CONFIGURED`, after which the
      configured one still runs;
    - a version 1 header → MSG_ERROR.
  - `--host-creates` (helper-created f16 textures): 6/6 frames.
  - `--feature both --robustness` with both DLLs: features 0x3. DLSS 5 and DLAA ran frame by frame over
    one pipe and one NGX instance, 16/16 frames OK, and the robustness checks passed.
  - Round trip at 640x360 through the CPU test client (it converts frames on the CPU): DLSS 5 9.7 ms,
    DLAA 18 ms. GPU time of the evaluate: DLSS 5 1.12 ms, DLAA 0.28 ms.
- **DLSS 5 regression.** `dsf-dlss5-offscreen` (DLSS 5, 6 frames) after the NGX refactor: 6/6
  evaluates.

**PR 3: game-side DLAA.**

- `src/dlss5_bridge.inl`: at the scene end, before the `Input=1` DLSS 5 pass:
  - send the colour (a copy of `T#3`, sanitized as in the HDR branch), depth and motion;
  - send FRAME{feature DLAA, jitter, RESET} and composite the output back into `T#3`;
  - keep the original alpha.
- Jitter is tied to DLAA's success (on only after the first OK FRAME_ACK, off at once on any failure).
- Extend the trace dumps with jittered HDR PFM pairs and the jitter values.
- `README.md` gets a DLAA section: requirements, download, hash, ini, limitations.
- `research/dlaa.md`: results.

**PR 3 as built** (`feat/dlaa-game`, 2026-10-30):

- **Files.** `src/dlss5_bridge.inl` (per-feature state and the DLAA pass), `src/dlaa.inl` (the ini, the
  jitter gate, the MLAA follow and split screen), hooks in `src/dlss5_feed.inl`. The helper and the
  protocol are unchanged from PR 2.
- **Ini.** `[DLAA] Enable=1` and `Preset=K|L` (or 11/12; anything else is K, logged).
  - Enable patches the vertex shaders as `Jitter=1` does and starts the helper bridge without
    `[DLSS5] Export` or `Helper`. The bridge flag `g_d5_helper` now means "DLSS 5 or DLAA"; DLSS 5 alone
    is `g_d5_nr`. INTZ depth, the object-motion tagging and the motion pass therefore run for DLAA alone
    too.
  - `DisableMLAA` and `DisableSwapChainMSAA` are empty in the shipped ini: empty, missing or `auto`
    follows Enable, `0` and `1` are kept as set. With Enable, `DisableMLAA=1` still only skips MLAA on
    frames DLAA anti-aliased.
  - The `[DLSS5]` helper keys (paths, `HelperTransport`, `FrameTimeLog`, `CrashDump`, `SelfTest`,
    `ObjectMotion`, `CockpitDepth`) apply to DLAA as well. `HelperBypass` stays DLSS 5 only: a bypassed
    DLAA would show the jitter.
- **Per-feature state in the bridge.** Each feature has its own state (off, idle, configuring, running),
  its own D3D11 textures and fences or Vulkan imports, its own frame numbers (from 1 after each
  CONFIGURE), configure attempts, composite texture and colour input. The connection keeps one state.
  - **The pipe.** The worker owns it while the bridge starts or any feature is configuring. Only the
    render thread asks for a CONFIGURE, so when it sees no feature configuring, the worker is off the
    pipe, and it may send FRAME. A feature that would send while the other is being configured skips
    the frame. The worker configures every requested feature in turn.
  - **HELLO.** A wanted feature that HELLO_ACK doesn't offer is off for the session, with the reason
    logged; the bridge fails only if no wanted feature is left. A feature that fails CONFIGURE or its
    frames three times in a row is off for the session; the helper is shut down when no feature is left.
  - **Depth and motion** are drawn once per frame, for whichever feature runs first; the CPU transport
    also reads them back once. DLSS 5 at the pre-HUD point (`Input=0`) now reuses the scene end's.
  - **RESET** on the first frame after CONFIGURE, on camera cuts and stale VPs (as before), and when the
    feature skipped the frame before (its history would be two frames old).
- **The DLAA pass**, at the scene end (`D5LeavePass`), after the VP solve and the car matching, before
  the `Input=1` DLSS 5 pass:
  1. `HdrSanitizePS` reads `T#3` through its texture (no copy) and writes its colour, without NaNs or
     negatives, into an f16 colour input. A scene target that isn't a texture skips DLAA (logged once).
  2. The colour input, depth and motion go to DLAA's slots over either transport. FRAME carries the
     feature, the jitter this frame's scene was drawn with (zero if it wasn't), `mv_scale` 1 and RESET.
     No exposure: the feature is created with AutoExposure.
  3. After an OK FRAME_ACK, the output comes back into the composite texture (Vulkan copy after the
     fence wait, or the CPU copy), and `HdrSanitizePS` draws it into `T#3` with colour writes only, so
     the scene keeps its alpha.

  The device state is saved and restored around it as for DLSS 5 (state block, render targets 0-3,
  depth), and read back and compared on the first frames, traced frames and SelfTest frames.
  `[DLSS5] SelfTest=1` with Enable runs this side with the helper replaced by a copy.
- **With DLSS 5.** `Input=1`: the second round trip reads `T#3` after DLAA's write-back, so it tone-maps
  and denoises DLAA's output (Stage A). `Input=0`: DLSS 5 reads the pre-HUD frame, which the post chain
  made from the written-back `T#3`, so it sees the anti-aliased scene too.
- **The jitter gate.** At each frame end, the next frame is jittered only if DLAA's output went back
  this frame, at the render size, with DLAA running and nothing being configured (`D5DlaaProducing`),
  and no split screen in the last 60 frames.
  - So the first frame after a CONFIGURE runs DLAA unjittered, and jitter starts with the next.
  - A failed, refused or timed-out FRAME, a dead helper (the pipe breaks or a read times out after 3 s),
    a Reset or a resize turn it off for the next frame. The frame on which DLAA fails was already drawn
    jittered and is shown without DLAA: one frame, unavoidable without a frame of lag.
  - The scene's LOD bias follows the jitter.
- **MLAA comes back mid-session.** The game draws both MLAA passes every frame, and the skip is decided
  pass by pass at `SetRenderTarget`. With Enable, sets 24 and 25 are skipped only if DLAA's output went
  into `T#3` earlier in the same frame. On any other frame (DLAA failed, skipped, still starting, off,
  split screen) the game's passes run and write `T#28`/`T#29` as usual, so a frame never lacks
  anti-aliasing. The first frames of each change are logged.
- **Split screen** (not seen in a trace yet; V6). Two signs of a second view:
  - a scene draw under a viewport that doesn't cover the render size (the viewport is tracked from
    `SetViewport` and reset at `SetRenderTarget(0)`);
  - 50 or more draws into `T#3` under set 35/30 after the scene end.

  Either one keeps DLAA, the jitter and the MLAA skip off for 60 frames, and is logged once with the
  viewport or the draw count. A letterboxed cutscene that narrows the viewport would also trigger it
  (to check, V8).
- **Robustness.** FRAME and FRAME_ACK keep their timeouts (3 s), the ACK's status, frame and feature are
  checked before any wait on fence_out, frame numbers are per feature, and Reset releases every
  default-pool texture of both features (the shared imports stay; a size change reconfigures).
- **Traces.** With Enable and `TraceDump=1`, both frames of a traced pair dump `scene-end` (the jittered
  HDR scene, PFM) and `scene-end-dlaa` (DLAA's output), and each traced frame logs its jitter and phase
  (`DLAA  frame N: ...`). `FrameTimeLog=1` adds a DLAA line (frames, bridge and evaluate times).
- **Tests run (offline, 2026-10-30).**
  - `.\build.ps1`: plugin, helper, offscreen and client tests; the cinematic fit and `vs_jitter_test`
    self-tests pass.
  - The bridge's pixel shaders, with the new `HdrSanitizePS`, compile as `ps_2_0` with
    `d3dcompiler_47` (`HdrSanitizePS` 256 bytes).
  - `dsf-dlss5-client-test --feature both --robustness` with this branch's helper, both DLLs (hashes
    checked): features 0x3, 48/48 frames, every robustness check passes. `--feature dlaa --host-creates`
    (the Vulkan direction): 24/24 frames. The game side's own code is not exercised offline.
- **Not tested yet** (no game or VM run was possible): everything in game. The checks are V6-V10 below.
- **Open.** One round trip for both features (PR 4); a frame-lagged mode that would also avoid the
  jittered frame on a failure; DLAA and the split-screen signs in cutscenes.

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
| V2 | Which scene draws move with jitter | PR 1 build, `Jitter=1 JitterDebugPixels=48`: cockpit, chase and Shift shots. Everything 3D in the scene should move (characters, particles, sky, cars); the HUD, shadows, minimap and mirrors should not. Then `JitterDebugPixels=0`: the scene should shimmer by under a pixel, the HUD not at all |
| V3 | The patcher over the real shader set | PR 1 build, `Jitter=1 [DLSS5] Trace=1`: count of patched and rejected vertex shaders over a drive and a cutscene (the offline scan expects 0 rejected); no D3D errors; no "game writes vertex constant c255" line |
| V4 | MLAA skip and the forced non-MSAA swap chain | `DisableMLAA=1`: the log names sets 24 and 25 skipped with `3405C219`/`AECA6459`, `T#29` cleared, and the composite otherwise unchanged (edges visibly aliased). `DisableSwapChainMSAA=1`: `CreateDevice` logs `MultiSampleType` 4 requested, forced to none |
| V5 | What the game requests today | the `MultiSampleType` log line from V4 with `AntiAliasing = 1`, confirming the static reading at runtime |
| V6 | Split screen | two viewports in set 35: does the plugin detect them and keep DLAA off? With Enable: the `DLAA: scene draw under viewport` or `more draws after the scene end` line, MLAA on, no jitter |
| V7 | DLAA alone, in game (host, DXVK) | `[DLAA] Enable=1`, `FrameTimeLog=1`: `CONFIGURE DLAA ... status=0`, `jitter on`, `MLAA pass skipped`, the state check lines, the DLAA frametime line; no `split screen` line in single-player driving |
| V8 | DLAA in cutscenes and the Shift zoom | Does DLAA stay on (no split-screen line), and do cuts send RESET? |
| V9 | DLAA with DLSS 5 | `Input=1` and `Input=0`: both features configured, two round trips with `Input=1`, DLSS 5's input anti-aliased |
| V10 | A killed or stalled helper | `jitter off`, `the game's MLAA runs`, the game goes on; also a Reset (alt-tab, resolution change) |

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
