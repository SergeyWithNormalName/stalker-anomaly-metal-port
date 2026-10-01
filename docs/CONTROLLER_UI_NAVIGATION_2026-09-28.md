# DualSense UI navigation repair — 2026-09-28

## Problem and cause

The September 27 repair still left scripted GAMMA inventory and New Game windows in pointer mode. Native inventory/talk support did not cover Lua `utils_ui.UICellItem` widgets. `ActiveDialog()` also missed the main menu holder, and the mapping emitted right-stick cursor motion even with fallback disabled.

## Resulting control contract

| DualSense control | UI action |
| --- | --- |
| D-pad / left stick | Move between actionable controls and item cells |
| Cross | Activate the focused control; select an inventory item; move a New Game item |
| Circle | Close the current popup/dropdown/dialog |
| Square | Open an inventory item's context menu |
| Triangle | Use/equip/unequip/transfer through the existing GAMMA callback |
| L1 / R1 | Change tabs or inventory sorting filters |
| L2 / R2 | Change item panes |
| Right stick | Scroll the current pane; pan the active native PDA map |
| R3 | Explicitly toggle pointer fallback for the current dialog |

Pointer fallback resets on dialog/context changes. Default focus mode does not emit continuous pointer motion. A real mouse can take control back. The engine keeps its logical cursor synchronized to selected controls for legacy Lua hover/info; the rendered pointer is replaced by a focus border.

## Implementation

- `GamepadInput` discovers the current receiver and top dialog from both main-menu and gameplay UI stacks, resets held input on dialog changes, and installs the optional GAMMA adapter.
- `GamepadUiNavigation` discovers native widgets and script callbacks, handles focus movement, scrolling, panes/tabs and activation. Scrollbar implementation buttons and decorative main-menu descendants are excluded. Native menu and properties-box keyboard selection stays with those controls. Nested mouse capture limits navigation to the active popup.
- `gamepad_ui_compat.script` adds focus metadata and semantic action dispatch to the existing GAMMA cell containers. It recognizes inventory, New Game and other containers with real primary callbacks, while excluding decorative/no-op/disabled containers and empty equipment slots.
- The MO2 materializer version 4 installs this source overlay after the winning profile. The winning `utils_ui`, inventory and New Game files remain untouched.
- `UICursor` uses the common atlas directly for its focus border: cursor construction occurs before the XML texture-ID registry is loaded.
- Mapping reset suppresses a held touch until release and prevents R3 from converting an already-held accept into a click.

## Validation

The full `Anomaly.DX11` target builds. The isolated controller suite passes 258 checks; Lua adapter and Python materializer tests pass. A same-harness before/after comparison under `scratch/gamepad-focus-20260928/mapping-regression` proves default right-stick cursor motion 1 → 0, selection accept Mouse1 → Enter, and held-touch acceptance after UI reset 1 → 0. Explicit R3 pointer mode still moves the pointer.

Runtime checks use the actual MyTree materialization, an isolated writable runtime and a copy of `metal_bar_quicksave`. Debug button pulses are fed through `OnGamepadFrame` and the real mapping/routing, rather than calling the final item action directly. CUA screenshots inspect the rendered result. This proves the UI path but is separate from a user's physical-controller acceptance.

Confirmed in the actual rendered New Game window:

- Main-menu Down changes the native selection to Load; Up + Accept opens New Game without a virtual-pointer click.
- Initial focus lands on an item rather than a scrollbar arrow; R2 changes to the loadout pane.
- Accept transfers a 650-point weapon into the chosen equipment and updates remaining points from 1700 to 1050.
- Right-stick pulses scroll the loadout; TOZ-34 remains focused in view with native and GAMMA cached scroll positions both 140.
- The native location dropdown opens and closes with Circle while the New Game dialog remains open.
- The One Life checkbox toggles through its normal callback, and difficulty changes from Light to Medium through the dropdown.

Native controls passed the live tree audit with no mouse capture and enabled/shown parents. Several Left steps within differently sized inventory cells are geometric focus moves, not a missing RTTI/control registration path. `GamepadUiTestInput(0)` logs the current focus and a named-control tree for future diagnosis.

## Evidence and boundaries

Build/test logs and the isolated test fixture live in `scratch/gamepad-focus-20260928`. Diagnostic `GamepadUiTestInput` is gated by `g_gamepad_debug`; the fixture script/main-menu wrapper exists only in scratch. A fresh normal process is required after synthetic replay before physical pad testing.

Unrelated renderer/MO2 WIP is preserved. This change does not establish visual parity of the Metal renderer or exhaustive acceptance of every mod-provided screen. Existing profile script warnings are outside this repair.
