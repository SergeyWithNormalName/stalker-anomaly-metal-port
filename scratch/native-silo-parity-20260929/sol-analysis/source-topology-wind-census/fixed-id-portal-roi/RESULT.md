# Portal fixed-ID potential screen: first pair

Candidate `b1bb4725...` completed `portal-upper-bound-on-01` and `portal-upper-bound-off-01` with exit0. The opt-in main-only oracle skipped exactly4,086 IDs and kept1,230 after ordinary clip rejection; all shaders/bindings and every shadow route are unchanged. This is a potential upperbound experiment, not accepted per-frame/general portal culling.

| Metric | OFF | ON |
|---|---:|---:|
| On-Glass FPS | 25.8471 | 30.7889 |
| Callback FPS | 25.9135 | 30.9163 |
| CPU encode p50 ms | 34.4871 | 28.4542 |
| GPU command-buffer elapsed p50 ms | 29.9982 | 22.4075 |
| Frame draw median | 14685 | 10627 |
| Displayp99 ms | 45.8333 | 37.5000 |
| Maximum display ms | 70.8333 | 95.8332 |

On-Glass change **+19.12%** is large enough to justify bounded general-visibility preparation. Full-frame draw difference is4,058, close to the intended4,086; remaining dynamic/AI work is live and its exact causal contribution is not frozen. The maximum display interval worsened despite the improved p99, so this is not stability acceptance.

Recorded numeric weather at commit/start/end, calendar08:09:47, camera/FOV/1920x1200 and factors match. Diagnostic/validation disabled; no CUA in either run. Experimental calendar/ALife factor1e-6 differs from ordinary play; Device/physics1. Both source and usualapp/bare were restored to exact prestate before launches; normal object code was freshly rebuilt and its hash equals the earlier normal rebuild `fb9d3d91...`. Fixture+runtime level/geom hashes were pinned by the harness.

No quality, temporal wind, source hierarchy/ancestor shortcut or unmodified Windows superiority admission is made. Conservative same-frame wind only qualifies the dataset at frame700; moving benders in later frames remain outside its contract. The hard-coded list is never installed into production. Next useful work is a small renderer-local/source CPU portal visibility service with live input/ownership/fail-open proof and separate varied-camera image/motion checks.

Exact metrics and limits: `roi-pair-result.json`; independent code review: `REVIEW_LQ_AGENT.md`; input dataset: adjacent `offline-portal-frusta/RESULT.md`.
