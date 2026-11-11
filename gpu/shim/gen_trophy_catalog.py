#!/usr/bin/env python3
"""Generate the in-game notification table from the public, reviewed trophy catalogs.

  gen_trophy_catalog.py OUT.inc TITLE_ID=catalog.json [TITLE_ID=catalog.json ...]

Every catalog's ids must be exactly 0..n-1 in order (a gap or a repeat is an error: the exit code is
non-zero and nothing is written), grades must be platinum/gold/silver/bronze, and every English name
and description must be free of `|` and line breaks, because the 13 Sentinels ledger
(achievements.txt, src/runtime_trophies.c) writes them as `id = name | description`.
"""
import json
import re
import sys
from pathlib import Path

GRADES = {"platinum", "gold", "silver", "bronze"}
TITLE = re.compile(r"^[A-Z0-9]{1,16}$")


def quoted(value):
    return json.dumps(value, ensure_ascii=False)


def load(title, path):
    if not TITLE.match(title):
        sys.exit(f"{path}: bad title ID {title!r}")
    catalog = json.loads(Path(path).read_text(encoding="utf-8"))
    if catalog.get("schema_version") != 1 or catalog.get("default_locale") != "en":
        sys.exit(f"{path}: schema_version must be 1 and default_locale en")
    ids = [t["id"] for t in catalog["trophies"]]
    if ids != list(range(len(ids))):
        sys.exit(f"{path}: trophy ids must be 0..n-1 in order, got {ids}")
    for trophy in catalog["trophies"]:
        if trophy["grade"] not in GRADES:
            sys.exit(f"{path}: trophy {trophy['id']} has grade {trophy['grade']!r}")
        english = trophy["translations"].get("en")
        if not english or not english.get("name"):
            sys.exit(f"{path}: trophy {trophy['id']} has no English name")
        for field in ("name", "description"):
            if re.search(r"[|\r\n]", english.get(field, "")):
                sys.exit(f"{path}: trophy {trophy['id']}: {field} holds '|' or a line break")
    return catalog


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out, pairs = Path(argv[1]), argv[2:]
    rows = ["// Generated from trophy_catalog*.json by gen_trophy_catalog.py. Do not edit.",
            "static constexpr TrophyText kTrophyTexts[] = {"]
    seen = set()
    for pair in pairs:
        title, _, path = pair.partition("=")
        if not path:
            sys.exit(f"expected TITLE_ID=catalog.json, got {pair!r}")
        if title in seen:
            sys.exit(f"title {title} given twice")
        seen.add(title)
        for trophy in load(title, path)["trophies"]:
            for language, text in trophy["translations"].items():
                rows.append(f'    {{{quoted(title)}, {trophy["id"]}, {quoted(language)}, {quoted(text["name"])}, '
                            f'{quoted(text.get("description", ""))}, {quoted(trophy["grade"])}, '
                            f'{quoted(trophy["group"])}}},')
    rows.append("};")
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main(sys.argv)
