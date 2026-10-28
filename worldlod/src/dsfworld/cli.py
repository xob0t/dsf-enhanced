"""worldlod command line."""

from __future__ import annotations

import argparse
import tomllib
from pathlib import Path
from typing import TYPE_CHECKING

from dsfworld import config
from dsfworld.priority import set_priority

if TYPE_CHECKING:
    from dsfworld.world import World


def dump_toml(data: dict) -> str:
    """The merged settings as TOML (tables of scalars, lists and inline tables)."""

    def value(v):
        if isinstance(v, str):
            return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, list):
            return "[" + ", ".join(value(x) for x in v) + "]"
        if isinstance(v, dict):
            return "{ " + ", ".join(f"{k} = {value(x)}" for k, x in v.items()) + " }"
        return repr(v)

    lines = []
    for section, table in data.items():
        lines.append(f"[{section}]")
        lines += [f"{k} = {value(v)}" for k, v in table.items()]
        lines.append("")
    text = "\n".join(lines)
    tomllib.loads(text)  # must stay valid
    return text


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="worldlod", description=__doc__)
    ap.add_argument("-c", "--config", type=Path, help=f"settings file (default: ./{config.USER_FILE} if present)")
    ap.add_argument("--archive", type=Path, help="read this world archive instead of the game's (e.g. a packed one)")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("config", help="print the merged settings")
    sub.add_parser("info", help="summarise the archive and the stock base LOD")
    sub.add_parser("install", help="back up the game's world archive and put the packed one in its place")
    sub.add_parser("uninstall", help="put the game's original world archive back")
    p = sub.add_parser("render", help="draw the stock and the packed base LOD over the same area")
    p.add_argument("--area", type=float, nargs=4, metavar=("MIN_X", "MIN_Z", "MAX_X", "MAX_Z"), help="metres")
    p.add_argument("--cell", help="the area of this region's cell, e.g. R_360")
    p.add_argument("--size", type=int, help="pixels")
    p.add_argument("--view", choices=["top", "oblique"])
    p.add_argument("--name", help="output file name prefix")
    p = sub.add_parser("bake", help="copy each region's [bake] classes into its cell's stock base part")
    p.add_argument("--force", action="store_true", help="rebuild regions that are up to date")
    p = sub.add_parser("pack", help="write the world archive with the baked cells in GEGR")
    p.add_argument("--verify", action="store_true", help="check the output (identity with no bake, references with)")
    return ap


def run_stage(cfg: config.Config, world: World, args: argparse.Namespace) -> None:
    """Run the stage named by args.command on the open world."""
    from dsfworld.stages import bake, info, pack, render

    if args.command == "render":
        area, name = args.area, args.name
        if args.cell:
            area = world.archive.cell(world.archive.resource(args.cell)).box
            name = name or args.cell
        render.run(cfg, world, area=area, size=args.size, view=args.view, name=name)
        return
    stages = {
        "info": lambda: info.run(world),
        "bake": lambda: bake.run(cfg, world, force=args.force),
        "pack": lambda: pack.run(cfg, world, verify=args.verify),
    }
    stages[args.command]()


def main(argv: list[str] | None = None) -> None:
    args = parser().parse_args(argv)
    cfg = config.load(args.config)
    if args.archive:
        if args.command in ("install", "uninstall"):
            raise SystemExit(f"--archive doesn't apply to {args.command}: it always replaces the game's file")
        cfg = cfg.reading(args.archive.resolve())
    if args.command == "config":
        print(dump_toml(cfg.data), end="")
        return
    set_priority(cfg.section("run")["priority"])
    if args.command in ("install", "uninstall"):
        from dsfworld.stages import install

        (install.install if args.command == "install" else install.uninstall)(cfg)
        return

    from dsfworld.world import World

    world = World.open(cfg)
    with world.archive:
        run_stage(cfg, world, args)


if __name__ == "__main__":
    main()
