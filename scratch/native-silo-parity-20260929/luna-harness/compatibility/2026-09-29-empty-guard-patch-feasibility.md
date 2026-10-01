# Isolated Windows empty-visual guard feasibility (no patch made)

## Assessment

A small entry detour is technically expressible for a private, scratch-cloned Windows compatibility executable. It rejects null/empty `N` before the old function acquires `PoolLock`, returns false with the original entry stack untouched, and resumes the original path for every nonempty name. This would be a **custom compatibility baseline**, not stock Silo/Windows parity and not FPS evidence. The pinned original executable and official runtime remain untouched; nothing has been patched or launched.

## Bounded facts

- PE/PDB-matched `CModelPool::Exists` is at VA `0x140cd5e60`; its first instruction is exactly five bytes, `48 89 5c 24 10` (`mov [rsp+0x10], rbx`).
- Its `.pdata` function range is RVA `[0x00cd5e60, 0x00cd61ef)` with unwind info at RVA `0x0150ada0`; prologue size is 31 bytes. The displaced first instruction only spills RBX to stack; it does not change RSP or RBX.
- Candidate executable padding is RVA `0x00416f55` through `0x00416f6f`: 27 consecutive `CC` bytes in `.text` (`Characteristics=0x60000020`). No `.pdata` range overlaps the cave, and the PDB `S_PUB32` scan found no public symbol start in it. The PE has no base-relocation directory, so neither patch range overlaps a relocation target.
- RVA-to-file mapping from the PE section table resolves the patched function-entry range at file offset `0x00cd5260` and the candidate cave at `0x00416355`; both ranges fit inside `.text` raw data. These details are rechecked by the draft writer before it would write.
- Entry-to-cave and cave-to-resume jumps use computed rel32 values. The resume target is original entry + 5, not the original entry.

The 23-byte candidate stub fits in the 27-byte cave, leaving four `CC` bytes:

```text
Entry replacement at RVA 0x00cd5e60:
E9 F0 10 74 FF                 ; rel32 jump to RVA 0x00416f55

Candidate cave at RVA 0x00416f55:
48 85 D2                       ; test rdx,rdx
74 0F                          ; null -> false return
80 3A 00                       ; cmp byte ptr [rdx],0
74 0A                          ; empty -> false return
48 89 5C 24 10                 ; exact displaced instruction on nonempty path
E9 FC EE 8B 00                 ; rel32 jump to original entry + 5 (RVA 0x00cd5e65)
31 C0 C3                       ; xor eax,eax; ret
```

The nonempty path runs the exact overwritten RBX spill, then resumes at original byte +5; all remaining function code and the unwind range stay at their original addresses. Neither test nor compare changes nonvolatile registers or RSP. They modify arithmetic status flags, which Microsoft documents as volatile across x64 calls; the direction flag remains unchanged. The false path returns `false` without creating a frame. It skips `PoolLock` acquisition, unlike the current source guard which checks under the lock; this path reads no shared pool state, but the internal lock side effect differs.

## Remaining limit

The candidate cave is an unused-looking `CC` run, outside every `.pdata` runtime-function entry, with no public symbol start or base relocation. A byte-level cross-reference proof that no obscure fixed-address reference points into it was not completed. It is plausible linker padding because code entering it would currently execute `int3`, but do not apply the detour without root review of that cave/reference limit. Any future use must operate on a new owned copy, keep the original SHA-pinned EXE untouched, and label the resulting control as a custom patched Windows compatibility reference.

The scratch-only writer draft is [draft_apply_empty_guard.py](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/luna-harness/compatibility/draft_apply_empty_guard.py). Root ran its dry-plan mode against the pinned PE and verified exactly two ranges / 28 changed bytes: file offsets `[0x00cd5260,0x00cd5265)` and `[0x00416355,0x0041636c)`. The planned patched SHA256 is `1a3ff9acbab231da7348ad61969d6e910d229233ae982b48a8e4e5a23b271c20`; no output executable or receipt file was written. The draft requires explicit `--write-owned-copy`, pins the original executable SHA, validates entry/cave bytes, section bounds, unwind and relocation checks, verifies exactly two modified ranges, writes only to a new destination path, and emits a patched-SHA receipt. No game launch or install/mod changes were made.

Format sources: [LLVM MSF file format](https://llvm.org/docs/PDB/MsfFile.html), [LLVM PDB/DBI stream](https://llvm.org/docs/PDB/DbiStream.html), [LLVM PDB GUID and age matching](https://llvm.org/docs/PDB/PdbStream.html), [LLVM CodeView S_PUB32](https://llvm.org/docs/PDB/CodeViewSymbols.html), and [Microsoft x64 ABI conventions](https://learn.microsoft.com/en-us/cpp/build/x64-software-conventions).
