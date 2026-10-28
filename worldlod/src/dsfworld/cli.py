"""worldlod: a high-detail map for Driver: San Francisco's upper Shift levels, built from the game's own
detailed regions. Start with `worldlod install`."""

from __future__ import annotations

import argparse
import tomllib
from pathlib import Path
from typing import TYPE_CHECKING

from dsfworld import config, ui
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
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--game", type=Path, help="the game folder (with Driver.exe); remembered after install")
    common.add_argument("-c", "--config", type=Path, help=f"settings file (default: ./{config.USER_FILE} if present)")
    built = argparse.ArgumentParser(add_help=False)
    for name, (_, what) in config.OPTIONAL.items():
        built.add_argument(f"--no-{name}", action="store_true", help=f"leave out {what}")
    built.add_argument("--force", action="store_true", help="rebuild everything, even what is up to date")
    dev = argparse.ArgumentParser(add_help=False)
    dev.add_argument("--archive", type=Path, help="read this world archive instead of the game's (e.g. a built one)")

    ap = argparse.ArgumentParser(prog="worldlod", description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True, metavar="command")
    sub.add_parser(
        "install",
        parents=[common, built],
        help="put DSF Enhanced into the game if it lacks it, build the new map (if needed) and install it",
    )
    sub.add_parser(
        "uninstall",
        parents=[common],
        help="put the game's original map back (and remove DSF Enhanced if install added it)",
    )
    sub.add_parser("build", parents=[common, built], help="build the new map without installing it")
    sub.add_parser("config", parents=[common, built], help="print the settings in effect")
    sub.add_parser("info", parents=[common, dev], help="summarise the game's world archive")
    p = sub.add_parser("bake", parents=[common, built, dev], help="bake the regions only (a build's first step)")
    p = sub.add_parser("render", parents=[common, dev], help="draw the stock and the built map over the same area")
    p.add_argument("--area", type=float, nargs=4, metavar=("MIN_X", "MIN_Z", "MAX_X", "MAX_Z"), help="metres")
    p.add_argument("--cell", help="the area of this region's cell, e.g. R_360")
    p.add_argument("--size", type=int, help="pixels")
    p.add_argument("--view", choices=["top", "oblique"])
    p.add_argument("--name", help="output file name prefix")
    return ap


def settings(args: argparse.Namespace) -> config.Config:
    """The settings file with the command line's choices on top."""
    cfg = config.load(args.config)
    if args.game:
        cfg.data["game"]["dir"] = str(args.game.resolve())
    flags = config.OPTIONAL.items()
    left_out = {cls for name, (cls, _) in flags if getattr(args, "no_" + name.replace("-", "_"), False)}
    cfg.data["bake"]["classes"] = [c for c in cfg.data["bake"]["classes"] if c not in left_out]
    if getattr(args, "archive", None):
        cfg = cfg.reading(args.archive.resolve())
    return cfg


def open_world(cfg: config.Config) -> World:
    from dsfworld.world import World

    with ui.status(f"Reading {cfg.archive}"):
        return World.open(cfg)


def run_stage(cfg: config.Config, world: World, args: argparse.Namespace) -> None:
    """Run the command that needs the open world."""
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
        "build": lambda: pack.run(cfg, world, force=args.force),
        "install": lambda: pack.run(cfg, world, force=args.force),
    }
    stages[args.command]()


def main(argv: list[str] | None = None) -> None:
    args = parser().parse_args(argv)
    cfg = settings(args)
    if args.command == "config":
        print(dump_toml(cfg.data), end="")
        return
    set_priority(cfg.section("run")["priority"])
    from dsfworld.stages import install

    if args.command == "uninstall":
        install.uninstall(cfg)
        return
    if args.command == "install":
        install.ready(cfg)  # before minutes of building
    world = open_world(cfg)
    with world.archive:
        run_stage(cfg, world, args)
    if args.command == "install":
        install.install(cfg)


if __name__ == "__main__":
    main()
