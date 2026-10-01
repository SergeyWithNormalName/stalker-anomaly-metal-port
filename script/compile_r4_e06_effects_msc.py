#!/usr/bin/env python3
"""Cook exact E06 leaf effects without touching existing shader bundles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--shader', action='append', required=True)
    parser.add_argument('--define', action='append', default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source, output = args.source_root.resolve(), args.output.absolute()
    if output.is_symlink() or root / 'runtime' in output.resolve().parents:
        parser.error('output must not be a symlink or live runtime directory')
    if output.exists() and any(output.iterdir()):
        parser.error('output must be new or empty')
    shaders = args.shader
    if any(Path(name).name != name or not name.endswith(('.vs', '.ps')) for name in shaders):
        parser.error('each shader must be an entry-point basename ending in .vs or .ps')
    for name in shaders:
        if not (source / name).is_file():
            parser.error(f'missing source: {name}')
    output.mkdir(parents=True, exist_ok=True)
    dxc = root / 'build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc'
    msc = root / 'build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter'
    defines = ['SMAP_size=2048', 'FP16_FILTER=1', 'FP16_BLEND=1', 'USE_HWSMAP=1',
               'USE_HWSMAP_PCF=1', 'USE_BRANCHING=1', 'USE_VTF=1', 'SM_5=1',
               'SSFX_INT_GRASS=8', 'SSFX_SSR_QUALITY=0', 'SSFX_WATER_QUALITY=1',
               'SSFX_WATER_PARALLAX=2', 'SSFX_IL_QUALITY=16', 'SSFX_AO_QUALITY=2',
               'SSFX_POM_REFINE=0', 'SSFX_TERRA_POM_REFINE=0',
               'SSFX_RAIN_QUALITY=2', 'SSFX_SSS_DIR_QUALITY=6', 'SSFX_SSS_OMNI_QUALITY=2', 'SSFX_MODEXE=1'] + args.define
    receipt = {'scope': 'exact_active_e06_leaf_non_msaa', 'source_root': str(source),
               'defines': defines, 'dxc': str(dxc.resolve()), 'msc': str(msc.resolve()),
               'dxc_version': subprocess.check_output([dxc, '--version'], text=True).strip(),
               'msc_version': subprocess.check_output([msc, '--version'], text=True).strip(),
               'inputs': {}, 'outputs': {}}
    # Hash the include tree as well as entry points. Keep the exact compiler
    # sources for inspection; this cook never rewrites HLSL.
    for path in sorted(source.rglob('*')):
        if path.is_file():
            receipt['inputs'][str(path.relative_to(source))] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in shaders:
        vertex = name.endswith('.vs')
        dxil = output / (name + '.dxil')
        with (output / (name + '.stderr')).open('w') as errors:
            subprocess.run([dxc, '-T', 'vs_6_0' if vertex else 'ps_6_0', '-E', 'main',
                            '-HV', '2016', '-I', source,
                            *[arg for define in defines for arg in ('-D', define)],
                            source / name, '-Fo', dxil], stderr=errors, check=True)
        (output / (name + '.dxil.txt')).write_text(subprocess.check_output([dxc, '-dumpbin', dxil], text=True))
        reflection = output / (name + '.reflection.json')
        subprocess.run([msc, dxil, '-o', output / (name + '.metallib'),
                        '--output-reflection-file', reflection, '--deployment-os=macOS',
                        '--minimum-os-build-version=14.0.0', '--minimum-gpu-family=Metal3'], check=True)
        data = json.loads(reflection.read_text())
        assert data['ShaderType'] == ('Vertex' if vertex else 'Fragment'), name
        assert vertex or 1 <= data['state']['num_render_targets'] <= 4, name
        for entry in data['TopLevelArgumentBuffer']:
            assert entry['Space'] == 0 and entry['Size'] == 24, (name, entry)
        receipt['outputs'][name] = {
            ext: hashlib.sha256((output / (name + ext)).read_bytes()).hexdigest()
            for ext in ('.dxil', '.metallib', '.reflection.json')}
        print(f'{name}: resources={data["ResourceCount"]} tlab={data["TopLevelArgumentBuffer"]}')
    (output / 'effects-msc-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
