"""Terminal output with rich: a progress bar or a spinner for the long steps, finished steps in green
with their details in grey, dim notes, errors in a red panel, and a summary panel at the end. Text
passed in is printed as is (never read as rich markup). On a console code page that lacks a
character, rich draws its boxes in ASCII and anything else prints as "?" instead of failing."""

from __future__ import annotations

import sys

from rich import box
from rich.console import Console
from rich.markup import escape
from rich.panel import Panel
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn, TimeElapsedColumn, TimeRemainingColumn
from rich.table import Table

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(errors="replace")
console = Console(highlight=False)


def say(text: str) -> None:
    console.print(escape(text))


def done(text: str, detail: str = "") -> None:
    """A finished step, with an optional detail in grey."""
    console.print(f"[bold green]{escape(text)}[/]" + (f"  [dim]{escape(detail)}[/]" if detail else ""))


def note(text: str) -> None:
    """Something worth knowing that needs nothing done."""
    console.print(f"  [dim]{escape(text)}[/]")


def fail(text: str) -> None:
    console.print(
        Panel(
            escape(text), title="[bold]Error[/]", title_align="left", border_style="red", box=box.ROUNDED, expand=False
        )
    )


def summary(title: str, rows: list[tuple[str, str]], footer: str = "") -> None:
    """A panel of label: value rows."""
    table = Table.grid(padding=(0, 2))
    table.add_column(style="cyan", no_wrap=True)
    table.add_column(overflow="fold")  # long paths wrap, not cut
    for label, value in rows:
        table.add_row(escape(label), escape(value))
    console.print(
        Panel(
            table,
            title=f"[bold]{escape(title)}[/]",
            title_align="left",
            subtitle=f"[green]{escape(footer)}[/]" if footer else None,
            subtitle_align="right",
            box=box.ROUNDED,
            border_style="green",
            expand=False,
            padding=(0, 1),
        )
    )


def status(message: str):
    """A spinner with message while the with-block runs."""
    return console.status(escape(message))


def progress() -> Progress:
    """A bar for counted work; it disappears when done."""
    return Progress(
        TextColumn("{task.description}"),
        BarColumn(),
        MofNCompleteColumn(),
        TimeElapsedColumn(),
        TimeRemainingColumn(),
        console=console,
        transient=True,
    )
