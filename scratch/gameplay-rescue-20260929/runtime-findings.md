# Runtime qualification receipts — 2026-09-29

## Current verdict

No gameplay FPS improvement is accepted yet. The Lua narrow-conversion crash fix has passed its first V2 full-world run (exit0); ordinary same-binary A/B has resumed on a fresh save created under the fixed binary. Historical V1 crashes and qualification limitations are retained below.

The original app remains restored at `build/Binaries/RelWithDebInfo/Anomaly.app`, executable SHA-256 `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`. Runtime screening used the separate fixed candidate SHA-256 `bfa70a1e4e8670431ea644400362f655a15a8157b7f426fce643e62934f3bd34`. Standalone probe builds subsequently refreshed the backend static library but did not link/change that original app.

## Canonical world and harness

`runs/canonical-oracle-02/runtime/appdata/savedgames/rescue-cordon-control.scop` was saved after a 30-second live warm-up, starting from the newer `stalker - autosave` in the prior baseline scratch runtime. Its SHA-256 is `3708e4732e9601d33b79223896f5ccc147df2a20f8d5908a64fb6b7680a91f1e`. The subsequent arms cloned that same runtime and save into separate directories; their initial `user.ltx` SHA is `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334`. App/save/user input manifests match across linear01/BVH01/BVH02.

The observation camera is (-221.082565,-18.284866,-168.182037), facing +Z, inside a dark Cordon corridor. Each run sets clear2, materializes interpolation weight0, pins sun at08:03, pauses weather interpolation, and starts the calendar at2018-10-26 08:03:23. The calendar then advances: Device factor1 and world factor6 remain live. This is a stationary full-world observation, not a walking route or a matched Silo world.

Postrun manifests cover 5,527 real configuration/script files or symlink records per arm. There are zero content changes relative to the canonical runtime. Only `gamedata/configs/axr_options.ltx` mtime changed; its bytes and SHA match. Receipts: `qualification/content-comparison.json` and each run's `output/content-manifest.json`. Installed archive directories remain shared symlinks; they were not rewritten or redistributed.

Harness repairs are in `harness/validation/`: direct executable launch from the authorized shell; inherited stats switched off at first update; window-specific macOS capture after timing; 15-second post-timing hold; display watcher exits when the owned game exits; tail counts in the summary. Ordinary runs have no E01/E07 CSV, oracle, GPU listener, CPU sample, or per-frame signpost flags. The per-callback Lua CSV overhead remains uncalibrated.

The first LaunchServices attempt, canonical-oracle-01, stopped before main at a macOS Documents permission prompt. Its sample/screenshot are preserved, it has no runtime result, and no permission was granted. Direct launch worked. Full-display captures remain obscured by that stale prompt/Stage Manager and are not visual evidence. Window-specific `game-window.png` is usable.

## Correctness diagnostic

Canonical-oracle-02 completed and exited0 after30-second warmup/15-second diagnostic with `XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE=1` and `XRAY_METAL_LOCAL_LIGHT_FACE_CULL=1`. The final DefVertex log reports150,000 shadow views,1146 source draws,7,088,301 broad-phase candidates, misses0: mean47.26 candidates/view. This is admission correctness evidence, not an FPS number.

During its measured diagnostic window, the two face counter samples show before285/291, eligible276/282, exempt0, unknown0, removed7/13, remaining278, batches23/24. Thus the original face candidate removes some work; the later six-halfspace helper has CPU proof but has not been run in gameplay. See `runs/canonical-oracle-02/output/counters.json`.

## Interrupted DefVertex A/B

| Run | Switch | Outcome |
| --- | --- | --- |
| defvertex-linear-01 | SHADOW_LINEAR=1; face/oracle absent | completed timed window and scripted finished |
| defvertex-bvh-01 | linear/face/oracle absent | exit139 during warm-up; no measurement |
| defvertex-bvh-02 | identical retry | exit139 during warm-up; no measurement |

Linear01:740 frame intervals over59.95s,12.3436 wall FPS; callback p50/p95/p99/max80/83/85/944ms. Two intervals exceed100ms; one exceeds250 and500ms. Camera drift is exactly0, clear2 and factors1/6 are stable, render size1920x1200. The measured calendar advances08:06:23→08:12:23. Its actual window screenshot shows the corridor and HUD without an obvious geometry break; it is qualitative evidence only.

The runtime lead accidentally edited the shell launcher while linear01 was active. Bash later encountered a read-offset/syntax error, losing post-exit launcher status/power receipts. Lua reached `finished` at frame1292 and orderly quit, and the measured trace/window capture are intact. This arm alone does not establish a candidate gain. Do not mutate a launcher file while its shell is running.

