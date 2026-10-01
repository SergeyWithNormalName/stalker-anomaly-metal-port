# MO2 controlled-save run preparation

## Current state

The Silo-route MO2 menu run now reaches `CRender::render_menu` from `CMainMenu::OnRender`, and its fresh child log loads builtin `d3d11.dll` and `dxgi.dll`. That clears the startup gate for a saved-state diagnostic; it is not gameplay, visual acceptance, or an FPS result.

The MO2 `MyTree` profile still has `LocalSettings=true` and `LocalSaves=false`. I checked the Anomaly plugin mapping and the actual menu-run game log rather than assuming `%appdata%`: the plugin maps game-root `appdata` into the virtual `appdata` directory, defines game documents as `%GAME_PATH%/appdata`, and lists saves under `%GAME_DOCUMENTS%/savedgames`. The menu-run `xray_crossover.log` records that the effective settings file was `Z:\...\.mo2-parity-01\bin\..\appdata\user.ltx`. Its current clone hash was `bd98e407…`, not the canonical `b82f466f…`, so preparation staged the canonical file into the disposable clone path used by the engine. The profile settings and modlist were left unchanged.

## Prepared clone-only inputs

`prepare_mo2_controlled_save_run.py --prepare` staged the exact slow-clock Cordon save (`4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225`) under the verified game-root appdata save directory. It copied the previously exercised `axr_main.script` hook unchanged into the selected profile's Overwrite tree. The hook preserves its original `on_game_start` body as `parity_harness_original_on_game_start()`, loads the diagnostic only behind `XRAY_PARITY_HARNESS=1`, and then calls the preserved handler. The script is hash-pinned and the previous clone user.ltx and MO2 INI were saved under this run's `input/` directory.

The clone's Anomaly DX11 executable remains the stock `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3` file. The only executable-row change in the private clone INI is its argument string: `-fsltx "Z:\\...\\.mo2-parity-01\\fsgame.ltx" -dbg -nointro -skiplogo -start "server(rescue-cordon-control-slowclock/single/alife/load) client(localhost)"`. The `-start server(.../single/alife/load) client(localhost)` form follows the existing Windows/Silo matched-run harness; the clone still launches through MO2 as `-p MyTree run -e "Anomaly (DX11)"`.

The saved `output/launch-plan.json` carries the same reviewed GPTK/D3DMetal route settings as menu V3: Wine CX 26.3, ESYNC, MetalFX enabled, and the full builtin D3D/DXGI override list. It also points the existing `matched_profile_saved_slowclock.lua` observer at this run's output. That observer checks the loaded calendar and both time factors, waits for the clear weather to materialize, records a short stable world interval, and exits. It is a bounded save-load/world-update proof, not a menu benchmark or a performance window.

## Root-owned next step

No process was launched by the preparation script. The no-launch runner draft is `run_mo2_controlled_save.py`; it imports the V3 snapshot/process/sandbox contracts and parses the prepared Qt INI row through the preparation helper. It refuses any prior result/phase/PID files, uses a 600-second cap, records new game and USVFS logs plus read-only D3DMetal `lsof` output after the loaded-state marker, and stops only the owned MO2 process and the dedicated copied prefix. It has no WineDbg or performance-trace path. It is dry-run by default and requires `--launch` plus `PARITY_ENABLE_MO2_SAVE_RUN=1`; root owns any execution.

Before a root-owned run, review the emitted INI argument row and input hashes, then launch the clone through MO2 using the command and environment in `runs/mo2-reference-startup-01/mo2-controlled-save-run-01/output/launch-plan.json`.

Afterward, accept only if the fresh USVFS log shows the Overwrite `axr_main.script` and staged save mapping, the engine log shows the canonical clone appdata `user.ltx` loaded, and observer markers validate the Cordon save's calendar `2018-10-26T08:09:47`, Device factor `1`, game factor `1e-6`, `weather_committed`, `measure_end`, and `post_window_end`. Require at least one `l01_escape` device row with the same calendar/time factors, unchanged primary hashes, unchanged profile settings/modlist, builtin D3D11/DXGI child-module lines, and an owned-prefix cleanup receipt. The launcher exits zero only for its recorded acceptance gate; still inspect `result.json` and source logs directly. Missing hook/save mapping, missing observer markers, a different calendar/settings path, timeout, or early exit is invalid. No screenshot is captured, so this is not visual equivalence or FPS acceptance.

## Integrity

The helper has no process-launch path. It writes only into the private MO2/game clone and this scratch run directory. It leaves the primary MO2, game, Silo bottle, profile modlist/settings, and production sources untouched.

The launcher draft passed an AST syntax parse only and was not run. No app, Wine process, game, build, shader cook, or performance trace was started for this preparation.
