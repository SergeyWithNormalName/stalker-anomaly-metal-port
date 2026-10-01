# PE-only startup entity pointer evidence

## Finding

The stock PE disassembly gives a useful and fairly narrow correlation: in `CGameObject::net_Spawn` the incoming `CSE_Abstract*` arrives in RDX and is copied to nonvolatile R13 at RVA `0x1f9750`. At RVA `0x1f991a`, the method calls an indirect function with `this` in RCX and the entity pointer in RDX; this matches the source `visual_name(E)` call. The returned RAX is then passed in RDX to another indirect renderer call at RVA `0x1f9938`, matching the source `Render->models_Exists(visualName)` ordering. R13 is still the original entity pointer at that second call site; it is not reused until RVA `0x1f9b0e`.

Therefore an empty-name breakpoint inside `CModelPool::Exists` can, in principle, be correlated to the entity: switch to the `CGameObject::net_Spawn` caller frame using debugger unwind information and inspect that frame's R13. This is a machine-code pointer-liveness result, not a layout guess. Verify the stopped stack actually has that `net_Spawn` frame and that the caller callsite is the renderer vtable call before attributing R13.

## Limits on entity name and ID

This bounded scan did **not** recover a trustworthy numeric entity-name or ID offset. The public `get_name(const CSE_Abstract*)` helper at VA `0x14060e0c0` is only a virtual dispatch (`mov (%rcx),%rax; jmp *0x28(%rax)`), so it does not expose a universal inline member load. The abstract `CSE_Abstract::visual()` public at VA `0x1400481c0` returns null; concrete visual behavior is type-specific. The relevant PDB public scan found no emitted `get_visual` or field-accessor public that supplies a universal offset. The PDB type stream was not decoded sufficiently to establish complete concrete layouts. Source `CSE_Visual::get_visual()` and `E->ID` therefore cannot safely be translated into guessed offsets in this older PE.

At the empty `Exists` stop, record its RDX string pointer and stack. In the unwound `net_Spawn` frame, record R13 as the entity pointer, then use the matching PDB's type-aware display/getter evaluation for the concrete entity to read ID and name. If typed fields/getters are unavailable in the chosen debugger, the entity pointer correlation remains possible but the exact ID/name readout is blocked; do not use a hard-coded offset. The stock function addresses and image/PDB identity are in the parent `PLAN.md`.

## Reproduction basis

Offline LLDB disassembly only; no PDB was loaded, and no process was launched. The inspected PE range `0x1401f9730..0x1401f9b90` came from the guarded clone, but `SECTION-PROBE.md` records a byte-for-byte comparison of this range against the exact stock PE. `0x1401f9b90` is not the function boundary; the next public function starts at RVA `0x1fab90`.
