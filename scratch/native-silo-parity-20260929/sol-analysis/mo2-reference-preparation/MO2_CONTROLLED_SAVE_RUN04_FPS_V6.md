# Run04 initial matched-frame diagnostic staging

Run04 is staged in the private MO2 clone only. The run01, run02, and run03 result files remain preserved and are hash-pinned in the run04 launch plan. Run03 was accepted with a completed saved-world update, exact save path, and hook nonce; the 20-second run04 observer starts from that same accepted condition contract.

The observer is exactly the 20/20 draft profile at SHA-256 `41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb`, derived from approved Lua v2 SHA `20568c539a6a58c4c907dc93225e84c8a383354e580caa7098f06d8e7afeb21f` by changing only the warmup and measurement constants from 3 seconds to 20 seconds. The staged-copy SHA is checked explicitly by V6.

The clone's canonical `user.ltx` is restored at SHA-256 `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334`. The stock `AnomalyDX11.exe` remains SHA-256 `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`. Arguments remain the proven escape-free literal. Run04 has a fresh private-hook receipt nonce and the plan requires `METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1`.

V6 is an immutable copy of the reviewed V5 runner with only the run output/script path, signpost environment requirement, explicit 20-second Lua pin, and run04 nonce key changed. A no-launch V6 preflight passed. The postcollector remains separate and will validate all measured rows before it captures only the marker-bounded window against the unique actual game host PID. No benchmark trace has been collected.

No screenshot is included in this clean measurement run. If a later visual sidecar is useful, the optional stock-renderer screenshot belongs after `measure_end`; do not compare it to the native 128x128 placeholder.

No game launch, build, primary MO2/mod/engine write, or trace collection was performed while preparing run04. Root owns review and execution.
