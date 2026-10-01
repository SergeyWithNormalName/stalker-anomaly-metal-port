# MO2 clone path-length audit

## Findings

The scratch clone's lexical MO2 mod root is much deeper than the installed root. A deterministic sample of the first 200 active `MyTree` mod entries mapped to 200 of 200 original mod folders. For each mod, I measured one filesystem-first file path without walking its full payload tree. Original Windows-style paths ranged from 75 to 169 characters; clone-lexical paths ranged from 184 to 278. Five of the 200 clone-lexical paths were at least 260 characters and fourteen exceeded 250. The corresponding original paths were all below 260. The longest sampled path was 278 characters.

This boundary is relevant in principle: the cloned USVFS 0.5.6.1 DLL contains the literal diagnostic string `executable filename can't be longer than 260 characters`. Targeted reads found no `LongPathsEnabled` value in the selected Silo bottle's `system.reg`, `user.reg`, or `userdef.reg`. Embedded manifests for cloned `ModOrganizer.exe`, `AnomalyDX11.exe`, and `usvfs_x64.dll` do not declare `longPathAware`.

The first menu attempt's USVFS log confirms hooks initialized, but it contains no explicit max-path failure. The exact-ManualBottle retry later loaded native `C:\\windows\\system32\\dxgi.dll` and `d3d11.dll` matching the copied DXVK payload, then stopped at the same pre-engine point; fresh Anomaly DXVK logs remained empty. Neither run recorded an explicit overlength-path failure. The clone root length is therefore a credible compatibility risk, not an established cause of the startup hang. The 260-character text in USVFS may be a generic Windows error-message string; it does not prove the startup failure came from a USVFS path-length guard.

One limitation matters: the clone's `mods` directory is a symlink to the original, shorter `gamma/gamma/mods` path. The sample uses the clone's lexical `.../runtime/mo2/mods` prefix. If MO2 or Wine canonicalizes that symlink before USVFS records source files, actual source path lengths may be shorter. Existing logs do not show which spelling USVFS retained. A short-alias launch and private MO2 data-path log are needed to verify the effective path.

## Short-alias preparation

The owned short alias `<PRIVATE_LOCAL_PATH>` now points to the existing COW runtime root; no game copy was created. The clone's `gamePath` and all configured custom-executable paths have been retargeted to that lexical alias. The exact pre-alias private INI was saved at `runs/mo2-reference-startup-01/output/ModOrganizer.before-short-alias.ini`; the final retarget receipt is `runs/mo2-reference-startup-01/short-alias-preparation.json`. The original-mods symlink still targets `<PRIVATE_LOCAL_PATH>`, and `MyTree` profile files are unchanged.

Under the proposed alias spelling, the longest sampled file path falls from 278 to 194 characters (84 fewer). The clone's MO2 data path has not yet been checked through a new process; Wine/MO2 may canonicalize the symlink. The prepared root-only retry is `sol-analysis/mo2-reference-preparation/launch_menu_startup_short_alias.py` and is dry-run by default. It uses the exact copied selected ManualBottle, stock game EXE hash, private DXVK log/state-cache directories, and primary MO2/game/Silo write-deny rules.

The root retry should launch `ModOrganizer.exe` through the alias, keep the private selected ManualBottle prefix and the same stock EXE/profile hashes, and keep the primary-tree write deny. Do not claim MO2 used the short data path until its own new startup log confirms it. If that log canonicalizes back to the long physical path, stop and choose an alias layout MO2 preserves before another game start.

## Renderer log and state-cache paths

The exact installed DXVK 1.10.3 `d3d11.dll`/`dxgi.dll` payloads contain `DXVK_LOG_PATH` and `DXVK_STATE_CACHE_PATH`. The versioned upstream [DXVK 1.10.3 README](https://github.com/doitsujin/dxvk/blob/v1.10.3/README.md) specifies each as a directory setting; `DXVK_LOG_PATH` redirects log files and `DXVK_STATE_CACHE_PATH` redirects pipeline cache files. The prepared launch wrapper sets both to directories under that run's scratch output, so DXVK has no reason to write to Silo's primary Logs or the source prefix. Their Windows-style path lengths are below 260 characters.
