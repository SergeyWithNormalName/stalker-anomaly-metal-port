# Gates for each isolated hypothesis

Start each hypothesis from the same frozen handoff commit in a separate branch/worktree. Record its intended effect, changed source files, fallback behavior, risk, expected CPU/GPU/draw-work change, and exact later A/B plan. Lead GPT-6.1 Sol; helpers only GPT-6 Luna or GPT-6.1 Sol.

Cloud-side source review and portable checks may support an implementation argument, but cannot establish Metal runtime correctness or performance. Mark candidates **not verified on Mac**. Preserve negative evidence; do not repeat rejected work without a new premise. Do not add together gains measured in different workloads or promote observer/census/upper-bound rejection counts as actual savings.

The later authorized Mac gate must establish:

1. Exact executable/source/configuration provenance, one common frozen baseline, matching save/mod/material inputs and camera/weather/time settings, and a defined measured window.
2. Matching quality, effective shadows/internal render targets, ordinary gameplay and motion. Treat the old one-pair 26/37 FPS diagnostic as limited instrumented evidence, including its near-frozen game clock and E01 caveat.
3. Repeatable A/B presentations, frame intervals and lows, CPU/GPU scopes measured separately, comparable draw work, observer/logging overhead, and power/thermal conditions. Overlapping CPU/GPU intervals are not additive.
4. Correctness in indoor/outdoor views and motion, conservative fail-open behavior for stale/malformed/unknown source context, stable source order where required, and no missing geometry or visual regression.
5. Normal GAMMA startup/mod compatibility and stability. Preserve primary installation and update compatibility, retain a rollback, and require an explicit promotion decision after sufficient evidence.

Existing reports cover only the runs they describe. The published source tree excludes full raw runtime evidence and binary dependencies; a build or isolated PASS does not fill those gaps.
