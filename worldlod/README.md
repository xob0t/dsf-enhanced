# worldlod

A high-detail map for Driver: San Francisco's upper Shift levels.

When you Shift up, the game draws one low-detail model of the whole city: flat,
vertex-coloured ground under plain building shells. worldlod rebuilds that model
from the game's own detailed areas, with real roads, kerbs, paving, trees and bushes,
building detail (storefronts, rooftop units, fire escapes, containers) and
painted road text. It reads your copy of the game and writes the new map into
it, with a backup. No game data is shipped.

## Install

1. Download `worldlod.zip` from the
   [latest release](https://github.com/xob0t/dsf-enhanced/releases/latest) and
   unzip it anywhere.
2. Close the game and double-click `install.cmd`.

The first time, it asks for the game folder (the one with `Driver.exe`) unless
you unzipped worldlod inside it, and installs
[uv](https://docs.astral.sh/uv/) if you don't have it (uv brings the Python
worldlod runs on). Then it:

1. puts [DSF Enhanced](https://github.com/xob0t/dsf-enhanced) into the game if
   it isn't there yet (from the zip's `plugin` folder; your
   `dsf_enhanced.ini` and ASI loader stay if you have them). The new map is
   bigger than the stock engine allows; DSF Enhanced's `[Limits]` fixes lift
   that, without them the game crashes while loading;
2. builds the map from your game's files (a minute or two);
3. backs up the game's original map to `worldlod-backup` and puts the new one
   in its place, then deletes its build files.

To undo: `install.cmd --uninstall` puts the original map back, and takes DSF
Enhanced out again if worldlod put it there.

## Options

Leave things out of the map:

| Option | Leaves out |
| --- | --- |
| `--no-trees` | trees and bushes |
| `--no-buildings` | building detail (the building shells stay) |
| `--no-road-text` | painted road text |

For example, in a terminal in worldlod's folder: `.\install.cmd --no-buildings`.
The roads and paving are always in. `.\install.cmd --help` lists every option.

A build runs at idle priority, so the PC stays usable meanwhile. Its files
(about 1 GB, in `%LOCALAPPDATA%\worldlod`) are deleted once it is installed.

## Settings

Everything else is in [src/dsfworld/default.toml](src/dsfworld/default.toml),
with a comment per setting. To change one, put it in a file and pass it with
`--config <file>`:

```toml
[bake]
min_model_size = { buildings = 4.0 }  # drop building detail smaller than 4 m

[simplify]
error = 1.0  # simplify the ground more (metres)
```

Tables keyed by class name (`[classes]`, `min_model_size`) replace the default
table as a whole.

## Commands

`install.cmd` runs `worldlod install` (or `uninstall`). From a checkout of this
repository, `uv run worldlod <command>` runs any of them:

| Command | Does |
| --- | --- |
| `worldlod install` | builds the map if it is missing or out of date, backs up the game's `san_francisco.dngc` once to `<game>/worldlod-backup/` (with its SHA-256) and puts the build in its place |
| `worldlod uninstall` | puts the backed-up original back |
| `worldlod build` | bakes the detailed regions into the base LOD, writes the new `san_francisco.dngc` and checks it (GEGR references, MDID, group sizes, streamer budgets) |
| `worldlod config` | prints the settings in effect |
| `worldlod info` | summarises the game's world archive |
| `worldlod bake` | a build's first step only: `bake/<region>.pkl` and `bake/summary.tsv` |
| `worldlod render [--cell R_360 \| --area ...] [--view top\|oblique]` | `render/<name>_<view>_{stock,baked,compare}.png`: the stock and the built map, textured, over the same area |

A build is skipped while the code, the settings and the game's archive are
unchanged; `bake` likewise reuses each region's earlier output. `--archive
<file>` makes `info`, `bake` and `render` read another world archive.

`install` refuses while the game runs, when your `dsf_enhanced.ini` turns one
of the needed `[Limits]` off, when the build fails its check, or when the game's
file changed since the last install (another mod). DSF Enhanced comes from the
repository's latest GitHub release; the files it replaces are kept in
`worldlod-backup`. It swaps files by rename, so a hard link to the original
keeps the original bytes.

## How it works

Findings on the formats and the engine are in
[research/world-lod.md](research/world-lod.md). Mesh simplification uses
[meshoptimizer](https://github.com/zeux/meshoptimizer) (MIT), whose simplifier
ships prebuilt as `src/dsfworld/_native/meshopt.dll` (Windows x64), so no C++
compiler is needed; `native/build.ps1` rebuilds it from `native/meshoptimizer`.

## Development

```powershell
uv sync
uv run worldlod --help
uv run pytest
uv run ruff check --fix
uv run ruff format
```

The repository's `build.ps1` also makes the release's `dist\worldlod.zip`
(`release\package.ps1`): `install.cmd`, the launcher (`release\launcher.ps1`),
this package with its `uv.lock` in `app\`, and the DSF Enhanced just built in
`plugin\`.
