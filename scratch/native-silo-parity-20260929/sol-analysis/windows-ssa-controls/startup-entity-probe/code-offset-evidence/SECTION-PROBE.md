# Stock startup section-name readout

## Exact stock-code check

The stock executable at `runs/windows-console-probe-01/silo/runtime/bin/AnomalyDX11.exe` hashes to `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`. The guarded clone used for earlier offline disassembly hashes to `1a3ff9acbab231da7348ad61969d6e910d229233ae982b48a8e4e5a23b271c20`. Their PE-mapped bytes for RVA `0x1f9730..0x1f9b90` are identical (1,120 bytes; SHA-256 `09468452dbf2817d73f2b9d28757a45ce4919cbca05a8c8c6675b0a36f9fbb62`). This validates the earlier prologue/callsite findings for the exact stock image in that bounded span. `0x1f9b90` is only the end of the inspected span; it is **not** the function boundary. The PDB lists the next public function at RVA `0x1fab90`.

## Section pointer evidence

At RVA `0x1f9938`, `net_Spawn` calls through the renderer table with `RDX` holding the string returned by the prior `visual_name(E)` call. The result is tested at `0x1f993b`; on the missing-model path, stock instructions at `0x1f9a5f` load `RDI = *(this + 0xec)`, then at `0x1f9a79` pass `RDI + 0x14` as the first string argument to the call at `0x1f9a92`. The matching stock PDB public at VA `0x1400d5d40` names that callee `CInifile::line_exist(const char*, const char*)`. This aligns with the source sequence `pSettings->line_exist(cNameSect(), "visual")` immediately after `models_Exists` (`src/xrGame/GameObject.cpp:281-283`); `CObject::cNameSect()` returns `NameSection` (`src/xrEngine/xr_object.h:259`).

This is a concrete stock-code location for the current `CObject` section-name shared-string handle: at the query's not-found branch, the `CGameObject` pointer is in RSI and its section-name handle is loaded from `this+0xec`; its string data is read from the shared-string value at `handle+0x14`. The latter relationship is evidenced by the exact pointer passed to the named `line_exist` function. Inspect both values live before interpreting them; do not dereference a null handle. Because the source assigns `cNameSect_set(E->s_name)` later in `net_Spawn` (around source line 299), the value observed before that assignment may come from earlier object-pool initialization. It is still the exact section key the failing fallback lookup uses, but do not automatically equate it with the incoming server entity's name.

## WineDbg one-startup interaction

The saved Silo WineDbg help documents launching a process under the debugger and the commands `break *<addr>`, `cont`, `bt`, `frame`, `print`, and `x`; it documents GDB-style repeat/format modifiers for `x`. A prior offline `--help` call was completed on an owned unused prefix; no game was launched in this review.

Use the exact stock executable, its matching PDB kept beside it, and the existing prepared runtime's normal arguments/environment, but invoke the executable through WineDbg for this one startup. For example, with the existing Silo paths supplied by the harness:

```text
wine64 <Silo>/lib/wine/x86_64-windows/winedbg.exe <exact-stock-AnomalyDX11.exe> <same startup arguments>
```

At the WineDbg prompt, install the numeric breakpoints before continuing:

```text
break *0x1401f9730
break *0x140cd5e60
cont
```

At each `CModelPool::Exists` stop, inspect `print $rdx`, then `x/s $rdx` when non-null. Continue past nonempty names. For an empty name, use `bt`, select the caller frame with `frame 1`, and inspect `print $rip`; require the return site to be `0x1401f993b` (the stock `net_Spawn` renderer query call) before attributing the saved registers. In that caller frame, `print $r13` is the incoming CSE entity pointer preserved through that call, and `print $rsi` is the CGameObject pointer. Read the section value with:

```text
x/1gx $rsi+0xec
```

If the displayed handle is non-null, add `0x14` to that displayed address and use `x/s <resulting-address>` to inspect the section string. WineDbg's help confirms the `x` command, repeat/format modifiers, and register expressions, but these exact expressions have not been exercised against a stopped game. If WineDbg rejects one, use its `print`/`x` prompts interactively and do not substitute an unverified offset. This recipe can identify the exact empty visual argument, spawned entity pointer, and section key used by fallback lookup. The entity's numeric ID and server-side `s_name` remain unresolved without type-aware access to the concrete CSE object; the section key is not a proven substitute for them.
