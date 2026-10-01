# Whole-light visibility observation draft

This scratch draft adds a strict opt-in observer for the final local-light
accumulation draw. `XRAY_METAL_WHOLE_LIGHT_VISIBILITY=1` enables selection;
`XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME` sets the first sampled engine
frame (default `300`). Selection is bounded to that frame plus the following
eleven frames, at most eight whole groups and therefore at most 48 faces per
frame.

Selection only sees the already planned shadow batches. A spotlight is one
`SingleRecord` keyed by its nonzero `lightId`; its `parentLightId` is attribution
metadata and may be zero or shared. The backend tag uses face zero and expected
count one. An omni is keyed by nonzero `parentLightId` and is selected only when
all six distinct faces are present exactly once in the final batches. Missing,
duplicate, colliding, invalid, HUD, or actually volume-consuming records omit
the whole group. Selection never changes the batch vectors.

The adapter sends visibility metadata only with the final accumulation draw;
the two stencil-mask draws remain ordinary draws. The render-pass query flag is
set only for a batch that has a selected record. Query counts are observational
and must never drive culling. A zero result means only
`zero_deferred_receiver_samples`; it does not establish invisibility or absence
of forward, translucent, or secondary-view consumers.

The scene emits bounded selection/exemption/quota counts. For selected shadow
tiles it reports existing prepared static rows, encoded static draws, submitted
dynamic draws, and the static row-transform byte count (256 bytes per prepared
row), logged as `static_transform_bytes`. That byte count excludes other
pipeline uploads. It does not enable E08
timers or manufacture per-parent milliseconds. Native
completion rows remain the authority for successful samples; a selected tag in
an aborted frame is not an accepted observation. Exemption-record and
incomplete-group counts are diagnostic dimensions and can overlap.

`tests/R4WholeLightVisibilitySelectionProbe.cpp` exercises cross-batch omni
completion, spot metadata, malformed groups, ID collisions, HUD/volume
exemptions, frame-window bounds, 8-group/48-face caps, and deterministic
rotation. It is not built or run here; the staged interface is separate from
the production backend header until root review and coordinated integration.
