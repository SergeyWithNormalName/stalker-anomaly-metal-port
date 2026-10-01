# CPU observation scope correlation draft

Scratch-only proposal against the prestate hashes in `prestate-sha256.txt`.
No production files are changed and no build/test is run here.

`R4WholeLightVisibilitySample::cpuObservationScopeId` defaults to zero. Zero
keeps existing GPU-only receipts unchanged. For a CPU prediction, the scene
passes its nonzero current-observation scope on each face. Completion grouping
keeps existing identity `(groupKind, groupId)`, then requires every expected
face to match both `engineFrame` and `cpuObservationScopeId`; mixed scopes make
the entire group incomplete, so the ledger never combines faces from separate
CPU observations. Group and face completion lines echo the CPU scope beside
actual backend `submitted_serial` and `acquire_epoch`.

The ledger probe adds one same-nonzero-scope complete Omni group and one mixed-
scope invalid Omni group. Existing scope-zero tests retain the current GPU-only
behavior. Scene author confirmed the exact field spelling `cpuObservationScopeId`.
