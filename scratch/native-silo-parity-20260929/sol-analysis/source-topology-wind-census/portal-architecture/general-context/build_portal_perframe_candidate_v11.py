"""Root-owned build/restore transaction for the immutable runtime observer stage v4."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import time

r = Path.cwd()
st = r / 'scratch/native-silo-parity-20260929'
d = st / 'sol-analysis/source-topology-wind-census/portal-architecture/general-context'
out = d / 'runtime-root-build-11'
out.mkdir(exist_ok=False)
candidate = st / 'candidate-portal-perframe-11'
assert not candidate.exists()
mf = json.loads((d / 'runtime-root-source-hashes-v11.json').read_text())
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
files = {row['file']: d / 'runtime-root-stage-11/stage' / row['file'] for row in mf['files']}
pre = {row['file']: row['prestate_sha256'] for row in mf['files'] if row['prestate_sha256'] is not None}
new = [row['file'] for row in mf['files'] if row['prestate_sha256'] is None]
for row in mf['files']:
    if row['prestate_sha256'] is not None:
        assert sha(r / row['file']) == row['prestate_sha256'], ('production drift', row['file'])
    else:
        assert not (r / row['file']).exists(), ('helper collision', row['file'])
    assert sha(files[row['file']]) == row['candidate_v11_sha256'], ('draft drift', row['file'])
app = r / 'build/Binaries/RelWithDebInfo/Anomaly.app'
exe = app / 'Contents/MacOS/AnomalyDX11'
bare = r / 'build/Binaries/RelWithDebInfo/AnomalyDX11'
normal = 'd3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0'
assert sha(exe) == sha(bare) == normal
for p in pre:
    dest = out / 'prestate' / p
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(r / p, dest)
shutil.copyfile(exe, out / 'ordinary-app-exe')
shutil.copyfile(bare, out / 'ordinary-bare-exe')
receipt = {'scope': 'defaultoff deferred per-ID portal+wind proof after existing clip, full-helper actual-ID and GPU oracle, source order/shadows preserved; no shipping admission',
           'started_epoch': time.time(), 'production_prestate': pre, 'ordinary_exe_before': normal}

def build(log):
    print('Starting', log, flush=True)
    with (out / log).open('w') as f:
        q = subprocess.run(['cmake', '--build', 'build', '--target', 'AnomalyDX11', '-j6'],
                           cwd=r, stdout=f, stderr=subprocess.STDOUT)
    print(log, 'exit', q.returncode, flush=True)
    return q.returncode

try:
    for p, src in files.items():
        shutil.copyfile(src, r / p)
    receipt['build_exit'] = build('build-candidate.log')
    if receipt['build_exit'] == 0:
        candidate.mkdir()
        shutil.copytree(app, candidate / 'Anomaly.app')
        keys = set(json.loads((st / 'candidate-r1-final-01/source-manifest.json').read_text())['source_hashes']) | set(files)
        source_hashes = {p: sha(r / p) for p in sorted(keys)}
        for p in keys:
            dest = candidate / 'source-snapshot' / p
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(r / p, dest)
        manifest = {'candidate': 'portal-perframe-11',
                    'candidate_executable_sha256': sha(exe), 'source_hashes': source_hashes,
                    'suppression': 'defaultoff optional on', 'frame': 'fresh eachframe', 'scope': receipt['scope'],
                    'note': 'Source and ordinary app/bare restored before game launch. Asynchronous readback receipts required.'}
        (candidate / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        receipt.update(candidate_executable_sha256=sha(exe), manifest_sha256=sha(candidate / 'source-manifest.json'))
finally:
    for p in pre:
        shutil.copyfile(out / 'prestate' / p, r / p)
    for p in new:
        (r / p).unlink(missing_ok=True)
    receipt['restored_source_hashes'] = {p: sha(r / p) for p in pre}
    assert receipt['restored_source_hashes'] == pre
    receipt['normal_build_exit'] = build('build-normal.log')
    if receipt['normal_build_exit'] == 0:
        receipt['fresh_normal_exe_sha256'] = sha(exe)
        shutil.copyfile(exe, out / 'fresh-normal-exe')
    shutil.copyfile(out / 'ordinary-app-exe', exe)
    shutil.copyfile(out / 'ordinary-bare-exe', bare)
    receipt['ordinary_outputs_restored'] = {'app': sha(exe), 'bare': sha(bare)}
    receipt['completed_epoch'] = time.time()
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    assert receipt['normal_build_exit'] == 0
print(json.dumps({k: v for k, v in receipt.items() if k not in (
    'production_prestate', 'restored_source_hashes')}, indent=2), flush=True)
