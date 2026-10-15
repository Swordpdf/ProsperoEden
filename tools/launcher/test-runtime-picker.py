#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native recovery picker must ignore drawing state left by a custom pack."""
from pathlib import Path
import argparse, json, os, shutil, subprocess, tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='pe-picker-') as temp:
    work=Path(temp);data=work/'data';pack=data/'launchers/state-test'
    pack.mkdir(parents=True);(data/'config').mkdir()
    shutil.copytree(root/'examples/grid-launcher/assets',pack/'assets')
    manifest=json.loads((root/'examples/grid-launcher/manifest.json').read_text())
    manifest['id']='state-test';(pack/'manifest.json').write_text(json.dumps(manifest))
    env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',PE_LAUNCHER_DATA=str(data))
    reference=None
    states=['', 'host.draw("opacity",{value=0})',
            'host.draw("clip",{x=0,y=0,w=0,h=0})',
            'host.draw("transform",{scale=2,dx=9000,dy=9000})']
    states.append(';'.join(states[1:]))
    for index,state in enumerate(states):
        (data/'config/launcher.json').write_text(json.dumps({'schema':1,'selected':'state-test'}))
        (pack/'main.lua').write_text('return {init=function() end,update=function() end,input=function() end,draw=function() '+state+' end}')
        before=work/f'before-{index}.png';after=work/f'picker-{index}.png'
        result=subprocess.run([str(args.binary.resolve()),str(root/'headless/prosperoeden/ui'),str(work),'640','360'],
            input=f'capture {before}\npicker\ncapture {after}\nkey cross 3\nstatus\nexit\n',
            text=True,capture_output=True,env=env,timeout=40)
        assert result.returncode==0,(result.stdout,result.stderr)
        assert 'PE_LAUNCHER_FALLBACK' not in result.stderr,result.stderr
        assert 'PE_LAUNCHER_SELECTED id=builtin' in result.stderr,result.stderr
        assert before.read_bytes()!=after.read_bytes(),'Native picker is invisible'
        if reference is None: reference=after.read_bytes()
        else: assert reference==after.read_bytes(),'Pack state altered native picker pixels'
print('PASS: native picker matches neutral rendering after opacity, clip, transform and combined state; selection recovers builtin')
