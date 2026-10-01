# Main Flora pixel-pair draft v2

V2 is a separate immutable review candidate. V1 remains unchanged in `draft/`, `main-flora-pixel-pair.patch`, and `draft-source-hashes.json`.

V2 contains only the two root-requested corrections over v1:

1. The exact-frame oracle owns its OFF/ON targets locally for the one acquired frame. It no longer caches Metal textures by `IR4Backend*`, avoiding stale-device ownership/address reuse.
2. The D32S8 pair report records width, height, byte count, and pointer availability; pair validation cannot PASS unless completed readback contents are non-null. The selected frame must be a nonempty decimal value within uint32 range.

`main-flora-pixel-pair-v2.patch` applies the complete candidate against the six pinned source prestate hashes in `draft-v2-source-hashes.json`. `main-flora-pixel-pair-v2-review-diff.patch` shows only v1-to-v2 changes. V2 changes only the scene and validation implementation copies; source prestate hashes remain identical to v1. No build or runtime test was performed by this agent.
