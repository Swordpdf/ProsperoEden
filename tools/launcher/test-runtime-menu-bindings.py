#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check upstream menu bindings across native screens and runtime launcher packs."""
import argparse
import itertools
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
checks = 0

def need(condition, message):
    global checks
    assert condition, message
    checks += 1

with tempfile.TemporaryDirectory(prefix='pe-menu-bindings-') as temporary:
    out = Path(temporary)
    data = out / 'data'
    folder = data / 'launchers/binding-test'
    shutil.copytree(root / 'examples/grid-launcher/assets', folder / 'assets')
    manifest = json.loads((root / 'examples/grid-launcher/manifest.json').read_text())
    manifest['id'] = 'binding-test'
    (folder / 'manifest.json').write_text(json.dumps(manifest))
    permutations = list(itertools.permutations(range(4)))
    script = '''
local permutations = PERMUTATIONS
local step = 0
local function save(result, key)
    host.action("state.save", {result=result, key=key, preferences=host.query("preferences")})
end
return {init=function() end, draw=function() end, input=function(e)
    local result
    if e.key=="right" then
        step=step+1;result=host.action("preferences.set", {menu_bind=permutations[step]})
    elseif e.key=="left" then result=host.action("preferences.set", {menu_volume=37})
    elseif e.key=="down" then result=host.action("preferences.set", {menu_bind={0,0,2,3}})
    elseif e.key=="up" then result=host.action("preferences.set", {menu_bind={0,1,2,3,0}})
    elseif e.key=="options" then result=host.action("preferences.set", {menu_bind={0,1,2,3.5}})
    elseif e.key=="triangle" then result=host.action("preferences.set", {menu_bind={0,1,2,-1}})
    else result={ok=true} end
    save(result,e.key)
end}
'''.replace('PERMUTATIONS', '{' + ','.join('{' + ','.join(map(str, p)) + '}' for p in permutations) + '}')
    (folder / 'main.lua').write_text(script)
    (data / 'config').mkdir()
    (data / 'config/launcher.json').write_text('{"schema":1,"selected":"binding-test"}')
    env = dict(os.environ, EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1',
               GALLIUM_DRIVER='llvmpipe', PE_LAUNCHER_DATA=str(data))
    with (out / 'stderr.log').open('w') as errors:
        process = subprocess.Popen([str(args.binary.resolve()), str(root / 'headless/prosperoeden/ui'),
                                    str(out), '960', '540'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors, text=True, env=env)
        need(process.stdout.readline().startswith('READY binding-test'), 'test pack loads')
        def command(value):
            process.stdin.write(value + '\n')
            process.stdin.flush()
            lines = []
            while True:
                line = process.stdout.readline()
                assert line, 'Preview stopped during ' + value
                lines.append(line)
                if line.startswith('OK '):
                    need('GL=0' in line, 'renderer: ' + value)
                    return ''.join(lines)
        def key(value, count=1):
            for _ in range(count): command('key ' + value + ' 6')
        def state():
            return json.loads((data / 'config/launcher-state/binding-test.json').read_text())
        def preferences():
            lines = command('prefs').splitlines()
            return json.loads(next(line[len('PREFERENCES '):] for line in lines if line.startswith('PREFERENCES ')))
        original_game_mapping = preferences()['mapping']
        for binding in permutations:
            key('right')
            need(state()['result']['ok'] and state()['preferences']['menu_bind'] == list(binding),
                 'all 24 menu permutations survive the runtime/native model bridge')
        key('left')
        need(state()['preferences']['menu_bind'] == list(permutations[-1]), 'partial patches preserve bindings')
        before = preferences()
        for button in ['down', 'up', 'options', 'triangle']:
            key(button)
            need(not state()['result']['ok'], 'invalid binding rejected: ' + button)
            need(preferences() == before, 'rejection preserves preferences: ' + button)
        # Native picker uses Select=Triangle and Back=Square under this permutation.
        command('picker')
        key('square')
        key('cross')
        need(state()['key'] == 'cross', 'pack input remains physical after picker cancellation')
        command('picker')
        key('triangle')
        need('STATE active=builtin' in command('status'), 'native picker honors remapped Select')
        command('tick 180')
        # Reset through the real built-in Controls > Menu keybinds page.
        key('r1', 3)
        key('down')
        key('triangle')
        key('down', 3)
        key('triangle')
        key('circle')  # Secondary is Circle under the reversed permutation: reset.
        need(preferences()['menu_bind'] == [0, 1, 2, 3], 'native menu keybind editor resets bindings')
        key('right')
        need(preferences()['menu_bind'] == [1, 0, 2, 3], 'native editor saves swapped assignments')
        key('cross', 2)  # Remapped Back returns to Controls, then Settings.
        command('picker')
        key('cross')
        command('picker')
        key('down')
        key('circle')
        need('STATE active=binding-test' in command('status'), 'launcher switching retains edited native bindings')
        need(preferences()['mapping'] == original_game_mapping, 'gameplay bindings remain unchanged')
        process.stdin.write('exit\n')
        process.stdin.flush()
        process.wait(timeout=20)
        need(process.returncode == 0, 'clean shutdown')
    need('PE_LAUNCHER_REJECTED' not in (out / 'stderr.log').read_text(), 'no runtime script failure')
print(json.dumps({'passed': True, 'checks': checks}))
