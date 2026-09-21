#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:?source root}"; OUT="${2:?output directory}"
DXC="${XRAY_DXC_EXECUTABLE:-$ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
[[ ! -d "$OUT" || -z "$(find "$OUT" -mindepth 1 -maxdepth 1 -print -quit)" ]] || { echo "output must be empty" >&2; exit 1; }
mkdir -p "$OUT"
for ITEM in 'effects_sun.vs vs_6_0' 'effects_sun.ps ps_6_0'; do
  set -- $ITEM; "$DXC" -T "$2" -E main -HV 2016 -I "$SRC" -D SM_5=1 "$SRC/$1" -Fo "$OUT/$1.dxil" 2> "$OUT/$1.stderr"
  "$MSC" "$OUT/$1.dxil" -o "$OUT/$1.metallib" --output-reflection-file "$OUT/$1.reflection.json" --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
done
jq -e '.ShaderType=="Vertex"' "$OUT/effects_sun.vs.reflection.json" >/dev/null
jq -e '.ShaderType=="Fragment" and .state.num_render_targets==1' "$OUT/effects_sun.ps.reflection.json" >/dev/null
printf 'evidence_scope=exact_effects_sun_dxc_dxil_msc\n' > "$OUT/lens-flare-msc-receipt.txt"
echo "R4_LENS_FLARE_MSC_READY bundle=$OUT"
