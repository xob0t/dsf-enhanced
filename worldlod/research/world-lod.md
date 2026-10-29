# World LOD: replacing the upper-Shift base mesh

Updated: 2026-10-29

Goal: upper Shift levels (user levels 3/4, internal 4/5) draw the city's low
"base" LOD, which looks poor, mainly the ground and the textures. The plan is to
bake a better base LOD offline from the detailed regions and write it into
`Install/san_francisco.dngc` in place of the stock one, so the engine keeps its
single global base owner and none of the capacity work from the archived
full-city loader (`archive/shift-map-2026-10-03`) is needed.

Evidence tags: `static` (read from data or Driver.exe 1.04), `runtime`
(measured in game), `hypothesis`. Addresses are VAs (image base 0x400000).

Code: the `worldlod` pipeline in this folder (`../README.md`). `worldlod render
--cell R_360` draws the stock and the built base LOD over the same cell.

## Archive layout (static)

`san_francisco.dngc` root: 566 `GEIR` regions (`R_xxx`), one `GEGR`
(`Region_Global`), `GEGL` (grid mapping, see the archive branch's
`gegl_mapping.md`), `RNLP`, `BINF`, `DMWK`. Every region is `MDXN` (renderer
resources: `DRSP` groups of `VXCK`/`IXCK`/`SBMC`/`TEXC`/`MACO`/`GPCH`) plus
`NONR` (`MODL`, `GEBI`, `SCND`, `GRTD`, ...). This is the same container family
as the cars in driversf-forza (`research/vehicle-model.md`), so its CHNK
reader/writer and VXCK/IXCK/SBMC knowledge apply directly.

Tag names from driver-tools: GEBI BuildingInstanceData, SCND RegionSceneData,
GRTD RegionTerrainData, GEPM PhysicsModelData, PROP PropData, HINF HeightInfo.

### GEGR is more than the low LOD

| Content | Size |
| --- | --- |
| 527 materials (`MACO`), 483 shaders (`GPCH`), 172 textures | DRSP 0-11 |
| 2,700 MODL parts: base LOD blocks plus a prop library | DRSP 12-34 geometry |
| 7,564 submodels, 1.12M vertices, 2.5M triangles (index count) | |

All 445 materials used by detailed regions live in GEGR, so baked geometry can
use them unchanged. Regions have no MACO or GPCH of their own. Regions also
instance 377 GEGR mesh ranges (24k triangles, 31,907 placements); these are the
shared props (lamp posts, trees, ...), drawn with a per-region second vertex
stream (`SBBH +0x18`, stride-4 colour).

DRSP groups hold at most 1,100 children and are about 2.6 MB each; the writer
must keep both limits (driversf-forza `research/spool-limits.md`).

## Formats (static)

### MODL

Same as the vehicle MODL: `{u32 parts, u32 lods, u32 lod_table, u32 0}`, parts
x 0x60 `{float box[16], u64 uid, u32 lod_count, u32 lod_offset, ..., u32
trailer @+0x5C}`, LOD records `{u32 mask, u32 count, u32 uid_list, u32
extra_list}`. For world parts the box is a corner (row 0) plus three full edge
vectors (rows 1-3), not half-axes.

LOD masks seen: regions mostly 2 (also 4/8/16/6); GEGR 7 (1,798 parts), 2
(806, the prop library), 6+2, 1+7. Trailer values: GEGR 9 (building
megamesh), 1 and 5 (terrain, sea, trees), 17, 3. Meanings of mask bits and
trailer are not decoded.

### Vertex positions

