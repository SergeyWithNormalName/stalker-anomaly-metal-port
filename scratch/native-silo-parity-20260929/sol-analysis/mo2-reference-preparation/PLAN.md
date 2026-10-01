# Isolated MO2 Windows reference plan

## Goal and current evidence

The valid Windows comparison route is the user's normal MO2 launch, selecting `Anomaly (DX11)` and pressing Run. Earlier direct launches from a flattened Silo game directory failed before the actor spawned, so they do not establish that the user's normal setup is broken.

The installed MO2 is 2.5.2 with USVFS 0.5.6.1. Its root is `<PRIVATE_LOCAL_PATH>`; the game root is `<PRIVATE_LOCAL_PATH>`. The selected profile is `MyTree`. Its `modlist.txt` SHA-256 is `bd97e39ac5c058725fc83ab6a8459c39a2403524f99d05c939da44a17580c1c7`, with 610 active entries, matching the saved runtime profile manifest. The configured DX11 executable is `Z:<PRIVATE_LOCAL_PATH>`, arguments `-dbg`, working directory `<PRIVATE_LOCAL_PATH>`. These values were read from the installed configuration; only those relevant fields were inspected.

## Clone layout to prepare later

Use a new scratch-only MO2 root and a separate Wine prefix. The game base must be a COW clone of the vanilla Anomaly installation, not the already flattened native-renderer tree. Keep the original installed MO2 and game tree untouched.

The later clone should contain a private copy of the MO2 executable/support files and writable configuration, the complete small `profiles/MyTree` directory, and a private `overwrite` copy. Give it private `logs`, crash-dump output, and an empty private `downloads` directory. Do not preserve the installed `downloads` symlink to the shared cache. Do not copy the large mod payloads: expose the original `mods` directory read-only to the clone, and verify that the MO2 log resolves that path as intended. No mod payload or profile files should be writable through the reference setup.

Edit only the cloned `ModOrganizer.ini`: point `gamePath` to the COW game base; update the configured DX11 executable's binary and working directory to that same clone; retain `-dbg`; retain the `MyTree` profile. Preserve the exact selected profile manifest and hash. Do not copy or reuse primary instance logs/config paths.

The installed root has no `portable.txt` in the bounded root check, while MO2's log reports its data path as the installed root. Therefore do not assume how MO2 will classify a copied root or which global instance it will select. First start only the cloned MO2 UI far enough to inspect its new log and instance identity, with the game executable not launched. If its data path, profile, or writable directories point outside the scratch clone, stop and correct the clone before any game launch. The isolated run is not ready until this preflight passes.

## Launch invocation after preflight

The installed MO2 CLI exposes global `-p/--profile` and the `run` command with `-e/--executable NAME`. The local binary strings and upstream command-line implementation support selecting a configured executable by its configured name. Once the clone's isolated instance is confirmed, the intended command is:

```text
<scratch-MO2-root>/ModOrganizer.exe -p MyTree run -e "Anomaly (DX11)"
```

Add `-i/--instance` only if the cloned setup's own metadata identifies a required named instance; do not invent an instance name. Do not supply a replacement argument or working directory on the CLI, so the cloned configured values remain the launch source. The command-line behavior is documented in the [MO2 command-line implementation](https://github.com/ModOrganizer2/modorganizer/blob/master/src/commandline.cpp); the local installed binary is the authority for this specific installation.

## Run record and stop conditions

Before the first game launch, record hashes for the cloned executable, cloned game executable, cloned `fsgame.ltx`, selected profile manifest, and source modlist. Record the private Wine prefix and exact command line. After the run, retain only the relevant MO2/USVFS logs and game log/crash output in the scratch run folder.

Accept a startup observation only if the MO2 log identifies the clone's private data path, `MyTree`, and the configured DX11 executable; the USVFS log shows the expected read-only mod roots and clone game base; and the launched process hash matches the recorded game executable. Compare whether the same empty-visual `lights_hanging_lamp` query occurs before interpreting any startup result. If MO2 resolves primary config/log/cache paths, if the wrong profile/executable is selected, or if hashes drift, stop and discard that run as invalid.

## Empty visual lookup: bounded source hypothesis

In current source, `CModelPool::Instance_Load` in `src/Layers/xrRender/ModelPool.cpp` first checks `FS.exist(N)`. If false, it searches for the name with `.ogf` in the level and game-mesh roots and returns null when those candidates are absent. If `FS.exist("")` returns a file descriptor, the code can preserve an empty filename and reach `FS.r_open("")`, which is consistent with the observed blank-name `FileDownload`. Ordinary filesystem enumeration registers actual entry names, not an empty name, so the source path does not establish that `FS.exist("")` should be true. MO2/USVFS changes the visible filesystem and must be tested through the actual route; no root-cause claim is made from the flat launch.

The smallest discriminating follow-up is a debugger-only observation in the actual MO2 launch: at the `CLocatorAPI::exist` call made by `CModelPool::Instance_Load` for the empty name, record the exact input pointer/string and the function's returned descriptor (or null) before continuing. This distinguishes an empty-name cache hit from a later path. Do not patch the executable or infer the result from the final `FileDownload` text alone.

## Scope status

This is a preparation plan only. It has not copied or modified the installed MO2/game/profile, started MO2, launched a game, or tested USVFS clone behavior. The clone's data-path preflight and any startup observation remain outstanding.
