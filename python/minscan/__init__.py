"""Python bindings for the Minolta DiMAGE ``minscan`` C library.

The package bundles ``libminscan`` in a wheel; a development checkout may load
the sibling ``builddir/libminscan.so`` or a system install instead.

    import minscan

    with minscan.Session("/dev/sg1") as dev:
        dev.configure(minscan.Media.FORMAT_120, minscan.Film.COLOUR_NEG, format_code=0x20)
        frame = dev.scan(dpi=3200, autofocus=True, capture_ir=True)
        print(frame.rgb.shape, frame.ir.shape)
"""

from __future__ import annotations

from ._api import (
    Film,
    Frame,
    Media,
    MinscanCancelled,
    MinscanError,
    MinscanTransientError,
    MinscanUnavailable,
    Phase,
    Session,
    devices,
    load,
    probe,
)

__all__ = [
    "Film",
    "Frame",
    "Media",
    "MinscanCancelled",
    "MinscanError",
    "MinscanTransientError",
    "MinscanUnavailable",
    "Phase",
    "Session",
    "devices",
    "load",
    "probe",
]

__version__ = "0.1.0"
