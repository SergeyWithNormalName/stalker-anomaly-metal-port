# Controller input loss at a campfire — 2026-09-28

The user reported normal gameplay followed by loss of sticks/buttons/triggers on approaching a campfire and seeing the F interaction prompt. Circle and Options both opened the main menu.

## Cause

CUISequencer always captures the engine input stack. Its current item can either consume input (`GrabInput() == true`) or forward it to `m_pStoredInputReceiver`. The gamepad adapter treated every current sequencer as a blocking loading/intro screen, mapping Circle and Options to Escape, other button/trigger presses to Enter, and sticks to UI navigation. Passive interaction prompts consequently replaced gameplay controls.

## Fix

- Resolve input ownership through passive sequencers, including nested prompts, using the current item's GrabInput flag each frame.
- Preserve gameplay/UI/suppressed context of the underlying level/menu/console. Send events through the actual sequencer so its action filters still apply.
- Restrict loading/intro Enter/Escape translation to blocking sequencers.
- Preserve held input and neutral state when a passive prompt forwards to the same owner. Actual owner/context/dialog changes still reset input.
- Record receiver/context/tutorial ownership transitions without requiring verbose controller sampling.

Changed files: GamepadInput.cpp, GamepadInput.h, GamepadInputRouting.h, UIGameTutorial.h, GamepadLogicTests.cpp. Renderer and normal-profile data were not edited.

## Verification

- Full `Anomaly.DX11` build passed and updated the normal DEV Anomaly.app bundle.
- Standalone controller tests: **280 checks, 0 failures**; registered CTest passed.
- The same tests with a faithful reconstruction of the previous sequencer routing policy: **280 checks, 15 failures**. This catches gameplay ownership, Circle-to-menu, lost movement/camera/triggers, nested prompts, underlying console/UI ownership and unnecessary neutral rearming.
- Final-binary runtime used an isolated profile/save copy. The real engine logged `context=gameplay receiver=12CUISequencer tutorial=tutorial_campfire_ignite grab=0 modal=0 menu=0 top=<none>`. A later `game_loaded` sequencer correctly logged `context=ui grab=1 modal=1`; on completion the passive campfire prompt returned to gameplay. Console ownership remained suppressed.
- The controller was absent in the final runtime probe (`connected=0`): this establishes actual engine routing, not physical-button acceptance. Existing mod script warnings appeared during startup and are outside this fix.

Evidence and before-edit source snapshots: `scratch/gamepad-routing-20260928/`. The isolated runtime's probe script is not installed in the normal profile. The normal launch command uses the rebuilt application; repeat the physical-controller campfire check there.
