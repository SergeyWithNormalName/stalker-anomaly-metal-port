#!/usr/bin/env python3
"""E08: cook only instanced regular-tree shadow VS; retain source wind/alpha math."""
import argparse, hashlib, json, pathlib, subprocess

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def replace_once(text, old, new):
    if text.count(old) != 1: raise ValueError(f'Expected one source contract: {old!r}')
    return text.replace(old,new)

def main():
    p=argparse.ArgumentParser();p.add_argument('--source-root',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
    project=pathlib.Path(__file__).resolve().parents[1];src=a.source_root.resolve();out=a.output.resolve()
    if out.is_relative_to(project/'runtime') or a.output.is_symlink() or (out.exists() and any(out.iterdir())):
        raise ValueError('Output must be new/empty and outside the live runtime')
    out.mkdir(parents=True,exist_ok=True)
    original=src/'shadow_direct_tree.vs';wind_path=src/'screenspace_wind.h'
    shader=original.read_text();wind=wind_path.read_text()
    shader=replace_once(shader,'#include "screenspace_wind.h"','''struct E08TreeInstance { float4x4 model; };
StructuredBuffer<E08TreeInstance> e08_tree_models : register(t1);
#include "screenspace_wind.e08.h"''')
    for name in ['v_shadow_direct_aref','v_shadow_direct']:
        shader=replace_once(shader,f'main ( {name} I )',f'main ( {name} I, uint e08_instance_id : SV_InstanceID )')
    shader=replace_once(shader,'float3 \tpos\t= mul(m_xform , I.P);','float3x4 e08_model = (float3x4)e08_tree_models[e08_instance_id].model;\n\tfloat3 \tpos\t= mul(e08_model , I.P);')
    shader=replace_once(shader,'pos.y - m_xform._24','pos.y - e08_model._24')
    # The source analytic trunk helper also reads the model translation for
    # its wave phase. Pass that same per-instance value, never the first tree.
    wind=replace_once(wind,'ssfx_wind_tree_trunk(float3 pos, float Tree_H, wind_setup W, bool prev)',
        'ssfx_wind_tree_trunk(float3 pos, float Tree_H, wind_setup W, bool prev, float modelY)')
    wind=replace_once(wind,'m_xform._24','modelY')
    wind=replace_once(wind,'ssfx_wind_tree_branches(float3 pos, float Tree_H, float tc_y, wind_setup W, bool prev)',
        'ssfx_wind_tree_branches(float3 pos, float Tree_H, float tc_y, wind_setup W, bool prev, float modelY)')
    wind=replace_once(wind,'ssfx_wind_tree_trunk(pos, Tree_H, W, prev)',
        'ssfx_wind_tree_trunk(pos, Tree_H, W, prev, modelY)')
    shader=replace_once(shader,'ssfx_wind_tree_branches(pos, H, tc.y, ssfx_wind_setup(), false)',
        'ssfx_wind_tree_branches(pos, H, tc.y, ssfx_wind_setup(), false, e08_model._24)')
    shader=replace_once(shader,'ssfx_wind_tree_trunk(pos, H, ssfx_wind_setup(), false)',
        'ssfx_wind_tree_trunk(pos, H, ssfx_wind_setup(), false, e08_model._24)')
    (out/'screenspace_wind.e08.h').write_text(wind)
    (out/'shadow_direct_tree_instanced.vs.hlsl').write_text(shader)
    dxc=project/'build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc';msc=project/'build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter'
    defines=['SMAP_size=2048','FP16_FILTER=1','FP16_BLEND=1','USE_HWSMAP=1','USE_HWSMAP_PCF=1','USE_BRANCHING=1','USE_VTF=1','SM_5=1','SSFX_INT_GRASS=8','SSFX_SSR_QUALITY=0','SSFX_WATER_QUALITY=1','SSFX_WATER_PARALLAX=2','SSFX_IL_QUALITY=16','SSFX_AO_QUALITY=2','SSFX_POM_REFINE=0','SSFX_TERRA_POM_REFINE=0','SSFX_SSS_DIR_QUALITY=6','SSFX_SSS_OMNI_QUALITY=2','SSFX_MODEXE=1','SKIN_NONE=1']
    receipt={'scope':'e08-flora-instanced-shadow','inputs':{str(q.relative_to(src)):sha(q) for q in sorted(src.rglob('*')) if q.is_file()},'schema':'direct-metal-e08-flora-shadow-instancing/v1','source_root':str(src),'original_tree_sha256':sha(original),'original_wind_sha256':sha(wind_path),'adapted_tree_sha256':sha(out/'shadow_direct_tree_instanced.vs.hlsl'),'adapted_wind_sha256':sha(out/'screenspace_wind.e08.h'),'instance_stride':64,'models_register':'t1','defines':defines,'artifacts':{},'dxc_version':subprocess.check_output([str(dxc),'--version'],text=True),'msc_version':subprocess.check_output([str(msc),'--version'],text=True)}
    for leaf in [False,True]:
        name='shadow_direct_tree_instanced'+('_aref' if leaf else '')+'.vs'
        args=[str(dxc),'-T','vs_6_0','-E','main','-HV','2016','-I',str(src)]
        for d in defines+(['USE_AREF=1'] if leaf else []):args+=['-D',d]
        args+=[str(out/'shadow_direct_tree_instanced.vs.hlsl'),'-Fo',str(out/(name+'.dxil'))]
        with (out/(name+'.stderr')).open('w') as err: subprocess.run(args,stderr=err,check=True)
        subprocess.run([str(msc),str(out/(name+'.dxil')),'-o',str(out/(name+'.metallib')),'--output-reflection-file',str(out/(name+'.reflection.json')),'--deployment-os=macOS','--minimum-os-build-version=14.0.0','--minimum-gpu-family=Metal3'],check=True)
        receipt['artifacts'][name]={ext:sha(out/(name+ext)) for ext in ['.dxil','.metallib','.reflection.json']}
        print(name,json.loads((out/(name+'.reflection.json')).read_text())['TopLevelArgumentBuffer'])
    receipt['outputs']=receipt['artifacts']
    (out/'flora-instanced-shadow-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
if __name__=='__main__':main()
