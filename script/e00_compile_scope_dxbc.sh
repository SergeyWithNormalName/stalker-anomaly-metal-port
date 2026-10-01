#!/bin/zsh
# Materialize precisely one E00 DXBC cache record; does not launch Anomaly.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
runtime="${SILO_WINE_RUNTIME:-$HOME/Library/Application Support/Silo/Runtimes/wine-cx-26.3.0-dxmt}"
bottle="${SILO_WINEPREFIX:-$HOME/Library/Application Support/Silo/ManualBottles/9048990D-C276-4EE6-B935-59ACF4B8979D}"
compiler_dll="${E00_D3DCOMPILER47_DLL:-$HOME/gamma/gamma/dlls/d3dcompiler_47.dll}"
toolchain="${E00_DXBC_TOOLCHAIN:-silo}"
shader_name="${E00_SHADER_NAME:-model_scope_lense}"
shader_stage="${E00_SHADER_STAGE:-ps}"
case "$shader_stage" in ps|vs) ;; *) print -u2 "E00_SHADER_STAGE must be ps or vs"; exit 2;; esac
shader_target="${shader_stage}_5_0"
key="${E00_SHADER_KEY:-20481111001100000000000001001011000100100801216200621000000}"
skin_define="${E00_SHADER_SKIN_DEFINE:-SKIN_2}"
source_root="$repo_root/build/metal-shader-sources/r3"
source="$source_root/$shader_name.$shader_stage"
tool_dir="$repo_root/tools/e00_dxbc"
output_root="${E00_DXBC_OUTPUT_ROOT:-$tool_dir/out}"
raw_dxbc="$output_root/$shader_name.$shader_stage.dxbc"
cache="$output_root/shaders_cache/r4/$shader_name.$shader_stage/$key"
helper="$output_root/e00_scope_dxbc.exe"
staged_compiler_dll="$output_root/d3dcompiler_47.dll"
provenance="$output_root/provenance-$shader_name.txt"

for required_path in "$runtime/bin/wine" "$source" "$compiler_dll"; do
    [[ -e "$required_path" ]] || { print -u2 "E00 missing required path: $required_path"; exit 1; }
done

mkdir -p "$output_root"
rm -f "$raw_dxbc" "$cache" "$helper" "$staged_compiler_dll"
cp -c "$compiler_dll" "$staged_compiler_dll"

# The Silo winegcc binary was built with stale runner paths.  WINEBUILD and
# explicit SDK include/lib paths make the command independent of those paths.
case "$toolchain" in
    silo)
        for required_path in "$runtime/bin/winegcc" "$runtime/bin/winebuild" \
                "$runtime/include/wine/windows/d3dcompiler.h" "$runtime/lib/wine/x86_64-windows/libkernel32.a"; do
            [[ -e "$required_path" ]] || { print -u2 "E00 missing Silo SDK path: $required_path"; exit 1; }
        done
        # Silo winegcc carries stale install paths.  WINEBUILD plus explicit
        # include/lib locations avoids them; its bundled lld-link is still a
        # host prerequisite, and this command fails closed when absent.
        WINEBUILD="$runtime/bin/winebuild" "$runtime/bin/winegcc" --target=x86_64-windows \
            -I"$runtime/include/wine/windows" \
            -L"$runtime/lib/wine/x86_64-windows" \
            -o "$helper" "$tool_dir/e00_scope_dxbc.c" -lkernel32
        ;;
    mingw)
        compiler="${E00_MINGW_GCC:-x86_64-w64-mingw32-gcc}"
        command -v "$compiler" >/dev/null || { print -u2 "E00 MinGW compiler not found: $compiler"; exit 1; }
        # No d3dcompiler import library: e00_scope_dxbc.c loads the native
        # versioned DLL at runtime through LoadLibraryA/GetProcAddress.
        "$compiler" -std=c11 -Wall -Wextra -Werror -o "$helper" \
            "$tool_dir/e00_scope_dxbc.c" -lkernel32
        ;;
    *)
        print -u2 "E00_DXBC_TOOLCHAIN must be silo or mingw, got: $toolchain"
        exit 2
        ;;
esac

# The signed Microsoft DLL is staged beside the helper, which is the first
# LoadLibrary search location. Silo's runtime ships wine but not winepath; its Z: drive is
# the POSIX root, so absolute workspace paths have this deterministic mapping.
source_win="Z:\\$(print -r -- "$source" | sed 's|/|\\\\|g')"
raw_win="Z:\\$(print -r -- "$raw_dxbc" | sed 's|/|\\\\|g')"
WINEPREFIX="$bottle" WINEDLLOVERRIDES='d3dcompiler_47=n' \
    "$runtime/bin/wine" "$helper" "$source_win" "$raw_win" "$skin_define" "$shader_target" | tee "$output_root/compiler-$shader_name.log"

python3 "$tool_dir/verify_cache.py" \
    --source "$source" --source-root "$source_root" --raw-dxbc "$raw_dxbc" \
    --cache-output "$cache" --expected-key "$key" | tee "$output_root/cache-verify-$shader_name.log"

{
    print "key=$key"
    print "source=$source"
    print "source_sha256=$(shasum -a 256 "$source" | awk '{print $1}')"
    print "target=$shader_target"
    print "entry=main"
    print "flags=D3D10_SHADER_PACK_MATRIX_ROW_MAJOR=0x00000008"
    print "defines=SMAP_size=2048,FP16_FILTER=1,FP16_BLEND=1,USE_HWSMAP=1,USE_HWSMAP_PCF=1,USE_BRANCHING=1,USE_VTF=1,$skin_define=1,USE_SOFT_WATER=1,USE_DOF=1,SUN_SHAFTS_QUALITY=1,USE_REFLECTIONS=1,SM_5=1,SSFX_SSR_QUALITY=0,SSFX_WATER_QUALITY=1,SSFX_WATER_PARALLAX=2,SSFX_IL_QUALITY=16,SSFX_AO_QUALITY=2,SSFX_POM_REFINE=0,SSFX_TERRA_POM_REFINE=0,SSFX_SSS_DIR_QUALITY=6,SSFX_SSS_OMNI_QUALITY=2,SSFX_MODEXE=1"
    print "wine_runtime=$runtime"
    print "wine_version=$($runtime/bin/wine --version)"
    print "toolchain=$toolchain"
    if [[ "$toolchain" == silo ]]; then
        print "winegcc_sha256=$(shasum -a 256 "$runtime/bin/winegcc" | awk '{print $1}')"
        print "winebuild_sha256=$(shasum -a 256 "$runtime/bin/winebuild" | awk '{print $1}')"
    else
        print "mingw_gcc=$(${E00_MINGW_GCC:-x86_64-w64-mingw32-gcc} --version | head -n 1)"
    fi
    print "native_d3dcompiler47_source=$compiler_dll"
    print "native_d3dcompiler47_sha256=$(shasum -a 256 "$compiler_dll" | awk '{print $1}')"
    print "raw_dxbc_sha256=$(shasum -a 256 "$raw_dxbc" | awk '{print $1}')"
    print "cache_sha256=$(shasum -a 256 "$cache" | awk '{print $1}')"
    print "command=$0"
} > "$provenance"

print "E00_DXBC_READY cache=$cache provenance=$provenance"
