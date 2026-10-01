#ifndef __XR_GAMEPAD_PROCESSOR__
#define __XR_GAMEPAD_PROCESSOR__
#pragma once

// Pure state processing for the gamepad layer: dead zones, response curves,
// trigger hysteresis, edge detection and tap/hold disambiguation.
//
// Nothing here touches the engine, the platform APIs or global state, so every
// function and every tracker can be exercised by a standalone unit test with no
// controller attached.

#include "xr_gamepad_types.h"

// Tunables, fed from console variables by the engine side.
struct SGamepadSettings
{
	SGamepadSettings()
	{
		enabled = true;

		moveDeadzone = 0.18f;
		moveResponse = 1.35f;
		moveDirThreshold = 0.15f;
		moveWalkBand = 0.50f;

		lookDeadzone = 0.15f;
		lookResponse = 2.0f;
		lookSensX = 1.0f;
		lookSensY = 1.0f;
		adsLookMult = 0.45f;
		invertY = false;

		triggerThreshold = 0.35f;
		triggerHysteresis = 0.10f;

		tapHoldMs = 220;

		hapticsEnabled = true;
		hapticsStrength = 1.0f;
		adaptiveEnabled = true;
		adaptiveStrength = 0.6f;

		touchpadUi = true;
		uiCursorSpeed = 1.0f;

		gyroMode = 1; // 0 off, 1 ADS only, 2 always
		gyroSens = 1.0f;
		gyroInvertY = false;
		gyroSmoothing = 0.35f;
	}

	bool enabled;

	float moveDeadzone;  // radial, [0, 0.6]
	float moveResponse;  // >0, 1 = linear
	float moveDirThreshold; // how far the stick must leave the dead zone before
	                        // a digital direction is reported, on the rescaled
	                        // 0..1 travel; 0.15 engages at roughly a third of
	                        // full deflection
	float moveWalkBand;     // below this travel the action path also holds
	                        // "walk", above it the actor runs

	float lookDeadzone;  // radial, [0, 0.6]
	float lookResponse;  // >0, 1 = linear
	float lookSensX;
	float lookSensY;
	float adsLookMult; // look rate multiplier while aiming
	bool invertY;

	float triggerThreshold;  // [0.05, 0.95]
	float triggerHysteresis; // [0, 0.4], subtracted when releasing

	int tapHoldMs; // >= 0

	bool hapticsEnabled;
	float hapticsStrength; // [0, 2]
	bool adaptiveEnabled;
	float adaptiveStrength; // [0, 1]

	bool touchpadUi;
	float uiCursorSpeed;

	int gyroMode;
	float gyroSens;
	bool gyroInvertY;
	float gyroSmoothing;
};

// Disambiguates a short tap from a held press. A tap is only reported on
// release, because until the hold deadline elapses the intent is genuinely
// ambiguous. Callers that need instant feedback for the tap action must accept
// that delay; the alternative is firing both actions.
enum EGamepadTapHoldEvent
{
	GAMEPAD_TAP_HOLD_NONE = 0,
	GAMEPAD_TAP_HOLD_TAP,          // released before the hold deadline
	GAMEPAD_TAP_HOLD_HOLD_STARTED, // still down after the hold deadline
	GAMEPAD_TAP_HOLD_HOLD_RELEASED // the owner of a started hold let go
};

// One processed frame. "Rate" values are per second; callers scale them by the
// frame delta so behaviour does not change with frame rate.
struct SGamepadFrame
{
	SGamepadFrame()
	{
		Reset();
	}

	void Reset()
	{
		connected = false;
		reset = false;
		extended = false;
		dualsense = false;
		moveX = moveY = moveMagnitude = moveTravel = 0.0f;
		moveFwd = moveBack = moveLeft = moveRight = false;
		lookX = lookY = 0.0f;
		lt = rt = 0.0f;
		ads = fire = false;
		adsPressed = adsReleased = false;
		firePressed = fireReleased = false;
		pressed = released = held = 0;
		touchActive = false;
		touchX = touchY = 0.0f;
		touchSecondaryActive = false;
		touchSecondaryX = touchSecondaryY = 0.0f;
		touchPressed = false;
		gyroX = gyroY = gyroZ = 0.0f;
		squareEvent = GAMEPAD_TAP_HOLD_NONE;
		touchpadEvent = GAMEPAD_TAP_HOLD_NONE;
		squareHolding = false;
		touchpadHolding = false;
	}

	bool connected;

	// Set when the frame follows a disconnect, focus loss or context switch.
	// Consumers must release everything they currently hold.
	bool reset;

	bool extended;
	bool dualsense;

	// Left stick after radial dead zone, response curve and length clamp.
	float moveX;         // -1..1, +X right
	float moveY;         // -1..1, +Y forward
	float moveMagnitude; // 0..1, after the response curve

