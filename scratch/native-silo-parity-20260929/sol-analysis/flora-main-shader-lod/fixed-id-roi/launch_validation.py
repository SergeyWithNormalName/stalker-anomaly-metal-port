#!/usr/bin/env python3
"""Open one prepared private app with validation; report launcher status separately."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

out = Path(sys.argv[1]).resolve()
assert (out / 'launch-preview-only.txt').read_text().strip() == 'launch_preview_only_no_process'
assert not (out / 'validation-launch.json').exists()
argv = (out / 'argv.txt').read_text().splitlines()
exe = Path(argv[0]).resolve()
assert hashlib.sha256(exe.read_bytes()).hexdigest() == 'ecf55ea105ce5b0e9aef083462e159b9ddc8ec3de107357186c82f81f16cc67a'
app = exe.parents[2]
assert app.suffix == '.app'
env = dict(line.split('=', 1) for line in (out / 'environment.txt').read_text().splitlines())
assert env['XRAY_METAL_FLORA_LEAF_LQ_ROI'] == '1'
env.update(MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1',
    MTL_SHADER_VALIDATION_DEFAULT_STATE='none',
    MTL_SHADER_VALIDATION_ENABLE_PIPELINES='R4 flora fixed-ID leaf_wave_lq MSC G-buffer',
    MTL_SHADER_VALIDATION_REPORT_TO_STDERR='1',
    MTL_SHADER_VALIDATION_DUMP_PIPELINES='1')
cmd = ['/usr/bin/open', '-n', '-W', str(app), '--stdout', str(out / 'stdout.log'),
       '--stderr', str(out / 'engine.log')]
for key, value in sorted(env.items()):
    cmd += ['--env', key + '=' + value]
cmd += ['--args'] + argv[1:]

def owned_pids():
    result = subprocess.run(['/bin/ps', '-axo', 'pid=,command='], text=True,
                            check=True, capture_output=True)
    found = []
    for line in result.stdout.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and (parts[1] == str(exe) or parts[1].startswith(str(exe) + ' ')):
            found.append(int(parts[0]))
    return found

assert not owned_pids(), 'this candidate is already running'
started = time.time()
launcher = subprocess.Popen(cmd, cwd=out.parent / 'runtime',
    stdout=(out / 'open.stdout.log').open('w'), stderr=(out / 'open.stderr.log').open('w'))
pid = None
reason = 'application_terminated'
try:
    while time.time() - started < 240:
        found = owned_pids()
        assert len(found) <= 1, 'unexpected duplicate owned app'
        if found and pid is None:
            pid = found[0]
            (out / 'owned-pid.txt').write_text(str(pid) + '\n')
            (out / 'launch-epoch.txt').write_text(str(int(started)) + '\n')
        if pid and not found:
            break
        if pid is None and launcher.poll() is not None:
            reason = 'launcher_finished_without_observed_app'
            break
        time.sleep(1)
    else:
        reason = 'bounded_timeout'
finally:
    found = owned_pids()
    if found:
        os.kill(found[0], signal.SIGTERM)
        for _ in range(5):
            if not owned_pids(): break
            time.sleep(1)
        if owned_pids(): os.kill(found[0], signal.SIGKILL)
    try:
        launcher_status = launcher.wait(timeout=10)
    except subprocess.TimeoutExpired:
        launcher.terminate()
        launcher_status = launcher.wait(timeout=5)
    receipt = dict(actual_game_pid=pid, launcher_exit=launcher_status,
        game_exit_code=None, reason=reason, started_epoch=started, ended_epoch=time.time(),
        validation_environment=env, note='open -W status is not a game exit code; no FPS acceptance')
    (out / 'validation-launch.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k != 'validation_environment'}, indent=2))
