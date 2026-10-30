# DLAA and DLSS 5 (experimental)

The short version is in the [README](../README.md#dlaa-and-dlss-5-experimental). This page has the details.

The game is 32-bit and NVIDIA's runtimes are 64-bit, so a small helper, `dlss5\dsf-dlss5-helper.exe`, runs them next to the game. It ships in the release ZIP. NVIDIA's DLLs don't, and can't. You download them yourself, and the helper accepts only the exact files listed below.

- DLAA is NVIDIA's DLSS anti-aliasing at the game's own resolution. The game's anti-aliasing is a filter on the finished frame, so power lines, poles, fences and foliage stay jagged and crawl when the camera moves. DLAA combines several frames, each drawn with the 3D scene shifted by a fraction of a pixel, and fixes that. Any RTX GPU (20-series or later).
- DLSS 5 (NVIDIA's 3D-Guided Neural Rendering) relights the picture with NVIDIA's model. In this game the change is subtle: firmer shading on buildings and roads, smoother film grain. RTX 50-series only, with a driver that supports DLSS 5 (tested on 621.01).

They are independent. With both on, DLAA runs first and DLSS 5 works on DLAA's picture. Both are off by default.

## Files

Copy the ZIP's `dlss5` folder into the game folder, so you have `dlss5\dsf-dlss5-helper.exe` next to `Driver.exe`. Then add the DLL for what you want into that folder.

| For | File | Where to get it | Size | SHA-256 |
|---|---|---|---|---|
| DLAA | `nvngx_dlss.dll` 310.9.16.0 | [NVIDIA/DLSS v310.9.16](https://github.com/NVIDIA/DLSS/tree/v310.9.16/lib/Windows_x86_64/rel), the `rel` copy, not `dev` | 58,956,912 bytes | `d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e` |
| DLSS 5 | `nvngx_dlssnr.dll` 310.9.16.0 | top level of `rtx-remix-for-x64-games-release-remix-1.6.1.zip` (557 MB) from [RTX Remix 1.6.1](https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.6.1) | 266,424,944 bytes | `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f` |

To check a file, run `Get-FileHash dlss5\nvngx_dlss.dll` in PowerShell in the game folder. The helper also checks NVIDIA's signature and the version before it loads either file.

### DXVK

Use DXVK 3.1.1. Without it every frame goes through the CPU, which costs about 15 ms more.

1. Download `dxvk-3.1.1.tar.gz` from [doitsujin/dxvk v3.1.1](https://github.com/doitsujin/dxvk/releases/tag/v3.1.1) (SHA-256 `40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608`).
2. Unpack it with `tar -xf dxvk-3.1.1.tar.gz`. Windows has `tar`.
3. Copy `dxvk-3.1.1\x32\d3d9.dll` next to `Driver.exe` (SHA-256 `265888c31ca78dffa290c39cb7e50bfb02762590e41927906e46fb32f01497fa`). Take `x32`, the game is 32-bit. If the folder already has a `d3d9.dll`, from ReShade for example, back it up. Only one file can have that name.

## Turn it on

In `dsf_enhanced.ini`:

```ini
[DLAA]
Enable=1

[DLSS5]
Export=1
Helper=1
```

Start the game. When a level loads, the helper starts and the picture changes after a second or two. If the helper can't start or rejects a DLL, the game carries on without it. `dsf_enhanced.log` (`DLSS5 helper:` and `DLAA:` lines) and `dsf-dlss5-helper.log` say why.

To turn it off, set the keys back to `0`. To remove it, also delete the `dlss5` folder and DXVK's `d3d9.dll`.

## Hotkeys

F10 turns DLSS 5 off and on, and F11 does the same for DLAA (`ToggleKey` in `[DLSS5]` and `[DLAA]`). They work for whatever was on when the game started, while the game window is in front. The helper keeps running while a feature is off, so turning it back on is instant. The first frame or two after that start from a fresh history.

`[DLSS5] ToggleIndicator` controls the label in the top left corner:

- `1` (default) shows "DLSS 5 ON" or "DLAA OFF" for 2 seconds after a press. Green is on, red is off, yellow is on but not running this session (the log says why).
- `2` keeps "DLSS 5" and "DLAA" on screen on every frame each one actually runs, and shows nothing while it's off or has stopped. Use it to label recorded comparison footage.
- `0` hides it.

Every press is logged on a `Toggle:` line. `ToggleKey` takes a key name (F1-F24, A-Z, 0-9, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock) or a virtual-key code (`0x79` is F10). Empty or `0` means no key.

## Settings

Changes need a game restart.

`[DLAA]`:

- `Enable=1` also turns on what DLAA needs. The 3D scene gets the sub-pixel shift (the HUD, shadows, minimap and mirrors don't). The game's own anti-aliasing (MLAA) is skipped on frames DLAA handled (`DisableMLAA`). The 4x MSAA the game asks for on the swap chain is dropped, since it only costs video memory (`DisableSwapChainMSAA`). Set either of those to `0` to keep it.
- `Preset` is the model. `K` is the default. `L` is sharper and steadier with less ghosting, at about twice the GPU time.
- `MipBias` (default `-1`) sharpens textures in the shifted scene, as NVIDIA recommends. Use `0` if fine textures shimmer.
- `JitterPhases` (default 16) is the length of the shift cycle.
- `Jitter=1` and `JitterDebugPixels` are for testing without `Enable`. The picture just shimmers.

`[DLSS5]`:

- `Input` picks what DLSS 5 sees.
  - `0` (default) is the finished frame before the HUD, with the game's colour grade, grain and bloom already in it.
  - `1` is the 3D scene before the game's post-processing, as RTX Remix does it. The game adds its bloom, colour grade, grain and HUD afterwards, so its warm grade survives. With `Input=0` and a high `ToneStrength`, DLSS 5 tends to wash the grade out.
- `HdrExposure` (default `0.6`, `Input=1` only) sets how bright the image DLSS 5 gets is. It's undone on the way back.
- `Style` (0-2, the model), `Intensity`, `ToneStrength` and `StructureStrength` (0-1) are RTX Remix's settings, with its defaults (0, 1, 0.3, 0.7). On this game the defaults change the picture by about 3-5 of 255 per colour channel on average. `Style=1`, `ToneStrength=1` and `StructureStrength=1` give the strongest change, about 5-13 of 255.

These `[DLSS5]` keys apply to DLAA too: `HelperPath`, `HelperRuntime`, `HelperLog`, `HelperTransport`, `FrameTimeLog`, `ObjectMotion`, `CockpitDepth` and `ToggleIndicator`.

## Cost

Measured on an RTX 5070 Ti with DXVK at 3440x1440. The game waits for the round trip to the helper, so that's what the frame pays.

| | GPU time | Round trip per frame |
|---|---|---|
| DLAA, preset K | about 1 ms | 2.2-2.4 ms |
| DLSS 5 | 5-6 ms | about 7 ms |

Preset L is estimated at twice DLAA's GPU time. Without DXVK, add about 15 ms. DLAA uses 150-200 MB of video memory and saves about 80 MB by dropping the swap chain's MSAA. `[DLSS5] FrameTimeLog=1` logs the costs.

## Known limitations

- Experimental, tested on one RTX 5070 Ti with DXVK. That covers single-player driving (chase, cockpit and side views, the Shift zoom, the pause menu) and Film Director replays, at 1280x720 and 3440x1440 borderless, DLAA preset K, and DLSS 5 with both `Input` values. Split screen, preset L, the CPU path without DXVK and other GPUs are untested in game.
- Cars, the player's car and the car interior get their own motion vectors. Pedestrians, the driver's hands, wheel spin, sparks, smoke, loose panels and scrolling textures use camera motion and can leave short trails when they move fast.
- If DLAA stops (no helper, a rejected DLL, a stalled or killed helper), the shift stops and the game's MLAA comes back by the next frame. The frame on which it fails is shown shifted without DLAA.
- Split screen keeps the game's MLAA. DLAA stays off when the scene is drawn in more than one view.
- In Film Director replays a few of the game's vertex shaders use the constant the shift needs. Those are drawn without the shift, so DLAA smooths them a little less.
- With `Input=1`, the game's quarter-resolution particle layer is added after DLSS 5, so DLSS 5 doesn't process it.
- Bink cinematics and loading screens should be skipped like the HUD but weren't tested with DLSS 5 on.
- Only the exact NVIDIA DLLs above are accepted. A newer NVIDIA release needs a newer helper.

## License and how it works

The helper contains NVIDIA's NGX loader from the NVIDIA DLSS SDK, under the NVIDIA RTX SDKs license (`dlss5\NVIDIA-RTX-SDKs-LICENSE.txt` in the ZIP). NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. This project is not sponsored or endorsed by NVIDIA.

How it works: [research/dlss5-renderer.md](../research/dlss5-renderer.md), [research/dlaa.md](../research/dlaa.md) and [dlss5/README.md](../dlss5/README.md).
