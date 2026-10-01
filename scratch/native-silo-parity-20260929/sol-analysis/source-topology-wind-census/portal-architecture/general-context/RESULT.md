# Cached portal context, source replay

The root-owned full-engine CLI transaction at `root-build-01` built and ran successfully, without opening a game or changing draw admission. The cache uses actual engine `CPortal::Setup`, `CDB::MODEL`, and `CFrustum` code. It owns its portal model and retains only parser-owned topology/bounds and plane/sphere values; the static CFORM is borrowed per query.

The pinned frame-700 replay resolved sector 25 and reached sectors `17:1;21:1;22:1;24:1;25:1;30:3`. All 5,316 raw authored sphere/AABB flags matched the earlier actual C++ replay: 4,427 outside, 889 intersecting, zero mismatches. The output CSV is byte-identical to the prior result, SHA `f882ab4e87da7fc8a384e615b703e9e8cceb54e69e50946fb3ea66935cf3397f`. Malformed-polygon rejection, stale-generation visible-all behavior, and synthetic-CFORM fallback passed.

The CFORM fixture is synthetic and explicitly labels sector 25; this does not qualify queries against the live engine model. Raw authored bounds are not wind-qualified bounds. This is an offline source-math/lifetime test, not live visibility, GPU pixels, motion, FPS, or Silo superiority.

Candidate EXE SHA `829ff35ad326a3abc20495b19157c5ed7d5799260d6ca508632822ec2ca34f4b`; stage manifest SHA `d30e13db664430c3d65f2380092369e105825ae7d2b8ba9251cf0478bced70da`. The separate helper translation units were temporarily added to the existing R4 target and exposed through `--portal-context-replay`; no dedicated partial-engine executable was used. Exact production prestate was restored, temporary helper files removed, normal rebuild passed, and ordinary app/bare EXE restored to `d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0`. The transaction ledger pins all ten staged files and restoration hashes.
