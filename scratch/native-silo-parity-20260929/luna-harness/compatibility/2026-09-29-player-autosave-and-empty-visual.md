# Successful Silo player autosave and empty visual branch

## Save loaded in the prior successful Silo run

The successful `newgame-silo` log shows the new-game path wrote `Player - autosave.scop` at `xray_crossover.log:7316`, then loaded `player - autosave.scop` from the same `appdata/savedgames` directory at line 13038 in 1.679 seconds. The load is before the measured window: the same run's `phases.csv` has `fresh_actor_first_update` at epoch `1790647595` (2026-09-29 02:06:35 UTC) and measurement start at `1790647655` (02:07:35 UTC). The log has no wall-clock timestamp on the save/load record, so the exact load second cannot be recovered; it is bounded before the first-actor callback and the measured interval.

The currently preserved pair at that path is:

- `player - autosave.scop`, 1,276,674 bytes, SHA-256 `333b3ab17af80c0fe81a400523bb7ea75840c52c1bbef79b1b53f86649eb9ab9`.
- `player - autosave.scoc`, 96,928 bytes, SHA-256 `c32cd62fbc94db0a381208862c6fbd5ea6ec33e8de60165f857a0ebccd95c5b7`.

These are the current file hashes. Their filesystem modification time is 2026-09-29 02:26:36 UTC, after the 02:07–02:09 measurement; the raw log does not record a hash at load. Treat these hashes as identifying the preserved candidate pair, not as proof of its exact bytes at the earlier load. The later `stalker - autosave.scop` is 1,529,797 bytes with `.scoc` 274,733 bytes; the Silo log shows it being saved at line 14591 after the player autosave was loaded. The old run exited 0 and reported 32.10 wall FPS, but that measures its own Silo new-game flow and says nothing about the later cross-engine failure.

## Empty-model path

Current source traces the general empty visual branch without needing to guess which entity failed:

1. `CHangingLamp::net_Spawn` calls its inherited `CGameObject::net_Spawn` at `src/xrGame/HangingLamp.cpp:92-97`.
2. For a server entity with a visual, `CGameObject::net_Spawn` gets `visual_name(E)` and calls `Render->models_Exists(visualName)` at `src/xrGame/GameObject.cpp:275-289`. The base `visual_name` returns `CSE_Visual::get_visual()` at lines 1179-1183.
3. R4 delegates `models_Exists` to `Models->Exists(name)` at `src/Layers/xrRenderPC_R4/r4.cpp:741`.
4. Current `CModelPool::Exists` returns false for a null/empty name before it reaches `Create`/filesystem prefetch at `src/Layers/xrRender/ModelPool.cpp:517-529`. That false result lets `CGameObject::net_Spawn` choose the object's configured `visual` fallback when one exists.

The Windows-control-start log now proves a Windows-created save loaded successfully before the same empty-name `FileDownload` / `CModelPool::Create` failure. That removes save ABI as an explanation for this crash. It still does not identify the failing entity: the fatal log has no filename or spawn record in the stack. The Silo runtime EXE is May 15, 2026; the saved prestate `ModelPool.cpp` lacks the current ten-line guard, while current source contains it. This makes the old binary lacking the guard likely, but the targeted Windows PDB disassembly was not present in the reviewed scratch artifacts. The PDB stack names `CModelPool::Create` but cannot prove the old `Exists` branch's instructions. Therefore old-EXE guard absence is not statically proven here, and the entity remains unknown.

No source, binary, mod, save, or runtime was changed. No game or build was run for this audit.
