# Controlled-save run03 v5 preparation

Run03 uses the exact approved `matched_profile_saved_slowclock_v2.lua` from the shared scratch source (SHA-256 `20568c539a6a58c4c907dc93225e84c8a383354e580caa7098f06d8e7afeb21f`). The observer change is pinned byte-for-byte; no new clock logic was added. The failed V2 and V4 receipts are preserved in their original run directories and their hashes are listed in the run03 plan.

The clone's canonical `user.ltx` was restored from `scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime/appdata/user.ltx` (SHA-256 `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334`). The pre-run02 clone LTX, post-run02 clone LTX, original INI, and run02 INI are preserved under the run-specific input folders.

The game arguments remain the escape-free literal `-fsltx ../fsgame.ltx -dbg -nointro -skiplogo -start server(rescue-cordon-control-slowclock/single/alife/load) client(localhost)`. V5 gives the private hook a new nonce and unique receipt path. Post-run save verification accepts the exact save basename with or without `.scop`, then requires the entire normalized path to equal the clone's `appdata/savedgames/rescue-cordon-control-slowclock.scop` path. The V4 double-backslash Wine module-log parser remains included.

Syntax parsing, V5 no-launch preflight, canonical-LTX/Lua/hash pins, and exact-path verifier tests passed. The path verifier accepted both actual engine naming forms and rejected wrong aliases, file names, and directories. No result or owned-PID file exists in run03. This package was not launched or built; it is ready for parent review. Qt GUI resave verification, if desired, remains a separate no-game step.
