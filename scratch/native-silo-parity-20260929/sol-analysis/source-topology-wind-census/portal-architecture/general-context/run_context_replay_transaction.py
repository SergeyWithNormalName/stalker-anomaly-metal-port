"""Root-owned cached portal context CLI build/replay/restore transaction."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import time

repo = Path.cwd()
st = repo / 'scratch/native-silo-parity-20260929'
base = st / 'sol-analysis/source-topology-wind-census/portal-architecture'
draft = base / 'general-context/root-stage-01'
out = base / 'general-context/root-build-01'
out.mkdir(exist_ok=False)
manifest = json.loads((draft / 'manifest.json').read_text())
pre = manifest['productionPrestate']
staged = manifest['stagedFileSha256']
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
normal = 'd3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0'
app = repo / 'build/Binaries/RelWithDebInfo/Anomaly.app/Contents/MacOS/AnomalyDX11'
bare = repo / 'build/Binaries/RelWithDebInfo/AnomalyDX11'
assert sha(app) == sha(bare) == normal
for p, h in pre.items():
    assert sha(repo / p) == h, ('production drift', p)
for p, h in staged.items():
    assert sha(draft / 'stage' / p) == h, ('draft drift', p)
new = [p for p in staged if p not in pre]
for p in new:
    assert not (repo / p).exists(), ('helper collision', p)
for p in pre:
    dest = out / 'prestate' / p
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(repo / p, dest)
shutil.copyfile(app, out / 'ordinary-app-exe')
shutil.copyfile(bare, out / 'ordinary-bare-exe')
receipt = {'scope': 'cached portal context in full engine CLI; no game/suppression/FPS',
           'started_epoch': time.time(), 'production_prestate': pre,
           'stage_manifest_sha256': sha(draft / 'manifest.json'), 'ordinary_exe_before': normal}

def call(args, logfile, timeout=None):
    print('Starting', logfile, flush=True)
    with (out / logfile).open('w') as f:
        r = subprocess.run(args, cwd=repo, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
    print(logfile, 'exit', r.returncode, flush=True)
    return r.returncode

try:
    verifier = base / 'cpp-walker/transaction-draft-v2/verify_inputs.py'
    receipt['inputs_exit'] = call(['python3', str(verifier), str(repo)], 'inputs.log')
    assert receipt['inputs_exit'] == 0
    for p in staged:
        (repo / p).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(draft / 'stage' / p, repo / p)
    receipt['candidate_source_hashes'] = {p: sha(repo / p) for p in staged}
    receipt['build_exit'] = call(['cmake', '--build', 'build', '--target', 'AnomalyDX11', '-j6'], 'build-candidate.log')
    if receipt['build_exit'] == 0:
        shutil.copyfile(app, out / 'AnomalyDX11-context-replay')
        receipt['candidate_exe_sha256'] = sha(app)
        args = [str(app), '--portal-context-replay',
                str(st / 'runs/portal-active-inputs-01/native/output/active-engine-level.bin'),
                str(st / 'runs/portal-active-inputs-01/native/runtime/gamedata/levels/l01_escape/level.geom'),
                str(st / 'runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv'),
                str(base / 'cpu-prototype/active-frame700-per-id.csv'),
                str(out / 'cpp-cached-frame700-per-id.csv')]
        receipt['replay_argv'] = args
        try:
            receipt['replay_exit'] = call(args, 'replay.log', 60)
        except subprocess.TimeoutExpired:
            receipt['replay_timeout'] = True
        if (out / 'cpp-cached-frame700-per-id.csv').is_file():
            receipt['row_csv_sha256'] = sha(out / 'cpp-cached-frame700-per-id.csv')
finally:
    for p in pre:
        shutil.copyfile(out / 'prestate' / p, repo / p)
    for p in new:
        (repo / p).unlink(missing_ok=True)
    receipt['restored_source_hashes'] = {p: sha(repo / p) for p in pre}
    assert receipt['restored_source_hashes'] == pre
    receipt['normal_build_exit'] = call(['cmake', '--build', 'build', '--target', 'AnomalyDX11', '-j6'], 'build-normal.log')
    if receipt['normal_build_exit'] == 0:
        receipt['fresh_normal_exe_sha256'] = sha(app)
        shutil.copyfile(app, out / 'fresh-normal-exe')
    shutil.copyfile(out / 'ordinary-app-exe', app)
    shutil.copyfile(out / 'ordinary-bare-exe', bare)
    receipt['ordinary_outputs_restored'] = {'app': sha(app), 'bare': sha(bare)}
    receipt['completed_epoch'] = time.time()
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    assert receipt['normal_build_exit'] == 0
print(json.dumps({k: v for k, v in receipt.items() if k not in (
    'production_prestate', 'candidate_source_hashes', 'restored_source_hashes', 'replay_argv')}, indent=2), flush=True)
