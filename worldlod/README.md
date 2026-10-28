# worldlod

A high-detail map for Driver: San Francisco's upper Shift levels.

When you Shift up, the game draws one low-detail model of the whole city: flat,
vertex-coloured ground under plain building shells. worldlod rebuilds that model
from the game's own detailed areas, with real roads, kerbs, paving, trees and bushes,
building detail (storefronts, rooftop units, fire escapes, containers) and
painted road text. It reads your copy of the game and writes the new map into
it, with a backup. No game data is shipped.

## Install

1. Install [uv](https://docs.astral.sh/uv/getting-started/installation/):

   ```powershell
   powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
   ```

2. Close the game, open a new PowerShell in the game folder (the one with
   `Driver.exe`) and run:

   ```powershell
   $tag = (irm https://api.github.com/repos/xob0t/dsf-enhanced/releases/latest).tag_name
   uvx --from "git+https://github.com/xob0t/dsf-enhanced@$tag#subdirectory=worldlod" worldlod install
   ```

   This runs worldlod from the latest release, once; nothing stays installed.
   The new map is bigger than the stock engine allows, so if the game doesn't
   have [DSF Enhanced](https://github.com/xob0t/dsf-enhanced) with its
   `[Limits]` fixes yet, worldlod puts the latest release in (keeping your
   `dsf_enhanced.ini` and ASI loader if you have them). Then it builds the map
   (a minute or two), puts it into the game (the original is backed up in
   `worldlod-backup`) and deletes its build files. From another folder, add
   `--game "C:\path\to\Driver San Francisco"`.

To undo, run the same two lines with `uninstall` instead of `install`: the
original map comes back, and DSF Enhanced goes again if worldlod put it there.

## Options

Leave things out of the map:

| Flag | Leaves out |
| --- | --- |
| `--no-trees` | trees and bushes |
| `--no-buildings` | building detail (the building shells stay) |
| `--no-road-text` | painted road text |

Add them to the install line, for example `... worldlod install --no-buildings`. The roads and paving are always
in. `worldlod build` builds without installing, `--force` rebuilds everything,
and `worldlod config` prints the settings in effect.

A build runs at idle priority, so the PC stays usable meanwhile. Its files
(about 1 GB, in `%LOCALAPPDATA%\worldlod`) are deleted once it is installed.

## Settings

Everything else is in [src/dsfworld/default.toml](src/dsfworld/default.toml),
with a comment per setting. To change one, put it in a `worldlod.toml` in the
folder you run worldlod from, or pass `--config <file>`:

```toml
[bake]
min_model_size = { buildings = 4.0 }  # drop building detail smaller than 4 m

[simplify]
error = 1.0  # simplify the ground more (metres)
```

Tables keyed by class name (`[classes]`, `min_model_size`) replace the default
table as a whole.

## Commands

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
