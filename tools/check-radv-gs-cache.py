#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cached driver must rebuild for a missing or changed GS patch stamp."""
from pathlib import Path
import subprocess, sys, tempfile, shutil
root=Path(__file__).resolve().parents[1]
script=root/'tools/patch-radv-gs-compute.py'
with tempfile.TemporaryDirectory() as folder:
    stamp=Path(folder)/'stamp'
    def valid(tool=script):
        return subprocess.run([sys.executable,str(tool),'--check',str(stamp)]).returncode==0
    assert not valid()
    stamp.write_bytes(subprocess.check_output([sys.executable,str(script),'--digest']))
    assert valid()
    changed=Path(folder)/'patch.py'
    changed.write_text(script.read_text()+'\n# changed patch implementation\n')
    assert not valid(changed)
    stamp.write_text('old digest\n')
    assert not valid()
prepare=(root/'tools/prepare-build.sh').read_text()
build=(root/'tools/build-radv-dependencies.sh').read_text()
assert 'patch-radv-gs-compute.py --check "$radv_release/EDEN_GS_SHA256"' in prepare
assert 'patch-radv-gs-compute.py" --digest > "$vulkan/.deps/native/radv-release/EDEN_GS_SHA256"' in build
assert build.index('EDEN_GS_SHA256') > build.index('build-radv')
assert 'patch-radv-disk-cache.py --check "$radv_release/EDEN_DISK_CACHE_SHA256"' in prepare
assert 'patch-radv-disk-cache.py" --digest > "$vulkan/.deps/native/radv-release/EDEN_DISK_CACHE_SHA256"' in build
# Execute the actual prepare-build conditional with a cached driver and a stub builder.
start=prepare.index('if [[ -f $radv_release/lib/libvulkan_radeon.ps5.a ]]')
end=prepare.index('\nif [[ ! -f build/radv-isolated/',start)
with tempfile.TemporaryDirectory() as folder:
    work=Path(folder);(work/'tools').mkdir();(work/'release/lib').mkdir(parents=True)
    (work/'release/lib/libvulkan_radeon.ps5.a').touch()
    (work/'release/PROVENANCE.txt').write_text('revision: pinned\n')
    (work/'tools/patch-radv-wsi.py').write_text('raise SystemExit(0)\n')
    shutil.copyfile(script,work/'tools/patch-radv-gs-compute.py')
    disk=root/'tools/patch-radv-disk-cache.py'
    shutil.copyfile(disk,work/'tools/patch-radv-disk-cache.py')
    (work/'release/EDEN_DISK_CACHE_SHA256').write_bytes(subprocess.check_output([sys.executable,str(disk),'--digest']))
    (work/'tools/build-radv-dependencies.sh').write_text('echo rebuilt >> calls\n')
    body='set -euo pipefail\nradv_release=release\nmesa_pin=pinned\nstep() { :; }\n'+prepare[start:end]
    def rebuilt():
        (work/'calls').unlink(missing_ok=True)
        subprocess.run(['bash','-c',body],cwd=work,check=True)
        return (work/'calls').exists()
    assert rebuilt(), 'Existing driver without GS stamp must rebuild'
    (work/'release/EDEN_GS_SHA256').write_bytes(subprocess.check_output([sys.executable,str(script),'--digest']))
    assert not rebuilt(), 'Unchanged cached driver must be retained'
    with (work/'tools/patch-radv-gs-compute.py').open('a') as f: f.write('\n# changed\n')
    assert rebuilt(), 'Changed GS patch must rebuild the cached driver'
print('PASS: actual prepare-build cache decision rebuilds missing/changed GS stamp, skips unchanged')
