"""worldlod command line."""

from __future__ import annotations

import argparse
import tomllib
from pathlib import Path

from dsfworld import config
from dsfworld.priority import set_priority


def dump_toml(data: dict) -> str:
    """The merged settings as TOML (tables of scalars and lists only)."""

    def value(v):
        if isinstance(v, str):
            return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, list):
            return "[" + ", ".join(value(x) for x in v) + "]"
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
    p = sub.add_parser("bake", help="copy each region's [bake] classes into its cell's stock base part")
    p.add_argument("--force", action="store_true", help="rebuild regions that are up to date")
    p = sub.add_parser("pack", help="write the world archive with the baked cells in GEGR")
    p.add_argument("--verify", action="store_true", help="check the output (identity with no bake, references with)")
    args = ap.parse_args(argv)

    cfg = config.load(args.config)
    if args.archive:
        cfg.data["game"]["archive"] = str(args.archive.resolve())
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
        elif args.command == "bake":
            from dsfworld.stages import bake

            bake.run(cfg, world, force=args.force)
        elif args.command == "pack":
            from dsfworld.stages import pack

            pack.run(cfg, world, verify=args.verify)


if __name__ == "__main__":
    main()
