"""Package-level checks that do not need a scanner."""

from __future__ import annotations

import re
from pathlib import Path

import pytest
from minscan import Film, Media, Phase, _api


def test_enums_match_the_c_api() -> None:
    assert (int(Media.FORMAT_35MM), int(Media.FORMAT_120)) == (0, 1)
    assert (int(Film.COLOR_NEG), int(Film.COLOR_POS),
            int(Film.MONO)) == (0, 1, 2)
    assert Phase.CALIBRATE.label == "Calibrating"
    assert Phase.SCAN.label == "Scanning"


def test_candidates_prefer_env_then_the_bundled_library(monkeypatch, tmp_path) -> None:
    monkeypatch.setenv("MINSCAN_LIB", "/custom/libminscan.so")
    monkeypatch.setattr(_api, "_pkg_config_libdir", lambda: None)
    monkeypatch.setattr(_api.ctypes.util, "find_library", lambda _name: None)
    monkeypatch.setattr(_api, "__file__", str(
        tmp_path / "minscan" / "_api.py"))

    candidates = _api._library_candidates()

    assert candidates[0] == "/custom/libminscan.so"
    assert str(tmp_path / "minscan" / "libminscan.so") == candidates[1]


def test_candidates_fall_back_to_a_source_builddir(monkeypatch, tmp_path) -> None:
    monkeypatch.delenv("MINSCAN_LIB", raising=False)
    monkeypatch.setattr(_api, "_pkg_config_libdir", lambda: None)
    monkeypatch.setattr(_api.ctypes.util, "find_library", lambda _name: None)
    root = tmp_path / "minscan"
    here = root / "python" / "minscan"
    here.mkdir(parents=True)
    monkeypatch.setattr(_api, "__file__", str(here / "_api.py"))

    candidates = _api._library_candidates()

    assert str(root / "builddir" / "libminscan.so") in candidates


def test_devices_does_not_probe_by_default(monkeypatch) -> None:
    from minscan import _api

    called = False

    def _probe(_path):
        nonlocal called
        called = True
        return _api.Media.FORMAT_120

    monkeypatch.setattr(_api, "detect", lambda: "/dev/sg9")
    monkeypatch.setattr(_api, "probe", _probe)

    assert _api.devices() == [("/dev/sg9", None)]
    assert called is False
    assert _api.devices(probe_media=True) == [
        ("/dev/sg9", _api.Media.FORMAT_120)]
    assert called is True


def _struct_fields(text: str, name: str) -> list[str]:
    """Field names of ``typedef struct NAME { ... } NAME;``, in declaration order."""
    block = re.search(
        rf"typedef\s+struct\s+{name}\s*\{{(.*?)\}}\s*{name}\s*;", text, re.DOTALL
    )
    assert block is not None, f"struct {name} not found"
    fields = []
    for line in block.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith(("/*", "//", "*")):
            continue
        fields.append(line.split()[1].rstrip(";"))
    return fields


def test_cdef_structs_match_the_c_header() -> None:
    """The hand-written cdef must follow include/minscan/capi.h field for field."""
    header = Path(__file__).parents[2] / "include" / "minscan" / "capi.h"
    if not header.exists():
        pytest.skip("source header not available in an installed package")
    text = header.read_text()
    for name in ("MsLoadedCarrier", "MsRequest"):
        assert _struct_fields(_api._CDEF, name) == _struct_fields(text, name)


def test_loaded_carrier_fields_bind() -> None:
    """Every field configure() writes must exist in the cdef."""
    try:
        ffi, _ = _api.load()
    except _api.MinscanUnavailable:
        pytest.skip("libminscan is not available")
    carrier = ffi.new("MsLoadedCarrier *")
    carrier.type = int(Media.FORMAT_120)
    carrier.film = int(Film.MONO)
    carrier.format_code = 0
    carrier.glassless = 0
    carrier.ev = 0.0
    assert (carrier.type, carrier.film) == (1, 2)
