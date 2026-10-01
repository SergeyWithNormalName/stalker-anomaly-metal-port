# V4 runtime motion and local candidate launcher

## Current state

The single bounded motion run is prepared but has not been launched. On 2026-09-29 the CUA desktop inventory returned: `Native apps: Error: The Mac is locked and automatic unlock could not unlock it. Ask the user to unlock the Mac manually before continuing.` No OS settings or permission prompts were touched. The parent owner was asked to hold all UI/game/GPU activity pending manual unlock. Existing V4 ordinary pair remains conditional callback/encode economics, not presented-gameplay acceptance.

The inventory call took about 22 seconds. It returned no native app list because the Mac was locked. `screencapture` evidence from `v4-combined-on-01` contains a black full-display PNG; its selected window ID was `726`, but window-specific capture failed. The existing selector uses `.optionAll` and returns the first owned window with the exact title. The new helper records window number, bounds, layer, alpha, sharing state, and `kCGWindowIsOnscreen`, preferring the largest on-screen owned game window. This will show whether the old failure came from a stale or off-screen match. A locked desktop may still prevent valid pixels, so no image is considered acceptance evidence until manually unlocked.

The motion harness exports `parity_harness_register`, which the cloned `axr_main.script` checks and invokes at `on_game_start`; a static check passed against `runs/v4-combined-on-01/runtime/gamedata/scripts/axr_main.script` (SHA-256 `c0f0fa05e7b495a06f9f841df716a69f4613beeeafe153e0db01aac6f279e182`). It sets heading through HPB.x, keeps `state_string` local, and has an owned 180-second watchdog with a five-second TERM grace, KILL escalation, status receipt, and cleanup for capture/watchdog children. Both launchers clear compute-skin, face-cull, linear DefVertex, E08 GPU-pass, and other inherited profile flags before applying the frozen V4 settings. Shell syntax checks pass; no game has validated registration or the camera path yet.

## Bounded run

`harness/runtime-motion/run_motion.sh` will refuse to start if an AnomalyDX11 process already exists. It clones `runs/v4-combined-on-01/runtime` once into `runs/v4-motion-01/runtime`, preserving the installed archive symlinks, then launches candidate-v4 directly with the raster-depth tree shader, instanced flora shader, and dynamic shadow index enabled. The Lua harness uses the known Cordon corridor camera, applies a small 27-second scripted view path (yaw changes and a 0.75 m forward/backward movement), keeps Device time at 1 and AI/world factor at 6, and exits itself. It writes stage markers and attempts on-screen CGWindow captures to the new run output. No source/build, shared launcher, production runtime, or original profile is changed.

Expected run path: `runs/v4-motion-01`; executable SHA-256 expected from the frozen candidate: `2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71`. There are no process IDs yet because the run is held. After manual unlock, the motion runner will record its game PID in `output/owned-pid.txt` and `output/live-owner.txt`.

The separate CPU-only bounds probe passed its deterministic random sweep and threshold assertions (`COMPUTE_SKIN_BOUNDS PASS cpu-position-bounds signed-weights basis-eligibility cpu-fallback`); final binary SHA-256 is `8a6237fe91252f0d3e851cf8cca5aeaa2ec77a5b758c0c8637cea7696f1b3745`. Exact build/run argv, outputs, and hashes are preserved under `compute-bounds-stage/out/random-sweep-retry-01/`. This check invokes no game or GPU and does not qualify the motion run.

## Local interactive candidate

`launch-interactive-candidate.sh` is the reversible one-command launcher. On first use it creates `interactive-candidate/runtime` from the V4 combined-on runtime, preserves installed archive symlinks, and switches `rs_stats` off only in that copy. It launches the frozen V4 app directly with the same three qualified candidate settings, no harness or profiler flags, and the existing rescue save. Subsequent launches reuse that local copied runtime. The production app and shared runtime defaults are untouched; this is a local candidate, not a redistributed modpack.
