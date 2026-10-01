"""Root transaction for the bounded live-fetch + same-frame visual-ID observer."""
from pathlib import Path
import hashlib,json,shutil,subprocess,time
r=Path.cwd();st=r/'scratch/native-silo-parity-20260929'
d=st/'sol-analysis/source-topology-wind-census/portal-architecture/wind-prototype/backend-fetch-observer'
out=d/'root-build-03';out.mkdir(exist_ok=False)
candidate=st/'candidate-flora-fetch-observer-03';assert not candidate.exists()
mf=json.loads((d/'manifest.json').read_text())
files={'src/Layers/xrRenderMetal/MetalR4Backend.mm':d/'MetalR4Backend.fetch-observer.draft.mm',
       'src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp':st/'sol-analysis/ssa-main-ids/draft/src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp'}
idsdraft=st/"sol-analysis/ssa-main-ids/draft"
for name in ("MetalR4FloraAdapter.h","MetalR4EngineScene.mm","MetalR4EngineScene.h","MetalRenderDeviceRender.mm"):
    path="src/Layers/xrRenderMetal/"+name;files[path]=idsdraft/path
files["src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm"]=d/"MetalRenderDeviceRender.fetch-frame.draft.mm"
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pre={p:sha(r/p) for p in files}
assert pre['src/Layers/xrRenderMetal/MetalR4Backend.mm']==mf['production_backend_sha256']
assert pre['src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp']=='3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc'
assert pre["src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm"]=="1087cfd01656b83d5fe0386d7730d193cea329ff3d2d86810f97b907edd048f9"
assert pre["src/Layers/xrRenderMetal/MetalR4EngineScene.mm"]=="c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53"
assert sha(files['src/Layers/xrRenderMetal/MetalR4Backend.mm'])==mf['candidate_sha256']
app=r/'build/Binaries/RelWithDebInfo/Anomaly.app';exe=app/'Contents/MacOS/AnomalyDX11';bare=r/'build/Binaries/RelWithDebInfo/AnomalyDX11'
normal='d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0';assert sha(exe)==sha(bare)==normal
for p in pre:
    dest=out/'prestate'/p;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/p,dest)
shutil.copyfile(exe,out/'ordinary-app-exe');shutil.copyfile(bare,out/'ordinary-bare-exe')
receipt={'scope':'one frame700 live Flora fetch observer plus same-frame current visual IDs, no culling/FPS claim',
         'started_epoch':time.time(),'production_prestate':pre,'ordinary_exe_before':normal}
def build(log):
    print('Starting',log,flush=True)
    with (out/log).open('w') as f:q=subprocess.run(['cmake','--build','build','--target','AnomalyDX11','-j6'],cwd=r,stdout=f,stderr=subprocess.STDOUT)
    print(log,'exit',q.returncode,flush=True);return q.returncode
try:
    for p,src in files.items():shutil.copyfile(src,r/p)
    receipt['build_exit']=build('build-candidate.log')
    if receipt['build_exit']==0:
        candidate.mkdir();shutil.copytree(app,candidate/'Anomaly.app')
        keys=set(json.loads((st/'candidate-r1-final-01/source-manifest.json').read_text())['source_hashes'])|set(files)
        sourcehashes={p:sha(r/p) for p in sorted(keys)}
        for p in keys:
            dest=candidate/'source-snapshot'/p;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/p,dest)
        manifest={'candidate':'flora-fetch-observer-03','candidate_executable_sha256':sha(exe),
                  'source_hashes':sourcehashes,'suppression':0,'frame':700,'scope':receipt['scope'],
                  'note':'Source and ordinary app/bare restoration precedes any game launch. No performance claim from diagnostic frame.'}
        (candidate/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        receipt.update(candidate_executable_sha256=sha(exe),manifest_sha256=sha(candidate/'source-manifest.json'))
finally:
    for p in pre:shutil.copyfile(out/'prestate'/p,r/p)
    receipt['restored_source_hashes']={p:sha(r/p) for p in pre};assert receipt['restored_source_hashes']==pre
    receipt['normal_build_exit']=build('build-normal.log')
    if receipt['normal_build_exit']==0:
        receipt['fresh_normal_exe_sha256']=sha(exe);shutil.copyfile(exe,out/'fresh-normal-exe')
    shutil.copyfile(out/'ordinary-app-exe',exe);shutil.copyfile(out/'ordinary-bare-exe',bare)
    receipt['ordinary_outputs_restored']={'app':sha(exe),'bare':sha(bare)};receipt['completed_epoch']=time.time()
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');assert receipt['normal_build_exit']==0
print(json.dumps({k:v for k,v in receipt.items() if k not in ('production_prestate','restored_source_hashes')},indent=2),flush=True)
