#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native keyboard parameters without opening a system dialog."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="eden-keyboard-") as directory:
    executable = Path(directory) / "keyboard-check"
    subprocess.run(["c++", "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                    str(root / "headless/system_keyboard_check.cpp"), "-o", str(executable)],
                   check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print("Native keyboard: ordinary, numeric, password and numeric-password parameters PASS")