`int16 x4` positions are 1/64 m (measured against the MODL boxes: raw extent =
64 x box size; matches the archive branch's shader capture). `w` is 64 on the
stride-12 terrain buffers and varies on building buffers (packed data,
unknown). Some GEGR buffers use `float16 x4` or `float32 x3` positions.

### SCND

`+0x14 float cell_size (512), float min_x, float min_z`. Cells are centred on
multiples of 512 (R_360: x -1280..-768, z 768..1280).

GEGR's SCND (2,992 bytes) is a quadtree over its GEBI instances
(`dsfworld/formats/scnd.py`): `+0x00` node offset (0x30), `+0x04` offset of a
u16 list of instance indices, `+0x08` node count (341: five levels), `+0x0C`
instance count (778), `+0x10` 2, `+0x14` side (14,848 m), `+0x18`/`+0x1C`
minimum x, z (-7,424). A node is `{u16 start, u16 count}` into the list.
Level L's nodes follow the levels above (offsets 0, 1, 5, 21, 85) and cover a
2^L x 2^L grid in Morton order, x in the even bits (checked on nodes 45, 97,
98, 99). Stock instances sit in the leaf under their centre unless they are
much bigger than it. `ShiftSector_BuildRenderState` walks this list (sector
`+0x2C14`) once per GEBI instance, so a GEBI instance that isn't in it makes
the engine read past the list: a build with 86 extra instances and no SCND
change stopped drawing while GEGR loaded (`vm1-20261029-053658`).

### GEBI (decoded, static)

Header (version 16; `GebiResourceWrapper_LoadAndDispatchAdd` 0x48C2B0 rebases
`+0x04`, `+0x20`, `+0x24`, `+0x28`):

| Offset | Field |
| --- | --- |
| +0x04 u32 | offset of the instance records |
| +0x08 u16, +0x0A u16 | cell column, row (world node `+0x0C`; GEGR has 0xFFFE, 0xFFFE) |
| +0x0C u16 | instance count (loader ORs 0x8000) |
| +0x0E u16[9] | cumulative counts per category; instances in [+0x10, +0x16) and [+0x18, count) get render class 6 |
| +0x1E u16 | model UID count |
| +0x20 u32 | offset of the u64 model UID table |
| +0x24, +0x28 u32 | a u16 per instance and one more, padded to 16 bytes: ranges `[t[i], t[i+1])` of 32-byte records at +0x28 that EmitDrawMetadata hands to FUN_006BBB30 (per-instance constants). GEGR's are all zero, +0x28 empty |

Instance record, 20 bytes (`ShiftSector_BuildRenderState` 0x6B7490):

| Offset | Field |
| --- | --- |
| +0x00 u32 | rotation: yaw-only when bit 31 of +0x04 is set (high u16 cos, low u16 sin, each `(v - 0x7FFF) / 32767`; high u16 0xFFFE = identity, 0x6B9610); otherwise a smallest-three quaternion (0xBA8AD0): bits 30-31 largest index, bits 28-29 index of the 10-bit component `((v >> 18) & 0x3FF) - 0x1FF) * 0.0013824`, the other two 9-bit `(v & 0x1FF) - 0xFF) * 0.0022597` from shift 9 then 0 |
| +0x04 u32 | bits 0-19 x, bits 20-30 flags, bit 31 yaw-only |
| +0x08 u16 | y |
| +0x0A u16 | model index into the UID table |
| +0x0C u32 | bits 0-19 z, bits 20-31 the instance's index (GEGR: 0..777) |
| +0x10 u32 | scale x/y/z = bits 22-31 / 12-21 / 2-11, each `v * 1.5 / 1023 + 0.5`; bit 1 mirrors X (0x6B9380) |

Position (0x48D4C0): `x, z = (v - 0x5A000) / 64`, `y = (v - 0x3200) / 128`, in
world space. The matrix is row-vector: rows 0-2 = rotation rows scaled by
(sx * mirror, sy, sz), row 3 = position. Flags `(+0x04 >> 20) & 0x19`: 1 and 8
select render classes; `& 0x66` is passed to the draw.

Checked on R_360: all 927 instances fall inside the cell; the flattened cell
draws coherent streets, kerbs, crossings and lamp posts.

`ShiftSector_BuildRenderState` fills fixed arrays of the region's render
state: 1,300 instances (matrix at float index 0x28AC, cell 0x79EC, lightmap UID
0x8700, two render class bytes) and 2,048 model indices (0x7C76). It takes
each instance's cell from its position (`row * 29 + col`) and the cell's
lightmap name from it. GEGR uses 778 instances and 1,838 model UIDs.

## What the base LOD is (static)

GEGR's GEBI places 778 instances, all scale 1 at y 0, one or more per cell
centre. Per cell there is a building block (trailer 9) and a terrain block
(trailer 1 or 5). By material family (instanced, most detailed LOD):

| Family | Base triangles | Detail triangles |
| --- | ---: | ---: |
| Buildings\Specular | 440,064 | 8,655,693 |
| Terrain\VertColourLowPolyTerrrain | 166,411 | 0 |
| Buildings\Not_Atlased, Opacity | 24,874 | 1,004,884 |
| HyperLowTree / trees and foliage | 4,118 | ~2.7M |
| Water\LowPolySea_01 | 1,207 | |
| Terrain\Blends, TERR_*, Roads\* | 0 | ~1.4M+ |
| Total | 636,686 | 14,793,987 (153,875 instances) |

So the upper-Shift ground is a vertex-coloured heightfield with no textures
(`VertColourLowPolyTerrrain`, stride-12 position + one ubyte4 element). The
building shells are the base megamesh with atlas materials
(`alley_01_cream_groundfloor`, `ALLEY_01_Cream_Window`, `ROOFTOP_CORNER`).
Material names come from `MACO/MATX`, which holds the source `.matxml` path.

Images (R_360, from an early preview stage since removed, colour = material,
flat shading, 45-degree view from the south;
`img/world-lod-R_360-base.png`, `img/world-lod-R_360-detail.png`). The base
terrain looks speckled only because the preview point-samples its huge
triangles sparsely. The detail cell has roads,
kerbs, paving, the freeway deck, props and some building detail; the large
building shells are only in the base.

## How the engine switches base and detail (static)

`ShiftSector_EmitDrawMetadata` (0x6BACB0), for a base descriptor
(`descriptor +0x10` set) and an instance in render group 0: it reads the model's
packed cell id from render state `+0x1E7B0`, then searches the live detailed
world nodes. If that cell is resident the model keeps its technique key; if not,
the key becomes `DAT_00F4069C` (a "far" technique). Group 1 always uses the far
key. So the base shells are drawn at every level and only the technique
changes, which matches the user's observation that tall buildings are shared
across Shift levels and only their textures change. How the base terrain is
hidden under resident detail is not traced yet.

## Per-region textures (static)

Each GEIR carries up to two textures:

- 528x528 DXT1 with mips (532 regions): a top-down map of the cell, red on
  buildings and yellow-green on streets, darker next to buildings. Probably
  baked sky visibility or shadow (`hypothesis`).
- 512x512 DXT5 (316 regions): an atlas of small lightmap patches, used by
  detail meshes through their second UV set (semantic 9) (`hypothesis`; the
  archive branch identified per-cell `g_texLightMap` binding).

Total 182 MB. None of it is resident for distant cells.

## Implications for the bake

1. Ground first: replace each cell's terrain block (`VertColourLowPolyTerrrain`)
   with the region's ground geometry (Terrain\*, Roads\*, freeway decks),
   flattened to world space, merged per material and simplified. Merging per
   cell and material gives about 12,640 draws for everything; ground only is far
   fewer.
2. Keep the building shells. Their upper-level look is the far technique;
   improving it is a separate problem (per-cell textures or a different
   technique for non-resident cells).
3. Props: drop small ones, keep large trees (maybe simplified).
4. Lighting: detail materials expect the per-region colour stream and, for some,
   the per-cell lightmap. Bake into vertex colour where the material allows.
5. GEGR is always resident (gameplay too) in a 32-bit process: keep the added
   geometry modest.

## Open questions

- LOD mask bits and the MODL trailer: which parts each Shift level draws.
- What hides the base terrain block when its cell is resident, and what the
  far technique `DAT_00F4069C` samples.
- Whether detail materials render correctly from the base owner without their
  region's colour stream and lightmap.
- GEBI tables at `+0x24`/`+0x28`, record `+0x0C` high bits.
- What the 95 region parts that GEBI doesn't place are for (6k triangles in R_360).
- Re-packing a modified GEGR (the unchanged archive already round-trips, below).

## Re-packing (static)

`worldlod pack --verify` rewrites `san_francisco.dngc` from its parsed tree and
gets a byte-identical file (532,107,264 bytes, SHA-256 equal). It needed one
rule the driversf-forza writer lacks: 190 `NONR` containers (e.g. R_208's)
end at their last entry's own alignment (PVSR, 16 bytes) instead of being
padded to the largest alignment among their entries (128). The reader records
this per container (`pad_end`) and the writer reproduces it. dngvehicles.sp
only has the padded form, so the forza writer would grow this archive by
20,480 bytes.

## Writing GEGR (static, verified offline)

What the bake needs to know, and how `dsfworld/gegr.py` uses it:

- **Techniques.** `MACO/MARC/MARH` starts with a technique key; `+0x10` is the
  u64 UID of the GPU program (`GPCH/GPCI`), whose `GPVD` lists the vertex
  inputs in VDCL form. All 311 detailed ground materials have the same six
  keys as the stock terrain material (`4E7019C2`, `81A34B93`, `F59DDF6D`,
  `55B3FEFD`, `13DC0D80`, `71240C6E`), so the base's far technique finds them.
- **Ground vertex inputs.** The main technique wants `(0, 5, 8, 9)` or
  `(0, 5, 8)` (position, normal, two UV sets) or `(0, 8, 19)` (colour in the
  buffer). Semantic 9 is a second tiling UV, not a lightmap UV. Only 7 ground
  placements in the city use the separate per-region stream (`SBBH +0x18`);
  the bake folds that stream into the copied vertices.
- **Part trailer = per-cell binding mode** (`FUN_006BA750`): bit 2 (4) looks up
  a per-cell material binding (passed whether the cell is resident), bit 3 (8)
  a per-model binding, bit 5 (0x20) a cached command list. Stock terrain parts
  have trailer 1 or 5, buildings 9. Baked ground mostly has flag 5, like the
  trailer-5 stock terrain, so distant cells take the same path.
- **Base instance flags.** Terrain and sea instances have GEBI flags 1, which
  `EmitDrawMetadata` always gives the far technique; buildings have 0x80 (render
  group 0: technique by cell residency). Technique key `F59DDF6D` gets no
  per-cell binding.
- **MODL layout:** header, parts, LOD table, all UID lists, then all flag lists.
  `build_modl` reproduces all 533 stock MODLs byte for byte.
- **VXDP** is n dependent submodel UIDs then n zero bytes; **IXDP** n UIDs;
  **IXHD** ends in 8 bytes copied from a stock IXHD.
- **MDID** lists every renderer UID of GEGR (483 GPCH, 527 MACO, 172 TEXC,
  7,564 SBMC, 18 VXCK, 6 IXCK, and 106 UIDs interleaved with the GPCH that
  match nothing). New UIDs go after the last stock UID of their kind.
- **New UIDs** follow driversf-forza's stock-looking form (byte 1 0x12, byte 7
  0x2D, top bit of byte 7 clear: the engine marks resolved slots with it),
  prefix "WLD", checked against the 197,118 UIDs in the archive.

Stock terrain coverage: 420 of the 532 detailed cells have exactly one terrain
part centred on the cell. 136 of those bake no ground: sea, bay bridge and
container yards, whose detail uses the same low-poly terrain material; they keep
their stock terrain. The 112 cells without a terrain part hold 2,784 ground
triangles in all (sea edge and the map border).

Result of the first bake (ground only, not simplified, no fill; 2026-10-28,
since superseded): 284 parts replaced, 7,480 submodels, 1,572,683 triangles,
42.4 MB of buffers in 23 new DRSP groups; the archive grew from 532 MB to
576 MB. `pack --verify` re-reads it and finds every reference intact,
and a preview of the packed archive drew the new ground under the stock building
shells.

### Open for the in-game test

- (Answered above: the base is not drawn at Shift level 2 and below.) Whether the engine hides the base terrain where a cell's detail is loaded.
  The stock terrain is not consistently below the detail ground (R_360: median
  2 m below, above in 14% of 4 m bins; R_208: above in 65%), so something
  must hide it in gameplay; it is not the GRTD (a 24 x 24 grid of 21.3 m cells,
  probably heights). `[bake] height_offset` moves the baked ground: positive
  (default 0.05 m) puts it above, so in upper Shift the base hides the few
  loaded detail cells and the map looks uniform; negative lets loaded detail
  win, which is safer if the base turns out to draw in gameplay too.
- What the per-cell binding (lightmap) does for a distant cell with the
  detailed ground materials.
- Memory: GEGR is resident in gameplay too; that first bake added 42 MB of
  managed buffers to a 32-bit process (the current default adds 239 MB; see
  the submodel table note below for the peak).

## When the base is drawn (runtime, user observation)

(Corrected by measurement on 2026-10-29, "Draws per frame" below: the base is
also drawn at street level, for distant cells and in secondary viewports.)

The base LOD is drawn only at the upper Shift levels, together with the detailed
cells the game loads around the cursor. At user level 2 and below the global
map is despawned. So the baked ground can't z-fight with gameplay; at the upper
levels it overlaps only the cells loaded around the cursor. `[bake]
height_offset` (default +0.05 m) puts the bake above them, so the map looks the
same inside and outside the loaded area.

## Bake fixes and optimisation (static, verified offline)

- **Stock terrain parts share their LOD with other stock geometry.** 160 of the
  420 terrain parts also hold the shared sea (`water/lowpolysea_01`), 22 the
  low-detail trees, 10 both. The bake replaces only the submodels whose material
  matches `[bake] replace` and keeps the rest; replacing the whole LOD had
  removed the sea around those cells.
- **Detail regions draw the low-poly terrain material too** (119 regions, 27.9
  km^2: hills, waterfront, the bridge approaches), so it is class `ground`, not
  `skip`.
- **Gap fill.** Some cells' detail has little ground (R_703, the bay bridge:
  one parking garage). The bake keeps the stock terrain triangles that the baked
  geometry leaves at least 5% uncovered seen from above (2 m grid), moved down
  by `[bake] fill_drop` (2 m) so they stay under the baked ground where the two
  meet. Stock-base area left uncovered per cell went from up to 96% (median
  0.1%) to at most 0.3%.
