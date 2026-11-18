#!/usr/bin/env python3
"""Give the generated icon headers internal linkage so the linker keeps one copy.

Every icon lives in a header, and a header reaches many translation units
through `UiAppHelpers.h`. A `static const` array has internal linkage, so each
including unit emits its own copy of the bitmap and the image carries the same
icon up to eight times over. Declaring the arrays and the descriptors
`inline constexpr` instead gives them external linkage with vague linkage, so
the linker merges them into a single copy that every unit shares. The pixel
data is unchanged, so nothing renders differently.

The icon generators live in the SDK submodule, which this repository cannot
rewrite, so this script post-processes their output the same way
`share-cn-font-intervals.py` post-processes the generated font headers. Run it
after regenerating any icon header.
"""

import argparse
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
ICON_DIR = ROOT / "src/components/icons"

# A bitmap array and the descriptor that points at it. Both `static const` and
# `static constexpr` give the object internal linkage, so each including unit
# emits its own copy; `inline constexpr` lets the linker keep one.
ARRAY = re.compile(r"^static (?:const|constexpr) (uint8_t|freeink::Icon) (\w+)(\[\d*\])? = \{", re.MULTILINE)


def _read(path):
    """Read a header and normalize its line endings to LF for matching."""
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def _write(path, content):
    """Write a header, keeping the line ending the file already uses."""
    newline = "\r\n" if path.exists() and b"\r\n" in path.read_bytes() else "\n"
    data = content.encode("utf-8")
    if newline == "\r\n":
        data = data.replace(b"\n", b"\r\n")
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def shared_outputs(directory):
    """Return the rewritten content of every icon header in the directory."""
    outputs = []
    paths = sorted(directory.glob("*.h")) if directory.is_dir() else [directory]
    for path in paths:
        text = _read(path)
        rewritten = ARRAY.sub(lambda match: f"inline constexpr {match.group(1)} {match.group(2)}"
                                            f"{match.group(3) or ''} = {{", text)
        if rewritten != text:
            outputs.append((path, rewritten))
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=ICON_DIR)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = shared_outputs(args.directory)
    if not outputs:
        print("icon headers already share their bitmaps")
        return
    for path, content in outputs:
        if args.check:
            raise SystemExit(f"{path}: icon headers need the inline constexpr rewrite")
        _write(path, content)
        print(f"rewrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")

if __name__ == "__main__":
    main()
