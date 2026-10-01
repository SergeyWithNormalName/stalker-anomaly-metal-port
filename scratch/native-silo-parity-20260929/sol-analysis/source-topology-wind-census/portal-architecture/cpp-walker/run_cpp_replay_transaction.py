"""Root-owned temporary engine-source replay build, always restoring WIP bytes."""
from pathlib import Path
import hashlib, json, shutil, subprocess, time

repo=Path.cwd()
st=repo/'scratch/native-silo-parity-20260929'
draft=st/'sol-analysis/source-topology-wind-census/portal-architecture/cpp-walker/transaction-draft'
out=draft.parent/'root-build-01'
out.mkdir(exist_ok=False)
manifest=json.loads((draft/'stage-manifest.json').read_text())
pre=manifest['productionPrestateSha256']; staged=manifest['stagedFileSha256']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
for p,h in pre.items(): assert sha(repo/p)==h, ('production prestate drift',p)
for p,h in staged.items(): assert sha(draft/'stage'/p)==h, ('draft hash drift',p)
new=[p for p in staged if p not in pre]
for p in new: assert not (repo/p).exists(), ('helper collision',p)
app=repo/'build/Binaries/RelWithDebInfo/Anomaly.app/Contents/MacOS/AnomalyDX11'
bare=repo/'build/Binaries/RelWithDebInfo/AnomalyDX11'
normal='d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0'
assert sha(app)==sha(bare)==normal
backup=out/'prestate'
for p in pre:
    dest=backup/p; dest.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(repo/p,dest)
shutil.copyfile(app,out/'ordinary-app-exe'); shutil.copyfile(bare,out/'ordinary-bare-exe')
receipt={'scope':'offline actual engine-source CPortal/CFrustum replay of captured frame700; suppression0',
         'started_epoch':time.time(),'ordinary_exe_before':normal,'production_prestate':pre,
         'staged_source_hashes':staged}
def call(args,log):
    print('Starting',log,flush=True)
    with (out/log).open('w') as f:
        result=subprocess.run(args,cwd=repo,stdout=f,stderr=subprocess.STDOUT)
    print(log,'exit',result.returncode,flush=True)
    return result.returncode
try:
    receipt['inputs_exit']=call(['python3',str(draft/'verify_inputs.py'),str(repo)],'inputs.log')
    assert receipt['inputs_exit']==0
    for p in staged: shutil.copyfile(draft/'stage'/p,repo/p)
    receipt['patch_exit']=call(['/usr/bin/patch','-p1','-i',str(draft/'replay-target.patch')],'stage-cmake.log')
    assert receipt['patch_exit']==0
    receipt['candidate_source_hashes']={p:sha(repo/p) for p in set(pre)|set(staged)}
    receipt['build_exit']=call(['cmake','--build','build','--target','XRay.Render.R4.PortalGraphReplayProbe','-j6'],'build-probe.log')
    if receipt['build_exit']==0:
        probe=repo/'build/Binaries/RelWithDebInfo/XRay.Render.R4.PortalGraphReplayProbe'
        assert probe.is_file(), 'built probe path differs; inspect build output'
        shutil.copyfile(probe,out/'PortalGraphReplayProbe')
        receipt['probe_sha256']=sha(probe)
        inputs=json.loads((draft/'frame700-inputs.json').read_text())
        args=[str(probe),str(st/'runs/portal-active-inputs-01/native/output/active-engine-level.bin'),
              str(st/'runs/portal-active-inputs-01/native/runtime/gamedata/levels/l01_escape/level.geom'),
              str(st/'runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv'),
              str(st/'sol-analysis/source-topology-wind-census/portal-architecture/cpu-prototype/active-frame700-per-id.csv'),
              str(out/'cpp-frame700-per-id.csv')]
        receipt['probe_argv']=args
        receipt['replay_exit']=call(args,'replay.log')
        if (out/'cpp-frame700-per-id.csv').is_file(): receipt['row_csv_sha256']=sha(out/'cpp-frame700-per-id.csv')
finally:
    for p in pre: shutil.copyfile(backup/p,repo/p)
    for p in new: (repo/p).unlink(missing_ok=True)
    receipt['restored_source_hashes']={p:sha(repo/p) for p in pre}
    assert receipt['restored_source_hashes']==pre
    receipt['normal_build_exit']=call(['cmake','--build','build','--target','AnomalyDX11','-j6'],'build-normal.log')
    if receipt['normal_build_exit']==0:
        receipt['fresh_normal_exe_sha256']=sha(app)
        shutil.copyfile(app,out/'fresh-normal-exe')
    shutil.copyfile(out/'ordinary-app-exe',app); shutil.copyfile(out/'ordinary-bare-exe',bare)
    receipt['ordinary_outputs_restored']={'app':sha(app),'bare':sha(bare)}
    receipt['completed_epoch']=time.time()
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    assert receipt['normal_build_exit']==0, 'normal build failed; inspect normal log'
print(json.dumps({k:v for k,v in receipt.items() if k not in ('production_prestate','staged_source_hashes','candidate_source_hashes','restored_source_hashes','probe_argv')},indent=2),flush=True)
