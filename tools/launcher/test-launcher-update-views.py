#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise launcher-owned update views using the fixture-linked runtime preview."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
checks = []
script = '''
local update
local function draw()
    host.draw("rect", {x=0,y=0,w=1920,h=1080,color=0x303030})
    host.draw("text", {text="Update example",x=200,y=200,size=32})
end
return {
 init=function(e) assert(host.query("runtime").api_minor>=1) end,
 draw=draw,
 input=function(e)
    if e.key=="triangle" then host.action("launcher_updates.open",{view="launcher"})
    elseif e.key=="square" then host.action("state.save",{preserved=42}) end
 end,
 updater_status=function(e)
    update=e.status
    for _,name in ipairs({"profile.add","preferences.set","state.save","launcher.select",
                          "launcher_updates.download","launcher_updates.cancel"}) do
        assert(host.action(name,{}).ok==false,"Action escaped updater restrictions")
    end
    assert(host.query("game_files",{}).ok==false)
    assert(host.query("state",{}).ok==false)
    assert(host.query("runtime").api_minor>=1)
 end,
 updater_input=function(e)
    if e.key=="circle" then host.action("launcher_updates.close")
    elseif e.key=="cross" and update and update.phase=="available" then
        assert(host.action("launcher_updates.download",{confirmed=false,version=update.release.version}).ok==false)
        assert(host.action("launcher_updates.download",{confirmed=true,version="9.9.9"}).ok==false)
        assert(host.action("launcher_updates.download",{confirmed=true,version=update.release.version}).ok)
    end
 end,
 updater_draw=function(e)
    assert(host.draw("cover",{game="missing"}).ok==false)
    draw()
 end
}
'''

with tempfile.TemporaryDirectory(prefix='pe-updater-views-') as temporary:
    temporary = Path(temporary)
    data = temporary/'data'
    pack = data/'launchers/update-example'
    fixtures = temporary/'fixtures'
    fixtures.mkdir()
    (data/'config').mkdir(parents=True)
    (data/'config/launcher.json').write_text('{"schema":1,"selected":"update-example"}')

    def prepare(source=script, bad=False):
        shutil.rmtree(pack, ignore_errors=True)
        shutil.copytree(root/'examples/grid-launcher', pack)
        manifest = json.loads((pack/'manifest.json').read_text())
        manifest.update(id='update-example', version='1.0.0', api={'major':1,'min_minor':1},
                        updates={'feed':'https://updates.example/launcher-updates.json'})
        (pack/'manifest.json').write_text(json.dumps(manifest))
        (pack/'main.lua').write_text(source)
        candidate = dict(manifest, version='1.1.0')
        archive = fixtures/'update-example-1.1.0.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zipped:
            for path in pack.rglob('*'):
                if path.is_file():
                    content = path.read_bytes()
                    if path.name == 'manifest.json': content = json.dumps(candidate).encode()
                    if path.name == 'main.lua':
                        content = ('return {init=function() error("fixture init failure") end,draw=function() end}'
                                   if bad else script).encode()
                    zipped.writestr('update-example/'+path.relative_to(pack).as_posix(), content)
        release = {key:candidate[key] for key in ['version','host_contract','api','state_schema','required_capabilities']}
        release.update(url='https://updates.example/'+archive.name, bytes=archive.stat().st_size,
                       sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        (fixtures/'launcher-updates.json').write_text(json.dumps({'schema':1,'id':'update-example','releases':[release]}))

    class Preview:
        def __enter__(self):
            self.log = (temporary/'stderr.log').open('w')
            env = dict(os.environ, EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1',
                       GALLIUM_DRIVER='llvmpipe', PE_LAUNCHER_DATA=str(data), PE_UPDATE_FIXTURES=str(fixtures))
            self.process = subprocess.Popen([str(args.binary.resolve()), str(root/'headless/prosperoeden/ui'),
                                             str(temporary), '640', '360'], stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=self.log, text=True, env=env)
            while not self.process.stdout.readline().startswith('READY '):
                assert self.process.poll() is None, 'Preview exited during startup'
            return self

        def command(self, command):
            self.process.stdin.write(command+'\n')
            self.process.stdin.flush()
            result = ''
            while True:
                line = self.process.stdout.readline()
                assert line, 'Preview exited: '+(temporary/'stderr.log').read_text()
                result += line
                if line.startswith('OK '):
                    assert 'GL=0' in line, line
                    return result

        def version(self): return json.loads((pack/'manifest.json').read_text())['version']

        def wait_version(self, version):
            for _ in range(200):
                self.command('tick 30')
                if self.version() == version and 'active=update-example' in self.command('status'): return
                time.sleep(.01)
            raise AssertionError('Update did not activate '+version)

        def confirm(self):
            for _ in range(100):
                self.command('key cross 10')
                if self.version() != '1.0.0': return
                time.sleep(.01)
            raise AssertionError('No update was installed: '+(temporary/'stderr.log').read_text())

        def __exit__(self, *_):
            self.process.stdin.write('exit\n')
            self.process.stdin.flush()
            assert self.process.wait(timeout=20) == 0
            self.log.close()

    prepare()
    with Preview() as preview:
        preview.command('key square 3')
        preview.command('key triangle 30')
        assert preview.version() == '1.0.0'
        checks.append('Opening a launcher view requires confirmation before installation')
        preview.confirm()
        preview.wait_version('1.1.0')
        assert 'PE_LAUNCHER_UPDATE_VIEW_NATIVE' not in (temporary/'stderr.log').read_text()
        checks.append('Restricted view survives replacement without loading old resources from disk')
        preview.command('recreate')
        assert 'Display resources changed during update' in (temporary/'stderr.log').read_text()
        checks.append('Display recreation safely falls back to native drawing after replacement')
        preview.command('key circle 30')
        assert json.loads((data/'config/launcher-state/update-example.json').read_text())['preserved'] == 42
        checks.append('New launcher resumes with saved state and selection intact')

    for name, original, replacement, key in [
        ('status', 'update=e.status', 'error("status fixture")', None),
        ('draw', 'updater_draw=function(e)', 'updater_draw=function(e) error("draw fixture")', None),
        ('input', 'updater_input=function(e)', 'updater_input=function(e) error("input fixture")', 'cross'),
        ('budget', 'updater_draw=function(e)', 'updater_draw=function(e) while true do end', None),
    ]:
        prepare(script.replace(original, replacement))
        with Preview() as preview:
            preview.command('key triangle 30')
            if key: preview.command('key '+key+' 3')
            assert 'PE_LAUNCHER_UPDATE_VIEW_NATIVE' in (temporary/'stderr.log').read_text(), name
            assert 'active=update-example' in preview.command('status')
            assert preview.version() == '1.0.0'
            preview.command('key circle 30')
            checks.append(name+' callback failure preserves the main pack and uses native recovery')

    prepare(bad=True)
    with Preview() as preview:
        preview.command('key triangle 30')
        # A failed replacement can be restored before the polling client sees its version.
        for _ in range(200):
            preview.command('key cross 10')
            if 'fixture init failure' in (temporary/'stderr.log').read_text(): break
            time.sleep(.01)
        else: raise AssertionError('Failed-init fixture never activated')
        preview.wait_version('1.0.0')
        preview.command('key circle 30')
        checks.append('Failed replacement initialization restores the previous launcher')

print(json.dumps({'passed':len(checks),'cases':checks}, indent=2))
