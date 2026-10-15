#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual wrapper-generation block for host/native and release/dev builds."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'headless/CMakeLists.txt').read_text()
a=s.index("    # The overlay's four core loads")
a=s.index('    if(',a)
b=s.index('    if(EDEN_DEV_PROFILE)',a)
block=s[a:b]
with tempfile.TemporaryDirectory() as tmp:
    script=Path(tmp)/'check.cmake'
    for native in (False,True):
        for dev in (False,True):
            expected='ON' if native and not dev else 'OFF'
            script.write_text(f"set(PS5_NATIVE {int(native)})\nset(EDEN_DEV_PROFILE {int(dev)})\n"+
                'set(wrapper "    return TranslateHaltReason(m_jit->Run());")\n'+block+
                'string(FIND "${wrapper}" "SampleCoreCpu" at)\n'+
                f'if(({expected} AND at LESS 0) OR (NOT {expected} AND NOT at LESS 0))\n'+
                'message(FATAL_ERROR "Overlay wrapper emitted for the wrong build")\nendif()\n')
            subprocess.run(['cmake','-P',str(script)],check=True)
print('PASS: host wrappers have no overlay symbol; native release wrapper samples core CPU')
