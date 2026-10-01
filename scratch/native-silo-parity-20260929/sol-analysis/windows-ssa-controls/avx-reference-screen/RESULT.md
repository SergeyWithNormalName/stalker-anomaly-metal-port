# Installed DX11 AVX reference screen

## Finding

The expected binary is installed under the basename `AnomalyDX11AVX.exe` (without the underscore in the supplied path). Its SHA-256 matches the requested digest exactly: `ade57887f93ca95e9398da60b374035b50e4c38295b9802a6f89bc33a0394bc6`. The PE is a 23,154,176-byte x86-64 Windows GUI executable with PE timestamp `2026-05-15 05:27:07 UTC`, RSDS GUID `8afc0b35-370c-4e4d-ba43-7ee1edf3a3f2`, age `1`, and embedded PDB path `.../AnomalyDX11AVX.pdb`.

No matching AVX PDB was found within the installed `<PRIVATE_LOCAL_PATH>` tree. The only nearby `AnomalyDX11` PDB variants fail the PE/PDB GUID+age check: the main `AnomalyDX11.pdb` is for GUID `b29860d9-ae5d-45d5-a5e0-16d672256fc0/1`; `.mt-2026.5.15` and `.backup` also mismatch. I ran the existing bounded `read_pdb_pub32.py` on the AVX PE with each local PDB; it rejected each mismatch before symbol extraction. I did not load any PDB into LLDB or disassemble guessed AVX addresses.

As a result, this screen cannot determine whether the AVX executable rejects an empty name in `CModelPool::Exists` before it reaches `Create`/`FileDownload`. The stock non-AVX DX11 audit proves that unsafe path only for SHA `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`; it must not be transferred to this distinct AVX image by matching date, product name, or nearby file placement.

## Startup-check value

The AVX executable is an installed candidate at the requested path and hash; the digest match alone does not prove canonical release provenance or empty-name behavior. One isolated, startup-only check is worthwhile if the immediate goal is to find a working Windows reference: run this exact EXE from a disposable clone with the same effective profile and intended level bootstrap, record its SHA/argv/log/exit state, and stop before benchmarking. If it reaches the intended loaded level without the empty-file crash or a new-game fallback, it is a usable candidate for later control work. If it fails, this screen cannot attribute the failure to `CModelPool::Exists`; obtaining its matching PDB or capturing a debugger stack/input would be the next proof.

## What the saved Silo launch receipts establish

The relevant Silo `argv.txt` files and `run_newgame_silo.sh` all launch cloned `.../runtime/bin/AnomalyDX11.exe` (the non-AVX build). They do not record which executable the user's ordinary Mod Organizer/launcher normally selects, so they do not establish that the AVX binary is the normal installation launch target. The installed directory contains both binaries. No unrelated launcher configuration was read or included.

## Scope

Read-only screen only: no game, build, install, or file modification. The only output is this report. No AVX function behavior is claimed without a matching PDB or direct disassembly evidence.


## Actual isolated startup result

`windows-avx-startup-01/silo` ran the exact unchanged AVX EXE. It failed before the actor/profile callback with `FileDownload: cannot open file` and an empty filename; the owned timeout stopped the fatal-dialog process (exit143). No phase CSV or FPS window exists. Therefore this AVX binary also does not supply a working unmodified reference for this cloned runtime. Exact failing entity is still unknown. No private guard was applied; originals remain unchanged. Receipt: `startup-result.json`.
