#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RADV's shader cache on disk stays on whichever user the app runs as.

Mesa turns its disk cache off when the effective user or group differs from the real one: on a
desktop that is a setuid program, which must not write a cache the real user owns. On the
console it is what an elevated app looks like when a jailbreak changes only some of its ids, and
there the cache is the app's own folder (MESA_SHADER_CACHE_DIR, headless/main.cpp). Without the
cache every pipeline a game has is compiled again at every start, which for a game with many
shaders is minutes of loading each time. Applied to the driver checkout after
patch-radv-gs-compute.py; running it again changes nothing.
"""
from pathlib import Path
import sys
import hashlib

fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)

root = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / 'mihawk-vulkan-review/.deps/work/radv-src'

os_file = src / 'src/util/disk_cache_os.c'
text = os_file.read_text()
check = '''   /* If running as a users other than the real user disable cache */
   if (!__normal_user())
      return false;
'''
kept = '''   /* ProsperoEden: the console app's ids may differ after elevation; its cache folder is its
    * own, so the cache stays on (tools/patch-radv-disk-cache.py). */
'''
# Checked by its code: a tree already patched has no user check left in disk_cache_enabled().
if kept not in text:
    assert text.count(check) == 1, 'disk_cache_enabled changed'
    os_file.write_text(text.replace(check, kept))
enabled = os_file.read_text().split('disk_cache_enabled()', 1)[1].split('\n}\n', 1)[0]
assert '__normal_user' not in enabled, 'disk_cache_enabled still checks the user'
print('RADV disk cache: on for an elevated app')
