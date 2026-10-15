#!/usr/bin/env python3
"""Native launcher appearance persistence and crash-report layout regression."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='pe-appearance-') as temporary:
    out = Path(temporary)
    env = dict(os.environ, EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1',
               GALLIUM_DRIVER='llvmpipe', PE_LAUNCHER_DATA=str(out/'data'))
    commands = ['key '+key+' 30' for key in
                ['r1','r1','r1','down','down','right','right','right','cross']]
    commands += ['key cross 30', 'key down 30', 'key cross 30',
                 'key circle 30', 'key circle 30', 'key down 30', 'key cross 30',
                 'key down 30', 'key down 30', 'key cross 30', 'appearance',
                 'key circle 30', 'key up 30', 'key cross 30', 'key cross 30',
                 'key cross 30', 'appearance', 'select builtin', 'appearance',
                 'crash /data/prosperoeden/logs/crash-20261015-120000.txt', 'appearance']
    if len(sys.argv) > 2:
        capture = Path(sys.argv[2]).resolve()
        commands.insert(commands.index('appearance') + 1, 'capture '+str(capture.with_name('accents.png')))
        commands += ['capture '+str(capture)]
        commands += ['key r1 30'] * 2 + ['key down 30'] * 6 + ['key cross 30',
                     'capture '+str(capture.with_name('diagnostics.png'))]
    commands += ['exit']
    result = subprocess.run([sys.argv[1], str(root/'headless/prosperoeden/ui'), str(out), '960', '540'],
                            input='\n'.join(commands)+'\n', text=True, capture_output=True, env=env, timeout=90)
    assert result.returncode == 0, result.stderr
    appearances = [line for line in result.stdout.splitlines() if line.startswith('APPEARANCE ')]
    assert appearances == ['APPEARANCE slate ice 1'] * 4, result.stdout
    assert 'GL=128' not in result.stdout, result.stdout
    print('Accent survives launcher recreation and crash notice: PASS')
