# ChangeTeam gameplay crash: confirmed numeric boundary defect

Crash artifacts: `runs/defvertex-bvh-01/output/crash.ips` and `runs/defvertex-bvh-02/output/crash.ips`. Both run candidate SHA256 `bfa70a1e4e8670431ea644400362f655a15a8157b7f426fce643e62934f3bd34`; both crash during warmup, so neither is a valid FPS sample. Old candidate remains preserved. Linear same-binary canonical run completed; that does not prove a renderer change caused or did not cause a crash.

## Exact fault and causal path

The fault is not null. BVH01 `CEntity::ChangeTeam +584` is `ldr x9, [x0, #0x10]`. `x0=0x1003fa38b0cb6317`, returned immediately by `CTeamHierarchyHolder::squad`; the requested squad index `x1/x21=6664 (0x1a08)`. BVH02 reaches the next group-table read at +592 with `x1=6664`, `x9=0xaa0203f39100c3fd`, group offset `x21=8`. Its pointer resembles code bytes because an out-of-range squad lookup read arbitrary nearby memory. This is not proof that a valid container entry was overwritten by the renderer.

`team_hierarchy_holder.h` defines a fixed 256-element `svector`. Source `team_hierarchy_holder.cpp:20` has only a `VERIFY3(squad_id < max_squad_count)` bound check. The release binary has no check: `CTeamHierarchyHolder::squad +32` reads `[team+0x10 + w1*8]` directly. Index 6664 is therefore an out-of-bounds read, sometimes followed by an out-of-bounds write if its accidental result is zero.

Actual ARM64 Lua typed callback at file address `0x1008117e8`: argument 3 comes from `lua_tonumber`, followed by `fcvtzs w23,d0`, then `mov x2,x23` and the byte-typed call. No `uxtb`/mask occurs. `CScriptGameObject::ChangeTeam(unsigned char,unsigned char,unsigned char)` also forwards the full register when promoting to `int`; the compiler assumes its byte argument is already canonical. The existing luabind primitive converter directly casts `double` to byte/word integers. A cast whose truncated value is unrepresentable is undefined behavior, allowing this lowering.

Both crash register states carry the same exact 6664 across that call chain. The numeric boundary explains the observed invalid hierarchy read without requiring a speculative null check, scheduler lock, or renderer buffer corruption hypothesis.

## Verified installed Windows compatibility

Installed `<PRIVATE_LOCAL_PATH>` and its PDB identify `CScriptGameObject::ChangeTeam` at VA `0x140a89a70`. Actual prologue:

```asm
movzbl %r9b, %esi   ; group
movzbl %r8b, %ebp   ; squad
movzbl %dl, %r14d   ; team
```

It forwards those zero-extended values to the entity method. Thus raw squad 6664 becomes low-byte 8 in the installed Windows method. This proves the byte-boundary behavior for this case; it does not prove generic Windows word conversion or NaN/inf policy. `windows-team-symbols.txt` records PDB-derived addresses.

## Script and scheduling provenance limits

The effective loose GAMMA `sim_board.script:289/294` passes server team and either smart `squad_id` or server squad to global `change_team_squad_group`. Base `_g.script` is archive-supplied; the known installed "Log spam remover" version of that helper forwards those arguments to `cl_obj:change_team`, but that loose mod is not claimed as the selected runtime winner. The exact archived caller that supplied 6664 is not yet recovered. `xr_effects.change_tsg` is commented out and is not claimed as the cause. Crash state plus actual callback instructions establishes the problematic numeric input and ABI even without a Lua-level script stack.

Current engine launches GameThread after setting `Device.isRendering=true`; `mt_scheduler=1`, `mt_schedulerRT=0` runs deferred scheduler/Lua object updates on that worker while main encodes Direct Metal. `secondary_tasks.wait()` follows `End`. The IPS shows main in DDS copy/Prewarm at the crash. This concurrency can alter when script updates execute, but there is no evidence that a hierarchy lock or scheduling rewrite is needed to fix these two faults. Other startup Lua errors (e.g. `faction_stocks` nil) are separate observed events, not substituted explanations for the squad lookup.

