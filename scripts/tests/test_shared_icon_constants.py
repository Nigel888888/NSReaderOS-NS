#!/usr/bin/env python3
"""The icon headers must keep their bitmaps in a single merged copy.

Every icon lives in a header, and `UiAppHelpers.h` pulls those headers into
every activity that hosts a FreeInkApp. A `static const` bitmap has internal
linkage, so each including translation unit emits its own copy and the image
carries the same icon several times over. `inline constexpr` gives the objects
external linkage with vague linkage, so the linker keeps exactly one copy that
all units share.

This test guards that rewrite: it fails if a generated icon header goes back to
internal linkage, which would silently grow the firmware again.
"""

import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
ICONS = ROOT / "src/components/icons"
SCRIPTS = ROOT / "scripts"

spec = importlib.util.spec_from_file_location("share_icon_constants", SCRIPTS / "share_icon_constants.py")
share = importlib.util.module_from_spec(spec)
spec.loader.exec_module(share)

# Any icon declaration that still carries internal linkage.
INTERNAL = re.compile(r"^static (?:const|constexpr) (?:uint8_t|freeink::Icon) \w+", re.MULTILINE)


class SharedIconConstantsTest(unittest.TestCase):
    def test_no_icon_bitmap_keeps_internal_linkage(self):
        offenders = {}
        for path in sorted(ICONS.glob("*.h")):
            found = INTERNAL.findall(path.read_text(encoding="utf-8", errors="ignore"))
            if found:
                offenders[path.name] = len(found)
        self.assertEqual(offenders, {}, f"icon bitmaps still duplicated per translation unit: {offenders}")

    def test_the_rewrite_is_idempotent(self):
        for path, output in share.shared_outputs(ICONS):
            self.assertEqual(output, path.read_text(encoding="utf-8", errors="ignore"),
                             f"{path.name} is not in the shared form")

    def test_every_icon_descriptor_points_at_a_shared_array(self):
        for path in sorted(ICONS.glob("*.h")):
            text = path.read_text(encoding="utf-8", errors="ignore")
            for match in re.finditer(r"^inline constexpr freeink::Icon (\w+) = \{([^}]*)\};", text, re.MULTILINE):
                fields = [field.strip() for field in match.group(2).split(",")]
                self.assertEqual(len(fields), 4, f"{path.name}: {match.group(1)}")
                bits = fields[3]
                self.assertRegex(bits, r"^\w+$", f"{path.name}: {match.group(1)} bits argument")
                self.assertIn(f"inline constexpr uint8_t {bits}[] = {{", text,
                              f"{path.name}: {match.group(1)} points at a non-shared array")

    def test_the_generators_emit_the_shared_form(self):
        # A regenerated header must come back shared, or the duplication returns.
        for name in ("build_rickyos_page_icons.cjs", "build_rickyos_navigation_icons.cjs",
                     "build_rickyos_app_icons.cjs", "gen_ricky_battery_icons.py",
                     "build_rickyos_mascot_icons.py"):
            text = (SCRIPTS / name).read_text(encoding="utf-8")
            self.assertNotIn("static const uint8_t", text, name)
            self.assertNotIn("static constexpr uint8_t", text, name)
            self.assertIn("inline constexpr uint8_t", text, name)


if __name__ == "__main__":
    unittest.main()
