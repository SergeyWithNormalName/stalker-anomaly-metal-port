#include "xr_gamepad_processor.h"

#include <cmath>
#include <algorithm>

namespace
{
constexpr float g_gamepad_epsilon = 1.0e-5f;

inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
}

float GamepadApplyStick(float x, float y, float deadzone, float exponent, float& outX, float& outY)
{
	deadzone = clampf(deadzone, 0.0f, 0.6f);
	if (exponent <= 0.0f)
		exponent = 1.0f;

	const float length = std::sqrt(x * x + y * y);
	if (length <= g_gamepad_epsilon || length <= deadzone)
	{
		outX = 0.0f;
		outY = 0.0f;
		return 0.0f;
	}

	// Rescale the travel that remains outside the dead zone so the stick still
	// reaches full deflection, then shape it. Dividing by the original length
	// before rebuilding the vector is what makes diagonals consistent: a
	// diagonal is worth exactly the same as a cardinal at the same distance.
	const float normalised = clampf((length - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
	const float shaped = (exponent == 1.0f) ? normalised : std::pow(normalised, exponent);
	const float inv = shaped / length;

	outX = x * inv;
	outY = y * inv;
	return shaped;
}

bool GamepadTriggerDigital(bool previous, float value, float threshold, float hysteresis)
{
	threshold = clampf(threshold, 0.05f, 0.95f);
	hysteresis = clampf(hysteresis, 0.0f, 0.4f);

	// The release point has to stay strictly below the press point, and it also
	// has to stay clear of zero: a trigger at rest reports a small residual
	// value, so a release point of zero would never be crossed and the trigger
	// would latch on forever. Half the threshold keeps a usable band whatever
	// the settings are.
	const float releaseAt = clampf(threshold - hysteresis, threshold * 0.5f, threshold);

	if (previous)
		return value > releaseAt;

	return value >= threshold;
}

CGamepadProcessor::CGamepadProcessor()
{
	m_prevButtons = 0;
	m_prevAds = false;
	m_prevFire = false;
	m_pendingReset = false;
	m_wasConnected = false;
	m_frame.Reset();
}

void CGamepadProcessor::Reset()
{
	m_pendingReset = true;
}

const SGamepadFrame& CGamepadProcessor::Process(const SRawGamepadState& raw,
	const SGamepadCapabilities& caps, const SGamepadSettings& settings, float dtMs)
{
	if (dtMs < 0.0f || dtMs > 1000.0f)
		dtMs = 16.0f;

	SGamepadFrame& f = m_frame;

	const bool reset = m_pendingReset;
	m_pendingReset = false;

	f.connected = caps.connected;
	f.extended = caps.extended;
	f.dualsense = caps.dualsense;
	f.reset = reset;

	if (!caps.connected)
	{
		// Nothing is connected. Report one frame carrying `reset` so the
		// consumer releases whatever it still believes is held, then go quiet:
		// repeating it every frame would be pointless traffic.
		const bool wasConnected = m_wasConnected;
		m_wasConnected = false;

		f.Reset();
		f.connected = false;
		f.reset = wasConnected || reset;

		m_prevButtons = 0;
		m_prevAds = false;
		m_prevFire = false;
		m_square.Reset();
		m_touchpad.Reset();
		return f;
	}

	m_wasConnected = true;

	// --- sticks ------------------------------------------------------------
	float mx = 0.0f, my = 0.0f;
	const float moveMag = GamepadApplyStick(raw.lx, raw.ly, settings.moveDeadzone, settings.moveResponse, mx, my);

	float lx = 0.0f, ly = 0.0f;
	GamepadApplyStick(raw.rx, raw.ry, settings.lookDeadzone, settings.lookResponse, lx, ly);
	if (settings.invertY)
		ly = -ly;

	f.moveX = mx;
	f.moveY = my;
	f.moveMagnitude = moveMag;

	// Cardinal directions come from the same radial dead zone, evaluated
	// linearly so the response curve cannot push the engage point around. The
	// length of that same vector is the travel the walk / run split is judged
	// on, so the two always agree about how far the stick has been pushed.
	{
		float dirX = 0.0f, dirY = 0.0f;
		f.moveTravel = GamepadApplyStick(raw.lx, raw.ly, settings.moveDeadzone, 1.0f, dirX, dirY);

		const float t = settings.moveDirThreshold;
		f.moveRight = dirX > t;
		f.moveLeft = dirX < -t;
		f.moveFwd = dirY > t;
		f.moveBack = dirY < -t;
	}

	f.lookX = lx;
	f.lookY = ly;

	// --- triggers ----------------------------------------------------------
	f.lt = clampf(raw.lt, 0.0f, 1.0f);
	f.rt = clampf(raw.rt, 0.0f, 1.0f);

	const bool ads = GamepadTriggerDigital(m_prevAds, f.lt, settings.triggerThreshold, settings.triggerHysteresis);
	const bool fire = GamepadTriggerDigital(m_prevFire, f.rt, settings.triggerThreshold, settings.triggerHysteresis);

	f.ads = ads;
	f.fire = fire;
	// A reset frame establishes a new baseline. Controls may have changed while
	// input was suspended, but that change must not become an action when input
	// resumes.
	f.adsPressed = !reset && ads && !m_prevAds;
	f.adsReleased = !reset && !ads && m_prevAds;
	f.firePressed = !reset && fire && !m_prevFire;
	f.fireReleased = !reset && !fire && m_prevFire;
	m_prevAds = ads;
	m_prevFire = fire;

	// --- buttons -----------------------------------------------------------
	const std::uint32_t buttons = raw.buttons & GAMEPAD_BUTTON_MASK;
	f.pressed = reset ? 0 : buttons & ~m_prevButtons;
	f.released = reset ? 0 : m_prevButtons & ~buttons;
	f.held = buttons;
	m_prevButtons = buttons;

	f.touchActive = raw.touchActive;
	f.touchX = raw.touchX;
	f.touchY = raw.touchY;
	f.touchSecondaryActive = raw.touchSecondaryActive;
	f.touchSecondaryX = raw.touchSecondaryX;
	f.touchSecondaryY = raw.touchSecondaryY;
	f.touchPressed = (f.pressed & GAMEPAD_BIT_TOUCHPAD) != 0;
	f.gyroX = raw.gyroX;
	f.gyroY = raw.gyroY;
	f.gyroZ = raw.gyroZ;

	// --- tap / hold --------------------------------------------------------
	// A reset abandons any press in flight without reporting a tap: the user
	// did not complete the gesture, so acting on it would be wrong.
	if (reset)
	{
		m_square.Reset();
		m_touchpad.Reset();
		f.squareEvent = GAMEPAD_TAP_HOLD_NONE;
		f.touchpadEvent = GAMEPAD_TAP_HOLD_NONE;
	}
	else
	{
		f.squareEvent = m_square.Update((buttons & GAMEPAD_BIT_X) != 0, dtMs, settings.tapHoldMs);
		f.touchpadEvent = m_touchpad.Update((buttons & GAMEPAD_BIT_TOUCHPAD) != 0, dtMs, settings.tapHoldMs);
	}

	f.squareHolding = m_square.IsHolding();
	f.touchpadHolding = m_touchpad.IsHolding();

	return f;
}