- **Simplification** (meshoptimizer, attribute-aware, absolute error). Ground is
  thin strips of different materials (lanes, kerbs, trims, paving): 78-99% of a
  material's vertices lie on its boundary, so per-material simplification with
  locked borders barely helps (84% of triangles kept at 0.5 m on 30 sample
  cells). Simplifying a whole cell as one mesh turns material boundaries into
  seams, which meshoptimizer collapses on both sides together without cracks:
  69% kept at 0.5 m with normal weight 0.5, 62% with 0.1; colour weight hardly
  matters. Outer cell edges stay locked so neighbours meet.
  `meshoptimizer` 0.2.30a0 doesn't declare ctypes argtypes for
  `meshopt_simplifyWithAttributes`; `dsfworld/simplify.py` declares them.

Result with these fixes (ground only; 2026-10-28, since superseded): 403 parts
replaced, 7,995 submodels, 1,049,811 triangles (931k baked ground and 118k stock
fill; 1.57M before simplification), 30.6 MB of buffers in 20 DRSP groups.

Current default (ground, trees, building detail and road text): 408 parts get
a bake, 14,409 submodels, 7,637,965 triangles, 32 cloned materials, 239 MB of
buffers in 116 new DRSP groups. The 12 cells that bake nothing keep their part
as it is; a cell that bakes no ground (5) keeps its stock terrain submodels
next to what it bakes.

