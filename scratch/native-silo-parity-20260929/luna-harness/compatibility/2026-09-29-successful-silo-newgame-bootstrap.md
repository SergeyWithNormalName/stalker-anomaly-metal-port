# Successful Silo new-game bootstrap and generated player autosave

## What the successful run actually did

`scratch/windows-metal-gap-20260929/run_newgame_silo.sh` starts `AnomalyDX11.exe` with `-fsltx ... -dbg -nointro -skiplogo`; it does not pass `-start`, `-load`, or the separate `start_full_newgame.script`. Its only gameplay bootstrap input is a script overlay already present in the isolated runtime's `gamedata/scripts/ui_main_menu.script`.

That overlay is inserted into `main_menu:Update()`. On the first main-menu update while there is no level or the game is at `fake_start`, it marks itself started, calls `ShowFactionUI()`, selects `rookie_village`, updates the location widget, and calls `dlg:OnStartGame()`. The log's `PERF_FULL_NEWGAME normal form serialization rookie_village token=0` confirms that code path ran. It does not click the UI widget, but it uses the stock `UINewGame` form and its `OnStartGame` method. That method serializes the form's character/faction/map/difficulty/economy/loadout options into `axr_main.config`, then calls the ordinary `main_menu:StartGame()`. `StartGame()` starts `server(all/single/alife/new) client(localhost)` and closes the menu. The engine log follows with `Creating new game...` and `New game is successfully created!`.

The separately preserved `scratch/windows-metal-gap-20260929/start_full_newgame.script` is a manual helper that opens `ui_main_menu.main_menu()`, shows the same faction form, selects rookie village, then invokes `OnStartGame()`. The successful runner does not reference this file; the in-runtime `ui_main_menu` overlay performs the equivalent UI-form route itself.

The measurement harness is separate. `run_newgame_silo.sh` sets `XRAY_PARITY_HARNESS=1` and `XRAY_PARITY_HARNESS_SCRIPT` to `newgame_profile.script`. The `axr_main.script` wrapper loads/registers that script at `on_game_start`; the profile waits for `actor_on_first_update`, records state, then starts warmup/measurement. It does not start the new game.

## Which scripts differ from the saved source runtime

Comparing only top-level `gamedata/scripts` files in `runtime/macos-gamma` with `newgame-silo/runtime` found 1,055 files in each; exactly two differ:

- `ui_main_menu.script`: source SHA-256 `84c11f31845de707f44f341cc971a71d52ff1ef1072b4232369a244d2febfe7d`; successful Silo overlay SHA-256 `2763acf91af40b8e3eec628fe550fe9918507c4c1302725a5f1c909faef14b15`. Its only diff is the 11-line automated standard-form new-game trigger above.
- `axr_main.script`: source SHA-256 `51adde1c505d140f7bce4b37119ed82ed554c335f227bcc5690c0375b02b26e0`; overlay SHA-256 `c0f0fa05e7b495a06f9f841df716a69f4613beeeafe153e0db01aac6f279e182`. It wraps original `on_game_start` to load `XRAY_PARITY_HARNESS_SCRIPT`, require `parity_harness_register()`, then invoke the original callback.

`newgame_profile.script` is the external environment-loaded measurement payload (SHA-256 `a192f82be12473d1cfecf0b54833f344f51af8f1e81521e2b8160db5a40e620b`). It emits the phase/state rows only; it is not the UI bootstrap.

The shared Windows/native comparison runtimes used this same harness overlay. The preceding bounded tree comparison found their 4,471 regular `gamedata/configs` and `gamedata/scripts` files identical except `axr_options.ltx` session metadata and the `modded_exes` version field. Thus a common new-game-form bootstrap is available in both copies without introducing an engine debug-map start.

## Where `Player - autosave.scop` came from

After the menu handler starts ALife New Game, the successful log records `[MAC_SAVE_FIX] new game: suppression armed before initial autosave`, then `[MAC_SAVE_FIX] save_state: suppression active for 120 update frames`, then saves 22,422 objects as `Player - autosave.scop` (`xray_crossover.log:7258,7304,7316`). Later in the same run, the same file is successfully loaded at line 13038, before the first-actor callback and measured marker (`phases.csv`: first actor 02:06:35 UTC; measure start 02:07:35 UTC). The engine's level-change save path constructs the user autosave name from `Core.UserName + " - autosave"` and calls `save(autoave_name)` (`src/xrGame/alife_update_manager.cpp:219-226`), matching the logged filename. The latest `player - autosave.scop` and `.scoc` hashes/mtime were reported in the adjacent player-autosave note; the files were rewritten later, so they are not proven byte-identical to their pre-measurement contents.

This is evidence for creating a control through the ordinary new-game form and then using the engine's own first player autosave. It does not establish that the observed weather/NPC state is reproducible from a fresh start, or that the later fatal empty-model path can be avoided. No game, Wine process, or build was run for this audit.
