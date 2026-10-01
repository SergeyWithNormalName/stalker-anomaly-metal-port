# Gameplay rescue validation plan

Updated: 2026-09-29. Scope: native Direct Metal on the current M1 Pro checkout. The earlier Windows/Silo report is contextual evidence; it is not used as a matched A/B because it ran a different binary and world state.

## Repository and protected prestate

- Git root: `<XRAY_SOURCE_ROOT>`.
- No `AGENTS.md` was found under this repository or its parent STALKER directory.
- Before rebuilding, the approved app bundle was copied to `prestate/validation/Anomaly-baseline.app` (45 MB). Its executable SHA-256 is `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`. The manifest also records the source scratch save and `user.ltx` hashes in `prestate/validation/baseline-sha256.txt`; the pre-build worktree status is in `prestate/validation/git-status.porcelain`.
- The game harness clones `scratch/windows-metal-gap-20260929/newgame-native/runtime` into a fresh run folder. It does not write to the original appdata, user profile, Wine prefix, or source save.

## What the first live baseline established

`runs/baseline/` is a launch-path diagnostic, not the controlled performance baseline: it used a cloned `player - autosave` save, ran with E01/E07 frame CSVs, GPU listener, and per-frame signposts disabled, and sampled a 60-second live Device window at engine resolution 1920x1200. It entered `l01_escape` directly from the save, so no LLDB startup-unpause step was needed; the engine log reports Device `time_factor=1`, and the Lua marker reports world `game_time_factor=6`.

The first diagnostic window measured 21.54 mean callback FPS, callback interval p50 46 ms / p95 50 ms / max 82 ms across 1,293 callbacks. It began in `w_rain2` and the camera direction changed during warm-up. Do not compare these numbers against a candidate or present them as a controlled before/after result. The game's `screenshot` console command did not create a visual artifact; use `screencapture` after the timed window in later runs.

## Controlled run fixture and harness

Use `harness/validation/run_live_profile.sh` with `harness/validation/live_profile.lua`. For each arm, it clones the same pristine runtime and save into a new `runs/<id>/runtime`. At the first gameplay update it verifies Device clock = 1, records the loaded state, pins camera position `(-221.082565, -18.284866, -168.182037)` facing +Z, materializes and holds `w_clear2` with render sun direction at08:03, and preserves the actual loaded ALife clock. It leaves world time factor6 and Device simulation running so AI/NPC activity remains live. It never rewinds the saved world clock; saved quest/NPC timers must remain coherent. The harness records frame callback timestamps, camera, level, weather, world/Device factors, calendar, and render dimensions on every callback. For a fresh settled canonical save, warm for60seconds before saving. Each ordinary A/B arm warms for30seconds and records60seconds. Phase markers also record materialized weather values, and the shell captures VM/swap counters at measurement start/end. It turns on the built-in stats overlay only after the measurement and captures the display with macOS `screencapture` two seconds after `measure_end`.

Each arm needs a fresh run ID and the same base runtime. Never reuse a run directory. Before interpreting a pair, compare each arm's `input-sha256.txt` to ensure the cloned source save and `user.ltx` hashes match, then verify `phases.csv` markers and stable camera/weather/time/resolution across the measured window. The measured workload is live NPC/AI simulation from a saved gameplay world with a fixed observation camera; it does not represent a player walking route.

Game launch command after candidate bundle copy:

```sh
RESCUE_APP="$PWD/scratch/gameplay-rescue-20260929/candidate/Anomaly.app" \
RESCUE_RUN_ID=baseline-defvertex \
RESCUE_ENV_ASSIGNMENTS='XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR=1' \
./scratch/gameplay-rescue-20260929/harness/validation/run_live_profile.sh
```

Same executable, same save, default candidate off:

```sh
RESCUE_APP="$PWD/scratch/gameplay-rescue-20260929/candidate/Anomaly.app" \
RESCUE_RUN_ID=candidate-defvertex \
./scratch/gameplay-rescue-20260929/harness/validation/run_live_profile.sh
```

The initial performance pair isolates DefVertex from face culling: baseline explicitly sets `XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR=1`; candidate leaves it unset; `XRAY_METAL_LOCAL_LIGHT_FACE_CULL` stays unset in both. Use ABBA only if those two runs show material drift or wide tails; do not generate repeated benchmark batches by default. A separate short 15-second correctness/counter run can set `XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE=1` and `XRAY_METAL_LOCAL_LIGHT_FACE_CULL=1` to check whether face admission removes any work; it is diagnostic only and cannot supply performance numbers.

The launcher forwards every semicolon-separated `RESCUE_ENV_ASSIGNMENTS` entry with `open --env`, records the environment and input SHA manifest, and refuses to run when another `AnomalyDX11` process exists. It records power and thermal state before/after. E01/E07 CSVs, Metal Performance Trace listener, GPU counters, CPU sampling, and per-frame signposts stay off for ordinary FPS runs. The lightweight callback trace is used only to calculate total frames / wall interval and frame callback p50/p95/max; report its measurement overhead as uncalibrated. If a candidate shows a credible signal, do one explicitly instrumented run to extract shadow/culling/draw counters, compare its wall timing against the low-instrumentation run, and label that run diagnostic.

## Build and tests

The existing trusted app build command is:

```sh
cmake --build build --target Anomaly.DX11 -j 4
```

The existing DefVertex probe target is `XRay.Metal.DefVertexAdapterProbe`; the visibility work adds `XRay.Metal.LocalLightFaceVisibilityProbe`. The targeted CTest names are `XRay.Metal.DefVertexAdapter.SelfTest`, `XRay.Metal.LocalLightFaceVisibility.SelfTest`, and the existing `XRay.Metal.LocalLightAdapter.SelfTest` mesh-containment test:

```sh
ctest --test-dir build/src/Layers/xrRenderMetal \
  -R '^(XRay\.Metal\.(DefVertexAdapter|LocalLightFaceVisibility|LocalLightAdapter)\.SelfTest)$' \
  --output-on-failure
```

The candidate build must be copied from `build/Binaries/RelWithDebInfo/Anomaly.app` to `candidate/Anomaly.app`, hashed, then the approved app path restored from `prestate/validation/Anomaly-baseline.app`. Do not launch or benchmark the just-built app until that candidate copy and the restored production-app SHA have both been verified.

## Result gates

For an FPS claim, report mean FPS as callbacks divided by wall duration plus callback interval p50/p95/max, with run IDs and exact binary SHA. Never sum nested CPU/GPU timing scopes. Keep draw/light/caster counters separate from wall frame timing. Require actual screenshot review for lighting/shadow correctness; counters or a clean build do not establish visual correctness. Reject a face-culling candidate as a performance step if its short diagnostic shows no removed faces/work, and reject it on any missing required light/shadow in the post-window image.
