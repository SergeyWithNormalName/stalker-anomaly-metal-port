# Fresh-newgame empty-visual differential (read-only)

## Finding

The current Silo failure is after a real new-game bootstrap and loading its new `Player - autosave.scop`, but before any actor callback. It is not a save-format load rejection: the log says `22420 objects are successfully loaded`, then `Game player - autosave is successfully loaded ...`, then fatal `FileDownload: can't open file :` with stack `CModelPool::Create -> CLevel::g_sv_Spawn -> cl_Process_Spawn -> CLevel::OnFrame` (`runs/windows-fresh-save-01/silo/runtime/appdata/logs/xray_crossover.log:13017-13019, 14041-14062`). There is no entity name or requested visual in the fatal report. Last preceding `[LSS] Spawning object` is `lc_cordon_south_darkscape_collapsed_tunnel` id 23057, but `CALifeSwitchManager::add_online()` logs that only after `server().Process_spawn()` returns, so this does not identify the failing entity.

## Old successful runtime vs current common input

The current run's prelaunch `runtime-manifest.json` covers 4,473 files. Comparing every listed file hash against the preserved old `newgame-silo/runtime` found exactly two differences: `appdata/user.ltx` and `gamedata/configs/axr_options.ltx`. The EXE and PDB are byte-identical across old and current clones:

- `AnomalyDX11.exe`: SHA256 `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`.
- `AnomalyDX11.pdb`: SHA256 `84aedf63be05f4b9192157faf46d23977f3f648ffc33a8d99138fd7eb92cd2f0`.
- Config/scripts/asset content in that manifest otherwise matches the old successful runtime.

`axr_options.ltx` differences are limited to session bookkeeping plus `[modded_exes] version`: old `session_id=239`, `session_start=1790648748000`, version `20260515`; canonical prelaunch input had session 240, start `1790645272000`, version `20260929` (current run rewrote session to 241 after launch). Other option/mod-enable lines are identical. The runtime EXE itself reports `Modded Exes version 2026.05.15`; no script reads the `axr_options.ltx` version field as a model selector.

Old `user.ltx` has 588 parsed keys, current canonical has 680. Differences are mostly extra explicit keys in the current file or old-only keys, with no overlapping key value changes found. The directly relevant visible actor settings match (`g_legs=1`, `g_legs_attach_to_camera=1`, `hud_fov=0.65`). Current-only renderer/multithreading options include `mt_calc_bones=1`, `mt_scheduler=1`, `r2_mt=on`; there is no source/log evidence tying them to an empty visual name. Thus the profile/config comparison does not surface a concrete model-name setting that explains this spawn failure.

The generated save artifacts do differ: old successful run's preserved player save is 1,276,674-byte `.scop` + 96,928-byte `.scoc`; current fresh save is 1,274,947-byte `.scop` + 96,268-byte `.scoc`. Logs report 22,422 objects in the old save and 22,420 in the current save. This is a concrete input difference that could change which ALife records get spawned online, but the failing record is not logged, so causality and entity identity are unproven.

## Source contract and candidate

Current worktree `CGameObject::net_Spawn()` obtains `visual_name(E)` from `CSE_Visual::get_visual()` and immediately calls `Render->models_Exists(visualName)` before applying the section's `visual` fallback (`src/xrGame/GameObject.cpp:278-288`). Current `CModelPool::Exists()` contains a new empty/null-name guard at `src/Layers/xrRender/ModelPool.cpp:519-529`; that guard was added in this dirty worktree on 2026-09-29. Without it, `Exists("")` falls through to `Create(N, ..., false)` and can reach `Instance_Load` / file open using an empty path. The current error shape therefore matches an empty incoming visual query that the current source guard prevents.

The failing Windows image is the same May 15 EXE that ran the old successful scenario; its fatal PDB stack maps `CModelPool::Create` to old source line 266. This line mapping does not identify the input name. The current dirty source guard was added on Sep 29, months after this EXE build. The EXE contains `CModelPool::Create` / `Exists` symbols and the fallback warning string, but not the new unique `MODEL_POOL_EMPTY_VISUAL_QUERY` guard diagnostic. `llvm-pdbutil`/`pdbparse` are unavailable here, so I did not recover machine-level disassembly from the PDB. The Windows binary therefore likely lacks the guard, but the exact call argument is not logged and absence of that diagnostic string alone is not a disassembly proof.

The best two candidates are consequently:

1. **Old Windows executable lacks the empty-visual `Exists()` guard.** Strong source/stack match to the fatal; it cannot alone explain why the earlier run passed because the EXE hashes are identical.
2. **The two different generated saves contain different ALife record sets.** The 22,422 vs 22,420 object counts show world/save divergence despite the same normal-form overlay and matched mod content. It may select a record with empty visual during online spawn; exact record remains unknown.

No named lamp/NPC/object is claimed. The direct new-game attempt has already established a reproducible failure; more launches or config guessing are not justified by this audit. The only source-level fix candidate is the empty-visual guard already in the current source, but applying it to the existing Windows binary would require a rebuilt Windows runtime and is outside this read-only audit.
