"""Terminal output: a progress bar or a status line for the long steps, plain lines otherwise."""

from __future__ import annotations

from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn, TimeElapsedColumn, TimeRemainingColumn

console = Console(highlight=False)


def say(*parts: object) -> None:
    console.print(*parts)


def status(message: str):
    """A spinner with message while the with-block runs."""
    return console.status(message)


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
