"""Package-level checks that do not need a scanner or the shared library."""

from __future__ import annotations

from minscan import Film, Media, Phase
from minscan import _api


def test_enums_match_the_c_api() -> None:
    assert (int(Media.FORMAT_35MM), int(Media.FORMAT_120)) == (0, 1)
    assert (int(Film.COLOUR_NEG), int(Film.COLOUR_POS), int(Film.MONO)) == (0, 1, 2)
    assert Phase.CALIBRATE.label == "Calibrating"
    assert Phase.SCAN.label == "Scanning"


def test_candidates_prefer_env_then_the_bundled_library(monkeypatch, tmp_path) -> None:
    monkeypatch.setenv("MINSCAN_LIB", "/custom/libminscan.so")
    monkeypatch.setattr(_api, "_pkg_config_libdir", lambda: None)
    monkeypatch.setattr(_api.ctypes.util, "find_library", lambda _name: None)
    monkeypatch.setattr(_api, "__file__", str(tmp_path / "minscan" / "_api.py"))

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
