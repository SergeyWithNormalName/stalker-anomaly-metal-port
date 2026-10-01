# Stock DX11 empty-visual startup probe

## Purpose and evidence

The saved `FileDownload` failure has an empty filename and reaches the stock DX11 model-pool path, but its logs do not identify the spawning entity. The matching stock image is `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`; its matching PDB is GUID `b29860d9-ae5d-45d5-a5e0-16d672256fc0`, age 1. Verified public RVAs are `CGameObject::net_Spawn=0x001f9730`, `CModelPool::Exists=0x00cd5e60`, and `CModelPool::Create=0x00cd4fb0` (see `luna-harness/compatibility/2026-09-29-pdb-exists-machinecode.md`).

Current checked-out source gives the causal fields: `net_Spawn(CSE_Abstract* DC)` treats `DC` as `E`, casts it to `CSE_Visual`, gets `visual_name(E)` from `visual->get_visual()`, and calls `models_Exists(visualName)` before its configured fallback (`src/xrGame/GameObject.cpp:265-288,1179-1184`). On Win64 method entry, the entity pointer argument is in RDX. Source is explanatory only; it does not establish that the dirty source layout matches this May 15 binary.

## Smallest decisive capture

Use a single unchanged-stock startup with a debugger and this exact EXE/PDB pair. At `CGameObject::net_Spawn` entry, record RDX and use the matching PDB's type-aware display for the supplied `CSE_Abstract*` (including its entity ID and section/name fields); do not manually dereference guessed offsets. At `CModelPool::Exists`, stop only when RDX is null or points to an empty C string. Record RDX, stack, and the `net_Spawn` caller frame's PDB locals/arguments. This directly ties the exact empty model argument to the entity that supplied it. Continue only far enough to confirm whether that call reaches `CModelPool::Create`; do not add a guard or alter the executable.

The exact stock `net_Spawn` call path, section key load, PE byte-range equality check, and the bounded WineDbg interactive command recipe are now in [`code-offset-evidence/SECTION-PROBE.md`](code-offset-evidence/SECTION-PROBE.md). The recipe uses only numeric breakpoints and WineDbg's documented GDB-like `break`, `cont`, `bt`, `frame`, `print`, and `x` commands. It correlates the empty visual pointer with R13 in the caller frame and reads the fallback section key through the stock callsite's `this+0xec` load. It still cannot safely supply the CSE entity's ID or server-side name without type-aware concrete-CSE inspection.

## Current tooling blocker

The Silo Wine runtime contains `lib/wine/x86_64-windows/winedbg.exe`; its `--help` and command help were captured on an owned unused prefix, without starting the game. `llvm-pdbutil` and `cvdump` are not installed, and LLDB's PDB path is already known to crash on this PDB. The bounded local CodeView type scan did not establish trustworthy complete `CSE_Abstract` / `CSE_Visual` member offsets, so this probe still does not give numeric CSE ID/name offsets. WineDbg's actual breakpoint expressions against a live stopped game have not been validated.

Before running the one startup, verify that the selected debugger can load the exact PDB and evaluate the typed `net_Spawn` parameter and caller frame. If the Silo `winedbg` frontend cannot do that, use an available native WinDbg instance with this exact EXE/PDB pair; otherwise the entity-identification probe is blocked until a compatible PDB type reader/debugger is available. Do not infer the offending entity from the last successful spawn log.
