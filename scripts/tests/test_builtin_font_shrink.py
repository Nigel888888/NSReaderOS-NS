#!/usr/bin/env python3
"""Check lossless font subsetting, shared indices and optional regeneration."""

import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "lib/EpdFont/scripts"
FONTS = ROOT / "lib/EpdFont/builtinFonts"
CHARACTERS = "0123456789 +-×÷.%=e�"
REGENERATE = "--regenerate" in sys.argv
if REGENERATE:
    sys.argv.remove("--regenerate")

spec = importlib.util.spec_from_file_location("share_intervals", SCRIPTS / "share-cn-font-intervals.py")
share = importlib.util.module_from_spec(spec)
spec.loader.exec_module(share)


def array(text, suffix):
    return re.search(rf"\w+{suffix}\[\d*\] = \{{(.*?)\n\}};", text, re.S).group(1)


def rows(text, suffix):
    return [tuple(int(value.strip(), 0) for value in row.split(','))
            for row in re.findall(r"\{\s*([^{}]+?)\s*\}", array(text, suffix))]


def glyphs_with_bitmaps(text):
    glyphs = rows(text, "Glyphs")
    bitmaps = bytes(int(value, 16) for value in re.findall(r"0x([\dA-F]{2})", array(text, "Bitmaps")))
    decoded = {}
    for offset, length, raw_length, count, first in rows(text, "Groups"):
        raw = zlib.decompress(bitmaps[offset:offset + length], -15)
        assert len(raw) == raw_length
        position = 0
        for index in range(first, first + count):
            width, height = glyphs[index][:2]
            length = ((width + 3) // 4) * height
            decoded[index] = raw[position:position + length]
            position += length
        assert position == len(raw)
    return {cp: (glyphs[offset + cp - first][:6], decoded[offset + cp - first])
            for first, last, offset in rows(text, "Intervals") for cp in range(first, last + 1)}


class BuiltinFontShrinkTest(unittest.TestCase):
    def test_calculator_bitmaps_and_coverage(self):
        for style in ("regular", "bold"):
            original = glyphs_with_bitmaps((FONTS / f"notosans_18_{style}.h").read_text())
            subset = glyphs_with_bitmaps((FONTS / f"calculator_18_{style}.h").read_text())
            self.assertEqual(set(subset), set(map(ord, CHARACTERS)))
            for cp, glyph in subset.items():
                self.assertEqual(glyph, original[cp], f"{style}: U+{cp:04X}")

    def test_shared_outputs_are_current(self):
        for path, output in share.shared_outputs(FONTS):
            self.assertEqual(path.read_text(), output)

    def test_sharing_changes_only_interval_storage(self):
        for shared_name, shared_file, sizes in share.SHARED_GROUPS:
            shared = (FONTS / shared_file).read_text()
            body = array(shared, "Intervals")
            with tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                for size in sizes:
                    name = f"notosans_cjk_{size}"
                    text = (FONTS / f"{name}.h").read_text()
                    text = text.replace(f'#include "{shared_file}"\n', '')
                    text = text.replace(f'{shared_name},', f'{name}Intervals,')
                    declaration = f'static const EpdUnicodeInterval {name}Intervals[] = {{{body}\n}};\n'
                    # Restore the generator's original location, immediately before the font descriptor.
                    text = text.replace(f'\n\nstatic const EpdFontData {name}',
                                        f'\n{declaration}\nstatic const EpdFontData {name}')
                    (directory / f"{name}.h").write_text(text)
                for path, output in share.shared_outputs(directory):
                    self.assertEqual(output, (FONTS / path.name).read_text())

    def test_mismatched_indices_do_not_write_anything(self):
        # Every group must be present, because the script walks them in order and
        # only the group under test carries the mismatch.
        for shared_name, shared_file, sizes in share.SHARED_GROUPS:
            with tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                for other_name, other_file, other_sizes in share.SHARED_GROUPS:
                    for size in other_sizes:
                        offset = 1 if (other_name == shared_name and size == sizes[-1]) else 0
                        (directory / f"notosans_cjk_{size}.h").write_text(
                            f"static const EpdUnicodeInterval notosans_cjk_{size}Intervals[] = {{\n"
                            f"    {{ 0x20, 0x21, 0x{offset:X} }},\n}};\n")
                before = {path: path.read_bytes() for path in directory.iterdir()}
                result = subprocess.run([sys.executable, str(SCRIPTS / "share-cn-font-intervals.py"), str(directory)],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("interval tables differ", result.stderr)
                self.assertEqual(before, {path: path.read_bytes() for path in directory.iterdir()})

    def test_shared_groups_keep_their_tiers_apart(self):
        # The two tiers are rasterized from different subsets, so their tables differ
        # and must not be merged into one shared header.
        common = array((FONTS / share.SHARED_FILE).read_text(), "Intervals")
        i18n = array((FONTS / "notosans_cjk_i18n_intervals.h").read_text(), "Intervals")
        self.assertNotEqual(common, i18n)
        for shared_name, shared_file, sizes in share.SHARED_GROUPS:
            for size in sizes:
                header = (FONTS / f"notosans_cjk_{size}.h").read_text()
                self.assertIn(f'#include "{shared_file}"', header)
                self.assertIn(f"{shared_name},", header)
                self.assertNotIn(f"notosans_cjk_{size}Intervals", header)

    @unittest.skipUnless(REGENERATE, "pass --regenerate with the font-generation Python environment")
    def test_calculator_generation_is_reproducible(self):
        for style in ("Regular", "Bold"):
            name = f"calculator_18_{style.lower()}"
            result = subprocess.run(
                [sys.executable, "fontconvert.py", name, "18", f"../builtinFonts/source/NotoSans/NotoSans-{style}.ttf",
                 "--2bit", "--compress", "--pnum", "--zopfli", "--characters", CHARACTERS],
                cwd=SCRIPTS, check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout, (FONTS / f"{name}.h").read_text())


if __name__ == "__main__":
    unittest.main()
