#!/usr/bin/env python3
"""Cook the reviewed quality-0 general SSR with an annotation-only DXC fold."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ORIGINAL = '5a0732bd2277256f20ccbd799b7e1ecaca97afd6d711411fff619821763e704c'
FOLDED = 'ec06b50b44c9ee87dd96681128c36653dedd4d290b5a72431bf4aa1416fc1889'
OLD = b'[unroll (q_ssr_steps[SSFX_SSR_QUALITY].x)]'
NEW = b'[unroll (8)]'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = args.source_root.resolve()
    output = args.output.absolute()
    original = (source / 'screenspace_reflections.h').read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL or original.count(OLD) != 1:
        parser.error('source does not match the reviewed SSR annotation contract')
    folded = original.replace(OLD, NEW)
    if hashlib.sha256(folded).hexdigest() != FOLDED:
        parser.error('fold changed bytes outside the reviewed annotation')
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix='xray-ssr-quality0-') as temporary:
        copied = Path(temporary) / 'r3'
        shutil.copytree(source, copied)
        (copied / 'screenspace_reflections.h').write_bytes(folded)
        command = [sys.executable, str(root / 'script/compile_r4_e06_effects_msc.py'),
                   '--source-root', str(copied), '--output', str(output)]
        for shader in ('stub_screen_space.vs', 'ssfx_ssr_gloss.ps', 'ssfx_ssr.ps',
                       'ssfx_ssr_blur.ps', 'ssfx_ssr_combine.ps'):
            command.extend(['--shader', shader])
        subprocess.run(command, check=True)
    receipt = {'quality': 0, 'table_value': 8, 'change': 'annotation-only unroll fold',
               'original_sha256': ORIGINAL, 'folded_sha256': FOLDED,
               'scope': 'single unroll annotation; ray steps, loops and math unchanged',
               'source_root': str(source)}
    (output / 'ssr-annotation-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
