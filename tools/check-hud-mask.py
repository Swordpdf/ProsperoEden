#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual generated helper with sanitizers; run with clang++ installed."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
def check(code):
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder)/'check.cpp'; source.write_text(code)
        binary = Path(folder)/'check'
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                        '-I'+str(root/'headless'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

check("""
#include "hud.h"
#include <cassert>
int main() {
    using namespace Eden;
    assert(ValidHudFields(0));
    assert(ValidHudFields(kHudFieldAll));
    assert(ValidHudFields(kHudFieldSupported));
    assert(!ValidHudFields(-1));
    assert(!ValidHudFields(kHudFieldSupported+1));
    for(unsigned bit=0;bit<31;++bit)
        assert(ValidHudFields(1u<<bit)==((kHudFieldSupported & (1u<<bit))!=0));
}
""")
store=(root/'headless/settings_store.h').read_text()
assert 'ValidHudFields(hud_fields)' in store
assert '!ValidHudFields(value.hud_fields)' in store
bridge=root/'headless/prosperoeden/pe/ui/runtime_launcher.cpp'
if bridge.exists(): assert '!Eden::ValidHudFields(v.hud_fields)' in bridge.read_text()
print('PASS: overlay defaults and every supported bit accepted; unknown/negative bits rejected')
