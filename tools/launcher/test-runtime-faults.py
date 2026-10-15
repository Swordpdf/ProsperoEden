#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the runtime preview with temporary packs and simulated services."""
import argparse, tempfile
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary",type=Path,help="Path to the pe_preview built by runtime-preview.sh")
options=parser.parse_args()
renderer=options.binary.resolve()
root=Path(__file__).resolve().parents[2]
scratch=tempfile.TemporaryDirectory(prefix="pe-runtime-tests-")
work=Path(scratch.name)
import json, os, shutil, subprocess, struct
out=work/'runtime-fault-tests';out.mkdir(exist_ok=True)
data=out/'data';packs=data/'launchers';packs.mkdir(parents=True,exist_ok=True)
(data/'config').mkdir(exist_ok=True)
manifest=json.loads((root/'examples/grid-launcher/manifest.json').read_text())
font=root/'examples/grid-launcher/assets/font.pefont'
base='return {init=function() end,update=function() end,draw=function() end,input=function() end}'
cases=[]
def pack(name,script=base,patch=None):
    folder=packs/name;folder.mkdir(exist_ok=True);(folder/'assets').mkdir(exist_ok=True)
    shutil.copy2(font,folder/'assets/font.pefont')
    m=dict(manifest,id=name);m.update(patch or {})
    (folder/'manifest.json').write_text(json.dumps(m))
    (folder/'main.lua').write_bytes(script if isinstance(script,bytes) else script.encode())
    return folder
def run(name,commands='',expected='builtin',reason=''):
    (data/'config/launcher.json').write_text(json.dumps({'schema':1,'selected':name}))
    env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',PE_LAUNCHER_DATA=str(data))
    p=subprocess.run([str(renderer),str(root/'headless/prosperoeden/ui'),str(out),'640','360'],
                     input=commands+'status\nexit\n',text=True,capture_output=True,env=env,timeout=40)
    (out/(name+'.log')).write_text(p.stdout+'\n'+p.stderr)
    assert p.returncode==0,(name,p.returncode,p.stderr)
    assert 'STATE active='+expected in p.stdout,(name,p.stdout,p.stderr)
    assert all('GL=0' in line for line in p.stdout.splitlines() if line.startswith('OK ')),name
    if reason:assert reason in p.stdout+p.stderr,(name,p.stdout,p.stderr)
    # A failed pack is never deleted or silently deselected on disk.
    assert (packs/name/'manifest.json').exists()
    assert json.loads((data/'config/launcher.json').read_text())['selected']==name
    cases.append({'case':name,'passed':True,'active':expected})

pack('bad-syntax','return {');run('bad-syntax',reason='REJECTED')
pack('wrong-api',patch={'api':{'major':2,'min_minor':0}});run('wrong-api',reason='incompatible')
pack('bytecode',b'\x1bLua\x54\x00\x19\x93\r\n\x1a\n');run('bytecode',reason='binary')
pack('path-escape',patch={'entry':'../main.lua'});run('path-escape',reason='Invalid pack resource path')
pack('missing-image',patch={'assets':{'missing':'assets/missing.tga'}});run('missing-image',reason='Missing resource')
folder=pack('oversized-image',patch={'assets':{'huge':'assets/huge.tga'}})
header=bytearray(18);header[2]=2;header[12:16]=struct.pack('<HH',8192,8192);header[16]=32
(folder/'assets/huge.tga').write_bytes(header);run('oversized-image',reason='resource limits')
pack('infinite-load','while true do end');run('infinite-load',reason='instruction budget')
pack('infinite-input','return {init=function() end,draw=function() end,input=function() while true do end end}')
run('infinite-input','key cross\n',reason='instruction budget')
pack('memory-limit','return {init=function() end,draw=function() end,update=function() local t={} while true do t[#t+1]=string.rep("x",1000000) end end}')
run('memory-limit',reason='memory')
pack('draw-error','return {init=function() end,draw=function() error("draw callback failed") end}')
run('draw-error',reason='draw callback failed')
folder=pack('symlink-image',patch={'assets':{'bad':'assets/link.tga'}})
link=folder/'assets/link.tga'
if link.is_symlink():link.unlink()
link.symlink_to(root/'headless/prosperoeden/ui/images/controller.tga')
run('symlink-image',reason='symbolic link')
pack('sandbox','return {init=function() assert(io==nil and os==nil and debug==nil and package==nil and load==nil and dofile==nil and pcall==nil) end,draw=function() end}')
run('sandbox',expected='sandbox')
pack('forced-recovery')
(data/'config/force-builtin-launcher').touch()
run('forced-recovery')
(data/'config/force-builtin-launcher').unlink()

report={'passed':True,'cases':cases,'scope':'Real Lua VM and desktop renderer; no console hardware.'}
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'passed':True,'cases':len(cases)}))
