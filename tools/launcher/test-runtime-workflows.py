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
import json, os, shutil, subprocess
out=work/'launch-workflow-tests'
out.mkdir(exist_ok=True);data=out/'data';folder=data/'launchers/workflow-test';folder.mkdir(parents=True,exist_ok=True)
shutil.copytree(root/'examples/grid-launcher/assets',folder/'assets',dirs_exist_ok=True)
m=json.loads((root/'examples/grid-launcher/manifest.json').read_text());m['id']='workflow-test'
(folder/'manifest.json').write_text(json.dumps(m))
(folder/'main.lua').write_text('''
local function a(name,args)
    local r=host.action(name,args or {});host.action("state.save",{operation=name,result=r});return r
end
return {init=function() end,draw=function() end,input=function(e)
    local library=host.query("library");local game=library.games[1].handle
    if e.key=="cross" then a("launch.request",{game=game})
    elseif e.key=="circle" then a("launch.cancel")
    elseif e.key=="triangle" then a("launch.commit")
    elseif e.key=="right" then a("profile.choose",{index=1})
    elseif e.key=="left" then a("profile.choose",{index=0})
    elseif e.key=="up" then a("sync.answer",{choice="console"})
    elseif e.key=="down" then a("sync.answer",{choice="play_anyway"})
    elseif e.key=="l1" then a("game.delete",{game=game,confirmed=true})
    elseif e.key=="square" then a("launch.request",{game="../../not-a-library-game"})
    elseif e.key=="options" then a("preferences.set",{mapping={0,0,0,0,0,0,0,0,0,0,0,0}})
    elseif e.key=="r1" then a("update.apply") end
end}
''')
(data/'config').mkdir(exist_ok=True)
(data/'config/launcher.json').write_text(json.dumps({'schema':1,'selected':'workflow-test'}))
env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',PE_LAUNCHER_DATA=str(data))
checks=[]
class Session:
    def __init__(self,name):
        self.name=name;self.log=[];self.err=(out/(name+'.log')).open('w')
        self.p=subprocess.Popen([str(renderer),str(root/'headless/prosperoeden/ui'),str(out),'640','360'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.err,text=True,env=env)
        assert self.p.stdout.readline().startswith('READY workflow-test')
    def command(self,line):
        self.p.stdin.write(line+'\n');self.p.stdin.flush();result=[]
        while True:
            value=self.p.stdout.readline();assert value,'Terminated during '+line
            self.log.append(value);result.append(value)
            if value.startswith('OK '):assert 'GL=0' in value;return ''.join(result)
    def key(self,name):self.command('key '+name+' 3')
    def result(self):return json.loads((data/'config/launcher-state/workflow-test.json').read_text())['result']
    def need(self,condition,label):assert condition,label;checks.append({'case':self.name,'check':label,'passed':True})
    def close(self):self.p.stdin.write('exit\n');self.p.stdin.flush();self.p.wait(timeout=15);assert self.p.returncode==0;self.err.close();(out/(self.name+'-commands.log')).write_text(''.join(self.log))

s=Session('current-profile');s.key('right');s.need(s.result()['ok'],'Current profile switches');s.key('cross');s.need(s.result()['ok'],'Launch request accepted');s.key('left');s.need(not s.result()['ok'],'Profile cannot change during pending launch');s.key('triangle');s.need(s.result()['ok'],'Ready launch commits');launches=s.command('launches');s.need('"profile":1' in launches,'Captured current profile reaches will_play');s.key('triangle');s.need(not s.result()['ok'],'Repeated commit rejected');s.need(s.command('launches').count('"profile":1')==1,'will_play called exactly once');s.close()
s=Session('unavailable');s.key('square');s.need(not s.result()['ok'],'Arbitrary game path rejected');s.key('options');s.need(not s.result()['ok'],'Duplicate mapping rejected');s.key('r1');s.need(not s.result()['ok'],'Update cannot apply before ready');s.command('room connecting');s.key('cross');s.need(not s.result()['ok'],'Connecting room blocks launch');s.command('room idle');s.key('cross');s.command('remove-first');s.key('triangle');s.need(not s.result()['ok'],'Deleted game is rechecked before commit');s.need('LAUNCHES []' in s.command('launches'),'Failed commits do not register play');s.close()
s=Session('save-conflict');s.command('sync conflict');s.key('cross');s.command('tick 30');s.key('triangle');s.need(not s.result()['ok'],'Save conflict blocks commit');s.key('up');s.command('tick 60');s.key('triangle');s.need(s.result()['ok'],'Conflict answer allows completed sync to launch');s.close()
s=Session('save-failed');s.command('sync failed');s.key('cross');s.command('tick 30');s.key('triangle');s.need(not s.result()['ok'],'Failed sync blocks launch until answered');s.key('down');s.key('triangle');s.need(s.result()['ok'],'Explicit play without sync is honored');s.close()
s=Session('save-cancel');s.command('sync conflict');s.key('cross');s.key('circle');s.key('triangle');s.need(not s.result()['ok'],'Cancelled sync cannot commit');s.need('LAUNCHES []' in s.command('launches'),'Cancelled sync never registers play');s.close()
s=Session('update-ready');s.command('offer v1.002.099');s.command('update-ready');s.key('r1');s.need(s.result()['ok'],'Ready update is applied through native service');s.need('done=1' in s.command('status'),'Successful update closes launcher');s.close()
(out/'report.json').write_text(json.dumps({'passed':True,'checks':checks,'scope':'Actual runtime host, simulated backend; requires console game testing.'},indent=2)+'\n')
print(json.dumps({'passed':True,'checks':len(checks)}))
