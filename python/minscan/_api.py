"""
CFFI ABI binding for ``libminscan``.
"""

from __future__ import annotations

import ctypes.util
import glob
import os
import subprocess
import sys
import threading
from collections.abc import Callable
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Any

import numpy as np

_CDEF = """
typedef struct MsSession MsSession;

typedef struct MsCarrier {
    int media;
    int film;
    int format_code;
    int glassless;
    double ev;
} MsCarrier;

typedef struct MsRequest {
    int dpi;
    int frame_group;
    int slot;
    int autofocus;
    int capture_ir;
    int ice;
    int prescan;
    int glassless;
    double ev;
} MsRequest;

typedef void (*MsProgressFn)(void *, int, int, int);

int ms_probe(const char *, int *, char *, size_t);
MsSession *ms_open(const char *, char *, size_t);
void ms_close(MsSession *);
int ms_configure(MsSession *, const MsCarrier *, char *, size_t);
int ms_calibrate(MsSession *, MsProgressFn, void *, char *, size_t);
int ms_scan(MsSession *, const MsRequest *, MsProgressFn, void *, char *, size_t);
void ms_cancel(MsSession *);
int ms_frame_geometry(const MsSession *, int *, int *, int *, size_t *);
const unsigned short *ms_frame_plane(const MsSession *, int);
void ms_frame_free(MsSession *);
int ms_eject(MsSession *, char *, size_t);
"""

_INSTALL_HINT = (
    "minscan is not installed. Install the Python package with `uv sync --group minscan`, "
    "or install the C library with `meson setup builddir && meson install -C builddir` and "
    "set MINSCAN_LIB to libminscan.so."
)

_FFI: Any = None
_LIB: Any = None


class MinscanUnavailable(RuntimeError):
    """The Python package is importable but no usable libminscan was found."""


class MinscanError(RuntimeError):
    """A libminscan operation returned a non-zero C status."""

    def __init__(self, code: int, message: str) -> None:
        self.code = int(code)
        super().__init__(message)


class MinscanTransientError(MinscanError):
    """Timeout / busy / not-ready: the caller may retry the operation."""


class MinscanCancelled(MinscanError):
    """The operation was cancelled through the progress callback or Session.cancel()."""


class Media(IntEnum):
    """Carrier/lens family, matching ``enum MsMedia`` in capi.h."""

    FORMAT_35MM = 0
    FORMAT_120 = 1


class Film(IntEnum):
    """Film identity, matching ``enum MsFilm`` in capi.h."""

    COLOUR_NEG = 0
    COLOUR_POS = 1
    MONO = 2


class Phase(IntEnum):
    """Progress phase, matching ``enum MsPhase`` in capi.h."""

    CALIBRATE = 0
    FOCUS = 1
    SCAN = 2

    @property
    def label(self) -> str:
        return {
            Phase.CALIBRATE: "Calibrating",
            Phase.FOCUS: "Focusing",
            Phase.SCAN: "Scanning",
        }[self]


@dataclass(frozen=True)
class Frame:
    """One acquired frame, copied out of the C session.

    ``rgb`` is always three planes (a mono read is replicated); ``ir`` is the
    fourth plane only when the request acquired it.
    """

    rgb: np.ndarray
    ir: np.ndarray | None
    dpi: int
    planes: int
    rows: int
    cols: int


