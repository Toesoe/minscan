#!/usr/bin/env python3
"""Layer boundary check for the minscan core.

wire/  may not include scan/, session/ or image/
scan/  may not include session/ or image/
image/ may not include session/
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

RULES = {
    "src/core/wire": ("src/core/scan/", "src/core/session/", "src/core/image/"),
    "src/core/scan": ("src/core/session/", "src/core/image/"),
    "src/core/image": ("src/core/session/",),
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def repo_relative(source, target):
    """Resolve an include either relative to the including file or to the repo root."""

    for candidate in (source.parent / target, ROOT / target):
        if not candidate.exists():
            continue
        try:
            return candidate.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            continue

    return None


def main() -> int:
    violations = []

    for layer, forbidden in RULES.items():
        for path in sorted((ROOT / layer).glob("*")):
            if path.suffix not in (".c", ".h"):
                continue

            for target in INCLUDE.findall(path.read_text()):
                resolved = repo_relative(path, target)

                if resolved is None:
                    continue

                for bad in forbidden:
                    if resolved.startswith(bad):
                        violations.append(f'{path.relative_to(ROOT)}: includes "{target}"')
                        break

    for line in violations:
        print(f"layer violation: {line}")

    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
