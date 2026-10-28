# worldlod

Builds a better base LOD for Driver: San Francisco's upper Shift levels.

At user Shift levels 3 and 4 the game draws one global low-detail model of the
city: building shells with atlas textures on top of a coarse vertex-coloured
terrain. The detailed regions hold the real ground (roads, kerbs, paving,
freeway decks), props and building detail, but the engine only loads the few
around the camera. This pipeline reads those regions from the player's own
`san_francisco.dngc` and bakes them into a new base LOD. It ships no game data.

With the default settings it replaces each cell's stock low-poly terrain with
the detailed regions' ground (simplified; the stock terrain fills what it
doesn't cover), trees and bushes, building detail (storefronts, rooftop units,
fire escapes, containers) and painted road text: 7.6M triangles in 408 cells,
239 MB of vertex and index data. Tested in game (Shift, driving).

The bigger base LOD needs DSF Enhanced's `[Limits]` fixes (on by default): the
stock engine crashes loading it. Findings are in
[research/world-lod.md](research/world-lod.md).

## Setup

Python 3.13 and [uv](https://docs.astral.sh/uv/). From this folder:

```powershell
uv sync --extra simplify
uv run worldlod info
```

The `simplify` extra (meshoptimizer) compiles on install and needs the Visual
Studio C++ build tools. Without it, set `[simplify] error = 0`.

The game folder comes from `[game] dir` in the settings or the `DSF_GAME_DIR`
environment variable.

## Settings

Defaults are in [src/dsfworld/default.toml](src/dsfworld/default.toml). Put the
keys you want to change in a `worldlod.toml` in the folder you run from, or
pass `--config <file>`. `uv run worldlod config` prints the merged settings.

```toml
[regions]
select = ["R_360", "R_093"]

[bake]
classes = ["ground", "foliage"]
min_model_size = { foliage = 4.0 }
```

The pipeline lowers its own process priority (`[run] priority`, default
`idle`), so the PC stays usable while it runs.

## Stages

| Command | Output (below `[run] work_dir`) |
| --- | --- |
| `worldlod info` | archive summary and the stock base LOD by material class |
| `worldlod bake` | `bake/<region>.pkl`: the region's `[bake] classes`, moved into the frame of its cell's stock terrain part; `bake/summary.tsv` |
| `worldlod pack [--verify]` | `pack/san_francisco.dngc` with the baked cells in GEGR; `--verify` re-reads it and checks GEGR's references (with `[bake] classes = []`: that it equals the game's file) |
| `worldlod render [--cell R_360 \| --area ...] [--view top\|oblique]` | `render/<name>_<view>_{stock,baked,compare}.png`: the stock and the packed base LOD, textured, over the same area (default: the whole city) |
| `worldlod install` | backs up the game's `san_francisco.dngc` once to `<game>/worldlod-backup/` (with its SHA-256) and puts the packed one in its place |
| `worldlod uninstall` | puts the backed-up original back |

`bake` reuses a region's earlier output while the settings it depends on are unchanged.
`--archive <file>` reads another world archive, e.g. `uv run worldlod --archive
out/pack/san_francisco.dngc info` summarises the packed one.

`install` refuses while the game runs, when the packed archive fails its check,
or when the game's file changed since the last install. It swaps files by
rename, so a hard link to the original (the stock file has one) keeps the
original bytes.

## Development

```powershell
uv run pytest
uv run ruff check --fix
uv run ruff format
```
