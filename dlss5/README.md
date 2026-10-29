# DSF Enhanced DLSS 5 helper

An open-source runner for NVIDIA DLSS 5 ("DLSS 3D-Guided Neural Rendering", NGX feature DLSSNR) for Driver: San Francisco. NGX is 64-bit only and the game is 32-bit, so the pass runs in a separate 64-bit process, `dsf-dlss5-helper.exe`. The game side shares its frame with the helper as GPU textures and gets the result back the same way.

Status: phase 0. The helper, the protocol and both tests run on an RTX 5070 Ti with the signed `nvngx_dlssnr.dll` from RTX Remix 1.6.1. DSF Enhanced does not talk to the helper yet. That needs the game-side capture (pre-HUD colour, depth, motion vectors), which is separate work.

Design notes, the NGX interface and the test record are in [`research/dlss5-helper.md`](../research/dlss5-helper.md).

## What is here

| Path | What it is |
| --- | --- |
| `include/dsf_dlss5_protocol.h` | The game ↔ helper protocol: messages, shared texture slots, fences, input conventions. Plain C structs that are the same on x86 and x64. |
| `include/dsf_dlss5_pipe.h` | Message framing over the named pipe, for both sides. |
| `src/helper_main.cpp` | `dsf-dlss5-helper.exe` (x64). |
| `src/nr_d3d12.*` | The DLSS 5 pass on D3D12 through the official NGX path. |
| `src/dlssnr_defs.h` | Our own declarations of the DLSSNR feature id and parameter names. The public DLSS SDK has no header for it. |
| `src/ngx_trust.*` | Checks that `nvngx_dlssnr.dll` has a valid NVIDIA Corporation signature and is version 310.9.16 or later. |
| `tests/offscreen_test.cpp` | `dsf-dlss5-offscreen.exe` (x64): runs DLSS 5 on a synthetic scene or a saved image and writes PNGs. No window, no game. |
| `tests/client_test.cpp` | `dsf-dlss5-client-test.exe` (x86): stands in for the game. It starts the helper and drives the full 32 → 64-bit protocol with D3D11 shared textures. |
| `tests/synthetic_scene.*` | A small CPU ray-traced street scene with exact depth and motion vectors. |

## Requirements

- Windows 10/11 and the Visual Studio 2022 C++ build tools (x64 and x86) with a Windows SDK.
- To run: an NVIDIA RTX 50-series GPU and a driver that supports DLSSNR (tested on 621.01).
- `nvngx_dlssnr.dll`, which you supply yourself (see below).

## Build

```powershell
.\dlss5\build.ps1
```

Every build first runs `fetch-sdk.ps1`. It checks the NGX headers and the static NGX loader (`nvsdk_ngx_s.lib`) from NVIDIA's public DLSS SDK (https://github.com/NVIDIA/DLSS, the 310.9.16 release, pinned to commit `9981c635`) against fixed SHA-256 values. It downloads any file that is missing or does not match into `dlss5/external/`, and stops if a download does not match either. Those files are NVIDIA's and are under the NVIDIA RTX SDKs license. They are gitignored, and so is everything under `dlss5/build/`.

## Getting nvngx_dlssnr.dll

This project does not ship NVIDIA's runtime and never will. The DLSS SDK license forbids redistributing it on its own. The signed DLL ships in NVIDIA's RTX Remix release:

1. Download `rtx-remix-for-x64-games-release-remix-1.6.1.zip` from https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.6.1.
2. Extract `nvngx_dlssnr.dll` from the root of the zip into a folder of your choice, for example `out\ngx`.

The file used for testing was version 310.9.16.0, 266,424,944 bytes, SHA-256 `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f`, signed by NVIDIA Corporation. The helper and the offscreen test accept only runtimes whose SHA-256 is on the allowlist in `src/ngx_trust.h` (today only that file). The file must also carry a valid NVIDIA Corporation signature and be version 310.9.16 or later. Patched or pre-release copies therefore fail. When NVIDIA publishes a newer `nvngx_dlssnr.dll`, allow it explicitly with `--allow-runtime-sha256 <hash>`; the signature and version checks still apply. The checked file stays locked against changes until NGX shuts down, and the helper refuses to run if NGX maps any other copy.

## Run the tests

```powershell
# DLSS 5 on the synthetic scene, 48 frames, PNGs and a log in out\dlss5-test
.\dlss5\build\x64\dsf-dlss5-offscreen.exe --runtime out\ngx --out out\dlss5-test --frames 48

# DLSS 5 on a screenshot (static, zero motion, flat depth unless --depth/--mv are given as PFM)
.\dlss5\build\x64\dsf-dlss5-offscreen.exe --runtime out\ngx --out out\dlss5-still --color shot.png --frames 16

# The full cross-process chain: 32-bit client -> helper -> DLSS 5 -> back
.\dlss5\build\x86\dsf-dlss5-client-test.exe --helper dlss5\build\x64\dsf-dlss5-helper.exe --runtime out\ngx --out out\dlss5-client
.\dlss5\build\x86\dsf-dlss5-client-test.exe ... --host-creates   # the helper creates the shared textures (the DXVK direction)
.\dlss5\build\x86\dsf-dlss5-client-test.exe ... --robustness     # then exercise the timeout and error paths
```

The offscreen test writes `input_first.png`, `input_last.png`, `output_last.png`, and `compare_last.png` (input | output | difference ×4). It also logs per-evaluate GPU time and how much the output differs from the input. Exit code 0 means every evaluate succeeded.

## Licensing

The code in this folder is MIT-licensed, like the rest of the repository. It contains no NVIDIA code or header text. `dlssnr_defs.h` names the interface from public, openly licensed sources (details in the research note). Builds link NVIDIA's static NGX loader. If you redistribute built binaries, the NVIDIA RTX SDKs license applies to that part. NVIDIA DLLs are never committed or packaged.
