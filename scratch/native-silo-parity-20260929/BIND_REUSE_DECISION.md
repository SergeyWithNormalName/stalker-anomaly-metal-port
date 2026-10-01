# Local-light binding reuse: reject after the first gameplay screen

The same immutable candidate executable (`e7a94d140a3ba3d3905e04b1909b833a8a2ec0c0aafd081984448122d1ae0e8a`) was run with the switch off and on. Both arms loaded the pinned Cordon save and used the same configuration, camera, shaders, 1920x1200 targets and existing compute-skin/dynamic-shadow-index/flora optimizations. Diagnostic E01/E07 output was enabled in both arms. AI remained active, so workloads are close rather than byte-identical; game time factor was 6, not frozen.

| First screen | Off | On |
|---|---:|---:|
| Callback FPS | 19.3114 | 19.1844 |
| CPU encode median, ms | 47.1369 | 47.5100 |
| CPU scene median, ms (nested) | 45.6590 | 46.0844 |
| Indexed draws median | 21117 | 21097 |
| Updated bytes median | 28123884 | 27912636 |
| GPU command-buffer elapsed median, ms (overlaps CPU) | 36.5922 | 37.0950 |

The off on-glass trace is valid (19.3111 FPS). The on trace has a 6 ms timeline gap and fails the predeclared admission check; its descriptive FPS is not accepted as a displayed-FPS comparison. These runs do not establish a statistically reliable regression. They give no useful speed benefit or reason to add more caching logic, so the candidate is rejected at the hypothesis stage. No reverse pair is necessary for rejection of an unpromising idea.

Correctness probes passed 56 exact RGBA16F/depth32/stencil8 pairs across seven production-PSO cases, shadow controls, retry and slot rollover. They establish correctness within those cases, not gameplay visual/motion acceptance and not performance gain. There is no dropped-binding negative control.

The five edited source files were restored from their exact prestate bytes, with before/after hashes checked. Production executable remains `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`. Frozen patches, candidate binary, raw runs, probe results and `bind-reuse-stage/rejection-restore-receipt.json` remain available for review.

This decision does not establish parity or superiority over Silo. The matched Windows saved-game reference still has a startup/spawn compatibility failure.
