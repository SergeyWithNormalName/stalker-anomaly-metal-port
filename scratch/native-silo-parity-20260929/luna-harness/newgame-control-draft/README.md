# Windows new-game control draft (not executed)

This is a separate draft for root review; it does not touch the active matched-pair runner, source install, GAMMA/mod source, or runtime. `control_profile.lua` is intended to be copied into an isolated clone's output directory and supplied as `XRAY_PARITY_HARNESS_SCRIPT`. The only shell helper, `print_windows_argv.sh`, prints a command and never launches it.

Select the bootstrap with `PARITY_START_MODE=newgame` or `PARITY_START_MODE=load`. For newgame, omit both `-load` and `-start`: existing clone `ui_main_menu.script` overlay chooses `rookie_village` in the normal new-game form and calls `dlg:OnStartGame()`. Its `UINewGame:OnStartGame()` serializes the configured character/faction/loadout and calls `owner:StartGame()`, which starts the normal `single/alife/new` path. This is the sequence used by the historical `run_newgame_silo.sh`; it is not physical UI automation. `load` emits the confirmed `-start "server(NAME/single/alife/load) client(localhost)"` form; bare `-load` was observed to run before ALife startup and fall into new-game behavior.

The control profile ignores `fake_start`, requires Device factor 1, pins the existing matched camera and weather preset, keeps ALife/NPCs active at game factor 6, waits 20 seconds after `actor_on_first_update` while precache/loading screens are absent, then calls the normal console `save parity-windows-fresh-cordon` once. It leaves five seconds for the server save request and screenshot to finish, records phase markers and exits. This is an artifact-generation/control run only. Root must inspect engine log receipt and output hashes, then explicitly test loading the exact resulting `.scop` (and `.scoc` if present) in each engine before considering any FPS pair.

## Saved evidence / save contract

The old successful Silo bootstrap was launched by `scratch/windows-metal-gap-20260929/run_newgame_silo.sh`, with `-fsltx`, `-dbg`, `-nointro`, `-skiplogo`, and no `-start`/`-load`. The clone-only overlay in `scratch/windows-metal-gap-20260929/newgame-silo/runtime/gamedata/scripts/ui_main_menu.script` logged `PERF_FULL_NEWGAME normal form serialization rookie_village token=0`. Log then reports `Creating new game...`, 18,677 spawn points loaded, and `Player - autosave.scop` saved with 22,422 objects. The profile’s `actor_on_first_update` was after the game transition; the save happened during new-game level change, before that profile callback. Therefore the profile's explicit named save is a later, actor-alive save after a deliberate settling window.

Source confirms `save NAME` is single-player/alive-actor gated, sends `M_SAVE_GAME` to the server and immediately requests a `.dds` screenshot. `game_sv_Single::save_game()` delegates to ALife storage; `CALifeStorageManager::save()` writes the compressed save with `SAVE_EXTENSION` (`.scop`). The Windows GAMMA clone's `alife_storage_manager.script` separately writes `.scoc` via the `CALifeStorageManager_before_save` callback and reads it in `CALifeStorageManager_load`; it is mod-owned marshalled script state, not the engine’s core save. The command's screenshot output is `<save stem>.dds`. Verify that all expected files are present after save; `.scoc` can be absent if that callback did not produce it. A `.dds` is a screenshot sidecar, not proof of equal render target size or state.

## Draft-only checks / gates

1. Clone the known Windows runtime and prefix into a new empty scratch run directory first; do not point at the preserved source runtime. Keep original save names and source files unchanged.
2. Set `XRAY_PARITY_HARNESS=1`, the clone-local Windows-mapped `XRAY_PARITY_HARNESS_SCRIPT`, `PERF_GAP_PHASES`, and `METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1`. Sanitize inherited `XRAY_METAL_*` before any shader-env sourcing. For the artifact-only pass, do not attach FPS interpretation.
3. For `newgame`, log must prove the overlay's `PERF_FULL_NEWGAME` and subsequent successful Cordon/ALife spawn. For `load`, require an ALife-started line and explicit successful load of the intended save. Reject runs with fallback `Creating new game...` when testing load.
4. Save receipt must show `parity-windows-fresh-cordon.scop`; hash it and any emitted `.scoc`/`.dds`. Verify camera, weather, game/Device factors, actor alive, level, and game clock from `.control.csv`/log. Preserve existing six nonfatal Lua mod-load errors as diagnostics; do not repair mods here.
5. Only after the exact fresh save reloads successfully in both arms, compare loaded state and actual internal render targets. Keep any later displayed/presented FPS comparison separate and use the shared Metal signpost timeline analyzer.

Inert command examples (the helper only prints argv):

```sh
PARITY_RUNTIME="/absolute/path/to/new-cloned-silo/runtime" PARITY_START_MODE=newgame bash scratch/native-silo-parity-20260929/luna-harness/newgame-control-draft/print_windows_argv.sh
PARITY_RUNTIME="/absolute/path/to/new-cloned-silo/runtime" PARITY_START_MODE=load PARITY_LOAD_NAME=parity-windows-fresh-cordon bash scratch/native-silo-parity-20260929/luna-harness/newgame-control-draft/print_windows_argv.sh
```
