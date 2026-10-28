# DSF Enhanced

An ASI plugin for Driver: San Francisco on PC. Target exe version 1.04

## What it does

- Fixes the aspect ratio and widens gameplay FOV on ultrawide screens.
- Fits Bink cinematics inside the area the game draws them in, so movies keep their proportions.
- Adds borderless windowed mode and an option to turn off VSync.
- Forces anisotropic and trilinear texture filtering. It also has mip and LOD-bias settings.
- Draws traffic cars at full detail out to their draw distance, raises other gameplay LOD thresholds, and lets you extend how far away civilian traffic spawns.
- Shrinks the distant brake and position lamp flares to small dots.
- In the garage's vehicles screen, Up/Down switch to the previous or next maker while a maker's cars are open, so every maker's cars can be browsed without going back to the maker list.
- Skips the legal-screen wait and dismisses the prompt about Ubisoft's servers, which no longer exist.
- Lifts two engine limits that bigger world or car archives run into and crash on: 45,000 resources loaded at once (`[Limits] ResourceArenaExtraNodes`, 135,000 more by default, about 3 MB) and 16,384 resource references waiting to be resolved (`[Limits] PendingResourceReferences`, 131,072 by default, 1.5 MB).

## Install

Copy three files from the release ZIP into the game folder, next to `Driver.exe`:

- `winmm.dll` (Ultimate ASI Loader)
- `dsf_enhanced.asi`
- `dsf_enhanced.ini`

Settings live in `dsf_enhanced.ini`. The defaults turn on borderless mode, 16x anisotropic filtering, trilinear filtering, and higher gameplay detail. A few values you may want to change:

- `TrafficSpawnRadius=500` sets how far from you civilian cars spawn and despawn, in metres. `350` is the game's radius; `500` is the maximum, because larger radii overflow the game's traffic pools and crash.
- `VehicleFullDetail=0` restores the game's vehicle LOD steps; `VehicleLODThresholdScale` then scales them.
- `LODEngineThresholdScale` (terrain, pedestrians, drivers) and `MapDetailLODThresholdScale` at `1.0` restore the native LOD thresholds.
- Clear `DRLFlareScale` and `DRLFlareBrightness` in `[Lights]` to keep the game's lamp flares.
- `[General] Log=0` stops the plugin from writing `dsf_enhanced.log`.

To uninstall, delete the three files. If the game already has a `winmm.dll`, from another ASI loader for example, back it up before installing. Nothing here touches `Driver.exe` or `gameconfig.txt`.

## Build

You need Windows, PowerShell, and the Visual Studio 2022 C++ build tools with the x86 toolchain and a Windows SDK.

From the repository root:

```powershell
.\build.ps1
```

This writes the release files, this README, the license, and `DSF-Enhanced.zip` to `dist/`. To install into a local copy of the game:

```powershell
.\install.ps1 -GamePath "D:\Games\Driver San Francisco"
```

`install.ps1` runs the build first if `dist/` is missing any files.

## Repository layout

- `src/` has the plugin source and the default `dsf_enhanced.ini`.
- `tests/` has the cinematic fit test. `build.ps1` runs it and stops if it fails.
- `third_party/winmm.dll` is the bundled [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), MIT-licensed.

## License

MIT. See `LICENSE`.
