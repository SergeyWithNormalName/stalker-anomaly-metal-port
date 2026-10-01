"""Run captured-frame C++ source replay in the full engine executable; no game/window."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,time

repo=Path.cwd();st=repo/'scratch/native-silo-parity-20260929'
draft=st/'sol-analysis/source-topology-wind-census/portal-architecture/cpp-walker/transaction-draft-v2'
out=draft.parent/'root-build-03';out.mkdir(exist_ok=False)
manifest=json.loads((draft/'stage-manifest.json').read_text());staged=manifest['stagedFileSha256']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pre={k:v for k,v in manifest['productionPrestateSha256'].items() if not k.endswith('.cmake')}
pre.update({p:sha(repo/p) for p in ('src/Layers/xrRender/R_calculate.cpp','src/xrEngine/x_ray.cpp')})
assert pre['src/Layers/xrRender/R_calculate.cpp']=='07d5190518b1401c337b342a22ed130560b87f12a25a969619514e3d950b9d46'
for p,h in pre.items():assert sha(repo/p)==h,('prestate drift',p)
for p,h in staged.items():assert sha(draft/'stage'/p)==h,('draft drift',p)
new=[p for p in staged if p not in pre]
for p in new:assert not (repo/p).exists(),('helper collision',p)
app=repo/'build/Binaries/RelWithDebInfo/Anomaly.app/Contents/MacOS/AnomalyDX11';bare=repo/'build/Binaries/RelWithDebInfo/AnomalyDX11'
normal='d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0';assert sha(app)==sha(bare)==normal
for p in pre:
    dest=out/'prestate'/p;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(repo/p,dest)
shutil.copyfile(app,out/'ordinary-app-exe');shutil.copyfile(bare,out/'ordinary-bare-exe')
receipt={'scope':'captured frame700 actual C++ source math in full engine CLI, no game, no suppression',
         'started_epoch':time.time(),'production_prestate':pre,'ordinary_exe_before':normal}
def call(args,log,timeout=None):
    print('Starting',log,flush=True)
    with (out/log).open('w') as f:res=subprocess.run(args,cwd=repo,stdout=f,stderr=subprocess.STDOUT,timeout=timeout)
    print(log,'exit',res.returncode,flush=True);return res.returncode
try:
    receipt['inputs_exit']=call(['python3',str(draft/'verify_inputs.py'),str(repo)],'inputs.log');assert receipt['inputs_exit']==0
    for p in staged:shutil.copyfile(draft/'stage'/p,repo/p)
    calculate=repo/'src/Layers/xrRender/R_calculate.cpp';text=calculate.read_text()
    mark='#include "stdafx.h"';assert text.count(mark)==1
    hook='\n#if defined(__APPLE__) && RENDER == 4\n#include "../xrRenderMetal/R4PortalGraphObserver.cpp"\n#define main RunPortalGraphReplay\n#include "../xrRenderMetal/tests/R4PortalGraphReplayProbe.cpp"\n#undef main\n#endif\n'
    calculate.write_text(text.replace(mark,mark+hook,1))
    entry=repo/'src/xrEngine/x_ray.cpp';text=entry.read_text()
    mark='int main(int argc, char* argv[])\n{\n';assert text.count(mark)==1
    hook='#if defined(__APPLE__)\n\tif (argc == 7 && !std::strcmp(argv[1], "--portal-cpp-replay"))\n\t{\n\t\textern int RunPortalGraphReplay(int, char**);\n\t\treturn RunPortalGraphReplay(argc - 1, argv + 1);\n\t}\n#endif\n'
    entry.write_text(text.replace(mark,mark+hook,1))
    receipt['candidate_source_hashes']={p:sha(repo/p) for p in set(pre)|set(staged)}
    receipt['build_exit']=call(['cmake','--build','build','--target','AnomalyDX11','-j6'],'build-candidate.log')
    if receipt['build_exit']==0:
        shutil.copyfile(app,out/'AnomalyDX11-cpp-replay');receipt['candidate_exe_sha256']=sha(app)
        args=[str(app),'--portal-cpp-replay',str(st/'runs/portal-active-inputs-01/native/output/active-engine-level.bin'),
              str(st/'runs/portal-active-inputs-01/native/runtime/gamedata/levels/l01_escape/level.geom'),
              str(st/'runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv'),
              str(st/'sol-analysis/source-topology-wind-census/portal-architecture/cpu-prototype/active-frame700-per-id.csv'),str(out/'cpp-frame700-per-id.csv')]
        receipt['replay_argv']=args
        try:receipt['replay_exit']=call(args,'replay.log',60)
        except subprocess.TimeoutExpired:receipt['replay_timeout']=True
        if (out/'cpp-frame700-per-id.csv').is_file():receipt['row_csv_sha256']=sha(out/'cpp-frame700-per-id.csv')
finally:
    for p in pre:shutil.copyfile(out/'prestate'/p,repo/p)
    for p in new:(repo/p).unlink(missing_ok=True)
    receipt['restored_source_hashes']={p:sha(repo/p) for p in pre};assert receipt['restored_source_hashes']==pre
    receipt['normal_build_exit']=call(['cmake','--build','build','--target','AnomalyDX11','-j6'],'build-normal.log')
    if receipt['normal_build_exit']==0:
        receipt['fresh_normal_exe_sha256']=sha(app);shutil.copyfile(app,out/'fresh-normal-exe')
    shutil.copyfile(out/'ordinary-app-exe',app);shutil.copyfile(out/'ordinary-bare-exe',bare)
    receipt['ordinary_outputs_restored']={'app':sha(app),'bare':sha(bare)};receipt['completed_epoch']=time.time()
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    assert receipt['normal_build_exit']==0
print(json.dumps({k:v for k,v in receipt.items() if k not in ('production_prestate','candidate_source_hashes','restored_source_hashes','replay_argv')},indent=2),flush=True)
