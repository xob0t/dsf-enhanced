# Ray tracing for Driver SF: options, effort and a recommendation

Status 2026-10-30: research only. No game, VM or Remix run was made for this note. The question is
how much work it would be to add ray-traced or path-traced rendering to Driver: San Francisco (PC
1.04, 32-bit D3D9) on top of DSF Enhanced, on the user's RTX 5070 Ti.

Evidence levels as in [dlss5-renderer.md](dlss5-renderer.md): *runtime*, *static* (bytecode,
headers, source), *inferred*, *estimate*. Effort is in person-weeks for one engineer who already
knows this code base (the plugin, the helper, the DXVK interop and worldlod's parsers). Someone new
to it should add 30-50 %.

## Summary

| | 1a. RTX Remix, D3D9 capture | 1b. RTX Remix, plugin feeds the API | 2. Hybrid DXR in our helper | 3. Screen-space effects |
| --- | --- | --- | --- | --- |
| What you get | Full path tracing (Remix's renderer, DLSS RR, DLSS 5) of the game's own draws | Full path tracing of geometry we submit | Ray-traced AO, then sun shadows, then reflections, added to the game's own image | AO, SSR and SSGI from the depth buffer |
| Looks like | The game re-lit by the sun, the sky and whatever lights we add. Without the game's lightmaps, road markings and detail maps, and with flat legacy materials, until assets are authored | Same renderer, with control over what goes in: the static city from the game files, cars and characters with real transforms | The game as it is, with grounded cars, contact shadows, sharper long-range shadows and real reflections on cars and wet roads | The game with ReShade-style AO and GI; screen-edge and disocclusion artefacts |
| First visible prototype | 3-5 weeks: path-traced daytime driving with gaps and glitches | 6-9 weeks: the static city path-traced around the car | 4-6 weeks: RT AO from the static city, denoised, in the HDR scene | 1-2 days with ReShade; 2-3 weeks native |
| "Good" result | 16-30 weeks, most of it assets and lighting, open-ended after that | 24-40 weeks, with less asset risk on roads and instancing | 18-30 weeks for AO, shadows and car/road reflections | 4-8 weeks native |
| Main risks | No camera or world matrices for Remix without our injection (NVIDIA: "a known challenge" for games like this); roads and baked lighting; CPU cost of vertex capture and anti-culling through the 32-bit bridge (GTA IV and NFS Carbon ports are CPU-bound); night lighting | HUD compositing; keeping a second scene description in step with the game; the size of the job | Feeding results into a forward renderer with no G-buffer; hit shading for reflections | Quality ceiling |
| Keeps DLSS 5 / DLAA / DXVK | No: the bridge replaces our `d3d9.dll`; Remix brings its own DLSS (and DLSS 5) | No, same | Yes, same helper and transport | Yes |
| GPU cost on a 5070 Ti | High; needs DLSS Performance at 1440p (estimate) | High | 1-4 ms per effect (estimate) | 0.5-2 ms |

**Recommendation.** Do option 2 in stages, starting with ray-traced ambient occlusion and sun
shadows from a static-city BVH built offline by worldlod's parsers, and use option 3 (one evening
of ReShade) first as the baseline to compare against. Remix is the only route to *path tracing*,
but in this game it is a remaster project, not a feature: every one of the game's 376 scanned
vertex shaders needs our plugin to hand Remix a camera and world matrices it can't find itself,
the game's look depends on lightmaps and multi-texture road shaders Remix ignores, and the city
then needs lights and materials authored at scale. NVIDIA itself says DX9.0c shader games
"probably won't work", and calls a game that keeps its WVP in shader constants, as this one does,
"a known challenge" with no timeline (Sources). If the user wants to see path tracing anyway, a
three-week Remix spike (milestones R0-R1 below) settles the open risks before anything bigger is
committed.

## What the renderer gives a ray tracer

From [dlss5-renderer.md](dlss5-renderer.md), [dlaa.md](dlaa.md) and a new static scan of the game's
shader packages (below):

- **Forward renderer, no G-buffer.** One HDR scene pass into `T#3` (`A16B16G16R16F`), 2,500 to 2,700
  draws and about 1.07 M triangles a frame in gameplay, 4,000 draws in the Shift overview. No normals,
  material or albedo targets exist. Depth is readable (INTZ), and the plugin already has the
  scene-end hook (`Input=1`), per-frame VP, motion vectors and per-car identity in stencil.
- **No fixed-function state.** Every draw uses vs_3_0 and ps_3_0. Clip transforms live in shader
  constants with six layouts (`g_ObjectToScreen` 226 shaders, vehicles' `g_mtxWorldRotationToScreen`
  76, `g_ViewToProj_Column*` 29, particles' `g_CameraToScreen` 21, `g_boneMatrixArray` 12,
  `s_WorldToScreen` 8). The plugin never hooked `SetTransform`, and the matrices it needs all come
  from constants, so the game most likely sets no fixed-function transforms (*inferred*; a counter
  on `SetTransform` in one run would confirm it).
- **Baked lighting.** 334 of the 548 scanned pixel shaders sample `g_samLightMap`. The static city
  is lit by lightmaps plus the sun with a PSSM shadow map (`g_sPSSM`, 122 shaders). Diffuse GI is
  therefore already in the image for the static world; what ray tracing can add is mostly
  occlusion and shadowing of dynamic objects, sharp long-range sun shadows, and reflections.
- **World geometry is in the game files and we can read it.** worldlod already parses
  `san_francisco.dngc` (MODL, VXCK/IXCK vertex and index chunks, int16 positions at 1/64 m, 445 GEGR
  materials, DDS textures). Its default bake, the whole city's detailed ground, trees, building detail
  and road text in 408 parts, is 8.5 M triangles in 260 MB of buffers. Shared
  props (lamp posts, trees) are 377 mesh ranges placed 31,907 times ([world-lod.md](../worldlod/research/world-lod.md)).
  That is an offline source for a static BVH that does not depend on what the game streamed in.

### Static scan: pixel shader samplers and vertex shader inputs

A read-only scan of `NonCityGadgets.fchunk`, `character_global.fchunk`, `SfxParticles.fchunk`,
`SfxMisc.fchunk`, `skies.sp`, `MegaMesh.sp`, `san_francisco.dngc` and `dngvehicles.sp`: every SM3
shader blob, de-duplicated, with its constant table and its input declarations (script kept outside
the repository; nothing extracted is committed).

| | World packages | `dngvehicles.sp` | Total |
| --- | --- | --- | --- |
| Unique ps_3_0 with a constant table | 421 | 127 | 548 |
| ... with `g_samDiffuseMap` on sampler 0 | 362 | 118 | 480 (88 %) |
| ... with `g_samLightMap` anywhere | 218 | 116 | 334 |
| ... with road or detail layers (`g_samRoadMarkings` 35, `g_samClosedupDetailedMap` 155, `g_samMacroLevelDetailMap` 34, `g_samDiffuseMap2` 38) | | | about 200 (overlapping) |
| ... with a normal map (`g_samNormalMap*`, `g_samNormMap`) | | | 20 |
| Unique vs_3_0 with a constant table | 290 | 86 | 376 |
| ... taking a `NORMAL` input | 167 | 80 | 247 |
| ... taking `BLENDINDICES` (skinned) | 9 | 0 | 9 |
| ... taking `POSITIONT` | 0 | 0 | 0 |

(The jitter test in [dlaa.md](dlaa.md) found 383 unique vertex shaders across all packages and the
executables; this scan did not include `Driver.exe`, `DFEngine.dll` or the GUI packages.)

## Option 1: RTX Remix

### Where Remix stands on shader games

- NVIDIA's compatibility page: Remix targets fixed-function DX8 and DX9 games; "DirectX 9.0c games
  are usually mostly shader-based and probably won't work", with "experimental support for simple
  vertex shaders".
- NVIDIA staff on rtx-remix issue #268 (2026-03-05), about Need for Speed: Carbon, which keeps its
  WorldViewProjection in `c10-c13` and never calls `SetTransform`: vertex capture "reverse-transforms
  shader output positions using the D3D9 transform state, which for this game can be incorrect or
  non-invertible"; full compatibility "remains a known challenge", with no timeline. Driver SF is
  the same case (WVP in `c0-c3`).
- Every published port of a shader game is a game-specific ASI that gives Remix what it can't find
  (xoxor4d's remix-comp projects):
  - **NFS Carbon** (nfsc-rtx, the closest driving game): "Fixed Function rendering in Shadermode",
    patches the game so view and projection are set in shader mode, a car-paint shader in a forked
    runtime, rain through the Remix API. Skinned meshes stay on vertex capture ("Fix T-Pose"; the
    fixed-function attempt lost face animation). Known downside: "Large amount of drawcalls (due to
    anti culling) can result in a cpu bottleneck".
  - **GTA IV** (gta4-rtx): most objects redrawn with fixed function "to increase performance
    (compared to grabbing data from Vertexshaders)", modified vertex shaders so Remix can capture
    normals, game lights turned into Remix lights through the API, a forked runtime. Its README calls
    it CPU-bottlenecked and stuttery while moving through the world. Press measurements range from
    about 40 fps at 4K with DLSS Performance on an RTX 5080 to 21-23 fps at native 4K on an RTX 5090.
  - **Mirror's Edge** failed: half-float texcoords and packed normals, which fixed function can't
    read, and no camera or mesh transforms found.
  - The method is stated as "manually reimplementing fixed function rendering": the ASI sets view,
    projection and world with `SetTransform` and calls `SetVertexShader(nullptr)` on the draws it can
    convert, leaving the rest to vertex capture.

Driver SF stores world positions as int16 at 1/64 m (a fixed-function world matrix can carry that
scale) and uses `FLOAT16` and `UBYTE4` elements in its vertex declarations (worldlod's
`formats/modl.py`), the obstacle that stopped Mirror's Edge. So for this game the realistic 1a route
is vertex capture with injected `SetTransform`, with fixed-function redraws only for draws whose
declarations fixed function can read.

### How Remix handles a shader game (static, dxvk-remix source)

Read from `src/d3d9/d3d9_rtx.cpp` and `src/dxvk/rtx_render/rtx_context.cpp` at dxvk-remix `main`
(fetched 2026-10-30) and the 1.6.1 option list (`RtxOptions.md`):

1. **A draw with a vertex shader is skipped unless vertex capture is on.** `makeDrawCallType` logs
   "Skipping draw call with shader usage as vertex capture is not enabled" when
   `UseProgrammableVS() && !useVertexCapture()`. With `rtx.useVertexCapture=True` (the default)
   Remix injects code into the translated vertex shader that writes each vertex's clip-space
   position, its input normal and (optionally) its output texcoord to a buffer.
2. **Capture needs the fixed-function matrices.** `prepareVertexCapture` uploads
   `invProj = inverse(viewToProjection)`, `viewToWorld = inverseAffine(worldToView)` and
   `worldToObject = inverseAffine(objectToWorld)`, and `processRenderState` takes those from
   `D3DTS_PROJECTION`, `D3DTS_VIEW` and (with `rtx.useWorldMatricesForShaders`) `D3DTS_WORLD`. The
   option text says it plainly: vertex capture "is useful for games using simple vertex shaders,
   that still also set the fixed function transform matrices". The camera manager also builds the
   ray-tracing camera from those matrices (`processCameraData`), and Remix does no ray tracing at
   all while the camera is invalid.
3. **Pixel shaders are ignored.** With a pixel shader bound, `processTextures<false>` takes the
   textures in sampler order: sampler 0 becomes the albedo, sampler 1 is only used by ray-portal
   materials. Lightmaps, detail layers and anything the pixel shader computes are lost.
4. **Shader skinning gets no skinning data.** `processSkinning` returns nothing when a vertex shader
   is bound; captured skinned meshes arrive already posed, as new geometry every frame.
5. **The Remix C API** (`public/include/remix/remix_c.h`, API 0.6.6) lets an application create
   meshes (CPU vertex arrays, optional skinning), materials (textures by file path, PBR parameters),
   lights and instances, set the camera (`SetupCamera` with view and projection, or position,
   axes, FOV and planes), set options and present. `commitExternalGeometryToRT` is the runtime side.
   RemixSDK.md still says the API "is 64-bit only", but Remix 0.6.0 (2024-12-03) "Added support for
   Remix API forwarding through the bridge, enabling 32-bit games to use the Remix API", and the
   32-bit gta4-rtx and nfsc-rtx use it (`bridge_remix_api.h`: `CreateMesh`, `DrawInstance`,
   `DrawLightInstance`).

What that means for Driver SF, draw by draw:

| Remix's requirement | Driver SF | Consequence |
| --- | --- | --- |
| Vertex capture for shader draws | All vs_3_0 | Must be on |
| `SetTransform(VIEW)` and `(PROJECTION)` | Not set (inferred); VP solved per frame by the plugin; separate view and projection in particle and some character constants | The plugin must call `SetTransform` with a view and a projection every frame, split from the solved VP or read from the particle constants. Without it Remix has no camera and traces nothing |
| `SetTransform(WORLD)` per draw, so captured vertices go back to object space | World rows in `c4-c6` (226 shaders), `g_ObjectToWorld` (vehicles), bones (skinned) | The plugin must set `WORLD` before every scene draw. Without it every draw becomes unique world-space geometry: no instancing of the 31,907 prop placements, no stable hashes, no asset replacement |
| Albedo on sampler 0 | 480 of 548 pixel shaders | Good for most draws |
| Everything else on samplers 1+ ignored | Lightmaps (334 shaders), road markings and detail layers (about 200) | Baked lighting goes, which is what path tracing wants. Road markings and road detail go too, and roads are most of the screen in a driving game |
| Normals from the input `NORMAL`, transformed by `WORLD` | 247 of 376 vertex shaders | Fine for world draws. Car panels are rotated per panel (`g_PanelTransforms`), so wheel and panel normals come out wrong unless the plugin passes the panel rotation |
| Colour writes on, a colour target bound | The shadow map pass writes colour (`A8R8G8B8`); the minimap and mirror strip are real 3D passes | The plugin must keep these passes from reaching Remix as scene geometry (turn shadows off in the game's settings, skip the minimap and mirror passes, or give them a camera Remix skips) |
| Ortho draws are UI only for fixed function | The post chain and HUD are shader quads | The plugin must skip the post chain and tag the HUD textures as UI so Remix composites at the HUD |

### 1a. Remix on the game's own draws

What the plugin would have to do (a "Remix compatibility" layer, the same kind of work the
community's shader-game projects do; see Sources):

1. Per frame: `SetTransform(VIEW)` and `SetTransform(PROJECTION)` from the solved camera. Remix also
   needs the right handedness and up axis (`rtx.zUp`, `rtx.sceneScale`; the game is metres).
2. Per scene draw, from the constant layout known at `CreateVertexShader` (the jitter patcher
   already classifies all 383 shaders): `SetTransform(WORLD)` from `c4-c6`, from
   `g_ObjectToWorld`, or identity for skinned and particle draws.
3. Hide the passes that aren't the main view: the 3072² shadow map, the minimap, the mirror strip,
   the post chain. The rear-view mirror could later become a Remix render-to-texture camera.
4. Texture categories in Remix's runtime menu or `rtx.conf`: sky (`skies.sp`), particles, decals
   (skid marks, the road decals that are separate geometry), UI (the HUD), ignore (imposters, lamp
   flares, god rays, the low-resolution particle layer).
5. Lights. The game has no D3D9 lights. The sun's direction changes with the mission's time of day,
   so a static distant light in `rtx.conf` is wrong; the plugin should read the game's sun constant
   and set it through the API or an option each frame. Street lights, car head and tail lights and
   signs need lights too, or night driving goes dark. `rtx.lightConverter` can spawn small effect
   lights on tagged flare textures; real headlights need spot lights placed from the car's
   `g_ObjectToWorld`.
6. Roads. Remix sees only the base road texture: markings and the near/far detail blend go. Fixes:
   replacement materials for the road meshes (hundreds of streamed tiles, by mesh hash), or an
   offline bake of the road layers into albedo textures that the plugin swaps onto sampler 0 per
   draw. Either is weeks.
7. Materials. Without replacements Remix uses its legacy material: the albedo with a constant
   roughness. Car paint, glass, chrome, wet asphalt and building windows need PBR materials.
   The Toolkit's AI tools can generate normal and roughness maps in bulk, but car paint (about 120
   cars, with liveries) and glass need hand work.

Interaction with DSF Enhanced:

- **`d3d9.dll`.** For a 32-bit game Remix ships a 32-bit `d3d9.dll` bridge client beside the exe.
  It forwards every D3D9 call, through shared-memory ring buffers, to the 64-bit
  `.trex\NvRemixBridge.exe`, which runs the 64-bit dxvk-remix `d3d9.dll` from `.trex`. (The x64
  package read here, for 64-bit games, has the runtime as `d3d9_x64.dll` behind a small `d3d9.dll`
  shim.) The client takes the
  `d3d9.dll` slot, so DXVK 3.1.1 goes, and with it `ID3D9VkInteropDevice`: our helper's GPU transport
  stops working. DLSS 5 and DLAA through the helper should be turned off under Remix. Remix has its
  own DLSS SR, Ray Reconstruction, frame generation and (in 1.6.1, `nvngx_dlssnr.dll`) DLSS 5.
- **The plugin's hooks** detour whatever device the game creates (dlss5-renderer.md, "Bring-up
  routes"), so they would hook the bridge client's device. Untested. The INTZ depth swap and the
  jitter patch must be off under Remix.
- **Bridge traffic.** Every D3D9 call crosses a process boundary. The game uploads large constant
  ranges per draw (183-register bone palettes, 208-register litter arrays, 96 panel transforms per
  car) and locks dynamic buffers from its main thread. Bridge throughput at 2,700 draws a frame is an
  open risk.
- **Vertex capture per draw.** Captured positions change every frame by construction, so the BLAS of
  every captured draw is rebuilt or refitted (*inferred* from the capture design;
  `rtx.enablePreservePath` may avoid part of it). 2,700 draws, 4,000 in the Shift overview with
  worldlod. Open risk, measured only by running it.
- **worldlod** adds detail to the upper Shift levels; more draws and triangles for Remix to take in,
  and a long sight line that anti-culling options would need to cover.
- **Engine limits** are unaffected.

Milestones:

| | Work | Weeks | Visible result |
| --- | --- | --- | --- |
| R0 | Remix x86 bridge release in a separate copy of the game on the host (the test VMs have no Vulkan driver, so Remix can't run there), DXVK out, `rtx.useVertexCapture=True`. Expect no ray tracing (no camera) | 0.5 | Remix menu, raster image, the log's compatibility lines |
| R1 | Plugin: `SetTransform` VIEW/PROJECTION per frame and WORLD per draw from the shader layouts; hide the shadow, minimap, mirror and post passes; HUD as UI (the nfsc-rtx approach) | 2-3 | **First prototype**: path-traced daytime driving with the fallback light, flat materials, roads without markings |
| R2 | Categories (sky, particles, decals, UI, ignore); sun from the game's constants; sky light | 1-2 | Plausible daylight, stable sky |
| R3 | Roads: bake the road layers or author road materials | 3-6 | Roads that look like roads |
| R4 | Cars: paint, glass, chrome, lights (head/tail as spot lights from car transforms) | 3-5 | Cars that look better than stock |
| R5 | City materials (PBR maps in bulk with the ComfyUI workflow that replaced the Toolkit's AI tools in 1.6.1, or PBRify; then hand fixes on glass, water, signs), night lights | 4-10 | Night driving, wet look |
| R6 | Performance and stability: bridge throughput, capture cost, anti-culling, Shift overview, cutscenes | 2-4 | Playable frame rate |
| | Total | **16-30** | Plus open-ended asset work for a remaster |

Blockers to check in R0-R1, before anything else: whether the bridge client accepts the game's
`D3DCREATE_MULTITHREADED` device and its main-thread buffer locks; frame time with capture on every
draw; whether the plugin's device hooks work through the bridge.

### 1b. The plugin feeds Remix through the API

With `rtx.useVertexCapture=False`, Remix skips every one of the game's draws (all are shader draws),
so it ray traces only what the plugin submits through `remix_c.h`:

- the static city, exported offline by worldlod's parsers: meshes in object space (int16 positions
  decoded), materials with the road layers baked into albedo, textures written as DDS for the
  API's file paths, the 31,907 prop placements as instances;
- per frame: the camera (`SetupCamera`), the sun and sky, car instances from `g_ObjectToWorld` and
  the panel transforms (car meshes from `dngvehicles.sp` or from the game's buffers), characters
  as skinned meshes with bones from `g_boneMatrixArray`;
- the streamed state: which regions are loaded, or simply everything within a radius.

It removes the camera and matrix guessing, the hash instability and the road problem, and makes
instancing exact. It costs a second scene description that must match the game's: the same
regions, the same cars, the same animation. The API works from 32-bit games through the bridge
since Remix 0.6.0 (above). Open: how the game's HUD is composited over Remix's output when none of
the game's draws is ray traced, and how much bridge traffic submitting the scene every frame
costs (meshes are created once; instances and lights are per frame). Weeks: 6-9 to the first prototype (static city around the
car), 24-40 for a result comparable to 1a's "good".

## Option 2: hybrid ray-traced effects in the D3D12 helper

The helper already shares textures and fences with the game under DXVK, runs at the scene end
(`Input=1` path) and has the depth, the VP of this and the last frame, and motion. The RTX 5070 Ti
supports DXR 1.1 inline ray queries, which suit post-style passes.

### The BVH

| Source | What | Weeks | Notes |
| --- | --- | --- | --- |
| Offline, static city | worldlod's parsers export every detailed part's finest LOD in world space, and the props as instances, to a cache file the helper loads | 2 | About 8.5-10 M triangles with the building shells: a few hundred MB of vertices and indices, under 1 GB with the BLAS (estimate). Fits the 16 GB card. Independent of streaming. Alpha-tested foliage needs its masks (any-hit or opacity micromaps) |
| Runtime, cars | Car meshes (per panel), placed each frame from `g_ObjectToWorld` and `g_PanelTransforms`, which the plugin already reads for motion vectors | 2-3 | Instanced BLAS per panel; transforms only per frame |
| Runtime, characters | Skinned, 183-register bone palettes | 2-3, or skip | Skip at first: pedestrians lose contact shadows only |
| Runtime, generic capture | Mirror vertex and index buffers on `Lock`, decode the declarations, rebuild per draw | 4-6 | Only if the offline route misses too much (props spawned at runtime, traffic furniture) |

DXVK's interop gives images, not buffers, so runtime geometry crosses on the CPU (shared memory),
which is fine for car meshes uploaded once and transforms per frame.

### What each effect needs and how it gets into a forward-shaded image

| Effect | Inputs | How it goes into the frame | Weeks |
| --- | --- | --- | --- |
| RT ambient occlusion | Depth (have), normals from depth (the game has almost no normal maps: 20 of 548 shaders, so geometric normals are close to shading normals), BVH | Multiply into `T#3` at the scene end, before bloom and tone mapping, weighted so sunlit pixels darken less | 3-4 with denoising |
| RT sun shadows | Depth, sun direction (game constant), BVH | Best: patch the lit pixel shaders' PSSM lookup to read a screen-space shadow mask instead (a pixel shader version of the vertex shader patcher). The mask must exist before the scene pass: trace it from last frame's depth reprojected, or trace primary visibility from the BVH once the first world draw gives the VP. Cheaper: darken only where RT says shadow and the PSSM says lit, at the scene end | 4-6 |
| RT reflections (car paint, glass, wet roads) | Smooth normals (trace primary rays against the car BLAS for stencil-tagged car pixels), a reflectivity mask (stencil, a pixel shader MRT patch on the vehicle family, or a road mask), hit shading | Replace the cubemap term: patch the car shaders (127 unique) to drop `g_samCubeMap`/`g_samDynCubemap` into a second target, add the traced reflection at the scene end. Hit shading: on-screen hits read the frame (hybrid SSR), off-screen hits need albedo (game textures exported for the helper) plus sun, shadow and sky | 6-10 |
| RT GI | Hit radiance everywhere | The lightmaps already hold the static world's GI; gains are small for the cost | 8-12, not recommended |
| Denoising | NRD (REBLUR for AO/reflections, SIGMA for shadows): view Z, normals and roughness, motion (have). Source on GitHub under the NVIDIA RTX SDKs licence (public, not OSI open source; the helper already ships NGX under that licence) | In the helper | 2 to integrate NRD and NRI |
| DLSS Ray Reconstruction | Linear diffuse and specular albedo, normals, roughness, depth, motion (Streamline's `kFeatureDLSS_RR`, or NGX with `nvngx_dlssd.dll`) | Replaces DLAA. Needs a G-buffer the game doesn't have; only worth it after the MRT patches above | 3-4 after the MRT work |

Milestones:

| | Work | Weeks | Visible result |
| --- | --- | --- | --- |
| H0 | Export the static city from worldlod's parsers; helper loads it, builds BLAS/TLAS; debug view of the BVH from the game's camera at the scene end | 2 | BVH drawn over the frame, lined up with the city |
| H1 | RTAO at the scene end, NRD REBLUR, composite into `T#3` | 2-3 | **First prototype** (4-6 weeks with H0): grounded buildings, kerbs and props, no AO on cars yet |
| H2 | Cars in the BVH (panels, transforms) | 2-3 | AO and contact shadows under and around cars |
| H3 | RT sun shadows through a shadow-mask patch of the lit shaders, SIGMA | 4-6 | Sharp shadows to the horizon, no PSSM cascades or acne |
| H4 | Car and wet-road reflections (car pixel shader patch, hybrid SSR + RT, exported albedo) | 6-10 | Real reflections on paint and glass |
| H5 | Characters, alpha-tested foliage, worldlod's upper Shift levels, tuning | 2-4 | |
| | Total | **18-30** | |

Risks: AO and shadows composited after forward shading can't be fully right (the game already
lit those pixels), so they need a weighting tuned by eye. The offline BVH must match the drawn
geometry, or shadows float and self-shadowing shows (bias, and later the runtime capture for
mismatches). The PSSM replacement and the reflection MRT need pixel shader patching across shader
families, which the vertex shader patcher's approach covers in principle (constant tables are in
every shader) but which is more varied than one `oPos` write. The helper round trip is already
about 3 ms a frame under DXVK; each effect adds its own GPU time.

## Option 3: screen-space effects

- **ReShade, today.** Scene depth already reaches ReShade on all three routes (dlss5-renderer.md,
  "Bring-up routes"). Free options: iMMERSE MXAO (GTAO with a visibility bitmask; free to use under
  a proprietary licence) and Zenteon's ZN_FX `ZN_GI.fx` (GPL-3.0, archived). Pascal Gilcher's RTGI
  is paid (Patreon, iMMERSE Pro). It traces in screen space against the depth buffer, takes normals
  from his Launchpad shader, and "can only directly observe surfaces currently facing the camera".
  It needs DX10 or later, so in this game it runs through DXVK with ReShade as a Vulkan layer, the
  route DSF Enhanced already uses. All of them work on the final LDR image, after the grain and the
  colour grade, and over the HUD unless the plugin's pre-HUD callback is used. One evening to set
  up.
- **Native, in the helper.** XeGTAO (MIT, archived) for AO, FidelityFX SSSR (MIT) for reflections and a
  simple SSGI, run at the scene end on the HDR target with the depth and motion we already make, and
  composited the same way as option 2's AO. 2-3 weeks to the first effect, 4-8 for all three.

Screen-space effects can't see what's off screen or behind something: AO and GI fade at the screen
edges and around the car, and reflections vanish when the reflected object leaves the screen. That
is the trade-off option 2 removes, at several times the work.

## Recommendation

1. **Baseline (a day):** ReShade with depth through the existing route, MXAO or GTAO and an SSR, to
   see what the cheap end looks like in this game.
2. **Option 2, H0-H1 (4-6 weeks):** the static-city BVH and RTAO. It reuses what DSF Enhanced
   already has (the helper, the transport, the scene-end hook, depth, VP, motion, worldlod's
   parsers) and leaves DLSS 5, DLAA and DXVK in place. Decide on H3 and H4 after seeing it.
3. **Remix only as a time-boxed spike (R0-R1, about 3 weeks)** if path tracing itself is the goal.
   The spike settles the open risks (camera injection, bridge throughput, capture cost). If they
   pass, 1b (the API route) is the cleaner long-term path, but it is a remaster project of six
   months or more, mostly assets and lights.

## Sources

Local, read on 2026-10-30:

- RTX Remix 1.6.1 `remix-x64.zip` (file list; strings of the 104 KB `d3d9.dll` shim, which loads
  `d3d9_x64.dll` or `d3d9_a64ec.dll`, exports `remixapi_InitializeLibrary` and names
  `NvRemixBridge.exe`; `LICENSE.txt`, MIT). It contains `NRD.dll`, `nvngx_dlss.dll`,
  `nvngx_dlssd.dll` (Ray Reconstruction), `nvngx_dlssg.dll` and `nvngx_dlssnr.dll`. Nothing in it was
  run.
- dxvk-remix `main`: `src/d3d9/d3d9_rtx.cpp`, `src/d3d9/d3d9_rtx.h`, `public/include/remix/remix_c.h`
  (API 0.6.6), `src/dxvk/rtx_render/rtx_context.cpp`, `RtxOptions.md`
  (https://github.com/NVIDIAGameWorks/dxvk-remix).
- DXVK v3.1.1 `src/d3d9/d3d9_interfaces.h`: the D3D9 interop exposes images (`GetVulkanImageInfo`,
  `CreateImage`), the queue and locks, but no buffers.
- Game shader packages (read only, see "Static scan"), worldlod's `formats/modl.py` and
  [world-lod.md](../worldlod/research/world-lod.md), and this repository's research notes.

Remix, NVIDIA (fetched 2026-10-30):

- Compatibility: https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/introduction/intro-compatibility.html
- Issue #268, shader constants instead of `SetTransform` (NFS Carbon), NVIDIA's answer of
  2026-03-05: https://github.com/NVIDIAGameWorks/rtx-remix/issues/268
- Releases: 1.6.1 of 2026-10-23 (minimum driver 616.92, the ComfyUI texture workflow, sparse
  rendering with Ray Reconstruction); 0.6.0, API forwarding through the bridge; vertex-capture
  precision fixes in 1.2.4 and 1.3.6; instancing and point-instancer work in 1.0-1.5.
  https://github.com/NVIDIAGameWorks/rtx-remix/releases,
  https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-0.6.0
- Remix API: https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/documentation/RemixSDK.md
- Runtime layout (`.trex`, `NvRemixBridge.exe`):
  https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/installation/install-runtime.html;
  the bridge: https://github.com/NVIDIAGameWorks/bridge-remix
- Captures, hash stability and anchor assets:
  https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-assets.html;
  ingestion: https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-ingestion.html
- Effort: the FAQ ("months"),
  https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.4.0-0/docs/remix-faq.html; Half-Life 2 RTX
  (100+ artists since 2023), https://developer.valvesoftware.com/wiki/Half-Life_2_RTX

Community ports of shader games:

- xoxor4d: https://github.com/xoxor4d/nfsc-rtx, https://github.com/xoxor4d/gta4-rtx,
  https://github.com/xoxor4d/t4-rtx (the method), https://github.com/xoxor4d/remix-comp-base,
  https://github.com/xoxor4d/remix-comp-projects (Mirror's Edge, Black Mesa, FEAR and others)
- GTA IV RTX performance:
  https://www.techspot.com/news/110119-rtx-remix-breathes-new-life-grand-theft-auto.html,
  https://www.dsogaming.com/?p=191700, https://www.dsogaming.com/?p=191235
- PBRify: https://github.com/Kim2091/PBRify_Remix

Ray tracing SDKs and screen-space effects:

- NRD: https://github.com/NVIDIA-RTX/NRD; RTXGI 2.0: https://github.com/NVIDIA-RTX/RTXGI; RTXDI:
  https://github.com/NVIDIA-RTX/RTXDI
- DLSS Ray Reconstruction in Streamline:
  https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md; DLSS SDK:
  https://github.com/NVIDIA/DLSS
- RTGI: https://guides-martysmods-com.pages.dev/shaders/immersepro/rtgidiffuse; iMMERSE:
  https://github.com/martymcmodding/iMMERSE; ZN_FX: https://github.com/Zenteon/ZN_FX; XeGTAO:
  https://github.com/GameTechDev/XeGTAO; FidelityFX SSSR: https://github.com/GPUOpen-Effects/FidelityFX-SSSR
