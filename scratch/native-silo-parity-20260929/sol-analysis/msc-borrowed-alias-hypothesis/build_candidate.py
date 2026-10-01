from pathlib import Path
import hashlib, json, shutil, subprocess, time

r=Path.cwd()
st=r/'scratch/native-silo-parity-20260929'
d=st/'sol-analysis/msc-borrowed-alias-hypothesis'
out=d/'root-build-01'
out.mkdir(exist_ok=False)
candidate=st/'candidate-msc-borrowed-alias-01'
assert not candidate.exists()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
mf=json.loads((d/'root-source-hashes.json').read_text())
files={row['path']:d/'stage'/row['path'] for row in mf['files']}
pre={row['path']:row['prestate_sha256'] for row in mf['files']}
for row in mf['files']:
    assert sha(r/row['path'])==row['prestate_sha256'],('production drift',row['path'])
    assert sha(files[row['path']])==row['candidate_sha256'],('draft drift',row['path'])
app=r/'build/Binaries/RelWithDebInfo/Anomaly.app'
exe=app/'Contents/MacOS/AnomalyDX11'
bare=r/'build/Binaries/RelWithDebInfo/AnomalyDX11'
probe=r/'build/src/Layers/xrRenderMetal/XRay.Metal.FloraInstancingGpuProbe'
normal='d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0'
assert sha(exe)==sha(bare)==normal
for p in pre:
    dest=out/'prestate'/p;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/p,dest)
for label,p in [('ordinary-app-exe',exe),('ordinary-bare-exe',bare)]:
    shutil.copyfile(p,out/label)
probe_before=sha(probe) if probe.exists() else None
if probe_before:shutil.copyfile(probe,out/'ordinary-probe-exe')
receipt={'scope':'defaultoff borrowed typed aliases within BindMscResources only; all owners, arena snapshots, GPU descriptors and indirect read declarations preserved',
         'started_epoch':time.time(),'production_prestate':pre,'ordinary_exe_before':normal,'ordinary_probe_before':probe_before}
targets=['AnomalyDX11','XRay.Metal.FloraInstancingGpuProbe']
def build(log):
    print('Starting',log,flush=True)
    with (out/log).open('w') as f:
        q=subprocess.run(['cmake','--build','build','--target',*targets,'-j6'],cwd=r,stdout=f,stderr=subprocess.STDOUT)
    print(log,'exit',q.returncode,flush=True);return q.returncode
try:
    for p,src in files.items():shutil.copyfile(src,r/p)
    receipt['build_exit']=build('build-candidate.log')
    if receipt['build_exit']==0:
        candidate.mkdir();shutil.copytree(app,candidate/'Anomaly.app')
        shutil.copyfile(probe,candidate/'FloraInstancingGpuProbe')
        shutil.copymode(probe,candidate/'FloraInstancingGpuProbe')
        receipt['probe_executable_sha256']=sha(probe)
        with (out/'msc-binding-gpu-probe.log').open('w') as f:
            q=subprocess.run([str(probe),'--gpu',str(r/'build/metal-runtime-shaders/shadow-casters-raster-depth-v1'),str(r/'build/metal-runtime-shaders/flora-shadow-instanced-e08'),str(out/'binding-probe-artifacts')],cwd=r,stdout=f,stderr=subprocess.STDOUT,timeout=60,env=dict(__import__('os').environ,XRAY_METAL_E08_BORROW_MSC_TYPES='1'))
        receipt['msc_binding_gpu_probe_exit']=q.returncode
        keys=set(json.loads((st/'candidate-r1-final-01/source-manifest.json').read_text())['source_hashes'])|set(files)
        source_hashes={p:sha(r/p) for p in sorted(keys)}
        for p in keys:
            dest=candidate/'source-snapshot'/p;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/p,dest)
        manifest={'candidate':'msc-borrowed-alias-01','candidate_executable_sha256':sha(exe),
                  'source_hashes':source_hashes,'scope':receipt['scope'],'msc_binding_gpu_probe_exit':q.returncode,
                  'probe_executable_sha256':sha(probe),'note':'Source and ordinary outputs restored before any game launch. Hypothesis only; FPS and broader quality unverified.'}
        (candidate/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        receipt.update(candidate_executable_sha256=sha(exe),manifest_sha256=sha(candidate/'source-manifest.json'))
finally:
    for p in pre:shutil.copyfile(out/'prestate'/p,r/p)
    receipt['restored_source_hashes']={p:sha(r/p) for p in pre}
    assert receipt['restored_source_hashes']==pre
    receipt['normal_build_exit']=build('build-normal.log')
    if receipt['normal_build_exit']==0:receipt['fresh_normal_exe_sha256']=sha(exe)
    shutil.copyfile(out/'ordinary-app-exe',exe);shutil.copyfile(out/'ordinary-bare-exe',bare)
    if probe_before:shutil.copyfile(out/'ordinary-probe-exe',probe)
    receipt['ordinary_outputs_restored']={'app':sha(exe),'bare':sha(bare),'probe':sha(probe)}
    receipt['completed_epoch']=time.time()
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    assert receipt['normal_build_exit']==0
print(json.dumps({k:v for k,v in receipt.items() if k not in ('production_prestate','restored_source_hashes')},indent=2),flush=True)