def _pkg_config_libdir() -> str | None:
    try:
        result = subprocess.run(
            ["pkg-config", "--variable=libdir", "minscan"],
            check=False,
            capture_output=True,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() or None


def _library_candidates() -> list[str]:
    """Where to look for libminscan, most authoritative first.

    A wheel puts the library beside this module; a system install is found via
    pkg-config or the dynamic loader. The source build directory is a developer
    fallback only, so a stale checkout cannot shadow an installed library.
    """
    candidates: list[str] = []
    seen: set[str] = set()

    def add(value: str | None) -> None:
        if value and value not in seen:
            seen.add(value)
            candidates.append(value)

    add(os.environ.get("MINSCAN_LIB"))

    here = Path(__file__).resolve().parent
    for name in ("libminscan.so", "libminscan.dylib", "minscan.dll"):
        add(str(here / name))

    libdir = _pkg_config_libdir()
    if libdir:
        add(str(Path(libdir) / "libminscan.so"))
        add(str(Path(libdir) / "libminscan.dylib"))

    add(ctypes.util.find_library("minscan"))

    home = Path.home()
    for prefix in (Path(sys.prefix), home / ".local", Path("/usr/local"), Path("/usr")):
        for sub in ("lib", "lib64"):
            add(str(prefix / sub / "libminscan.so"))
            add(str(prefix / sub / "libminscan.dylib"))

    # Source-tree fallbacks: editable installs and a plain checkout.
    root = here.parents[1] if len(here.parents) >= 2 else here
    add(str(root / "builddir" / "libminscan.so"))
    add(str(root / "builddir" / "libminscan.dylib"))
    add("libminscan.so")
    add("libminscan.dylib")
    return candidates


def load() -> tuple[Any, Any]:
    """Load the shared library once and return ``(ffi, lib)``."""
    global _FFI, _LIB
    if _FFI is not None and _LIB is not None:
        return _FFI, _LIB

    try:
        import cffi
    except ImportError as exc:  # pragma: no cover - package dependency
        raise MinscanUnavailable(_INSTALL_HINT) from exc

    ffi = cffi.FFI()
    ffi.cdef(_CDEF)
    errors: list[str] = []
    for candidate in _library_candidates():
        try:
            lib = ffi.dlopen(candidate)
        except OSError as exc:
            errors.append(f"{candidate}: {exc}")
            continue
        _FFI, _LIB = ffi, lib
        return _FFI, _LIB

    raise MinscanUnavailable(_INSTALL_HINT + " Tried: " + "; ".join(errors))


def _error(ffi: Any, err: Any) -> str:
    try:
        text = ffi.string(err).decode("utf-8", "replace")
    except Exception:
        text = ""
    return text or "unknown minscan error"


def _raise(ffi: Any, err: Any, code: int) -> None:
    if code == 0:
        return
    message = _error(ffi, err)
    if "cancelled" in message.lower():
        raise MinscanCancelled(code, message)
    if code in (-6, -7, -8):  # timeout / busy / not ready
        raise MinscanTransientError(code, message)
    raise MinscanError(code, message)


def probe(device: str | os.PathLike[str]) -> Media | None:
    """Return the carrier media, or None when a device answers with no carrier."""
    ffi, lib = load()
    media = ffi.new("int *")
    err = ffi.new("char[512]")
    rc = lib.ms_probe(os.fsencode(os.fspath(device)), media, err, len(err))
    _raise(ffi, err, int(rc))
    value = int(media[0])
    return Media(value) if value in (int(Media.FORMAT_35MM), int(Media.FORMAT_120)) else None


def devices(pattern: str = "/dev/sg*") -> list[tuple[str, Media | None]]:
    """Probe every matching device path. Devices that do not answer are skipped."""
    found: list[tuple[str, Media | None]] = []
    for path in sorted(glob.glob(pattern)):
        try:
            found.append((path, probe(path)))
        except MinscanError:
            continue
    return found


class Session:
    """One open transport, configured for one carrier.

    The session owns the transport and the last frame. ``scan`` copies the
    frame into numpy arrays and frees the C frame before returning, so callers
    never hold a pointer into the library's buffer.
    """

    def __init__(self, device: str | os.PathLike[str]) -> None:
        self._ffi, self._lib = load()
        self.device = os.fspath(device)
        self._session = self._ffi.NULL
        self._closed = False
        self._config: tuple[int, int, int, bool, float] | None = None
        self._media: Media | None = None

        err = self._ffi.new("char[512]")
        session = self._lib.ms_open(os.fsencode(self.device), err, len(err))
        if session == self._ffi.NULL:
            raise MinscanError(-1, _error(self._ffi, err))
        self._session = session

    # ── configuration ────────────────────────────────────────────────
    def configure(
        self,
        media: Media,
        film: Film,
        *,
        format_code: int = 0,
        glassless: bool = False,
        ev: float = 0.0,
    ) -> None:
        self._check_open()
        identity = (int(media), int(film), int(
            format_code), bool(glassless), float(ev))
        if self._config == identity:
            return

        carrier = self._ffi.new("MsCarrier *")
        carrier.media = identity[0]
        carrier.film = identity[1]
        carrier.format_code = identity[2]
        carrier.glassless = int(identity[3])
        carrier.ev = identity[4]

        err = self._ffi.new("char[512]")
        rc = self._lib.ms_configure(self._session, carrier, err, len(err))
        _raise(self._ffi, err, int(rc))
        self._config = identity
        self._media = Media(identity[0])

    # ── acquisition ──────────────────────────────────────────────────
    def scan(
        self,
        *,
        dpi: int,
        frame_group: int = 1,
        slot: int = 0,
        autofocus: bool = False,
        capture_ir: bool = False,
        ice: bool = False,
        prescan: bool = False,
        glassless: bool = False,
        ev: float = 0.0,
        progress: Callable[[float, Phase], None] | None = None,
        cancel: threading.Event | None = None,
    ) -> Frame:
        self._check_open()
        if self._config is None:
            raise MinscanError(-10, "configure() the carrier before scanning")

        requested_dpi = int(dpi)
        if requested_dpi <= 0:
            requested_dpi = 600 if self._media == Media.FORMAT_35MM else 400
        req = self._ffi.new("MsRequest *")
        req.dpi = requested_dpi
        req.frame_group = int(frame_group)
        req.slot = int(slot)
        req.autofocus = 0 if prescan else int(bool(autofocus))
        req.capture_ir = 0 if prescan else int(bool(capture_ir))
        req.ice = 0 if prescan else int(bool(ice))
        req.prescan = int(bool(prescan))
        req.glassless = int(bool(glassless))
        req.ev = float(ev)

        callback = self._progress_callback(progress, cancel)
        err = self._ffi.new("char[512]")
        rc = self._lib.ms_scan(self._session, req, callback,
                               self._ffi.NULL, err, len(err))
        _raise(self._ffi, err, int(rc))
        return self._read_frame(requested_dpi=requested_dpi, prescan=bool(prescan))

    def preview(self, **kwargs: Any) -> Frame:
        """A hardware prescan/index pass; the same arguments as ``scan``."""
        kwargs["prescan"] = True
        return self.scan(**kwargs)

    def calibrate(
        self,
        *,
        progress: Callable[[float, Phase], None] | None = None,
        cancel: threading.Event | None = None,
    ) -> None:
        self._check_open()
        callback = self._progress_callback(progress, cancel)
        err = self._ffi.new("char[512]")
        rc = self._lib.ms_calibrate(
            self._session, callback, self._ffi.NULL, err, len(err))
        _raise(self._ffi, err, int(rc))

    def cancel(self) -> None:
        if not self._closed:
            self._lib.ms_cancel(self._session)

    def eject(self) -> bool:
        self._check_open()
        err = self._ffi.new("char[512]")
        rc = int(self._lib.ms_eject(self._session, err, len(err)))
        if rc == -11:  # MS_ERR_NOTIMPL
            return False
        _raise(self._ffi, err, rc)
        return True

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._lib.ms_close(self._session)
        self._session = self._ffi.NULL

    def __enter__(self) -> Session:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ── internals ────────────────────────────────────────────────────
    def _check_open(self) -> None:
        if self._closed:
            raise MinscanError(-1,
                               f"minscan session for {self.device} is closed")

    def _progress_callback(
        self,
        progress: Callable[[float, Phase], None] | None,
        cancel: threading.Event | None,
    ) -> Any:
        @self._ffi.callback("void(void *, int, int, int)")
        def _report(_ctx: Any, phase: int, done: int, total: int) -> None:
            if cancel is not None and cancel.is_set():
                self._lib.ms_cancel(self._session)
                return
            if progress is None:
                return
            try:
                progress((float(done) / float(total))
                         if total else 0.0, Phase(int(phase)))
            except Exception:
                pass

        return _report

    def _read_frame(self, *, requested_dpi: int, prescan: bool) -> Frame:
        ffi = self._ffi
        rows = ffi.new("int *")
        cols = ffi.new("int *")
        planes = ffi.new("int *")
        stride = ffi.new("size_t *")
        rc = self._lib.ms_frame_geometry(
            self._session, rows, cols, planes, stride)
        _raise(ffi, ffi.new("char[512]"), int(rc))

        rows_i, cols_i, planes_i, stride_i = int(rows[0]), int(
            cols[0]), int(planes[0]), int(stride[0])

        def plane(index: int) -> np.ndarray:
            ptr = self._lib.ms_frame_plane(self._session, index)
            if ptr == ffi.NULL:
                raise MinscanError(-1,
                                   f"minscan frame plane {index} unavailable")
            return np.ndarray(
                (rows_i, cols_i),
                dtype=np.uint16,
                buffer=ffi.buffer(ptr, rows_i * stride_i),
                strides=(stride_i, 2),
            )

        try:
            if planes_i >= 3:
                rgb = np.stack([plane(0), plane(1), plane(2)], axis=-1)
            else:
                mono = plane(0)
                rgb = np.stack([mono, mono, mono], axis=-1)
            ir = np.array(plane(3)) if planes_i == 4 else None
        finally:
            self._lib.ms_frame_free(self._session)

        if prescan:
            actual_dpi = 600 if self._media == Media.FORMAT_35MM else 400
        else:
            actual_dpi = requested_dpi
        return Frame(rgb=rgb, ir=ir, dpi=actual_dpi, planes=planes_i, rows=rows_i, cols=cols_i)
