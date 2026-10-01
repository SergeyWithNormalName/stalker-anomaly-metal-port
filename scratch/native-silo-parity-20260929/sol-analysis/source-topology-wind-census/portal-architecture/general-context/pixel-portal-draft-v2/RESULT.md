# v2 draft result

Prepared a separate Scene-only v2 package after root review of v1. The v1 package remains unchanged.

The v2 prepare callback explicitly validates both the live wind snapshot and environment snapshot against the callback view's engine frame. The portal candidate helper receives those freshness results and rejects if either fails. The environment snapshot type has both `valid` and `engineFrame`; the detail-grass motion snapshot has the same fields.

Before testing any per-visual bounds, v2 requires 5,316 rest records, a matching adapter `admittedVisuals` count, exact set equality with the known type-7 Flora IDs derived from retained level geometry, and 5,316 unique visual IDs with successful source-geometry joins. Any missing, duplicate, extraneous, or structurally unjoined source ID disables the entire candidate mask. A present record with unknown rest/wind data or incomplete owner/frustum data remains visible individually. The manifest's branch description now distinguishes all-draw OFF from candidate-filtered ON.

This is static scratch review only. No compile, build, runtime, image, or performance result is claimed. The draft still consumes a separately owned adapter API and filter method, so it is intentionally not build-ready or shipping-ready.
