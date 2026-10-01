# Root-owned temporary build map

No files in `src/` were edited here. A root-owned build transaction can stage these scratch files at the listed temporary paths, stage `context-probe-target.patch` against the prestate CMake file, build `XRay.Render.R4.PortalGenerationContextProbe`, run it against the same frame-700 inputs used by `cpp-walker/transaction-draft/verify_inputs.py`, then restore exact prestate before any ordinary build.

- Reuse the `R4LevelGeometry` parser header/source and `R4PortalGraphObserver.h` from `cpp-walker/transaction-draft/stage/`; their hashes are already pinned there.
- `R4PortalGenerationContext.h/.cpp` -> `src/Layers/xrRenderMetal/`
- `R4PortalGraphPreparedObserver.cpp` -> `src/Layers/xrRenderMetal/`
- `tests/R4PortalGenerationContextProbe.cpp` -> `src/Layers/xrRenderMetal/tests/`
- `tests/Frame700Capture.h` -> `src/Layers/xrRenderMetal/tests/`

The probe's fallback CFORM is explicitly synthetic and labels sector 25 so the narrow source start-sector path is exercised without retaining any fixture pointer. The exact level/level.geom/ID/raw-flag hashes still must be admitted by the existing input-verification step. Probe exit 6 is malformed-portal rejection, exit 9 checks stale-generation visible-all behavior, exit 11 checks the synthetic CFORM fallback, and exit 16 rejects a cached replay that differs from the frozen visible/outside/mismatch counts. This is not an in-game observation or actual live CFORM proof.
