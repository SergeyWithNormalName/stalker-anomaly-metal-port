# Generic source SSA: initial runtime result

Candidate executable SHA256 `1c81af85f17fc7de4f2e1ab133563331209aad407b89d851d8b5bebea70f2bd7`,
manifest SHA256 `54654b51c95172797548c9545a42a4c112ca219994767018fd1be97f223641f6`.
Same binary OFF/ON, fixed canonical save/user/shaders/camera,1920x1200,
MetalFX off,20s warmup/20s measured window,no CUA during numeric runs.

| Metric | OFF | ON |
|---|---:|---:|
| On-Glass FPS | 25.7038 | 27.2621 |
| Callback FPS | 25.7119 | 27.3189 |
| CPU encode p50 ms | 34.7959 | 32.8698 |
| Indexed draws p50 | 14759.0000 | 13420.0000 |
| GPU command-buffer elapsed p50 ms | 30.1037 | 28.6915 |
| Presented interval p99 ms | 45.8333 | 45.8332 |
| Presented 1% low FPS | 20.6897 | 19.3549 |

Mean displayed direction +6.06%. Weather fields at measurement start match exactly;
calendar differs3 seconds and AI is live. Both exit0 with contiguous valid17s
MetalPerfTrace coverage and zero timeline gap. Nested scopes/GPU elapsed are not
additive frame budget or hardware occupancy. Median total draw difference1339
exceeds1199 because independent dynamic work differs. 1%low worsened in this one
short pair; no improved-stability or Windows-superiority acceptance is claimed.

Runtime Lua route inspection succeeded (`known=1`,4 materials). All temporary
resource-manager maps reported0 textures/RT/VS/PS/states/passes/elements/shaders;
no legacy GPU resources were compiled. Leaf/trunk materials eligible. ON samples
300/600/900 reject1199, submit4117; OFF submits5316 with6347 clip rejects unchanged.

Visual-only runs deliberately permit CUA and are NOT FPS evidence. A rotating
outdoor eye(-221.082565,-8.284866,-168.182037) displayed trees/houses/sky/HUD;
culls changed with live view. An OFF run exited0 but screenshot timed out. The
same-process fixed-camera control then changed source setting r__ssa_discard7->0->7:
0 produced5316 draws/zero SSA rejects; restored7 produced4120/1196. CUA images for
both states were seen. Tree wind and AI/calendar continued, so images were not
pixel-identical. A possible foreground trunk/pole appearance difference remains
unqualified; no complete pixel/motion PASS. Luna source review proves settled
eye/matrix provenance is coherent and raw sphere matches source static SSA.

A new offline indexed-position check joins all5316 captured IDs to exact active
level geometry, transforms their actual referenced vertices by their tree matrix,
and compares them to the authored sphere. Under a0.1%radius+1mm tolerance,214
spheres fail containment, including113 of the1199 numerical rejects. Maximum
ratio is4.199 on visual11299. This does not show a Metal/source formula mismatch;
it shows why matching raw source policy alone is insufficient image acceptance.
Keep uncertain bounds drawing; investigate a conservative qualification before
promoting this draft. `sphere-geometry.csv` and `sphere-geometry.log` retain evidence.

All11 touched existing source files were reversed exactly and the new helper
removed after candidate freeze. Ordinary app/bare restored to resumed-turn
SHA256 d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0.
The successful ordinary rebuild is saved separately (cabc919d...). No mods,
original Windows EXE, or primary user.ltx were modified. Candidate remains
scratch-only/default-off and is not production accepted.