Both crash IPS files are copied into each BVH run's `output/crash.ips`; they fault in `CEntity::ChangeTeam` at+584/+592 through Lua set_team and deferred GameThread scheduling. The read-only triage is owned by scene_cpu/root. Both failed arms are excluded from timing comparison.

## Standalone candidate qualification

`qualification/cpu-ctest.log`:4/4 passed, DefVertexAdapter, DynamicWorldAdapter, LocalLightFaceVisibility, FloraAdapter. This includes the revised face helper. No production app was linked for these tests.

Compute GPU oracle initially failed because `vertex` is reserved in MSL. visibility_architecture renamed the argument to `vertexIndex` with no math/tolerance change; `qualification/compute-gpu-ctest-02.log` passes1/1. The standalone bounded timing log contains131,073-vertex1–4weight cases,10GPU samples each:

| Weights | CPU reference repack, one sample ms | GPU command incl clear/readback p50 ms | CPU encode p50 ms |
| --- | --- | --- | --- |
| 1 | 4.306125 | 0.428458 | 0.023500 |
| 2 | 6.474625 | 0.485208 | 0.027083 |
| 3 | 7.889416 | 0.361375 | 0.024875 |
| 4 | 8.269625 | 0.328792 | 0.023750 |

All cases have exact packed normal/tangent/binormal bytes and UV/alpha. Float position exact mismatches are0/61684/27553/80607; maximum relative error is0/1.0366e-7/1.0962e-7/1.9321e-7, within the pre-existing1e-6position gate. This only screens numerical feasibility and gross economics. It excludes production dispatch/bounds/cache integration and does not measure gameplay net gain. Receipt: `qualification/compute-timing.log`.

Flora synthetic GPU probe reports PASS. Positive leaf coverage3214/3317/3298 and trunk coverage6732pixels agree between legacy and instanced pairs; alpha discard reduces3214 versus6732covered pixels. The probe rejects zero coverage and checks paired R32 equality. `qualification/flora-gpu.log`, `qualification/flora-synthetic-coverage.json`, and `flora-gpu/` contain results. The shell wrapper subsequently rejected the reserved zsh variable `status`; the probe itself printed PASS and its source returns0 on that outcome. Production same-state local+sun Flora pairs remain untested pending the stability fix and a new candidate build.

The broader standalone compute oracle subsequently FAILS strict packed normal equality on adversarial threshold cases:2W stress0/1,3W stress0,4W stress0 have max normal-byte delta1. Tangent/binormal/UV/alpha remain exact; position relative error stays within1e-6. Do not enable production compute from the narrower pass or its timing screen. `qualification/compute-stress.log` records all24cases; no tolerance was widened. The initial installed corpus returns probe/admission PASS for31leaves:30GPU-compared leaves and1CPU-fallback leaf. Source paths/hashes and raw fixture digests are in `qualification/installed-skin-fixtures/manifest.json`; outputs are in `qualification/compute-installed-results.json`. This initial corpus pass did not override the adversarial strict-gate failure.

## V2 stability repair preparation

Root authorized a generic defined narrow-numeric Lua conversion repair after source/Windows ABI triage:5converter calls, truncation/modulo2^N, preserving wider types. The source fix is isolated in `src/3rd party/luabind/detail/policy.hpp` plus new helper/tests; no scheduler/team/gameplay-script changes. `qualification/lua-narrow-v2.log` centrally passes17inputs×5types for signed/unsignedchar and UBSan/float-cast-overflow, including typed callback6664→8. Source freeze was confirmed by both implementers. V2 full app rebuild started into `candidate-v2/build.log`; the oldbfa candidate remains preserved. The next stability boot will create a fresh settled control from the original earlyplayer save under V2, avoiding use of the pre-fix canonical world as accepted V2 evidence.

The isolated numeric repair now matches observed ARM64 FMA ordering and disables implicit contraction. All24synthetic/stress cases pass with bitwise positions/basis/UV/alpha, and the contract-on negative control reproduces the original1-LSB failures. Installed coverage is30GPU-compared leaves /82,530vertices with bitwise equality, plus1CPU-fallback B&S HUD leaf /17,139vertices using existing CPU tangent handling (the single-weight normalized fallback basis). This is not a multiweight tangent rebuild claim. Coverage details are in `qualification/compute-installed-coverage.json`; exact scratch compile/link commands and frozen reference-library hashes are in `qualification/compute-numeric-scratch/receipt.json`. No production compute implementation or new timing run is qualified by these numerical checks.

The V2 fixture protocol removes `set_game_time` entirely: all saved ALife clocks and quest/NPC timers remain monotonic. `loaded_state` is recorded before render weather/camera pinning. Verified render weather helpers modify environment descriptors/sun direction without writing ALife. VM/swap counter snapshots are taken at timed-window start/end with capture timestamps.


## V2 first live correctness run