## Trees and building detail (2026-10-28)

- **Leaf-card lighting.** Of all GEGR materials, the far technique
  (0xF59DDF6D) of exactly 32 reads `g_vLightMapSingleSample`: the leaf-card
  foliage (bushes, park and urban trees). Every other material, buildings and
  props included, reads `g_vLightMapOffsetScale` like the ground. The single
  sample is set only by the dynamic-object renderers, so those 32 drew black in
  the base; `[bake] leaf_cards = "flat"` draws them as flat quads with clones of
  `trees/palm_02` (lit in game, 2026-10-28).
- **Winding.** Stock GEGR terrain and palms are wound so that
  cross(b - a, c - a) points along the stored normal (172,008 of 172,058
  terrain triangles), so the flat cards are wound that way facing up.
- **Building detail.** With `buildings` baked (2 m minimum model size, Bay and
  Carquinez girders left out): 5.0M of the 7.6M triangles, 72% of those in one
  atlas material, `alley_01_cream_groundfloor` (storefronts, rooftop units,
  signs). That material spans 48,470 distinct models, mostly placed once; the
  40 largest contribute 4%, so no per-model cut helps much. Cell-wide
  simplification of the buildings keeps 82% at 0.5 m with locked borders, 69%
  unlocked (23 sample cells): small open meshes.
