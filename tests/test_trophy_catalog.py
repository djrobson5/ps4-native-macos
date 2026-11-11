"""The trophy catalogs (gpu/shim/trophy_catalog*.json) and their generator: ids must run 0..n-1,
a gap or a bad row is refused (non-zero exit, nothing written), and 13 Sentinels' catalog has the
shape its ledger and the packaging script rely on.
Run: python3 tests/test_trophy_catalog.py  (ctest runs it too)"""
import json
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

SHIM = Path(__file__).resolve().parent.parent / "gpu" / "shim"
GENERATOR = SHIM / "gen_trophy_catalog.py"
BLOODBORNE = SHIM / "trophy_catalog.json"
SENTINELS = SHIM / "trophy_catalog_13s.json"


def generate(directory, *pairs):
    out = Path(directory) / "out.inc"
    result = subprocess.run([sys.executable, "-I", str(GENERATOR), str(out), *pairs],
                            capture_output=True, text=True)
    return result, out


class Generator(unittest.TestCase):
    def mutated(self, directory, change):
        catalog = json.loads(SENTINELS.read_text(encoding="utf-8"))
        change(catalog)
        path = Path(directory) / "bad.json"
        path.write_text(json.dumps(catalog), encoding="utf-8")
        return path

    def refused(self, change, message):
        with tempfile.TemporaryDirectory() as directory:
            result, out = generate(directory, f"CUSA19610={self.mutated(directory, change)}")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)
            self.assertFalse(out.exists())

    def test_both_catalogs_generate_with_their_title_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            result, out = generate(directory, f"CUSA03173={BLOODBORNE}", f"CUSA19610={SENTINELS}")
            self.assertEqual(result.returncode, 0, result.stderr)
            text = out.read_text(encoding="utf-8")
            self.assertEqual(text.count('{"CUSA19610", '), 56)
            self.assertIn('{"CUSA19610", 55, "en", "Supreme Defender"', text)
            self.assertIn('{"CUSA03173", 39, "en"', text)

    def test_a_gap_in_the_ids_is_refused(self):
        self.refused(lambda c: c["trophies"].pop(20), "0..n-1")

    def test_a_repeated_id_is_refused(self):
        self.refused(lambda c: c["trophies"][5].update(id=4), "0..n-1")

    def test_ids_not_starting_at_zero_are_refused(self):
        def shift(catalog):
            for trophy in catalog["trophies"]:
                trophy["id"] += 1
        self.refused(shift, "0..n-1")

    def test_an_unknown_grade_is_refused(self):
        self.refused(lambda c: c["trophies"][3].update(grade="diamond"), "grade")

    def test_a_pipe_or_line_break_in_a_name_or_description_is_refused(self):
        def pipe(catalog):
            catalog["trophies"][3]["translations"]["en"]["name"] = "A | B"
        def newline(catalog):
            catalog["trophies"][3]["translations"]["en"]["description"] = "A\nB"
        self.refused(pipe, "'|' or a line break")
        self.refused(newline, "'|' or a line break")

    def test_the_same_title_twice_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _ = generate(directory, f"CUSA19610={SENTINELS}", f"CUSA19610={SENTINELS}")
            self.assertNotEqual(result.returncode, 0)


class SentinelsCatalog(unittest.TestCase):
    catalog = json.loads(SENTINELS.read_text(encoding="utf-8"))

    def test_56_trophies_with_the_public_grades(self):
        trophies = self.catalog["trophies"]
        self.assertEqual([t["id"] for t in trophies], list(range(56)))
        self.assertEqual(trophies[0]["grade"], "platinum")
        self.assertEqual(Counter(t["grade"] for t in trophies),
                         {"platinum": 1, "gold": 2, "silver": 5, "bronze": 48})
        self.assertEqual(self.catalog["default_locale"], "en")
        for trophy in trophies:
            self.assertEqual(list(trophy["translations"]), ["en"])

    def test_names_and_descriptions_are_ledger_safe_and_present(self):
        names = set()
        for trophy in self.catalog["trophies"]:
            text = trophy["translations"]["en"]
            for field in ("name", "description"):
                self.assertTrue(text[field].strip(), trophy["id"])
                self.assertNotRegex(text[field], r"[|\r\n]")
            names.add(text["name"])
        self.assertEqual(len(names), 56)

    def test_icons_are_public_cdn_links_only(self):
        for trophy in self.catalog["trophies"]:
            self.assertRegex(trophy["icon_url"], r"^https://image\.api\.playstation\.com/trophy/np/[A-Za-z0-9_/.-]+\.PNG$")
        self.assertEqual(len({t["icon_url"] for t in self.catalog["trophies"]}), 56)

    def test_the_numbering_is_noted_as_unverified(self):
        self.assertIn("unverified", self.catalog["notes"])


if __name__ == "__main__":
    unittest.main()
