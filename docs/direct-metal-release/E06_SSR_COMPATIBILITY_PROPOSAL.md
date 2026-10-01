# E06 general SSR: DXC annotation compatibility proposal

**Historical proposal, executed for active quality 0 in packet 5.** Plan §10 says “Использовать исходный HLSL через DXC→MSC, reflection и receipts, без замены «похожим» MSL.” Active original HLSL remains unchanged in the profile and compiler input tree.

DXC1.9 rejects `screenspace_reflections.h:57`:

```hlsl
[unroll (q_ssr_steps[SSFX_SSR_QUALITY].x)]
```

For the active `SSFX_SSR_QUALITY=0`, the original table has `q_ssr_steps[0].x=8`. The proposed compatibility copy changes only the annotation to:

```hlsl
[unroll (8)]
```

The original loop condition, ray initialization, maximum-hit table, texture samples and all pixel calculations remain byte-for-byte unchanged. This proposal is limited to quality0; other quality variants require their own explicit table value/receipt. It does not imply Windows pixel equivalence, history correctness or a Metal graph implementation.

- Original cook: [log](../../scratch/release_runs/20260927-e06-start/cook-general-ssr.log), failed at the trace PS after VS/gloss.
- HV2021 retry: [log](../../scratch/release_runs/20260927-e06-start/ssr-hv2021.log), same annotation parse error plus legacy vector-ternary incompatibility.
- Reviewable [one-line diff](../../scratch/release_runs/20260927-e06-start/SSR_ANNOTATION_PROPOSAL.patch) and [original/proposed hashes](../../scratch/release_runs/20260927-e06-start/SSR_ANNOTATION_PROPOSAL.json).
- Copied-source [proposal cook log](../../scratch/release_runs/20260927-e06-start/cook-ssr-proposal.log): all5 shaders pass DXC→MSC, with DXIL dump, reflection and receipt in the isolated scratch bundle. At the time of this proposal, no live bundle/launcher pointed at it.

The dated execution record below supersedes the proposal-only status for quality 0. It does not imply admission of other quality variants or visual parity.

## Packet 5 execution record — 2026-09-27

The active quality-0 annotation wrapper was used for the integrated general SSR
cook. It substitutes the table constant `8` for the DXC-rejected loop annotation
expression; the loop condition, trace body, sample behavior, and remaining
shader source are retained. The cook receipt preserves the original and wrapped
source hashes and the exact transform. Recooking the same input produced bytes
identical to the receipted bundle. See [the SSR integration record](E06_SSR_CONTRACT.md).

This closes the compiler blocker for the active quality-0 leaf only. The
reduced-scale blur viewport/UV correction is source-aligned with Windows. The
final same-save p5-s3 run exercised scale2 and scale1 at 1920×1080 (see the
results in [the SSR contract](E06_SSR_CONTRACT.md)). No Windows pixel-parity
claim follows from the successful cook or runtime pass readbacks.

## Final packet 5 acceptance update — 2026-09-27

`p5-accept` exits 0 with all 514/514 validation reports passing and zero
nonfinite channels across 659 texture reports. Current SSR acceptance is the
corrected shader validation and temporal cold/reuse lifecycle. The earlier
p5-final stage captures used stale SSR history code and incorrect validation
labels, so they are retained as historical readback evidence only. The source
now resets history across SSR scale `1→2→1`; the final 1920×1080 same-save S3
witness completed with 179/179 reports PASS, 220 textures, and zero nonfinite
channels. This does not change the quality-0-only scope of the annotation wrapper
or establish Windows image parity.
