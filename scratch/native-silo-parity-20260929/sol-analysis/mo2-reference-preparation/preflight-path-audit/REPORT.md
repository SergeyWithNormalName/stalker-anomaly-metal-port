# MO2 clone path preflight audit

## What the UI-only run established

The cloned MO2 process started from `runs/mo2-reference-startup-01/runtime/mo2`. Its private log identifies that same directory as both its data path and working directory, and identifies the cloned game root at `runs/mo2-reference-startup-01/runtime` as the STALKER Anomaly plugin's game path. The selected profile in the clone's `ModOrganizer.ini` is `MyTree`. This confirms private MO2 settings/log location and the selected profile; the process was closed without launching a game.

The clone has private empty `logs`, `downloads`, `crashDumps`, and `webcache` directories. The profile and overwrite folders are local copies. No separate path overrides for profiles, downloads, overwrite, or cache appear in the cloned INI; those directories therefore rely on MO2's normal layout below its confirmed private data path. This UI pass confirms the log/data root, but does not by itself verify every directory MO2 actually writes to.

## Correction: old-root warnings are not explained by clone entries

The cloned INI does contain unused custom executable entries that still point into the original Anomaly and MO2 roots. However, the original primary MO2 log contains 48 identical failed-directory warnings. The warnings also persisted after the clone's executable paths were retargeted. The unused-entry theory is therefore withdrawn: these messages are pre-existing behavior and do not explain the clone startup result. Do not spend further effort eliminating them or treat them as evidence that the cloned game path is wrong.

The established facts remain: the clone log reports a private data path and the scratch game-plugin root; the private profile and writable directories are present. The warning's internal caller is not identified, and the warning alone is not a reason to reject the private path setup.

## Current next gate

The subsequent stock DX11 launch used the private MO2 `run -e "Anomaly (DX11)"` route with the original EXE hash, but did not reach a confirmed menu/loaded state before its bounded watchdog stopped it. USVFS hooks did initialize. Keep game-load and save-profile experiments blocked until startup is understood. Compare the cloned Wine prefix/backend against Silo's saved selected ManualBottle and configured runtime before another retry; preserve primary write denies and hash checks.

## Evidence locations

- MO2 preflight log: `runs/mo2-reference-startup-01/runtime/mo2/logs/mo_interface.log`, lines 2–3 (private process/data paths), line 8 (cloned game-plugin root), lines 9–16 (old-root failed opens).
- Primary MO2 comparison log: `<PRIVATE_LOCAL_PATH>` contains the same warning family (48 entries); clone warnings remained after unused executable paths were retargeted.
- Cloned configuration: `runs/mo2-reference-startup-01/runtime/mo2/ModOrganizer.ini`; only key names and relevant paths were inspected, not account/token values.
- Cloned profile settings and game root file: `runs/mo2-reference-startup-01/runtime/mo2/profiles/MyTree/` and `runs/mo2-reference-startup-01/runtime/fsgame.ltx`.

No game was launched for this audit, and no primary configuration or installation file was changed.
