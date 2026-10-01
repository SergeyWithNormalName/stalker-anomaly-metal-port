# Direct Metal source handoff — 2026-10-01

This branch publishes a frozen source snapshot for further code work. It does not establish Windows/GPTK parity, a gameplay FPS win, or a release-quality renderer. **Not verified on Mac for this publication.** No games, builds, tests, or benchmarks were run to prepare it.

## Start here

1. Read [the snapshot manifest](docs/cloud-handoff/SOURCE_SNAPSHOT_MANIFEST.json) and [experiment outcomes](docs/cloud-handoff/EXPERIMENT_LOG.md).
2. Read [the latest continuation](docs/direct-metal-release/NATIVE_SILO_PARITY_CONTINUATION_2026-09-30.md) and [the matched native/MO2 diagnostic](scratch/native-silo-parity-20260929/sol-analysis/mo2-reference-preparation/mo2-fps-run04-draft/native-baseline-analysis/RESULT-v2.md).
3. Use [the validation plan](docs/cloud-handoff/VALIDATION_PLAN.md). Freeze this handoff commit as one common starting point; create a separate branch/worktree for every new hypothesis. Do not combine exploratory gains or silently enable scratch candidates.

## Source mapping

- Public parent: `377562f5242c00a84b899d49353a78065bd00ce9` (`main`). Only its existing public ancestry is retained.
- Local source base: `c8add841849cf7c4f1269d94e0addaac3b5d3427` (`feature/native-metal-r4`) plus its frozen working-tree source state, including 574 modified tracked files at export time.
- Export captured at `2026-10-01T05:10:27.617857+00:00`. The 11,278 exported file-hash checks passed again before publication preparation. The compared live source had the same HEAD and no differences in those exported paths; later changes are not merged into this snapshot.
- Private repository history, full-checkout patches, archive, Library identities, and conversation exports are not published.

The engine source is the captured ordinary source. Historical candidates, patches and reports under `scratch/` remain exploratory evidence and are not production acceptance. Historical executable hashes do not certify the full dependency closure of this source tree. Raw evidence omitted from this publication cannot be independently replayed here.

## Current conclusions

The goal remains native Metal faster than the Windows executable through MO2/Silo/GPTK under matched quality and workload. The saved 26.00 vs 37.06 On-Glass FPS comparison is one short instrumented fixed-camera diagnostic, with `Device=1` and game-time factor `1e-6`, live AI, and native E01 overhead. It is not an established ordinary-gameplay Windows baseline or an overall winner. Quality/shadow parity remains open.

Portal/wind and immutable source-certificate cost reductions have limited positive screens. General LMH Stage13 is a draft. Static-globals versioning, material reordering, and earlier parallel-shadow/constant-grouping screens have negative or rejected outcomes. The experiment log preserves their reasons and next gates; build success and isolated PASS results are not acceptance.

## Published and omitted material

Included: engine/Direct Metal source, SDK headers and vendored source, CMake and shader/build scripts, project documentation, cleaned technical reports, small hypothesis patches and portable analysis helpers, and source-hash/redaction manifests. Existing upstream license/attribution material is retained.

Omitted: game assets and installation/runtime trees, saves, shader/SDK binaries, build products, core dumps, raw profiles/large geometry grids, local launch/config receipts, credentials/keys/cookies, personal paths and chat exports, duplicate prestate trees, and historical helpers that require private local paths. Documentation references to omitted files are historical pointers, not available inputs.

Only publication-related launcher changes were made: personal MO2/game paths now use `XRAY_MO2_ROOT` and `XRAY_GAME_ROOT`, with generic `$HOME/gamma/...` defaults. The publication transform manifest lists every changed copied file. Launch scripts are historical Mac tools and may build or manage processes; do not execute them during cloud inspection. The runtime/game data and optional dependencies must be provided separately in a later authorized Mac session.

Model contract: lead GPT-6.1 Sol; helpers only GPT-6 Luna or GPT-6.1 Sol. Every candidate stays **not verified on Mac** until its later runtime, image/motion, stability, and performance gates pass.
