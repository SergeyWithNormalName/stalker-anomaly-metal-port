#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STAGE="$ROOT/scratch/gameplay-rescue-20260929/compute-backend-stage"
OUT="$STAGE/cpu-economics"
mkdir -p "$OUT"
COMMON=(
  -DDIRECTINPUT_VERSION=0x0800 -DDOSWIN32 -DIDIRECTPLAY2_OR_GREATER -DNOCOMM
  -DNODRAWTEXT -DNOGDICAPMASKS -DNOHELP -DNOICONS -DNOKEYSTATES -DNOMCX
  -DNOMEMMGR -DNOMENUS -DNOMETAFILE -DNOMINMAX -DNOPROFILER -DNOSERVICE
  -DOPTICK_ENABLE_GPU=0 -DPORTABLE_BUGSLAYERUTIL -DPURE_ALLOC -DSTRICT
  -DVC_EXTRALEAN -DWIN32_LEAN_AND_MEAN -DXRCORE_API= -DXRCORE_STATIC
  -DXRPHYSICS_API= -D_CRT_SECURE_NO_DEPRECATE
  -D_SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
  -D_SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS -D_STLP_USE_DECLSPEC=1
  -D_WIN32_DCOM -D_WIN32_WINNT=_WIN32_WINNT_WIN7 -Dxr_pure_interface=__interface
  -I"$ROOT/src" -I"$ROOT/src/3rd party" -I/opt/homebrew/include
  -I/opt/homebrew/include/SDL2 -I"$ROOT/src/Layers/xrRenderMetal"
  -I"$ROOT/build/src/Layers/xrRenderMetal/generated" -I"$ROOT/src/xrCore"
  -I"$ROOT/src/3rd party/DXERR" -I"$ROOT/build/Externals/LZO"
  -I"$ROOT/build/Externals/LZO/LZO" -I"$ROOT/build/Externals/optick"
  -I"$ROOT/build/Externals/optick/optick" -I"$ROOT/src/Layers/xrAPI"
  -I"$ROOT/src/xrCDB" -fdeclspec -fms-extensions -fno-operator-names
  -Wno-implicit-function-declaration -Wno-c++11-narrowing
  -Wno-invalid-token-paste -Wno-nonportable-include-path
  -Wno-shift-negative-value -Wno-address-of-temporary -Wno-register
  -ftrivial-auto-var-init=zero -Wuninitialized -O2 -g -DNDEBUG
  -std=gnu++17 -flto=thin -arch arm64
)
/usr/bin/c++ "${COMMON[@]}" -c "$STAGE/ComputeSkinCpuEconomicsProbe.cpp" \
  -o "$OUT/ComputeSkinCpuEconomicsProbe.o"
/usr/bin/c++ -O2 -g -DNDEBUG -flto=thin -arch arm64 \
  -Wl,-search_paths_first -Wl,-headerpad_max_install_names \
  "$OUT/ComputeSkinCpuEconomicsProbe.o" \
  "$ROOT/build/src/Layers/xrRenderMetal/CMakeFiles/XRay.Metal.ComputeSkinBoundsProbe.dir/tests/R4LocalLightRolloverGpuSupport.cpp.o" \
  -o "$OUT/ComputeSkinCpuEconomicsProbe" \
  "$ROOT/build/src/Layers/xrRenderMetal/libXRay.Metal.R4Backend.a" \
  "$ROOT/build/Binaries/RelWithDebInfo/libXRay.Core.a" \
  /opt/homebrew/lib/libSDL2.dylib -framework Cocoa -framework Metal \
  -framework QuartzCore -framework ImageIO \
  "$ROOT/build/Externals/DxErr/libDxErr.a" \
  "$ROOT/build/Externals/LZO/libLZO.a" \
  "$ROOT/build/Externals/optick/liboptick.a" \
  "$ROOT/build/Binaries/RelWithDebInfo/libXRay.Core.Crypto.a" \
  /opt/homebrew/opt/openssl@3/lib/libcrypto.dylib
"$OUT/ComputeSkinCpuEconomicsProbe" \
  "$ROOT/scratch/gameplay-rescue-20260929/qualification/installed-skin-fixtures"
