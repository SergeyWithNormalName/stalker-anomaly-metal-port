# Windows comparator SSA/config provenance check

## Evidence

The pinned stock `AnomalyDX11.stock.exe` is 25,280,000 bytes, SHA-256
`c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`, PE
timestamp `1778823367` (2026-05-15 UTC). The existing [May 15 PDB/machine-code
report](../../luna-harness/compatibility/2026-09-29-pdb-exists-machinecode.md)
identifies this same binary/PDB generation; it is older than the dirty source
snapshot used for the native renderer.

The pinned DX11 image contains `r2_allow_r1_lights`, `r2_ssa_lod_a`, and
`r2_ssa_lod_b` narrow strings. It contains no narrow or UTF-16LE strings for
`r__ssa_discard`, `r__ssa_discard_exp`, or `r__ssa_discard_fade_k`. The sibling
DX11AVX and DX10/DX10AVX executables do contain those discard-control strings.
This is evidence about exposed names in these binaries, not proof of runtime
defaults or whether an alias/script path exists.

The Windows starting config is
`scratch/native-silo-parity-20260929/windows-control-runtime/appdata/user.ltx`
(744 lines, SHA-256 `b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334`). The post-run, Windows-resaved config is
`runs/r1-final-weather-v3-01/silo/runtime/appdata/user.ltx` (652 lines, SHA-256 `dc1a7125eacdddfada625715c47fd60ba8d0eaae5a05d2828f937fbf6aac419e`). The native run copy has 744 lines and SHA-256 `93460c4461b7623af5b84a20d5a2f74f4239ddb1dc7c480f18fac8338216338a`. A
key/value comparison finds 99 settings absent after the save, seven newly
present keys, and one changed key (`rs_stats on` to `off`). In particular,
these native runtime values did not survive in the Windows-saved file:

| Key | Pinned starting value | Windows save |
|---|---:|---|
| `r__ssa_discard` | `7.` | absent |
| `r__ssa_discard_exp` | `0.5` | absent |
| `r__ssa_discard_fade_k` | `4.` | absent |
| `r2_lights_details` | `off` | absent |
| `r2_mt` | `on` | absent |
| `r2_shadow_lod_min` | `0.02` | absent |
| `r2_shadow_omnipart_vischeck` | `1` | absent |
| `r__hom_dynamic` | `off` | absent |
| `r__rain_exp` / `r__rain_k` | `1.` / `99.` | absent |
| `r__tex_evict_enabled` / `interval` / `batch_size` / `age_frames` | `0` / `600` / `200` / `1800` | absent |
| `r_wallmarks_static` / `r_wallmarks_dynamic` / `r_wallmarks_ssa_k` | `1` / `1` / `0.5` | absent |
| `rs_render_portals` | `off` | absent |

The Windows save does retain `r2_allow_r1_lights off` and `r2_ssa_lod_a 64.` /
`r2_ssa_lod_b 48.`. The latter are R2 LOD controls; their presence does not
establish equivalence to the native `r__ssa_discard*` policy. In the current
source, `r__ssa_discard*` drive `r_ssaDISCARD` and its fade in static geometry
and detail paths (`xrRender_console.cpp`, `R_calculate.cpp`,
`r__dsgraph_build.cpp`, `DetailManager.cpp`).

## Assessment

This undermines any claim that the generic SSA comparator is already
source-equivalent or that its configuration parity is established. The
post-run Windows save does not retain the native threshold/fade values, and
the pinned stock DX11 binary lacks those names as strings. Neither observation
reveals the effective stock runtime threshold, fade, or implementation. Do not
infer those defaults from string absence or from the other DX binaries.

## Initially proposed diagnostic (superseded by runtime result below)

On the pinned stock DX11 runtime, issue `help` after renderer startup and save
the complete console listing; the stock image contains `help` and `cfg_save`
strings, but no `dump_cvar` string, so do not rely on the newer source's
`CCC_DumpCVars` command. In a private clone, issue each `r__ssa_discard*`
setting with its pinned starting value, then issue `help` again and `cfg_save`.
Check both the console's accepted/current status and whether all three keys
survive in the saved LTX. Also record `r2_ssa_lod_a/b`, `r2_allow_r1_lights`,
and the renderer controls above. If the names are unknown, rejected, or omitted
after save, mark SSA-policy parity as failed and do not benchmark the pair. If
they are registered and retained, capture their live values plus the stock
static visual's actual rule before claiming a policy match. Keep the same saved
scene, camera, weather, resolution, shader cache, and frame phase; record the
selected EXE hash and resulting config hash.

This check is diagnostic only. It does not establish an FPS winner, and the
generic SSA performance goal remains unproved.

## Actual stock runtime result, September 30

`runs/windows-console-probe-02/silo` used the unchanged pinned stock EXE. The external diagnostic ran at `axr_main.on_game_start` before the original startup and actor callback. It did not measure FPS. All 21 targeted requests and the flushed `console_probe_complete` marker were recorded. The normal asynchronous quit did not stop loading within the five-second grace, so the owned process was boundedly terminated: exit 143, `console_probe_complete_bounded_shutdown`. This is a completed control diagnostic, not a successful gameplay exit.

The three `r__ssa_discard*` queries returned unregistered/nil before and after attempts to set the pinned values; the actual engine log also explicitly reports all three as `Unknown command`. Thus runtime exposed-policy parity for these controls **failed**. Their internal effective defaults/rule remain unknown. Queries confirm renderer R4, `r2_allow_r1_lights off`, LOD a/b 64/48, sun on, MSAA off, SSAO disabled, detail density 0.68, detail radius 70, FOV 80 and geometry LOD 1 at this early phase. Query failure alone for a different command (for example `r2_smap_size`) is not assigned an effective default.

The first attempt with broad `help` crashed the old engine in `CCC_Token::Info` while traversing its token list. The successful second diagnostic removed `help`; do not repeat that unsafe broad listing. Evidence: `runtime-probe-result.json`, `runs/windows-console-probe-02/silo/output/phases.csv.console.csv` and the actual `runtime/appdata/logs/xray_crossover.log`. Config writes were confined to the private run clone; installed GAMMA and its stock EXE remain unchanged.
