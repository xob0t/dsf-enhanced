# worldlod

Builds a better base LOD for Driver: San Francisco's upper Shift levels.

At user Shift levels 3 and 4 the game draws one global low-detail model of the
city: building shells with atlas textures on top of a coarse vertex-coloured
terrain. The detailed regions hold the real ground (roads, kerbs, paving,
freeway decks), props and building detail, but the engine only loads the few
around the camera. This pipeline reads those regions from the player's own
`san_francisco.dngc` and bakes them into a new base LOD. It ships no game data.

Status: the bake works offline. It copies the detailed regions' ground (roads,
kerbs, paving, freeway decks, terrain) into the base LOD in place of each
cell's stock low-poly terrain, simplifies it and fills what it doesn't cover
with the stock terrain: 1.05M triangles in 403 cells, 31 MB of vertex and
index data. It has not been tried in the game yet. Findings are in
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

[flatten]
classes = ["ground", "props"]
min_model_size = 2.0
```

The pipeline lowers its own process priority (`[run] priority`, default
`idle`), so the PC stays usable while it runs.

## Stages

| Command | Output (below `[run] work_dir`) |
| --- | --- |
| `worldlod info` | archive summary and the stock base LOD by material class |
| `worldlod flatten` | `flatten/<region>.npz`: each region's models placed in world space, with a material and class per triangle; `flatten/summary.tsv` |
| `worldlod preview [--base]` | `preview/<region>.png`, and with `--base` the stock base over the same cell |
| `worldlod bake` | `bake/<region>.pkl`: the region's `[bake] classes`, moved into the frame of its cell's stock terrain part; `bake/summary.tsv` |
| `worldlod pack [--verify]` | `pack/san_francisco.dngc` with the baked cells in GEGR; `--verify` re-reads it and checks GEGR's references (with `[bake] classes = []`: that it equals the game's file) |

A stage reuses its earlier output while the settings it depends on are unchanged.
`--archive <file>` reads another world archive, e.g. `uv run worldlod --archive
out/pack/san_francisco.dngc preview --base` renders the packed base LOD.

Nothing is installed into the game yet; the packed archive stays in `out/pack`.

## Development

```powershell
uv run pytest
uv run ruff check --fix
uv run ruff format
```
