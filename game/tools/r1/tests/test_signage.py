"""Contract tests for the road signs (`signage.py`).

The predictive model decides where a sign stands; these hold what it looks
like to the project's rules:

  - every colour is a measured luminance factor of retroreflective sheeting
    (EN 12899-1, class RA1), not a paint chip (CLAUDE.md rule 2);
  - every sign the catalogue lists has its model and its faces on disk, on
    both mounts, at the heights the IISR sets;
  - a sign is a few hundred vertices: a thousand of them cost less than one
    city block of the arena (rule 5).
"""

from __future__ import annotations

import json
import unittest

from r1 import signage
from r1.signage import CATALOGUE, GAME, MOUNTS, PLATES, SIGNS


def _luminance(c) -> float:
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


class SheetingIsMeasured(unittest.TestCase):
    def test_every_colour_is_within_its_class_range(self):
        # EN 12899-1 table 8, luminance factor beta: white >= 0.35,
        # yellow >= 0.27, red 0.03-0.15, black <= 0.03.
        self.assertGreaterEqual(_luminance(signage.WHITE), 0.35)
        self.assertLessEqual(_luminance(signage.WHITE), 0.45)
        self.assertGreaterEqual(_luminance(signage.YELLOW), 0.26)
        self.assertLessEqual(_luminance(signage.YELLOW), 0.35)
        self.assertTrue(0.03 <= _luminance(signage.RED) <= 0.15)
        self.assertLessEqual(_luminance(signage.BLACK), 0.03)


class CatalogueIsWhole(unittest.TestCase):
    def setUp(self):
        self.doc = json.loads(CATALOGUE.read_text(encoding="utf-8"))

    def test_the_catalogue_lists_every_sign_with_both_mounts_on_disk(self):
        self.assertEqual(set(self.doc["signs"]), set(SIGNS))
        for code, entry in self.doc["signs"].items():
            self.assertEqual(set(entry["mounts"]), set(MOUNTS))
            for mount in entry["mounts"].values():
                self.assertTrue((GAME / mount["model"]).exists(), mount["model"])
                self.assertLess(mount["vertices"], 600, code)

    def test_every_face_is_drawn_for_every_range(self):
        for code in PLATES:
            for _, rng in MOUNTS.values():
                self.assertTrue(signage.face_path(code, rng).exists(), (code, rng))

    def test_the_town_kit_draws_every_capital_a_french_place_is_written_in(self):
        kit = self.doc["towns"]["FR"]
        for ch in "ABCDEFGHIJKLMNOPQRSTUVWXYZÀÂÄÇÉÈÊËÎÏÔÖÙÛÜŸ-'":
            self.assertIn(ch, kit["glyphs"])
            self.assertTrue((GAME / kit["glyphs"][ch]["model"]).exists(), ch)
        for plate in kit["plates"].values():
            for part in ("left", "middle", "right"):
                self.assertTrue((GAME / plate[part]).exists())
        self.assertTrue((GAME / kit["post"]).exists() and (GAME / kit["bar"]).exists())

    def test_plates_stand_where_the_iisr_puts_them(self):
        # The lowest plate's lower edge: 1 m in the country, 2.30 m in town,
        # so the top of the sign stands above that by its plates.
        for code in SIGNS:
            rural = self.doc["signs"][code]["mounts"]["rural"]["height"]
            urban = self.doc["signs"][code]["mounts"]["urban"]["height"]
            self.assertGreater(rural, 1.4)
            self.assertGreater(urban, 2.6)


if __name__ == "__main__":
    unittest.main()
