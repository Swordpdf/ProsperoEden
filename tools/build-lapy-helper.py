#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and verify the pinned upstream Lapy helper for ProsperoEden."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
TITLE = "PPSA99008"
LAPY = ROOT / ".deps/PS5-Lapy-JB-Daemon-57fcc19"
PS5LOG = ROOT / ".deps/lapy-ps5log-1ae1f918"
SDK = ROOT / ".deps/lapy-ps5-payload-sdk-v0.42"
SOURCE = LAPY / f"build/owned_root_helper-{TITLE}"
OUTPUT = ROOT / "build/lapy-owned-helper"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    environment = os.environ.copy()
    environment.update(PS5_PAYLOAD_SDK=str(SDK), LOGGING_CLIENT=str(PS5LOG))
    subprocess.run(["make", "owned-helper", f"TARGET_TITLE={TITLE}"], cwd=LAPY,
                   env=environment, check=True)

    elf = SOURCE / "lapy.elf"
    manifest_path = SOURCE / "lapy-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    protocol = LAPY / "source/lapy_elevation_protocol.h"
    required = {
        "schema": "lapy-owned-build/1",
        "target_title": TITLE,
        "mode": "elf-helper",
        "max_requests": 1,
        "service": False,
        "require_client_result": False,
        "console_validated": False,
    }
    for key, expected in required.items():
        if manifest.get(key) != expected:
            raise RuntimeError(f"Lapy manifest {key}: {manifest.get(key)!r}, expected {expected!r}")
    if manifest.get("features") != ["root_layout_probe_retry"]:
        raise RuntimeError("Lapy helper lacks the required root-layout retry feature")
    if manifest.get("elf_sha256") != digest(elf):
        raise RuntimeError("Lapy helper differs from its upstream build manifest")
    if manifest.get("protocol_sha256") != digest(protocol):
        raise RuntimeError("Lapy protocol differs from its upstream build manifest")
    if elf.read_bytes()[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError("Lapy helper is not a little-endian ELF64")

    OUTPUT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(elf, OUTPUT / "lapy.elf")
    shutil.copy2(manifest_path, OUTPUT / "lapy-manifest.json")
    shutil.copy2(LAPY / "LICENSE", OUTPUT / "LICENSE.Lapy")
    print(f"Lapy helper verified: {manifest['build_id']} {manifest['elf_sha256']}")


if __name__ == "__main__":
    main()
