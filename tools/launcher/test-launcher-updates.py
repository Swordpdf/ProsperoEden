#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the native updater with local fixtures, including hostile archives and crash recovery.

Usage: test-launcher-updates.py <test-updates binary>
Build instructions are in docs/LAUNCHER_UPDATES.md. No requests are sent to the network.
"""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import warnings
import zipfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
args = parser.parse_args()
binary = args.binary.resolve()
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('publisher', root / 'tools/launcher/package-update.py')
publisher = importlib.util.module_from_spec(spec); spec.loader.exec_module(publisher)
passed = []

with tempfile.TemporaryDirectory(prefix='pe-pack-updates-') as temporary:
    temporary = Path(temporary)
    data = temporary / 'data'
    fixtures = temporary / 'fixtures'; fixtures.mkdir()
    original = json.loads((root / 'examples/grid-launcher/manifest.json').read_text())
    original.update(id='test-pack', version='1.0.0', updates={'feed': 'https://updates.example/launcher-updates.json'})
    updated = dict(original, version='1.1.0')
    pack = data / 'launchers/test-pack'

    def reset():
        if data.exists(): shutil.rmtree(data)
        pack.mkdir(parents=True)
        (pack / 'assets').mkdir()
        (pack / 'assets/font.pefont').write_bytes(b'fixture-font')
        (pack / 'main.lua').write_text('return {init=function() end,draw=function() end}')
        (pack / 'manifest.json').write_text(json.dumps(original))
        (data / 'config/launcher-state').mkdir(parents=True)
        (data / 'config/launcher-state/test-pack.json').write_text('{"screen":"library","focus":7}')
        (data / 'config/launcher.json').write_text('{"schema":1,"selected":"test-pack"}')
        (data / 'games').mkdir(); (data / 'games/untouched').write_text('game data')

    def archive(manifest=None, entries=None):
        files = {'test-pack/manifest.json': json.dumps(updated if manifest is None else manifest),
                 'test-pack/main.lua': 'return {init=function() end,draw=function() end}',
                 'test-pack/assets/font.pefont': b'new-font'}
        output = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as zipped:
                for name, value in files.items(): zipped.writestr(name, value)
                for name, value in entries or []: zipped.writestr(name, value)
        return output.getvalue()

    def feed(blob=None, releases=None):
        blob = archive() if blob is None else blob
        (fixtures / 'test-pack-1.1.0.zip').write_bytes(blob)
        release = {key: updated[key] for key in ['version','host_contract','api','state_schema','required_capabilities']}
        release.update(url='https://updates.example/test-pack-1.1.0.zip', bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(), notes='Test update')
        result = {'schema':1,'id':'test-pack','releases': releases if releases is not None else [release]}
        (fixtures / 'launcher-updates.json').write_text(json.dumps(result))
        return result

    def run(mode, success=True):
        result = subprocess.run([str(binary),str(data),str(fixtures),mode],text=True,capture_output=True,timeout=20)
        if success: assert result.returncode == 0, (mode,result.stdout,result.stderr)
        return json.loads(result.stdout)

    def untouched():
        assert (data/'config/launcher-state/test-pack.json').read_text() == '{"screen":"library","focus":7}'
        assert json.loads((data/'config/launcher.json').read_text())['selected']=='test-pack'
        assert (data/'games/untouched').read_text()=='game data'

    def failed(name, blob=None, mutate=None):
        reset(); value=feed(blob)
        if mutate:
            mutate(value)
            (fixtures/'launcher-updates.json').write_text(json.dumps(value))
        result=run('install')
        assert result['phase']=='failed',(name,result)
        assert json.loads((pack/'manifest.json').read_text())['version']=='1.0.0',(name,result)
        untouched();passed.append(name)

    reset();feed();result=run('install');assert result['phase']=='installed',result
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.1.0'
    assert json.loads((data/'launcher-updates/backups/test-pack/manifest.json').read_text())['version']=='1.0.0'
    untouched();passed.append('install, backup and preservation')
    result=run('rollback');assert result['phase']=='installed',result
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.0.0';untouched();passed.append('rollback')
    result=run('rollback');assert result['phase']=='installed',result
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.1.0';passed.append('rollback retains the replaced version')
    reset();feed();result=run('stage');assert result['phase']=='ready'
    assert json.loads((pack/'manifest.json').read_text())['version']=='1.0.0';passed.append('staging leaves live files unchanged')
    reset();feed();assert run('cancel-ready')['phase']=='cancelled';untouched();passed.append('cancel staged update')
    reset();feed();run('check');assert run('automatic')=={'started':False};passed.append('persisted automatic-check cooldown')
    reset();feed();assert run('automatic-off')=={'started':False};assert run('automatic')=={'started':False};passed.append('persisted automatic checks disabled')
    assert run('check')['phase']=='available';passed.append('manual check bypasses cooldown')
    reset();feed();result=run('destroy-check');assert result['cancelled'] and result['destructor_ms']<100,result
    passed.append('frontend destruction cancels network worker without waiting')
    for settings in [
        {'schema':1,'automatic_checks':'bad','checked':{'test-pack':'bad'}},
        {'schema':1,'checked':{'test-pack':-1}},
        {'schema':1,'checked':{'test-pack':9999999999999999999}},
        {'schema':1,'checked':[]},
    ]:
        reset();feed();(data/'config/launcher-updates.json').write_text(json.dumps(settings))
        assert run('automatic')['phase']=='available';passed.append('invalid automatic-check settings recover '+str(settings))
    reset();feed();m=dict(original);m.pop('updates');(pack/'manifest.json').write_text(json.dumps(m))
    assert run('check')['phase']=='unconfigured';passed.append('legacy packs remain valid without updates')
    reset();f=feed();release=f['releases'][0]
    for field,value in [('host_contract','launcher-v2.0'),('api',{'major':2,'min_minor':0}),('api',{'major':1,'min_minor':2}),
                        ('required_capabilities',['unknown.v1']),('state_schema',2),('min_app','1.004.000')]:
        incompatible=dict(release,version='9.0.0');incompatible[field]=value
        feed(releases=[incompatible,release]);assert run('choose')['version']=='1.1.0';passed.append('skip incompatible '+field+' '+str(value))
    feed(releases=[dict(release,version='0.9.0'),dict(release,version='1.0.0')]);assert run('choose') is None;passed.append('no downgrade or equal-version update')
    feed(releases=[dict(release,version='1.002.001'),dict(release,version='1.2.10')]);assert run('choose')['version']=='1.2.10';passed.append('numeric version ordering')
    failed('SHA-256 mismatch', mutate=lambda f:f['releases'][0].update(sha256='0'*64))
    failed('declared size mismatch', mutate=lambda f:f['releases'][0].update(bytes=f['releases'][0]['bytes']+1))
    failed('oversized download rejected', mutate=lambda f:f['releases'][0].update(bytes=128*1024**2+1))
    failed('HTTP archive URL rejected', mutate=lambda f:f['releases'][0].update(url='http://updates.example/a.zip'))
    failed('credentials in URL rejected', mutate=lambda f:f['releases'][0].update(url='https://token@updates.example/a.zip'))
    failed('wrong feed ID', mutate=lambda f:f.update(id='other-pack'))
    failed('unsupported feed schema', mutate=lambda f:f.update(schema=2))
    failed('duplicate feed version', mutate=lambda f:f['releases'].append(f['releases'][0]))
    failed('invalid version', mutate=lambda f:f['releases'][0].update(version='nightly'))
    failed('negative size', mutate=lambda f:f['releases'][0].update(bytes=-1))
    failed('invalid ZIP',blob=b'not a ZIP')
    for path in ['test-pack/../escape','../escape','/absolute','test-pack/a\\b','test-pack/a:b','other/manifest.json','test-pack//empty','test-pack/./dot']:
        failed('archive path '+path,blob=archive(entries=[(path,'payload')]))
    failed('duplicate archive path',blob=archive(entries=[('test-pack/main.lua','return {}')]))
    failed('file/directory collision',blob=archive(entries=[('test-pack/assets','collision')]))
    link=zipfile.ZipInfo('test-pack/link');link.create_system=3;link.external_attr=0o120777<<16
    failed('symlink entry',blob=archive(entries=[(link,'../../escape')]))
    device=zipfile.ZipInfo('test-pack/device');device.create_system=3;device.external_attr=0o020666<<16
    failed('device entry',blob=archive(entries=[(device,'')]))
    failed('oversized entry count',blob=archive(entries=[(f'test-pack/extra/{i}','') for i in range(4097)]))
    failed('changed manifest version',blob=archive(dict(updated,version='1.9.0')))
    failed('changed manifest ID',blob=archive(dict(updated,id='other-pack')))
    failed('changed feed source',blob=archive(dict(updated,updates={'feed':'https://other.example/feed.json'})))
    failed('changed manifest capabilities',blob=archive(dict(updated,required_capabilities=[])))
    failed('changed state schema',blob=archive(dict(updated,state_schema=2)))
    failed('missing resource',blob=archive(dict(updated,font='assets/missing.pefont')))
    failed('escaping manifest resource',blob=archive(dict(updated,font='../outside')))
    failed('invalid Lua syntax',blob=archive(dict(updated,entry='invalid.lua'),[('test-pack/invalid.lua','return {')]))
    failed('Lua bytecode',blob=archive(dict(updated,entry='invalid.lua'),[('test-pack/invalid.lua',b'\x1bLua\x54\x00\x19\x93\r\n\x1a\n')]))
    # Construct the durable states left at each rename boundary.
    for phase,old,new_live,backup,prior in [('prepared',False,True,True,False),('prepared',True,False,True,False),('prepared',True,True,True,False),
                                           ('committed',True,True,True,False),('committed',True,True,False,True),('committed',False,True,True,True)]:
        reset();w=data/'launcher-updates/work/test-pack';w.mkdir(parents=True)
        (data/'launcher-updates/backups').mkdir()
        if old:shutil.copytree(pack,w/'old')
        if not new_live:shutil.rmtree(pack)
        elif phase=='committed' or old:
            (pack/'manifest.json').write_text(json.dumps(updated))
        if backup:shutil.copytree(w/'old' if old else pack,data/'launcher-updates/backups/test-pack')
        if prior:shutil.copytree(pack,w/'prior')
        j=data/'launcher-updates/transactions/test-pack.json';j.parent.mkdir();j.write_text(json.dumps({'schema':1,'id':'test-pack','phase':phase}))
        run('recover')
        expected='1.0.0' if phase=='prepared' else '1.1.0'
        assert json.loads((pack/'manifest.json').read_text())['version']==expected
        assert not j.exists() and not w.exists();untouched();passed.append('recovery '+str((phase,old,new_live,backup,prior)))
    reset();feed();pack.rename(pack.with_name('real-pack'));pack.symlink_to('real-pack',target_is_directory=True)
    assert run('install')['phase']=='failed';passed.append('installed pack symlink refused')
    reset();feed();(pack/'manifest.json').write_text(json.dumps(updated))
    published=temporary/'published';publisher.package(pack,published,'https://updates.example/test-pack-1.1.0.zip',notes='Fixture release')
    with zipfile.ZipFile(published/'test-pack-1.1.0.zip') as z:
        assert all(n.startswith('test-pack/') for n in z.namelist())
    shutil.copy2(published/'launcher-updates.json',fixtures/'launcher-updates.json')
    shutil.copy2(published/'test-pack-1.1.0.zip',fixtures/'test-pack-1.1.0.zip')
    (pack/'manifest.json').write_text(json.dumps(original))
    assert run('install')['phase']=='installed';untouched();passed.append('publisher output installs through native updater')

print(json.dumps({'passed':len(passed),'cases':passed},indent=2))
