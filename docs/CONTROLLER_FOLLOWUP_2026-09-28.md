# DualSense PDA, equipment and focus follow-up — 2026-09-28

## Results

1. **PDA touch control:** the live DualSense profile exposes no touchpad collection (`touches=0`), and `touchpadButton.isTouched` remained false while primary/secondary axes changed. Lifted contacts returned raw `(0,0)`. The bridge now recognizes those legacy contact axes before surface normalization; absent secondary input no longer masquerades as a finger at the centre. Mapping routes one-finger drag and two-finger pan/pinch to map methods without requiring R3 pointer mode or the mouse cursor to be over the map. Touch input honors the user setting and reset/lift gate.
2. **PDA right stick:** independent map pan rate is 600 logical UI units/second at full stick. The old route was `900 * 0.1 = 90`; full-stick travel is now about 6.7 times faster. Map panning uses float deltas rather than pointer rounding; stick movement moves the view, while touch drag moves content.
3. **Gyro verification before repair:** raw recordings preceded the change. During the requested right-turn phase, Z dominated (90-second recording integral about -6.12 rad at the intermediate read, while Y was small and inconsistent). The old code used raw Y only. The capability probe reports `hasGravityAndUserAcceleration=0`, rotation rate supported, total acceleration approximately `(0.00769,0.16557,-0.97348)`. Apple defines gyro sign by the right-hand rule and gravity in the same device frame. The bridge now computes right-positive horizontal rate by projecting rotation onto down. It uses separate gravity if provided, otherwise a smoothed plausible total-acceleration direction; strong acceleration retains the previous basis. Pitch is unchanged. Tests cover flat/upright/tilted grip, direction, sensor gating and the measured right-turn sample. This verifies hardware axes and the mathematical conversion; it is distinct from the user's final in-game aiming feel.
4. **Worn equipment selection:** GAMMA pre-creates empty manual equipment cells and reuses them in `AddItemManual`/`RemoveItemManual`. Focus metadata was established while empty and never refreshed. The adapter refreshes metadata after both mutations, making filled equipment/belt/quick slots selectable and clearing empty slots. A regression models the same lifecycle.
5. **Focus colour:** changed the outline from bright blue to restrained light gray (RGB 225, alpha 225). Render priority remains -3: the registry processes higher priorities first, so this is already after main-menu 4 and combo-popup 3.
6. **Dropdown selection:** the outline now targets the selected combo row rather than the entire expanded combo. The selected row is scrolled into view and its outline is clipped to the list viewport. CUA confirmed a gray outline on Light and then Medium after a synthetic Down command through the normal gamepad adapter.

## Validation

- `Anomaly.DX11` and the Apple bridge probe compile; the normal DEV bundle is updated.
- Controller suite: **301 checks, 0 failures**; registered CTest passed.
- `luajit script/test_gamepad_ui_compat.lua` and `python3 script/test_materialize_mo2_profile.py` passed.
- Live raw-motion/touch recordings and capability probe were taken with the user's connected controller. The final runtime detected the physical DualSense; dropdown navigation verification used debug pulses with actual mapping/receiver routing in an isolated profile. A fresh process is needed before physical-control acceptance after synthetic replay.
- The test app exited normally. Normal profile's compatibility script is already a symlink to the edited source. Renderer source, user saves and normal user settings were not changed.

## Evidence and sources

Artifacts, pre-edit root-owned sources, recordings, build/test logs and hashes: `scratch/gamepad-followup-20260928/`. `ui-evidence.txt` contains native focus diagnostics and the visual inspection record. Runtime-only main-menu wrappers/control files are absent from the production profile.

Apple primary references: [GCRotationRate.y](https://developer.apple.com/documentation/gamecontroller/gcrotationrate/y), [GCMotion.gravity](https://developer.apple.com/documentation/gamecontroller/gcmotion/gravity); the installed SDK GCMotion.h documents `acceleration = gravity + userAcceleration` and the capability guard. World-axis interpretation was independently audited read-only by a GPT-6 Luna agent; equipment and highlighting were also delegated to Luna and reviewed/integrated by root.
