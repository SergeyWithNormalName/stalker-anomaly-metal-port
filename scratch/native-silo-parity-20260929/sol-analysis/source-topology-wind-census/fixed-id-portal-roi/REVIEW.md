# Independent review: fixed-ID portal ROI draft

## Decision

**Ready for root-owned scratch build/runtime validation, not for integration.** The candidate is confined to one adapter copy, is default-off behind `XRAY_METAL_FLORA_PORTAL_ROI=1`, validates the exact fixture and admitted ID/material set, and skips only selected main-G-buffer IDs after the existing Metal clip test and before per-draw upload work. The shadow submission path never consults the fixture. No build or game run was performed in this review.

## Fixture and source checks

- The base adapter copy hash equals the production file at draft start: `3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc`.
- I independently parsed the 4,086-row fixture and compared it with the pinned 5,316-ID frame-700 capture. All fixture IDs exist, all leaf/trunk material labels agree, and view, projection, and eye words match bit-for-bit. The fixture counts are 1,765 leaves and 2,321 trunks; its SHA-256 is `35bce89dfa74883f8615d5cdf129327fbff515ede1af464bb6159b4630693c1d`.
- Runtime parsing rejects a missing/non-absolute path, wrong schema/count, malformed/non-finite camera words, duplicate/invalid rows, inventory omissions, duplicate admitted IDs, or material mismatches. These cases leave the ordinary draw path intact.
- The main loop performs the pre-existing AABB clip rejection first. Only an exact selected ID is skipped next; all other visuals continue through the same draw order and existing shader/resource/upload code. The fixture state is unused by shadow functions.
- The camera gate checks exact view/projection/eye bits. The motion gate checks `view.windValid`, as requested, but does not capture or compare frame time, animation, or bender values.

## Limits before interpreting results

The source still has no loaded Windows sector graph on this Metal path. The 4,086 source-geometry/wind rows are an offline per-ID, frame-700 upper-bound candidate set; they omit source HOM and hierarchy/full-visible shortcuts. An exact camera match on a later frame can reactivate the same IDs while live benders differ. The code reports `wind_temporal_qualification=0`; keep that limit explicit and do not present the run as proof of a safe persistent cull.

The fixture is a local file contract, not a self-authenticating asset package. Root's runtime harness should pin its SHA and the active level/geometry/camera inputs. The adapter independently checks only schema, camera bits, IDs and material against its admitted inventory.

I checked the generated unified diff and whitespace, and confirmed the production adapter remains byte-identical to the copied base. This was a source review, not a compiler check: the root owns the isolated build and one-run validation. The runtime line is designed to report `skipped`, remaining `kept` visuals after existing clip rejection, selected IDs already rejected by that clip test, and configured count once on the first matching frame.
