Root executes these only after scoped patch/source review. This worker has not run any of them. The existing probe path was verified; its old binary is not a test of the new draft.

```bash
cd <XRAY_SOURCE_ROOT>
cmake --build build --target XRay.Metal.LocalLightRolloverGpuProbe --config RelWithDebInfo -j 4
```

Use the qualified `build/metal-runtime-shaders/local-lights` bundle. The new CLI mode controls off/on internally and restores prior bind/retry environment values on exit.

```bash
XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=mixed \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 127 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/mixed-127

XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=mixed \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 128 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/mixed-128

XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=point \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 3 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/point

XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=spot \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 3 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/spot

XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=omni \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 3 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/omni

XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=spot \
XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_SHADOWED=1 \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 3 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/shadowed-spot

XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE=omni \
XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_SHADOWED=1 \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --bind-reuse-gpu build/metal-runtime-shaders/local-lights 3 \
  scratch/native-silo-parity-20260929/sol-analysis/probe-out/shadowed-omni
```

No diagnostic environment should leak into a game launch. Inspect each generated `local-light-rollover-report.json`: `bind_reuse_compared=true`, eight exact color/depth/stencil pairs, discard/retry/slot-wrap gate, positive signal and exact 3VS/3PS versus 1VS/2PS counters. For each shadowed family also require `shadow_signal_pixels>0`, `max_shadow_lit_minus_blocked_delta>0.001`, and `shadow_depth_stencil_preserved=true`. These compare two actual renders with the same shadowed PSOs and atlas depths `.95` versus `0.0`. The dedicated `bind_reuse_family` records the selected family; the existing generic `types` list describes the fixture's supported families. Report fixture failures with their raw outputs.
