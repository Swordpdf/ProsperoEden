#!/usr/bin/env python3
# ProsperoEden - The launcher's screens take every colour from the theme in use.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""check-launcher-theme.py

A theme fills colour roles (docs/THEMES.md); a screen that writes a colour of its own would keep
that colour in every theme. This fails when

  - a file of the launcher's screens (headless/prosperoeden/pe/ui) other than the themes' own
    writes a colour as a number (Color::rgb(0x...)), or names one of the constants the themes
    replaced (theme::k...),
  - the UI kit's folder that the console build compiles the theme table from is not the one
    tools/deps.json fetches.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCREENS = ROOT / "headless/prosperoeden/pe/ui"
THEMES = {"theme.cpp", "theme_kit.cpp"}


def main():
    problems = []
    for source in sorted(SCREENS.glob("*.[ch]pp")):
        if source.name in THEMES:
            continue
        for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if re.search(r"Color::rgb\(\s*0x", line):
                problems.append(f"{source.name}:{number}: a colour written as a number; name its role (th().accent, ...)")
            if re.search(r"\btheme::k[A-Z]", line):
                problems.append(f"{source.name}:{number}: a constant the themes replaced; use th()")
    manifest = json.loads((ROOT / "tools/deps.json").read_text(encoding="utf-8"))
    kit = next((item for item in manifest["items"] if item["name"] == "ui-kit"), None)
    if kit is None:
        problems.append("tools/deps.json: no ui-kit input")
    else:
        folder = kit["dest"] + "/src"
        if folder not in (ROOT / "headless/CMakeLists.txt").read_text(encoding="utf-8"):
            problems.append(f"headless/CMakeLists.txt does not compile the theme table from {folder}")
    for problem in problems:
        print(f"check-launcher-theme: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("check-launcher-theme: the screens name roles, and the kit's folder is the pinned one")
    return 0


if __name__ == "__main__":
    sys.exit(main())
