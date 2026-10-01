# Stock DX11 static SSA policy: bounded PDB and machine-code audit

## Finding

The saved PE/PDB pair is sufficient to recover the stock DX11 static insert's
hard SSA cutoff and the code that computes its threshold. It is **not** enough
to recover a separate `r_ssaDONTSORT` policy or the exact runtime numeric
threshold at a particular measured frame.

The audited stock PE is 25,280,000 bytes, SHA-256
`c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`. Its
PE CodeView GUID/age matches `AnomalyDX11.pdb`:
`b29860d9-ae5d-45d5-a5e0-16d672256fc0`, age `1` (PDB SHA-256
`84aedf63be05f4b9192157faf46d23977f3f648ffc33a8d99138fd7eb92cd2f0`). This is
the May 15, 2026 binary family identified in the existing [PDB/machine-code
compatibility report](../../../luna-harness/compatibility/2026-09-29-pdb-exists-machinecode.md).

## Proven addresses and rule

The matching PDB's public-symbol stream provides these exact addresses:

| Symbol | RVA | VA | Evidence |
|---|---:|---:|---|
| `?r_ssaDISCARD@@3MA` | `0x016D569C` | `0x1416D569C` | `.data`/BSS float written by `CRender::Calculate` |
| `?g_fSCREEN@@3MA` | `0x016D56A0` | `0x1416D56A0` | adjacent `.data`/BSS screen-normalization float |
| `?ps_r__ssaDISCARD@@3MA` | `0x01111A64` | `0x141111A64` | `.rdata` float `3.5` in this image |
| `?r_dsgraph_insert_static@R_dsgraph_structure@@QEAAXPEAVdxRender_Visual@@@Z` | `0x00C9A910` | `0x140C9A910` | static visual graph insertion |
| `?Calculate@CRender@@UEAAXXZ` | `0x00CEB240` | `0x140CEB240` | computes `g_fSCREEN` and SSA thresholds |
| `?ps_r__ssaDONTSORT@@3MA` | `0x01111A54` | `0x141111A54` | `.rdata` float `32.0`; no derived public `r_ssaDONTSORT` symbol found |

The full read-only disassembly of `r_dsgraph_insert_static` loads visual
center components from offsets `+0x40`, `+0x44`, `+0x48`, loads one radius
float from `+0x4C`, computes squared camera distance and adds the binary's
`1e-7` epsilon, then divides the radius by that distance. It compares this
SSA value with the global at RVA `0x016D569C`; the at-or-below branch goes to
the function epilogue without inserting the visual. The path uses **R /
distance²**, not R² / distance². There is no second radius multiplication in
the entry-to-compare block. The PDB names the procedure; the saved disassembly
shows the arithmetic and branch at VA `0x140C9A941`–`0x140C9A9AA`.

The radius interpretation is consistent with the checked-out source layout:
`dxRender_Visual::Load` copies `hdr.bs.r` to `vis.sphere.R`
([FBasicVisual.cpp:209-216](../../../../../src/Layers/xrRender/FBasicVisual.cpp:209)),
and `Fsphere::set` assigns that value directly without squaring
([_sphere.h:7-14](../../../../../src/xrCore/_sphere.h:7)). The stock machine
code establishes that it loads one float from visual offset `+0x4C`; the
matching May 15 binary's loader was not separately disassembled here. The raw
OGF-radius interpretation is supported by current source layout, not proven
from the stock loader's machine code.

`CRender::Calculate` stores the normalization at the PDB-named `g_fSCREEN`
slot and then stores `12.25 / g_fSCREEN` at `r_ssaDISCARD`. The `12.25` is an
image constant (`RVA 0x012E49C4`), equal to the square of the PDB-visible
`ps_r__ssaDISCARD` data value `3.5`. The preceding instructions construct the
screen factor from viewport width × height, `(90 / FOV)²`, and
`(1e-7 + ps_r__LOD)`; the matching PDB locates `ps_r__LOD` at RVA
`0x0154DFA0`. Consequently the code-backed stock policy is:

