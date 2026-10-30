# DSF Enhanced

Mods for Driver: San Francisco on PC (exe version 1.04). Each [release](https://github.com/xob0t/dsf-enhanced/releases/latest) has two ZIPs:

- `DSF-Enhanced-<version>.zip` is the plugin.
- `worldlod-<version>.zip` builds a detailed map for the upper Shift levels from your copy of the game. It installs the plugin too.

## What the plugin does

- Fixes the aspect ratio and widens the FOV on ultrawide screens. Bink cinematics keep their proportions.
- Adds borderless windowed mode, and an option to turn VSync off.
- Forces 16x anisotropic and trilinear filtering.
- Raises detail. Traffic cars stay at full detail out to their draw distance, traffic spawns up to 500 m away, and sun shadows use 3072x3072 shadow maps instead of 1536x1536.
- Shrinks the flares of distant brake and position lamps to small dots.
- Shows your passengers in the cockpit view. The game hides them, so after a Shift into a full car they vanish.
- In the garage, Up and Down switch to the previous or next maker while a maker's cars are open.
- Plays Film Director replays without the "old film" overlay (grey-green tint, scratches, dust).
- Skips the legal-screen wait and the prompt about Ubisoft's servers, which are gone.
- Raises engine limits that bigger maps and car packs crash on. Without this, entering Film Director with worldlod's map crashes.
- Experimental: NVIDIA DLAA anti-aliasing and DLSS 5, see below.

## Install

Copy `winmm.dll`, `dsf_enhanced.asi` and `dsf_enhanced.ini` from the ZIP into the game folder, next to `Driver.exe`. To uninstall, delete them. If the folder already has a `winmm.dll` from another ASI loader, back it up first. Nothing here changes `Driver.exe` or `gameconfig.txt`.

Every setting is in `dsf_enhanced.ini`, with a comment above it. The ones you're most likely to change:

| Setting | Default | |
|---|---|---|
| `[Quality] TrafficSpawnRadius` | `500` | How far away traffic spawns, in metres. The game uses 350. More than 500 overflows the game's traffic pools and crashes. |
| `[Quality] VehicleFullDetail` | `1` | `0` brings back the game's car LOD steps. |
| `[Quality] CockpitPassengers` | `1` | `0` hides passengers in the cockpit view, as the game does. |
| `[Shadows] ShadowMapSize` | `3072` | `4096` is a bit sharper and uses 268 MB of video memory. `0` keeps the game's size. |
| `[Display] RemoveVSync` | `0` | `1` turns VSync off. |
| `[Replay] DisableFilmEffect` | `1` | `0` keeps Film Director's old-film look. |
| `[General] Log` | `1` | `0` stops writing `dsf_enhanced.log`. |

## DLAA and DLSS 5 (experimental)

Both need an NVIDIA RTX GPU. The game is 32-bit and NVIDIA's runtimes are 64-bit, so a small helper in the ZIP's `dlss5` folder runs them.

- DLAA is NVIDIA's anti-aliasing at native resolution. It smooths the power lines, fences and foliage that the game's own anti-aliasing leaves jagged and crawling. Any RTX card. About 2.3 ms a frame at 3440x1440.
- DLSS 5 relights the picture with NVIDIA's neural model. In this game the change is subtle: firmer shading, smoother grain. RTX 50-series only. About 7 ms a frame at 3440x1440.

To set it up:

1. Copy the ZIP's `dlss5` folder into the game folder.
2. Put NVIDIA's DLL into it. They aren't in the ZIP, NVIDIA's license doesn't allow it.
   - For DLAA, `nvngx_dlss.dll` 310.9.16 from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS/tree/v310.9.16/lib/Windows_x86_64/rel).
   - For DLSS 5, `nvngx_dlssnr.dll` from the top level of the x64 ZIP in [RTX Remix 1.6.1](https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.6.1).

   The helper accepts only these exact files. Their hashes are in `dlss5\README.txt`.
3. Put DXVK 3.1.1's `x32\d3d9.dll` next to `Driver.exe` ([download](https://github.com/doitsujin/dxvk/releases/tag/v3.1.1)). It works without DXVK, but every frame then goes through the CPU and costs about 15 ms more.
4. In `dsf_enhanced.ini`, set `[DLAA] Enable=1`, and for DLSS 5 `[DLSS5] Export=1` and `Helper=1`.

In game, F10 turns DLSS 5 off and on and F11 does the same for DLAA, so you can compare. A label in the corner shows the state for 2 seconds. `[DLSS5] ToggleIndicator=2` keeps a "DLSS 5" or "DLAA" label on screen while each one runs, for recording comparisons. If something fails, the game carries on without it, and `dsf_enhanced.log` says why.

Settings, costs and known limitations are in [docs/dlss5-dlaa.md](https://github.com/xob0t/dsf-enhanced/blob/main/docs/dlss5-dlaa.md).

## worldlod

When you Shift up, the game draws the whole city as one low-detail model: flat ground and plain building shells. worldlod rebuilds that model from the game's own detailed areas, with real roads, kerbs, trees, building detail and painted road text. It reads your `san_francisco.dngc` and writes the new map into it, keeping a backup. No game data is shipped.

Stock on the left, worldlod on the right:

![A plaza and a building site, stock and worldlod](worldlod/research/img/close-plaza.jpg)

![City blocks and a park, stock and worldlod](worldlod/research/img/close-blocks.jpg)

To install, unzip `worldlod-<version>.zip` anywhere, close the game and double-click `install.cmd`. The first time, it asks for the game folder and installs [uv](https://docs.astral.sh/uv/) for the Python worldlod runs on. It adds the plugin if it's missing, then builds and installs the map in a minute or two. `install.cmd --uninstall` puts the original map back. Options and details are in [worldlod/README.md](worldlod/README.md).

## Building

You need Windows, PowerShell, and the Visual Studio 2022 C++ build tools (x86 and x64) with a Windows SDK. From the repository root:

```powershell
.\build.ps1                                          # everything into dist\
.\build.ps1 -SkipDlss5                               # without the DLSS 5 / DLAA helper
.\install.ps1 -GamePath "D:\Games\Driver San Francisco"
```

The helper build downloads NVIDIA's DLSS SDK headers and NGX loader, pinned by SHA-256. To release, push a version tag (`git tag v1.3.0`, `git push origin v1.3.0`). GitHub Actions then builds, tests and publishes both ZIPs.

The source is in `src/`, tests in `tests/`, the helper in `dlss5/`, the map pipeline in `worldlod/`, and design notes in `research/`.

## License

MIT, see `LICENSE`. `third_party/winmm.dll` is [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (MIT). The DLSS 5 / DLAA helper contains NVIDIA's NGX loader under the NVIDIA RTX SDKs license. This project isn't affiliated with NVIDIA.
