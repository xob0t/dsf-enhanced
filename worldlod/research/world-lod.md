# World LOD: replacing the upper-Shift base mesh

Updated: 2026-10-28

Goal: upper Shift levels (user levels 3/4, internal 4/5) draw the city's low
"base" LOD, which looks poor, mainly the ground and the textures. The plan is to
bake a better base LOD offline from the detailed regions and write it into
`Install/san_francisco.dngc` in place of the stock one, so the engine keeps its
single global base owner and none of the capacity work from the archived
full-city loader (`archive/shift-map-2026-10-03`) is needed.

Evidence tags: `static` (read from data or Driver.exe 1.04), `runtime`
(measured in game), `hypothesis`. Addresses are VAs (image base 0x400000).

Code: the `worldlod` pipeline in this folder (`../README.md`). `uv run worldlod
preview --base` renders detailed regions next to the stock base for the same cell.

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
| +0x24, +0x28 u32 | two more tables (+0x24 is indexed per model in EmitDrawMetadata) |

Instance record, 20 bytes (`ShiftSector_BuildRenderState` 0x6B7490):

| Offset | Field |
| --- | --- |
| +0x00 u32 | rotation: yaw-only when bit 31 of +0x04 is set (high u16 cos, low u16 sin, each `(v - 0x7FFF) / 32767`; high u16 0xFFFE = identity, 0x6B9610); otherwise a smallest-three quaternion (0xBA8AD0): bits 30-31 largest index, bits 28-29 index of the 10-bit component `((v >> 18) & 0x3FF) - 0x1FF) * 0.0013824`, the other two 9-bit `(v & 0x1FF) - 0xFF) * 0.0022597` from shift 9 then 0 |
| +0x04 u32 | bits 0-19 x, bits 20-30 flags, bit 31 yaw-only |
| +0x08 u16 | y |
| +0x0A u16 | model index into the UID table |
| +0x0C u32 | bits 0-19 z, bits 20-31 unknown (increase through the table) |
| +0x10 u32 | scale x/y/z = bits 22-31 / 12-21 / 2-11, each `v * 1.5 / 1023 + 0.5`; bit 1 mirrors X (0x6B9380) |

Position (0x48D4C0): `x, z = (v - 0x5A000) / 64`, `y = (v - 0x3200) / 128`, in
world space. The matrix is row-vector: rows 0-2 = rotation rows scaled by
(sx * mirror, sy, sz), row 3 = position. Flags `(+0x04 >> 20) & 0x19`: 1 and 8
select render classes; `& 0x66` is passed to the draw.

Checked on R_360: all 927 instances fall inside the cell; the flattened cell
draws coherent streets, kerbs, crossings and lamp posts.

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

Images (R_360, colour = material, flat shading, 45-degree view from the south;
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