```text
g_fSCREEN = width * height * (90 / FOV)^2 * (1e-7 + ps_r__LOD)
r_ssaDISCARD = 12.25 / g_fSCREEN
static SSA = OGF_sphere_radius / (distance_squared + 1e-7)
drop when r_ssaDISCARD >= static SSA
```

This is a build-code rule and a PE-image constant; it does not assert what
`ps_r__LOD`, FOV, or `r_ssaDISCARD` held in a particular gameplay frame. The
PDB and disassembly confirm the formula without using the absence of a console
string to guess defaults.

## Limits and parity consequence

The stock function has a hard rejection branch. The decoded static-insert path
contains no `GetFvectorHash` call, no squared-survival `pow` call, and no fade
parameters. `CalcSSA` and `GetFvectorHash` have no separate public PDB symbols;
the static rule is recovered from the inline machine code above. The current
dirty source instead computes `R*R/distSQ` and adds an exponential fade in
`r_dsgraph_insert_static` (`src/Layers/xrRender/r__dsgraph_build.cpp:530-538,
888-946`). These are materially different policies, so generic source-SSA
parity with this stock DX11 binary is **not established**.

The PDB has a named `ps_r__ssaDONTSORT` data value (32.0), but the public-symbol
scan found no `r_ssaDONTSORT` global. The bounded `CRender::Calculate` span
does not store a separately named derived DONTSORT value, and this audit did
not trace a complete stock sort gate. The DONTSORT rule is therefore **no-go /
unproved**; do not infer it from the current source or from the variable name.

The earlier read-only console probe was scoped before actor startup and marked
gameplay acceptance false. It found the three new `r__ssa_discard*` console
controls unregistered on the stock-derived executable. The pinned DX11 image
also lacks those command-name strings, while its PDB exposes the fixed
SSA-related data above. No new run is needed to establish the hard static
cutoff.

If a later comparison needs the actual threshold for one measured frame, the
smallest next diagnostic is one read-only memory sample of `moduleBase +
0x016D569C` after `CRender::Calculate`, recorded with same-frame width, height,
FOV, and `ps_r__LOD`. That is a single diagnostic capture, not a performance
run. It would supply the numeric frame value while preserving this proven
code-level rule.

## Audit artifacts

- `scan_publics.py` uses the existing bounded standard-library MSF/PDB reader
  and verifies the PE/PDB GUID/age before resolving symbols.
- `public-symbols.json` records the relevant public symbols and addresses.
- `static_insert_disasm.txt` and `render_calculate_disasm.txt` are LLDB
  disassemblies of the PE only; the PDB was not loaded into LLDB.
- No game was launched, no executable was modified, and no main target was
  built.

## Later runtime numeric readback and CPU predicate screen

The short condition-only `runs/saved-slowclock-silo-01` ran the existing private startup-guard image `1a3ff9ac...`, preserving stock SSA code/data. Read-only LuaJIT FFI samples checked the main module's PE signature/image extent and read the four pinned RVAs after Calculate, at weather commit, measure start/end. All three reported `r_ssaDISCARD=4.20095967e-6`, `g_fSCREEN=2916000.25`, `ps_r__LOD=1`, and `ps_r__ssaDISCARD=3.5`. No global was written. This confirms the numeric cutoff on that diagnostic frame; it is not an unchanged-stock gameplay/FPS acceptance.

A standalone CPU screen applied the recovered numeric predicate to the exact 5,316 native frame-700 OGF source spheres/eye. It rejects **261**, all contained in the **1,199** rejected by the current generic source rule. Minimum relative distance from stock numeric threshold was 0.0244%, so this set is not a threshold-rounding observation. These are same-ID numeric rule decisions, **not actual Windows draw counts**: portal/HOM/material/LOD paths were not replayed and the stock loader's raw-radius semantics remain source-supported rather than disassembled. Thus a stronger stock SSA cutoff does not explain this native stream's excess; visibility/shader-path gaps need separate investigation. Artifacts: `frame-id-policies.csv`, `numeric-policy-result.json`, `compare_frame_ids.cpp`, and the run's `phases.csv.stock-ssa.csv`.
