"""worldlod command line."""

from __future__ import annotations

import argparse
import tomllib
from pathlib import Path

from dsfworld import config
from dsfworld.priority import set_priority


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


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(prog="worldlod", description=__doc__)
    ap.add_argument("-c", "--config", type=Path, help=f"settings file (default: ./{config.USER_FILE} if present)")
    ap.add_argument("--archive", type=Path, help="read this world archive instead of the game's (e.g. a packed one)")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("config", help="print the merged settings")
    sub.add_parser("info", help="summarise the archive and the stock base LOD")
    p = sub.add_parser("flatten", help="place each detailed region's models in world space")
    p.add_argument("--force", action="store_true", help="rebuild regions that are up to date")
    p = sub.add_parser("preview", help="render flattened regions to PNG")
    p.add_argument("--base", action="store_true", help="also render the stock base LOD over each cell")
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
    args = ap.parse_args(argv)

    cfg = config.load(args.config)
    if args.archive:
        cfg.data["game"]["archive"] = str(args.archive.resolve())
    if args.command in ("install", "uninstall"):
        from dsfworld.stages import install

        set_priority(cfg.section("run")["priority"])
        (install.install if args.command == "install" else install.uninstall)(cfg)
        return
    if args.command == "config":
        print(dump_toml(cfg.data), end="")
        return
    set_priority(cfg.section("run")["priority"])

    from dsfworld.world import World

    world = World.open(cfg)
    with world.archive:
        if args.command == "info":
            from dsfworld.stages import info

            info.run(cfg, world)
        elif args.command == "flatten":
            from dsfworld.stages import flatten

            flatten.run(cfg, world, force=args.force)
        elif args.command == "preview":
            from dsfworld.stages import preview

            preview.run(cfg, world, base=args.base)
        elif args.command == "render":
            from dsfworld.stages import render

            area, name = args.area, args.name
            if args.cell:
                area = world.archive.cell(world.archive.resource(args.cell)).box
                name = name or args.cell
            render.run(cfg, world, area, args.size, args.view, name)
        elif args.command == "bake":
            from dsfworld.stages import bake

            bake.run(cfg, world, force=args.force)
        elif args.command == "pack":
            from dsfworld.stages import pack

            pack.run(cfg, world, verify=args.verify)


if __name__ == "__main__":
    main()