- **Submodel table limit.** The engine's table of loaded submodels (UID ->
  submodel; owner pointer 0x19BBA74, table at +0x30) takes its 24-byte nodes
  from a pool of 15,000 (table +0x10..+0x57E50) with no end check, followed by
  its 15,013 buckets. The SBMC loader inserts (0xBBB6C5 -> 0xBBBE40), releasing
  a submodel erases (0xBBBCA0). The full build (21,845 GEGR submodels) faulted
  at 0xBBBE70 while loading; DSF Enhanced `[Limits] SubmodelTableExtraNodes`
  continues the pool in an extension block. With it, the VM run loaded, drove
  and Shifted; the table peaked at 15,523. Peak process memory: 1,873 MB
  virtual (ground only 1,432 MB); Driver.exe is large-address-aware.

## Draws per frame (2026-10-29)

The frame's draw queue holds 16,384 entries in Driver.exe 1.04 (driversf-forza
`research/spool-limits.md`, "Draw queue"; DSF Enhanced PR #12 raises it). The
v1.2.0 map took a 16:9 frame to 11,264 entries or more; the user's 21:9 setup
crashed on it.

### Measuring (runtime, test build only)

A test build of DSF Enhanced (PR #12 plus a read-only counter, not shipped)
hooks the queue flip FUN_00BB5780: before it flips, `manager+4` is the queue of
the frame just built. It logs each second's frame count, highest and mean
entries, and every 3 s copies one frame's 32-byte entries, with 96 bytes of the
object at entry `+0x0C`. For draws queued by `ShiftDrawMetadata_Enqueue`
(0x6BA880 -> FUN_00BB5A50) that object is the submodel, its own UID at `+0x10`,
so each draw maps to a stock GEGR submodel (the base LOD or the shared prop
library), a worldlod one (by class), a region's, or something else (cars,
people, sky, MegaMesh). The entry's first u64 is the sort key; its top byte
names the pass/viewport (the key's top 7 bits come from metadata `+0x0E`,
FUN_006C1780):

