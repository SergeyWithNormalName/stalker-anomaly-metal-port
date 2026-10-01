# Bounded Windows dump check

The earlier “no dump” conclusion was incorrect: ordinary tracked-file enumeration skipped an ignored minidump. A `find` scan over only `scratch/windows-metal-gap-20260929` and `scratch/native-silo-parity-20260929/runs` found one file:

`runs/windows-console-probe-01/silo/runtime/appdata/logs/xray_player_09-30-26_16-27-00.mdmp`

SHA-256: `dee14198690f3a5e95de1a65a1a3fbacd9b262b11ef1d6b020cd97695255f47f` (694,930 bytes). Its matching `xray_crossover.log` records the `help` command stack through `CCC_Help::Execute`, `CConsole::ExecuteCommand`, and Lua `parity_harness_register` / `on_game_start`; the exception address is `0x14008e3cf` (image RVA `0x8e3cf`). A minimal read-only MiniDump header/directory read finds eight streams. The exception stream reports access violation `0xc0000005`, read access, fault address `0x6f440000`; the retained MemoryList has 60 ranges and does not cover that address. This dump is the console-probe Help crash, not the `FileDownload` / `CModelPool::Create` startup crash. It cannot identify that failing entity or visual.

No matching `FileDownload`/`CModelPool` dump was found in those bounded roots. The specific entity name, ID, and visual therefore remain absent from saved evidence. The smallest decisive diagnostic remains a debugger capture at `CGameObject::net_Spawn` entry during one unchanged Windows startup, recording its entity argument's name, ID, and visual, then confirming whether an empty visual reaches `CModelPool::Create`. Do not infer the failing entity from the last successful spawn line.

The SSA radius discussion in `RESULT.md` is now explicitly qualified: the May 15 machine code proves a scalar load at visual `+0x4C` and its arithmetic use, while only the checked-out source maps that field to the raw OGF sphere radius. No stock loader machine-code proof was made.