`runs/v2-canonical-oracle-01` completed exit0 with the pinned V2 executable SHA256 f66ac10ccd4b706506b8e191f684745323d219e2398b96f7e85884c14ff46fc3. Fresh early player save, 60-second live warmup, 15-second diagnostic, new rescue-cordon-control save. Both exact admission oracles report 307000 shadow views and zero misses; dynamic indexed_views=307000, fallback_views=0, candidate_checks=5470016. DefVertex candidates=14532771, source count1146. Six-plane face cull still only removes about13 of303 at the latest sampled frame. These instrumentation results are not performance evidence. Actual game-window capture shows the expected dark interior corridor. Drawable settles at1920x1200. Layer vsync remains YES despite saved rs_v_sync off: native backend initialization is hardcoded YES; this is documented and held constant for both arms, not silently treated as uncapped presentation.

Root took over validation after both Sol workers returned usage-limit errors; no additional agent retries. Readiness fix was applied after preserving c16124 source prestate. Draft state tests passed optimized and UBSan, but the modified production adapter is not yet built or runtime-qualified. V2 remains pinned and does not include that later fix.


## Ordinary V2 screen and invalidated Flora depth evidence

V2 linear01 and BVH01 both exit0. Linear:12.7566FPS,p50/p95/p99=78/80/82ms; BVH:12.7095FPS,77/94/114ms. Same binary/save/config, no oracle flags, identical materialized environment values. Live population is not bit-identical (575 versus577-581 captured renderables), and no overall gain is established. Preserve these as screening, not accepted improvement.

Flora live oracle01 exit0 with18pairs,8positive local and5positive sun passes; one sun-c1 frame120 pair differs at(1227,628):0.0267156865 versus0.065196082. This is NOT a depth difference: subsequent actual-source/DXIL review proves existing tree PS output is opaque0 / alpha-tested texture color underUSE_HWSMAP. Production R32 is then sampled as depth. Earlier text claiming the ordinary compat PS already wrote raster depth was incorrect. Pair equality only proved equivalent wrong color output; it never qualified depth. Old synthetic probe even documented texture-red output and lacked independent expected-depth checks.

Corrective cooker now wraps the original opaque/AREF functions (preserving clip), returning SV_Position.z, using a new separate bundle shadow-casters-raster-depth-v1. Strengthened GPU oracle independently checks analytic projected depth and leaf depth intervals, retains exact legacy/instanced equality, and checks trunk wind unconditionally. Analytic tolerance1e-4 accommodates raster XY quantization; this does not relax exact paired maps. New bundle passes the stronger synthetic gate. Original bundle preserved for a negative control. Production qualification pending. No active mod sources changed.

Flora oracle01 E01 after pair caps: diagnostic218frames,p50encode67.80ms,scene66.13ms,dynamicPrewarm15.94ms,GPUelapsed36.14ms,wait0.00025ms,drawable0.030ms. This diagnostic includes a2-second CPU sample and is not a performance acceptance run. Parent scopes are nested.

V3 readiness build5e7791c9d571452db1488ca604b727d3c38291a1d61b4ebff667e851471d9592 passed DynamicWorldAdapter and PreparedState CPU tests. Its first live start aborted before first_update with the previously observed empty-file model-load path: CModelPool::Exists→Create→Instance_Load→FS.r_open, from CHangingLamp net_Spawn. Separate from the fixed Lua squad crash. New source bounds Exists(null/empty) to absent before prefetch, allowing existing caller visual fallback; runtime validation pending. This is a valid query-boundary repair, but do not claim the old crash causality closed until exercised in game.


### Locked-display qualification correction and current team

Luna runtime owner used CUA inventory and found the Mac locked. V4 window captures failed and full-display images are black, consistent with that state. V4 combined ordinary pair is17.1433vs15.18865callbackFPS (+12.87%), p5058vs66ms, inputSHA and materializedweatheridentical. These are CONDITIONAL throughput measurements under the observed displaystate, NOT accepted presented gameplay/visual/motion proof. Root asked user asynchronously to unlock; do not unlock automatically/changeOSsettings. Live/game/motion run held; code and offscreen correctness continue.

Luna is available despite two Sol usage-limit failures. Independent Luna read-only review found no actionable defects in depthwrapper/strengthenedprobe, readinessstate, or emptyvisualquery guard (no additionalruntimeproof claimed). Current roles: luna_patch_review implements narrow CPU compute-skin bounds helper; luna_compute_backend implements opt-in native compute job API/epoch/ownership + offscreenproof; luna_runtime owns centralbuild/GPU/game coordination and scratch motion/interactive launcher. Root remains architecture/review. Compute adapter routing MUST stay absent until nativejobproof, exactbounds/fallback, transactionalpendingposecommit/discard/retry contracts reviewed. Do not stop the overall task due13%screen or treat remainingpotential as exhausted.