| Top byte | What |
| --- | --- |
| 0x94 | the main view |
| 0x14, 0x18 ... 0x38 | ten secondary viewports. At street level one of them renders each frame, in turn; the frame the world becomes ready renders all ten at once. They draw the base LOD (stock: 30-130 draws each) |
| 0x00, 0xA0, 0xA8, 0xAA, 0xAE, 0x0C | other passes (shadow-like 0x00 draws region detail, no base) |

VM runs: the harness of driversf-forza (`run-series.ps1 -Vm`), the user's
`dsf_enhanced.ini` (LOD scale 0.25, full-detail traffic, 500 m traffic radius),
a 1600 x 900 window; "21:9" uses the user's frustum on that window
(`TargetAspect=2.388889`, `FOVScalar=0.023453`, the values DSF Enhanced derives
at 3440 x 1440). Each run loads the save into the Shift overview, holds it
60 s, zooms down, spawns a car and drives 40 s.

### Why the map drew so much (static and runtime)

- **A draw is a submodel.** `ShiftRenderClasses_Emit` (0x6BA9B0) queues one
  draw per submodel of each LOD whose mask matches. The bake put each cell's
  ground, trees, building detail and road text, split by material, into the
  cell's stock terrain part: 14,409 submodels, about 35 per cell, where the
  stock part has one or two. Draws follow submodels, not triangles: PR #8's
  settings (+11% triangles, 14,424 submodels) drew the same.
