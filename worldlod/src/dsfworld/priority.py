"""Lower this process's CPU priority so a build doesn't disturb the PC."""

from __future__ import annotations

import os
import sys

_WINDOWS = {"idle": 0x40, "below_normal": 0x4000, "normal": 0x20}
_NICE = {"idle": 19, "below_normal": 10, "normal": 0}


def set_priority(level: str) -> None:
    if level not in _WINDOWS:
        raise SystemExit(f"[run] priority must be one of {', '.join(_WINDOWS)}, not {level!r}")
    if sys.platform == "win32":
        import ctypes

        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), _WINDOWS[level])
    elif _NICE[level]:
        os.nice(_NICE[level])
