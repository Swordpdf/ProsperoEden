#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the PS5 directory-reader branch against a Linux syscall adapter."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='eden-launcher-fs-') as temporary:
    output = Path(temporary)
    binary = output / 'test'
    subprocess.run([
        os.environ.get('HOST_CXX', 'c++'), '-std=c++20', '-O2', '-Wall', '-Wextra',
        '-DPE_RUNTIME_NATIVE_FS_TEST=1', '-I' + str(root / 'headless/prosperoeden'),
        str(root / 'tools/launcher/test-runtime-filesystem.cpp'),
        '-Wl,--wrap=lstat', '-Wl,--wrap=opendir', '-o', str(binary),
    ], check=True)
    result = subprocess.run([str(binary), str(output / 'data')], text=True, capture_output=True, check=True)
    print(json.dumps({'passed': True, 'result': result.stdout.strip(),
                      'scope': 'Native reader with simulated sandbox denials; no console hardware.'}))
