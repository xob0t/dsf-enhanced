// What DSF Enhanced publishes for a DLSS 5 consumer when [DLSS5] Export=1
// (research/dlss5-renderer.md, "Export"). Plain C layout, the same in 32- and 64-bit builds,
// so the 64-bit helper can map it too. A prototype: the helper's protocol will replace it.
//
// Shared memory: a page-file mapping named "Local\DSFEnhanced.DLSS5.<pid>" (the game's
// process id) holding one Dsfe5Shared. `sequence` is a seqlock: it is odd while DSF Enhanced
// writes `frame`; read it, copy `frame`, read it again, and retry if it changed or is odd.
//
// In process: dsf_enhanced.asi exports
//   int __cdecl DSFE_RegisterPreHudCallback(Dsfe5PreHudCallback callback, void* user);
//   int __cdecl DSFE_UnregisterPreHudCallback(Dsfe5PreHudCallback callback, void* user);
// (GetModuleHandleA("dsf_enhanced.asi") + GetProcAddress). The callback runs on the game's
// render thread, inside the frame, with the game's own D3D9 device and state.
//
// Matrices are the game's vertex-shader constant rows as uploaded (row r = register base+r),
// so clip = (dot(row0, p), dot(row1, p), dot(row2, p), dot(row3, p)) for a world-space point
// p = (x, y, z, 1). D3D9 clip space: x, y in -w..w, y up, z in 0..w.

#pragma once
#include <stdint.h>

#define DSFE5_SHARED_NAME_FMT "Local\\DSFEnhanced.DLSS5.%lu"
#define DSFE5_MAGIC 0x35455344u   /* 'DSE5' */
#define DSFE5_VERSION 1u

typedef enum Dsfe5Point
{
    DSFE5_POINT_SCENE_END = 1,  /* the HDR scene target is complete (opaque and transparent), before post-processing */
    DSFE5_POINT_PRE_HUD = 2     /* post-processing and tone mapping done, the HUD not drawn yet */
} Dsfe5Point;

#define DSFE5_FLAG_VIEW_PROJ 0x1u       /* view_proj holds this frame's scene matrix */
#define DSFE5_FLAG_PREV_VIEW_PROJ 0x2u  /* prev_view_proj holds the previous frame's */

#pragma pack(push, 4)
typedef struct Dsfe5FrameInfo
{
    uint32_t size;               /* sizeof(Dsfe5FrameInfo) */
    uint32_t point;              /* Dsfe5Point of the last update */
    uint32_t frame_index;        /* DSF Enhanced's present counter */
    uint32_t flags;              /* DSFE5_FLAG_* */
    uint32_t width, height;      /* scene colour target size */
    uint32_t render_width, render_height; /* back buffer size */
    float view_proj[16];         /* rows 0..3 */
    float prev_view_proj[16];
    float jitter_pixels[2];      /* always 0: the game does not jitter, and DLSS 5 does not read jitter */
    float reserved[6];
} Dsfe5FrameInfo;

typedef struct Dsfe5Shared
{
    uint32_t magic;              /* DSFE5_MAGIC */
    uint32_t version;            /* DSFE5_VERSION */
    uint32_t size;               /* sizeof(Dsfe5Shared) */
    volatile uint32_t sequence;  /* seqlock over frame */
    uint32_t present_count;
    uint32_t scene_end_count;
    uint32_t pre_hud_count;
    uint32_t pre_hud_missed;     /* frames whose scene ended but whose pre-HUD point was not found */
    Dsfe5FrameInfo frame;
} Dsfe5Shared;
#pragma pack(pop)

#ifdef __cplusplus
struct IDirect3DDevice9;
struct IDirect3DSurface9;
typedef void (__cdecl* Dsfe5PreHudCallback)(void* user, IDirect3DDevice9* device, IDirect3DSurface9* color,
    IDirect3DSurface9* depth, const Dsfe5FrameInfo* info);
#endif
