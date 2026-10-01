# Controlled-save run02 v4 preparation

Run01 remains immutable as a failed diagnostic receipt. Its captured argv used a Qt-saved `-fsltx` value in which backslashes and quoting were lost; the exact game process arguments are preserved in `run01/output/root-live-argv.json`. The process was stopped before staging run02, and no game or MO2 process was running under the private alias at staging time.

Run02 uses this escape-free argument string:

`-fsltx ../fsgame.ltx -dbg -nointro -skiplogo -start server(rescue-cordon-control-slowclock/single/alife/load) client(localhost)`

The clone's `workingDirectory` remains `.mo2-parity-01/bin`, so `../fsgame.ltx` resolves to the private clone's game-root file. The engine source at `src/xrEngine/x_ray.cpp:690-691` searches for `-start ` and executes the suffix from the `start` command through `Console->Execute`; no embedded shell quotes or Windows path escapes are needed for the start command. Existing successful Windows logs record the full `Game <save>.scop is successfully loaded from file '<path>'` message, so run02 requires the captured normalized path to equal the canonical clone's full `appdata/savedgames/<save>.scop` path.

The run02 Overwrite hook adds a unique nonce receipt inside its gated `XRAY_PARITY_HARNESS=1` wrapper. The plan pins that nonce and generated hook hash; the runner requires exact receipt contents. This proves that the unique run02 wrapper executed. It does not use USVFS substring co-occurrence as a file-resolution proof. The runner separately requires the engine's successful save-load record with exact normalized path.

`run_mo2_controlled_save_v4.py` passed syntax parsing and a no-launch dry run against run02. Synthetic checks passed for exact save-path normalization (including `bin/..`) and the nonce receipt. `prepare_mo2_controlled_save_run02_v4.py` staged only the private clone config/hook and the new run02 input/output package. The original INI and post-V2 corrupted INI are both retained in `run02/input/`. The V2 result is hash-pinned in the run02 plan.

Qt round-trip remains unverified. The chosen argument contains no quotes, backslashes, commas, percent escapes, or INI metacharacters, and the current literal was read back by the existing runner's simple parser. This is not a substitute for a real QSettings round-trip; root can do a no-game MO2 save/read check before any later game launch. No launch or build was performed here.