	// How far the stick has travelled out of the dead zone, 0..1, before the
	// response curve is applied. This is the honest "how hard is the player
	// pushing" number, which is what a walk / run split should be judged on:
	// the curved magnitude squeezes the low half of the travel towards zero and
	// would put the split unrealistically close to full deflection.
	float moveTravel;

	// Digital direction for the action based movement path. X-Ray's movement
	// wants cardinal flags, so these are derived here, from the same dead zone,
	// rather than re-derived by the game side with a second threshold.
	bool moveFwd, moveBack, moveLeft, moveRight;

	// Right stick after radial dead zone and response curve. Multiply by the
	// frame delta to get radians/pixels of rotation for this frame.
	float lookX; // -1..1, +X right
	float lookY; // -1..1, +Y up (already flipped when invertY is set)

	float lt; // analog, always raw 0..1 so future features keep the magnitude
	float rt;

	bool ads;  // digital, hysteresis applied
	bool fire; // digital, hysteresis applied

	bool adsPressed, adsReleased;
	bool firePressed, fireReleased;

	std::uint32_t pressed;  // rising edges this frame
	std::uint32_t released; // falling edges this frame
	std::uint32_t held;     // currently down

	bool touchActive;
	float touchX, touchY;
	bool touchSecondaryActive;
	float touchSecondaryX, touchSecondaryY;
	bool touchPressed; // touchpad click rising edge

	float gyroX, gyroY, gyroZ;

	// Combo buttons. The tap/hold decision is made here so every consumer gets
	// the same answer; `*Holding` is the live state of a begun hold.
	EGamepadTapHoldEvent squareEvent;   // Square tap = reload, hold = use
	EGamepadTapHoldEvent touchpadEvent; // touchpad tap = PDA, hold = inventory
	bool squareHolding;
	bool touchpadHolding;
};

// --- Pure helpers -----------------------------------------------------------

// Radial dead zone with direction preservation and diagonal normalisation.
// The remaining travel is rescaled to [0, 1] and shaped by `exponent`
// (1 = linear, >1 = finer control near the centre). Returns the output length.
float GamepadApplyStick(float x, float y, float deadzone, float exponent,
	float& outX, float& outY);

// Schmitt trigger for an analog value promoted to a digital button.
bool GamepadTriggerDigital(bool previous, float value, float threshold, float hysteresis);

// --- Tap / hold -------------------------------------------------------------

// The caller feeds the raw button state every frame and reacts to the returned
// event.
class CTapHoldTracker
{
public:
	CTapHoldTracker()
	{
		Reset();
	}

	// Clears the state without emitting anything. Use on disconnect / focus loss.
	void Reset()
	{
		m_down = false;
		m_heldLongEnough = false;
		m_elapsedMs = 0;
	}

	// `dtMs` must come from a monotonic clock. `holdMs <= 0` makes every press a
	// hold and no press ever a tap.
	EGamepadTapHoldEvent Update(bool down, float dtMs, int holdMs)
	{
		if (down)
		{
			if (!m_down)
			{
				m_down = true;
				m_heldLongEnough = false;
				m_elapsedMs = 0.0f;
			}

			if (!m_heldLongEnough)
			{
				m_elapsedMs += dtMs;
				if (holdMs <= 0 || m_elapsedMs >= float(holdMs))
				{
					m_heldLongEnough = true;
					return GAMEPAD_TAP_HOLD_HOLD_STARTED;
				}
			}
			return GAMEPAD_TAP_HOLD_NONE;
		}

		if (!m_down)
			return GAMEPAD_TAP_HOLD_NONE;

		m_down = false;
		if (m_heldLongEnough)
		{
			m_heldLongEnough = false;
			return GAMEPAD_TAP_HOLD_HOLD_RELEASED;
		}

		m_elapsedMs = 0.0f;
		return GAMEPAD_TAP_HOLD_TAP;
	}

	bool IsDown() const { return m_down; }
	bool IsHolding() const { return m_down && m_heldLongEnough; }

private:
	bool m_down;
	bool m_heldLongEnough;
	float m_elapsedMs;
};

// --- Frame processor --------------------------------------------------------

// Owns the previous-frame state needed for edges, hysteresis and tap/hold.
class CGamepadProcessor
{
public:
	CGamepadProcessor();

	// Forces the next Process() call to report a reset and clears all edges.
	// Call on disconnect, focus loss and context switches.
	void Reset();

	// `dtMs` is the frame delta from a monotonic clock.
	const SGamepadFrame& Process(const SRawGamepadState& raw, const SGamepadCapabilities& caps,
		const SGamepadSettings& settings, float dtMs);

	const SGamepadFrame& Frame() const { return m_frame; }

private:
	SGamepadFrame m_frame;
	std::uint32_t m_prevButtons;
	bool m_prevAds;
	bool m_prevFire;
	CTapHoldTracker m_square;
	CTapHoldTracker m_touchpad;
	bool m_pendingReset;
	bool m_wasConnected;
};

#endif // __XR_GAMEPAD_PROCESSOR__
