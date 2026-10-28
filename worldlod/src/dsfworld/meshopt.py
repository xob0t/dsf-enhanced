"""meshoptimizer's attribute-aware simplifier, from the DLL shipped in _native/.

meshoptimizer (MIT, native/meshoptimizer/LICENSE.md) has no Python wheels, so
its simplifier is built once for Windows x64 (native/build.ps1) and called
through ctypes: installing worldlod needs no C++ compiler.
"""

from __future__ import annotations

import ctypes
import sys
from functools import cache
from pathlib import Path

import numpy as np

LOCK_BORDER = 1 << 0  # meshopt_SimplifyLockBorder: keep the mesh's open edges in place
ERROR_ABSOLUTE = 1 << 2  # meshopt_SimplifyErrorAbsolute: target_error in mesh units, not relative
DLL = Path(__file__).with_name("_native") / "meshopt.dll"


@cache
def _simplify_with_attributes():
    if sys.platform != "win32" or not DLL.exists():
        raise SystemExit(f"[simplify] needs {DLL.name} (Windows x64); set [simplify] error = 0 to bake without it")
    f = ctypes.CDLL(str(DLL)).meshopt_simplifyWithAttributes
    uint_p, float_p, size = ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_float), ctypes.c_size_t
    f.argtypes = [
        uint_p, uint_p, size,  # destination, indices, index_count
        float_p, size, size,  # positions, vertex_count, positions stride
        float_p, size, float_p, size,  # attributes, attributes stride, weights, attribute_count
        ctypes.POINTER(ctypes.c_ubyte), size,  # vertex_lock, target_index_count
        ctypes.c_float, ctypes.c_uint, float_p,  # target_error, options, result_error
    ]  # fmt: skip
    f.restype = size
    return f


def simplify_with_attributes(
    indices: np.ndarray, positions: np.ndarray, attributes: np.ndarray, weights: np.ndarray, *, error: float, options: int
) -> np.ndarray:
    """Indices of the simplified mesh, as few as error allows (target 1 triangle).

    indices: (k,) uint32; positions: (n, 3) and attributes: (n, m) float32; weights: (m,) float32."""
    indices = np.ascontiguousarray(indices, np.uint32)
    positions = np.ascontiguousarray(positions, np.float32)
    attributes = np.ascontiguousarray(attributes, np.float32)
    weights = np.ascontiguousarray(weights, np.float32)
    out = np.zeros_like(indices)

    def ptr(a: np.ndarray, t):
        return a.ctypes.data_as(ctypes.POINTER(t))

    n = _simplify_with_attributes()(
        ptr(out, ctypes.c_uint), ptr(indices, ctypes.c_uint), len(indices),
        ptr(positions, ctypes.c_float), len(positions), positions.strides[0],
        ptr(attributes, ctypes.c_float), attributes.strides[0], ptr(weights, ctypes.c_float), len(weights),
        None, 3,
        error, options, None,
    )  # fmt: skip
    return out[:n]