- **The base region is visible in every viewport.** For the base descriptor
  (`+0x10` set) `ShiftSector_EmitDrawMetadata` forces LOD mask 2 and visible
  (decompiled: `local_78 = 2; local_99 = 1`), whatever the distance LOD
  (FUN_006A5010, keyed by the instance's LOD category) says; only the
  viewport's frustum culls. So a LOD mask can't keep baked parts out of a pass,
  and submodel flag 0x100 (skipped in the pass 0xF3FFF0 by
  `ShiftRenderClasses_Emit`) changed nothing in the secondary viewports (VM
  test, 2026-10-29).
- **At street level** the main view draws baked parts of cells 768 m and more
  from the car (none around it), and the secondary viewport of the frame draws
  100-700 more.
- The 29 x 29 grid that the crash stack walked (FUN_007686F0, from the
  per-viewport pass FUN_0075D300) draws models named "1".."841" (FUN_007689C0
  hashes `"%d"`), which are in MegaMesh.sp, not in the world archive: stock
  content, not worldlod's.

### What changed

- **Block parts.** Buildings, trees and props (`[bake] block_classes`) of
  each 2 x 2 block of cells (`block_cells`) are baked in the frame of the
  block's centre and merged by material into one new MODL part, placed by a
  new GEGR instance there (a copy of a terrain instance, flags 1) and listed in
  SCND's quadtree (above). int16 positions at 1/64 m reach 512 m from the
  centre, so 2 x 2 is the most; a placement that overhangs the block's edge
  stays in its cell's part. Ground stays per cell: its parts keep the cells'
  lightmap binding (PR #10 binds the instance's one cell) and the gap fill.
  Block parts lack trailer bit 4, so no cell lightmap is bound to them. Ground
  in blocks too (`block_classes` with "ground") gives 10,767 submodels without
  the atlas, but every ground placement on a block's outer edge falls back
  (1,024 m don't fit 65,535 steps of 1/64 m) and it breaks PR #10's lighting.
- **Leaf-card atlas.** The 32 leaf-card textures (DXT1; 31 at 512, one at 256)
  go into one 4096 x 2048 DXT1 atlas, copied block by block from each texture's
  own mips (8 levels, down to one block per tile, so no tile bleeds into
  another's colours); the cards' UVs are clamped and moved into their tile.
  The clones differ only by alpha-test reference then: 4 materials instead of
  32 (`[bake] leaf_card_atlas`). 5.6 MB more in GEGR.
- With `block_cells = 1` and `leaf_card_atlas = false` the build is the v1.2.0
  archive byte for byte.

| Build | Submodels | Archive | GEGR streamer budget |
| --- | ---: | ---: | ---: |
| v1.2.0 | 14,409 | 775 MB | 332 MB |
| blocks | 12,177 | 775 MB | 332 MB |
| blocks + atlas (default) | 11,183 | 780 MB | 337 MB |

### Measured (VM, entries in a frame's queue)

The highest frame of each second, from the counter's log; "peak" is the
highest of the run, which for every worldlod build is the frame where the
world becomes ready and all ten secondary viewports render the whole map at
once (the 11,264 driversf-forza logged). "Overview" is the value the Shift
overview settles at once GEGR has streamed in (the save's camera; the same
in every run). Street and driving are the highest frames after zooming down
and while driving; there traffic and people dominate and vary from run to
run, so worldlod's own draws are given too.

| 16:9 | stock | v1.2.0 | PR #8 settings | blocks | blocks + atlas |
| --- | ---: | ---: | ---: | ---: | ---: |
| peak (world ready) | 6,338-6,624 | 11,725-12,034 | 12,035 | 10,936 | 10,073 |
| overview | 1,226 | 10,570 | 10,583 | 8,920 | 8,360 |
| worldlod's draws in that overview | - | 9,500 | 9,520 | | 7,298 |
| street, highest | 3,353-3,619 | 5,721 | | | (1) |
| driving, highest | 3,700-4,927 | 7,269 | | | (1) |

(1) Not measured at 16:9: two runs stayed in the overview (below); the 21:9
runs cover street and driving.

| 21:9 frustum | stock | v1.2.0 | blocks + atlas |
| --- | ---: | ---: | ---: |
| peak (world ready) | 3,929 | 11,683 | 10,031 |
| overview after 60 s | 1,011 | 8,961 | 7,076 |
| street, highest (worldlod's own) | 4,334 | 6,461 (579) | 5,561 (355) |
| driving, highest (worldlod's own) | 3,786 | 4,282 (442) | 5,854 (455) |

The peak frame of the default build still holds 8,274 worldlod draws, 5,465
of them ground: ground per cell is now the biggest share. Runs:
`vm1-20261029-052155`, `vm2-20261029-064316`, `vm2-20261029-065945` (stock);
`vm2-20261029-052211`, `vm2-20261029-065124`, `vm1-20261029-075847` (v1.2.0);
`vm2-20261029-052829` (PR #8); `vm1-20261029-054628` (blocks);
`vm1-20261029-065831`, `vm2-20261029-074542`, `vm2-20261029-080200` (blocks +
atlas).

**Memory.** With GEGR loaded the engine's submodel table holds 12,280 entries
(v1.2.0: 15,506; the stock pool has 15,000 nodes, "Submodel table limit"
above). Peak private memory in the 21:9 runs: 1,702 MB against 1,692 MB.

**Street level.** The main view draws baked parts only of cells 396 m and more
from the car, and no block part at all (any pass, any distance), while block
parts draw at every upper-Shift level of the descent, in step with the cell
parts (about 0.23 block draws per cell draw). So from the street the distant
cells show their baked ground but not their baked buildings and trees, as in
the stock game; v1.2.0 drew those too. Not traced why; block instances sit in
coarser SCND nodes than the cells' (the deepest node that holds their 1 km
box), which the street-level walk may not visit.

**Visual check.** Offline, `worldlod render` of 1 km around R_360 draws the
same 171,712 triangles for v1.2.0 and the default build; blurred by 4 px, 0.6%
of the pixels differ by more than 30 (the preview point-samples, so unblurred
images differ in speckle). In game, the settled overview of the two builds
(`img/draws-overview-v120-vs-blocks.jpg`, crop
`img/draws-overview-crop-v120-vs-blocks.jpg`) shows the same buildings, trees
and roads; haze and glare differ between the runs. The offline render pair is
`img/draws-render-v120-vs-blocks.jpg`.

**Test harness notes.** The VM's desktop is 1600 x 900, so 21:9 is the user's
frustum on a 16:9 window. With a fully loaded worldlod overview the VM renders
2-3 frames a second and the harness's 120 ms key taps get lost: those runs
stayed in the overview (which is how the settled overview values were read).
A scratch copy of the harness with 900 ms taps zoomed and drove.
