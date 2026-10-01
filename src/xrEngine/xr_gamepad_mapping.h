#ifndef __XR_GAMEPAD_MAPPING__
#define __XR_GAMEPAD_MAPPING__
#pragma once

// Turns a processed gamepad frame into key events.
//
// This is the policy half of the gamepad layer and it is deliberately free of
// engine dependencies: actions are opaque integers that the caller resolves to
// key codes, and every emitted event goes through an output interface. That is
// what lets the disconnect, context switch and stuck key rules be covered by
// the unit tests without a controller, a level or a running engine.

#include "xr_gamepad_processor.h"
#include "xr_gamepad_gyro.h"
#include "xr_gamepad_chord.h"
#include "xr_gamepad_radial.h"

#include "xr_gamepad_touchpad.h"

#include <cstdint>
#include <vector>

// Logical controls the mapping drives. The game layer resolves each of these to
// whatever key its action is currently bound to.
enum EGamepadMappedAction
{
	GPA_MoveForward = 0,
	GPA_MoveBack,
	GPA_MoveLeft,
	GPA_MoveRight,
	GPA_Walk,
	GPA_Sprint,
	GPA_Jump,
	GPA_Crouch,
	GPA_Fire,
	GPA_Aim,
	GPA_LeanLeft,
	GPA_LeanRight,
	GPA_WeaponFunc,
	GPA_FireModeNext,
	GPA_Torch,
	GPA_Pause,
	GPA_DpadUp,
	GPA_DpadDown,
	GPA_DpadLeft,
	GPA_DpadRight,
	GPA_Reload,
	GPA_Use,
	GPA_Pda,
	GPA_Inventory,
	GPA_Back,
	GPA_Count
};

enum EGamepadUiSpecialAction
{
	UI_ACTION_NONE = 0,
	UI_ACTION_CONTEXT_MENU, // Square / X
	UI_ACTION_TRANSFER,     // Triangle / Y
	UI_ACTION_TAB_PREV,     // L1
	UI_ACTION_TAB_NEXT,     // R1
	UI_ACTION_PANEL_PREV,   // L2
	UI_ACTION_PANEL_NEXT,   // R2
	UI_ACTION_SCROLL_UP,    // Right stick in focus mode
	UI_ACTION_SCROLL_DOWN
};

// Where the mapping sends what it produces.
class IGamepadActionOutput
{
public:
	virtual ~IGamepadActionOutput() {}

	virtual void GamepadKeyPress(int dik) = 0;
	virtual void GamepadKeyRelease(int dik) = 0;
	virtual void GamepadKeyHold(int dik) = 0;

	// Deltas are in the engine's mouse / screen convention, not the frame's:
	// +X right, +Y down, exactly as the platform mouse handler reports them.
	virtual void GamepadMouseMove(int dx, int dy) = 0;
	virtual void GamepadMouseWheel(int direction) = 0;
	virtual void GamepadCursorMove(float dx, float dy) = 0;
	// Logical UI units: independent of pointer position and physical mouse state.
	virtual void GamepadMapPan(float dx, float dy) {}
	virtual void GamepadMapZoom(bool zoomIn) {}

	virtual void GamepadChordEvent(int chord) {}
	virtual void GamepadUiAction(int uiAction) {}
};

// Resolves a logical action to the key it is bound to. Returns 0 when the
// action is unbound, which is normal and not an error.
typedef int (*GamepadActionResolver)(int mappedAction);

// Who owns input this frame. This is not a boolean because there is a third
// state that matters: a receiver the gamepad has no business feeding at all.
// The legacy console, the tutorial sequencer and the imgui editor all capture
// the engine input stack and are none of them gameplay, so synthesised key
// strokes there would type letters or fight the owner.
enum EGamepadInputContext
{
	GAMEPAD_CONTEXT_SUPPRESSED = 0,
	GAMEPAD_CONTEXT_GAMEPLAY,
	GAMEPAD_CONTEXT_UI
};

