#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end native confirmation, activation and failed-init rollback using fixture HTTP.

Pass the runtime preview linked with test-launcher-update-transport.cpp instead of remote/http.c.
"""
import argparse, hashlib, json, os
from pathlib import Path
import shutil, subprocess, tempfile, zipfile

parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path);args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
checks=[]
with tempfile.TemporaryDirectory(prefix='pe-updater-ui-') as temp:
    temp=Path(temp);data=temp/'data';pack=data/'launchers/update-test';fixtures=temp/'fixtures';fixtures.mkdir()
    shutil.copytree(root/'examples/grid-launcher',pack)
    base=json.loads((pack/'manifest.json').read_text());base.update(id='update-test',version='1.0.0',updates={'feed':'https://updates.example/launcher-updates.json'})
    (data/'config').mkdir();(data/'config/launcher.json').write_text('{"schema":1,"selected":"update-test"}')
    script='''
return {init=function(e)
  local runtime=host.query("runtime")
  assert(runtime.api_major==1 and runtime.api_minor==1)
end,draw=function() host.draw("text",{text="VERSION",x=200,y=200,size=32}) end,
input=function(e)
  if e.key=="triangle" then host.action("launcher_updates.open")
  elseif e.key=="square" then host.action("state.save",{preserved=42}) end
end}
'''
    def prepare(bad=False):
        (pack/'manifest.json').write_text(json.dumps(base));(pack/'main.lua').write_text(script.replace('VERSION','Original'))
        release_pack=temp/'candidate';shutil.rmtree(release_pack,ignore_errors=True);shutil.copytree(pack,release_pack)
        updated=dict(base,version='1.1.0');(release_pack/'manifest.json').write_text(json.dumps(updated))
        (release_pack/'main.lua').write_text('return {init=function() error("fixture init failure") end,draw=function() end}' if bad else script.replace('VERSION','Updated'))
        archive=fixtures/'update-test-1.1.0.zip'
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as zipped:
            for path in release_pack.rglob('*'):
                if path.is_file():zipped.write(path,'update-test/'+path.relative_to(release_pack).as_posix())
        release={key:updated[key] for key in ['version','host_contract','api','state_schema','required_capabilities']}
        release.update(url='https://updates.example/update-test-1.1.0.zip',bytes=archive.stat().st_size,sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),notes='Fixture update')
        (fixtures/'launcher-updates.json').write_text(json.dumps({'schema':1,'id':'update-test','releases':[release]}))
    env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',PE_LAUNCHER_DATA=str(data),PE_UPDATE_FIXTURES=str(fixtures))
    def run(commands,name):
        result=subprocess.run([str(args.binary.resolve()),str(root/'headless/prosperoeden/ui'),str(temp),'640','360'],input=commands+'status\nexit\n',text=True,capture_output=True,env=env,timeout=40)
        assert result.returncode==0,(name,result.stdout,result.stderr)
        assert all('GL=0' in line for line in result.stdout.splitlines() if line.startswith('OK ')),result.stdout
        assert 'STATE active=update-test' in result.stdout,(name,result.stdout,result.stderr)
        return result
    prepare();run('key square 3\nkey triangle 30\ntick 90\nkey circle 3\n','confirmation only')
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.0.0';checks.append('opening updates never installs without confirmation')
    result=run('key triangle 30\ntick 90\nkey cross 30\ntick 240\nkey circle 5\n','install')
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.1.0',result.stderr
    assert json.loads((data/'config/launcher-state/update-test.json').read_text())['preserved']==42
    assert 'PE_LAUNCHER_SELECTED id=builtin' in result.stderr;checks.append('active pack unloaded, replaced and reactivated with state retained')
    result=run('key triangle 30\ntick 60\nkey square 3\nkey cross 30\ntick 180\nkey circle 5\n','rollback')
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.0.0';checks.append('host confirmation rolls back active pack')
    prepare(bad=True)
    result=run('key triangle 30\ntick 90\nkey cross 30\ntick 300\nkey circle 5\n','failed init')
    assert 'fixture init failure' in result.stderr,result.stderr
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.0.0';checks.append('failed new init automatically restores and reactivates previous pack')
    assert json.loads((data/'config/launcher.json').read_text())['selected']=='update-test';checks.append('saved selection preserved')
print(json.dumps({'passed':len(checks),'cases':checks},indent=2))