## Authorized bounded fix and checks

Only five luabind Lua-to-C++ primitive converters changed: `char`, `signed char`, `unsigned char`, `short`, `unsigned short`. New helper truncates toward zero, uses a representable fast path, wraps finite out-of-range values modulo 2^N, and explicitly adjusts signed results into their representable range. NaN/inf return zero as a documented benign policy, with no claimed Windows proof. Nil/matcher/API behavior and all 32/64-bit conversions remain unchanged. Team storage, ChangeTeam signatures, scheduler and mod scripts are untouched.

Persistent actual-policy/LuaJIT probe: `src/3rd party/luabind/tests/NarrowNumericProbe.cpp`; runner `bash script/test_luabind_narrow_numeric.sh <output-directory>`. It checks 17 inputs across five types (including 6664, -1, 256, positive/negative fractions, boundaries, huge finite doubles, NaN/inf), nil, matching, wide conversions, and a noinline typed byte callback through optimized ARM64 ABI. Callback receives `17/8/1` for Lua `17/6664/257`.

All three configurations passed: ARM64 `-O2 -DNDEBUG`, `-funsigned-char`, and UBSan plus `float-cast-overflow` with fail-stop. The SAME test compiled against preserved prestate policy fails specifically on 6664 (`narrow-regression-control/legacy-probe`, exit1). `git diff --check` and shell syntax check pass. Full app V2 build and gameplay qualification are central runtime work; no game was launched by this worker.

Prestate policy SHA256 `41c40988842c1578657336ed6462aff1f2985720c619f79c8f2582902c9dba63`; candidate policy `d09c34c14f7edeced78f584ea742487137c5647a42698a973068175c743312a4`. New helper `409748c70e425a8e772dc5194d352cd35f830fd55b84a9f7adc2896f7960f991`; probe `515b22afda64f9d1c67fa51d8291437b830b8c3caabc21338925f8256960ab4d`; runner `d6baf28323ab03484f91041af66b6ff018f5a2c967dcbb634f03885014280847`. Scoped patch: `luabind-narrow-fix.diff`.

## Durable raw evidence and reproduction

All artifact paths below are relative to this report's directory. `change-team-arm64-disassembly.txt` contains offline LLDB disassembly directly from the preserved bfa candidate: actual Lua callback, `CEntity::ChangeTeam`, and unchecked `CTeamHierarchyHolder::squad`. File addresses are unslid Mach-O addresses; the two IPS image bases differ because of ASLR. The entity begins at `0x100a37cd8`, so +584/+592 are `0x100a37f20`/`0x100a37f28`. `CTeamHierarchyHolder::squad` begins at `0x1009df414`; its +32 load is `0x1009df434`. The typed callback forwards `x2=x23` at `0x100811908`, without narrowing after `fcvtzs w23,d0`.

`change-team-windows-disassembly.txt` contains offline LLDB disassembly from the installed Windows PE at the PDB-derived byte method address. Neither executable was launched for these reads. ARM64 LLDB warns that the current build's renderer archive debug objects cannot be loaded while the central V2 rebuild replaces them; the preserved executable's direct machine instructions above remain available and match the IPS offsets.

Raw disassembly SHA256: ARM64 `e2b92c84c1a0d9186351c0ac6d1e307d73c38db48e9f1dba6d90f11c700ea55e`; Windows `d4fba3964ee70eb77c019e81b0c5c85ef1bf17d84ac5e3ddbcbf8739b3cbc609`. Central repeated native converter checks are recorded in `qualification/lua-narrow-v2.log` (all three PASS). The regression control binary under `narrow-regression-control/legacy-probe` was built from the same persistent probe with the prestate policy include overlay and fails on 6664. The durable runner command is:

```sh
bash script/test_luabind_narrow_numeric.sh scratch/gameplay-rescue-20260929/narrow-tests
```

Acceptance remains pending the central V2 build and the same canonical gameplay stability screen. These CPU/ABI checks prove defined conversion and the diagnosed boundary repair; they do not prove absence of all gameplay crashes.