struct SGamepadMappingConfig
{
	SGamepadMappingConfig()
	{
		lookRatePx = 1300.0f;
		cursorRate = 900.0f;
		mapPanRate = 600.0f;
		touchMapPanScale = 900.0f;
		navDelayMs = 350.0f;
		navRepeatMs = 110.0f;

		// Only controls that could cause a spurious action are latched. A
		// button that is already down when the context changes produces no
		// fresh edge, so buttons do not need this.
		neutralTrigger = 0.15f;
		neutralStick = 0.20f;

		uiNavUp = 0xc8;    // DIK_UP
		uiNavDown = 0xd0;  // DIK_DOWN
		uiNavLeft = 0xcb;  // DIK_LEFT
		uiNavRight = 0xcd; // DIK_RIGHT
		uiAccept = 0x1c; // DIK_RETURN; pointer accept is selected only by explicit R3 mode
		uiPointerClick = 0xed + 100; // MOUSE_1, independent of selection accept
		uiScrollThreshold = 0.55f;
		uiMapPan = false;
	}

	float lookRatePx;
	float cursorRate;
	float mapPanRate;
	float touchMapPanScale;
	float navDelayMs;
	float navRepeatMs;
	float neutralTrigger;
	float neutralStick;

	int uiNavUp;
	int uiNavDown;
	int uiNavLeft;
	int uiNavRight;
	int uiAccept;
	int uiPointerClick;
	float uiScrollThreshold;
	bool uiMapPan;
};

class CGamepadMapping
{
public:
	CGamepadMapping();

	void Configure(const SGamepadMappingConfig& config) { m_config = config; }
	const SGamepadMappingConfig& Config() const { return m_config; }

	void SetOutput(IGamepadActionOutput* output) { m_output = output; }
	void SetResolver(GamepadActionResolver resolver) { m_resolver = resolver; }

	// Releases everything held and arms the neutral latch.
	void Reset();

	// Consumes one frame. `context` must come from real engine state, never a
	// timer: it decides which control set owns the input, if any.
	void Process(const SGamepadFrame& frame, const SGamepadSettings& settings, float dtMs,
		EGamepadInputContext context);

	bool NeedsNeutral() const { return m_needsNeutral; }
	int HeldCount() const { return int(m_held.size()); }
	bool IsHeld(int dik) const;
	EGamepadInputContext LastContext() const { return m_lastContext; }

	bool FallbackCursorEnabled() const { return m_fallbackCursor; }
	void SetFallbackCursor(bool enabled) { m_fallbackCursor = enabled; }
	void ToggleFallbackCursor() { m_fallbackCursor = !m_fallbackCursor; }

	CGyroAimProcessor& Gyro() { return m_gyro; }
	const CGyroAimProcessor& Gyro() const { return m_gyro; }
	CGamepadChordTracker& ChordTracker() { return m_chordTracker; }
	CTouchpadGestureProcessor& Touchpad() { return m_touchpad; }
	const CTouchpadGestureProcessor& Touchpad() const { return m_touchpad; }

private:
	// Releases every key still held. Called on a context change, a receiver
	// change and a device reset, which is what stops anything being stuck.
	void ReleaseAll();

	void PressDik(int dik);
	void ReleaseDik(int dik);
	void HoldDik(int dik);

	int Resolve(EGamepadMappedAction action) const;
	void PressAction(EGamepadMappedAction action) { PressDik(Resolve(action)); }
	void ReleaseAction(EGamepadMappedAction action) { ReleaseDik(Resolve(action)); }

	void ApplyGameplay(const SGamepadFrame& frame, const SGamepadSettings& settings, float dtMs);
	void ApplyUi(const SGamepadFrame& frame, const SGamepadSettings& settings, float dtMs);

	struct SNavState
	{
		SNavState() : wasDown(false), heldMs(0.0f), repeated(false) {}
		bool wasDown;
		float heldMs;
		bool repeated;
	};

	IGamepadActionOutput* m_output;
	GamepadActionResolver m_resolver;
	SGamepadMappingConfig m_config;

	std::vector<int> m_held;

	bool m_needsNeutral;
	bool m_suppressTouchUntilLift;
	EGamepadInputContext m_lastContext;
	bool m_initialised;

	// Sub key remainders: the mouse path takes integers, so a slow stick would
	// otherwise round to zero forever.
	float m_lookAccumX;
	float m_lookAccumY;
	float m_cursorAccumX;
	float m_cursorAccumY;

	SNavState m_nav[4];
	SNavState m_scroll[2];
	CGyroAimProcessor m_gyro;
	CGamepadChordTracker m_chordTracker;
	CTouchpadGestureProcessor m_touchpad;
	bool m_fallbackCursor;
};

#endif // __XR_GAMEPAD_MAPPING__
