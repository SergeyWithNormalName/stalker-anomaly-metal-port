from pathlib import Path
import shutil,subprocess,json,hashlib,time
r=Path.cwd(); st=r/'scratch/native-silo-parity-20260929'; d=st/'sol-analysis/source-topology-wind-census/portal-architecture/active-input-observer-v2'; c=st/'candidate-portal-active-input-observer-02'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
assert not c.exists(), 'candidate destination already exists'
pre=json.loads((d/'prestate-hashes.json').read_text()); files=sorted(str(p.relative_to(d/'draft')) for p in (d/'draft').rglob('*') if p.is_file())
new=[p for p in files if p not in pre]
for p,h in pre.items(): assert sha(r/p)==h, (p,'prestate drift')
for p in new: assert not (r/p).exists(), (p,'new helper collision')
app=r/'build/Binaries/RelWithDebInfo/Anomaly.app'; bare=r/'build/Binaries/RelWithDebInfo/AnomalyDX11'; exe=app/'Contents/MacOS/AnomalyDX11'; normalhash=sha(exe); assert normalhash==sha(bare)=='d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0'
backup=d/'ordinary-outputs'; backup.mkdir(); shutil.copyfile(exe,backup/'app-exe');shutil.copyfile(bare,backup/'bare-exe')
receipt={'candidate':'portal-active-input-observer-02','normal_output_before':normalhash,'suppression':0,'started_epoch':time.time()}
built=False
try:
 for p in files: shutil.copyfile(d/'draft'/p,r/p)
 with (d/'build-candidate.log').open('w') as log: result=subprocess.run(['cmake','--build','build','--target','AnomalyDX11','-j6'],cwd=r,stdout=log,stderr=subprocess.STDOUT)
 receipt['build_exit']=result.returncode
 if result.returncode: raise RuntimeError('candidate build failed; see build-candidate.log')
 c.mkdir(); shutil.copytree(app,c/'Anomaly.app')
 sourcekeys=set(json.loads((st/'candidate-r1-final-01/source-manifest.json').read_text())['source_hashes'])|set(files)
 hashes={p:sha(r/p) for p in sorted(sourcekeys)}
 for p in sorted(sourcekeys):
  dest=c/'source-snapshot'/p;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/p,dest)
 manifest={'candidate':'portal-active-input-observer-02','build_exit':0,'candidate_executable_sha256':sha(exe),'source_hashes':hashes,'frame':700,'suppression':0,'notes':['Default-off observe-only active reader + primary near + live CFORM rays + actual Flora uniforms.','Does not create graph, change visibility or retain engine/CDB pointers.','Source/binary rollback precedes gameplay.']}
 (c/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 receipt.update(candidate_executable_sha256=manifest['candidate_executable_sha256'],manifest_sha256=sha(c/'source-manifest.json'));built=True
finally:
 for p in pre: shutil.copyfile(d/'prestate'/p,r/p)
 for p in new: (r/p).unlink(missing_ok=True)
 receipt['restored_source_hashes']={p:sha(r/p) for p in pre}
 assert receipt['restored_source_hashes']==pre
 with (d/'build-normal.log').open('w') as log: norm=subprocess.run(['cmake','--build','build','--target','AnomalyDX11','-j6'],cwd=r,stdout=log,stderr=subprocess.STDOUT)
 receipt['normal_build_exit']=norm.returncode
 if norm.returncode==0:
  receipt['fresh_normal_exe_sha256']=sha(exe); shutil.copyfile(exe,d/'fresh-normal-exe')
 shutil.copyfile(backup/'app-exe',exe); shutil.copyfile(backup/'bare-exe',bare)
 receipt['ordinary_outputs_restored']={'app':sha(exe),'bare':sha(bare)};receipt['complete_epoch']=time.time()
 (d/'freeze-result.json').write_text(json.dumps(receipt,indent=2)+'\n')
 if norm.returncode: raise RuntimeError('normal source rebuild failed')
assert built
print(json.dumps({k:v for k,v in receipt.items() if k not in ('restored_source_hashes',)},indent=2))
