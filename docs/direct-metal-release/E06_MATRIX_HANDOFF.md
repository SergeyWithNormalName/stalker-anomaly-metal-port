# E06 matrix handoff — 2026-09-27

- Created `PARITY_MATRIX.md` from the existing material and weather/effects census; no full discovery repeated.
- Current launcher materializes `runtime/macos-gamma/profile`; current `appdata/user.ltx` has volumetric sunshafts, DOF and volumetric lights enabled; fullscreen heatvision/nightvision disabled; SSFX Bloom controls active. Active `SSFX_BLOOM` combine_2 samples `s_ssfx_bloom`; Metal supplies the black dummy there. Legacy bloom filter work is bypassed after luminance build under `o.ssfx_bloom`, so it is not the active-profile gap.
- `r2_sunshafts_mode volumetric` is not evidence for Windows screen-space `phase_sunshafts`; keep those paths distinct.
- Census `ACTIVE_VERIFIED` is historical exercise evidence only. This handoff adds no runtime PASS, reference match, or human acceptance. All parity gaps remain OPEN.
- Priority findings: active SSFX Bloom input and emissive remain central full-screen gaps; enabled volumetric sun lighting, volumetric light volumes and DOF are profile gaps. Water remains delegated separately; matrix records only census state.
- No build, cook, game launch, or capture performed. Allowed edits were limited to `PARITY_MATRIX.md` and this handoff.

## Integrator follow-up

The historical black-dummy/prepared-only findings above are superseded only where current E06_EXECUTION/PARITY_MATRIX provide explicit code/runtime evidence: active bloom chain, selflightl subset, 256² luminance source, corrected AO/IL blur footprint, and rain/lens draws. Complete coverage/reference/human gates remain open.
