# Fixed-camera main Flora SSA upper-bound: measured result

The one-file numerical performance oracle is promising, not a shipping cull.
Same candidate executable SHA-256 `2629dfc72ad58a4724a35bc9a5e8fbd8c025e2ecbf94a4cd5b19d4579927d745`
was used OFF/ON. The current build records `-O2`, ThinLTO, and its frozen CMake
cache in `candidate-ssa-roi-01`; earlier binaries are not its performance baseline.

| Metric | OFF | ON |
|---|---:|---:|
| On-Glass FPS | 25.4742 | 27.2111 |
| Callback FPS | 25.4747 | 27.2069 |
| CPU encode p50 ms | 35.1622 | 32.1162 |
| Indexed draws p50 | 14268 | 13074 |
| GPU command-buffer elapsed p50 ms | 29.8758 | 28.4945 |
| Presented interval p99 ms | 45.8333 | 41.6666 |
| Presented 1% low FPS | 21.0526 | 22.6415 |

Displayed direction +6.82%; CPU encode delta -3.046 ms. Nested timings and GPU
elapsed overlap; no hardware occupancy or additive frame budget claim.

Runs `ssa-roi-off-01` / `ssa-roi-on-01` both exit 0, valid contiguous MetalPerfTrace
coverage with zero timeline gap, 1920x1200, MetalFX off, same pinned save/user/shaders,
fixed eye/direction, 20s warmup/20s measure, E01 only, no CUA access during either run.
Measured weather telemetry is identical (including weight 0, far648/fog638, wind430,
ambient/hemi/sun/fog colors and sun direction). AI is active; calendars differ by8s
and independent dynamic workload can vary. Thus this is a bounded hypothesis screen,
not a repeated whole-game performance acceptance.

ON admitted all1199 pinned IDs and recorded `view_match=1`, `skipped=1199`,
`submitted=4117`, `clip_culled=6347` at samples300/600/900. OFF submitted5316 with
the same6347 camera rejects. Main-only work reduction is therefore confirmed;
the shadow paths did not read the diagnostic skip flag. Median total draw delta1194
is close to1199 because independent game workload differs slightly.

The production source was reversed to its exact prestate SHA3cd57e6f... before either
game run. The current normal app/bare output was preserved and restored to its
resumed-turn prestate SHA `d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0`;
`restore-receipt.json` records the successful normal rebuild and exact output restore.
No ID list, shader change, or suppression remains in production source.

Proceed only to a separate generic source-rule implementation with exact OGF
sphere, authoritative per-frame SSA settings, qualified exemption/material provenance,
fail-open unknowns, and main G-buffer scope. Confirm its decisions against this
one-frame ID oracle, then perform visible/motion proof and same-binary A/B. This
result does not establish Windows/Silo superiority.
