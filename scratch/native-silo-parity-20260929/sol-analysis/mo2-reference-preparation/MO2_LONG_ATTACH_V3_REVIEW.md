# MO2 long menu observation v3 review

## Scope and decision

This is a scratch-only controller draft for a root-owned future run. It was not launched. The existing v2 observation attached to Wine guest PID 424 (host PID 74888), reached the WineDbg prompt, detached cleanly, and exited the debugger with status 0. At its single 60-second snapshot, the main thread stack was in `CLocatorAPI::Recurse` / `ProcessOne` traversing `gamedata/sounds/ambient/soundscape/underground/spooks_below/noise`. That is one sampled stack, not evidence of an infinite hang or a proven cause.

V3 changes the observation directory to `menu-startup-long-attach-01`, sets one gated attach point at 300 seconds, and caps the menu-only run at 900 seconds. It retains v2's fresh-prompt protocol, raw PTY, unique new host process plus fresh USVFS guest PID pairing, separate PID fields, primary-source write-deny sandbox, output cap, prompted detach-before-quit, and bounded cleanup. It does not add a readiness heuristic: current artifacts do not establish an authoritative menu-ready marker, so a log-string guess would be less reliable than the requested single 300-second sample.

## Backend provenance correction

V2's launch route was not a faithful Silo backend reference. Its `d3d11,dxgi=n,b`, `D3DM_ENABLE_METALFX=0`, and DXVK log/cache settings selected the copied prefix's native DLL route. The current Silo manual-game entry instead identifies graphics `gptk`, MetalFX enabled, backend `auto`, and ESYNC. The primary launch-log header dated 2026-09-22 records `D3DM_ENABLE_METALFX=1`, Wine CX DYLD fallback paths, `WINEESYNC=1`, and overrides ending in `nvngx=b` for the Direct3D/DXGI DLL list. V3 validates the current config fields and that primary header, uses the recorded override/MetalFX route plus bounded `+loaddll` diagnostics, and records both config and launch-log hashes when run.

The copied prefix's DXVK DLL hashes remain checked only as prefix-integrity provenance; they do not establish the renderer selected by the `=b` overrides. The receipt labels the route as a reference until Anomaly's child-process module-load evidence corroborates it. Do not treat the old v2 run as Silo/GPTK/MetalFX renderer evidence.

The launch-log inspection for this review was limited to the first 2 KiB, stopping at its first NUL (951 bytes before NUL). Only route tokens and the recorded launch timestamp were used. The v3 receipt hashes the complete file for identity without printing its body.

## Checks and limits

- V3 is dry-run unless both `--launch` and `PARITY_ENABLE_LAUNCH=1` are supplied; attach also requires `--attach-winedbg` and `PARITY_ENABLE_WINE_ATTACH=1`.
- The current config and primary launch reference are validated before launch. A mismatch aborts instead of silently substituting a backend.
- The existing PTY-only CR transport test remains applicable: it starts no Wine/game process and verifies that raw mode preserves `\r` instead of translating it to `\n`.
- The `bt all` output is retained because it yielded the useful main-thread trace despite WineDbg warnings. Stack locals are not treated as trustworthy.
- V3 syntax was checked without executing the controller. No app, Wine, game, build, shader cook, or production source was changed by this preparation.

## Pins

- V2 controller SHA-256: `61fdc5ad638b1a74da1a9e15c29fb6d8932de62a776725d8abc1cb0666fafbc7`
- V2 clean WineDbg transcript SHA-256: `ece4676a8551e849a4d6341b9e6c6d1995366af0df10ce75d13336ba88bf3b21`
- V2 launch receipt SHA-256: `444f44cdf9acfc940f628518689e06a07e41989bc0bb6c350cbf4c48fb936798`
- V3 controller SHA-256: `46eadeb03337401fb3f435e1126c4248609d7084ef81fcf3fe827885fcbaec74`

V3 is ready for parent review only. Parent owns backend/module-load validation and any launch decision.
