# v3 draft result

Root's frame-700 run rejected v2 because the Scene compared the prepared adapter population to 5,316, which is the frame's captured main-draw count after normal clipping. The same run's adapter inventory reports 11,663 prepared Flora draws and 6,347 ordinary clip removals.

V3 removes the captured-frame count from prepared-record admission. It requires a nonempty RestRecords list, exact count equality with `LastTelemetry().admittedVisuals`, and exact unique-ID set equality with the source type-7 Flora pool admitted by retained geometry plus the actual captured B_TREE leaf/trunk, non-Object-LOD material policy. It does not pin that prepared count to 11,663 or any other fixed number. Per-record rest-bound and owner/frustum uncertainty continues to keep that record visible. The frame's 5,316 visible draws are left to the adapter's ordinary camera clipping and are not used as a population gate.

The copied root stage06 Scene snapshot is preserved as the v3 base, including its two callback-name fixes and `R4FloraWindEnvelope.h`. The package contains only scratch changes. No compile, build, or game run was performed by this draft author; the runtime rejection is evidence from root's prior stage06 run, not v3 validation. No pixel or performance result is claimed.
