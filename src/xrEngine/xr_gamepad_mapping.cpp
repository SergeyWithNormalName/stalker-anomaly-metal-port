#include "xr_gamepad_mapping.h"

#include <algorithm>
#include <cmath>

namespace
{
// Which physical control drives each logical action in gameplay.
struct SGamepadButtonBinding
{
	std::uint32_t bit;
	EGamepadMappedAction action;
};

const SGamepadButtonBinding g_gamepad_button_map[] = {
	{GAMEPAD_BIT_A, GPA_Jump},
	{GAMEPAD_BIT_B, GPA_Crouch},
	{GAMEPAD_BIT_Y, GPA_WeaponFunc},
	{GAMEPAD_BIT_LB, GPA_LeanLeft},
	{GAMEPAD_BIT_RB, GPA_LeanRight},
	{GAMEPAD_BIT_LS, GPA_Sprint},
	{GAMEPAD_BIT_RS, GPA_FireModeNext},
	{GAMEPAD_BIT_START, GPA_Back},
	{GAMEPAD_BIT_BACK, GPA_Torch},
	{GAMEPAD_BIT_DPAD_UP, GPA_DpadUp},
	{GAMEPAD_BIT_DPAD_DOWN, GPA_DpadDown},
	{GAMEPAD_BIT_DPAD_LEFT, GPA_DpadLeft},
	{GAMEPAD_BIT_DPAD_RIGHT, GPA_DpadRight},
};

constexpr float g_gamepad_ms_to_s = 1.0f / 1000.0f;
}

CGamepadMapping::CGamepadMapping()
{
	m_output = 0;
	m_resolver = 0;
	m_needsNeutral = true;
	m_suppressTouchUntilLift = false;
	m_lastContext = GAMEPAD_CONTEXT_SUPPRESSED;
	m_initialised = false;
	m_lookAccumX = m_lookAccumY = 0.0f;
	m_cursorAccumX = m_cursorAccumY = 0.0f;
	m_fallbackCursor = false;
}

void CGamepadMapping::Reset()
{
	ReleaseAll();
}

bool CGamepadMapping::IsHeld(int dik) const
{
	return std::find(m_held.begin(), m_held.end(), dik) != m_held.end();
}

void CGamepadMapping::PressDik(int dik)
{
	if (!dik || !m_output || IsHeld(dik))
		return;

	m_held.push_back(dik);
	m_output->GamepadKeyPress(dik);
}

void CGamepadMapping::ReleaseDik(int dik)
{
	if (!dik)
		return;

	std::vector<int>::iterator it = std::find(m_held.begin(), m_held.end(), dik);
	if (it == m_held.end())
		return;

	m_held.erase(it);

	if (m_output)
		m_output->GamepadKeyRelease(dik);
}

void CGamepadMapping::HoldDik(int dik)
{
	if (!m_output || !IsHeld(dik))
		return;

	m_output->GamepadKeyHold(dik);
}

void CGamepadMapping::ReleaseAll()
{
	if (m_output)
	{
		// Reverse order: the last key pressed is the first one released, which
		// keeps the resulting action transitions in a sensible order.
		for (size_t i = m_held.size(); i > 0; --i)
			m_output->GamepadKeyRelease(m_held[i - 1]);
	}

	m_held.clear();
	m_gyro.Reset();
	m_chordTracker.Reset();
	m_touchpad.Reset();
	m_suppressTouchUntilLift = true;
	for (SNavState& state : m_nav)
		state = SNavState();
	for (SNavState& state : m_scroll)
		state = SNavState();
	m_fallbackCursor = false;

	m_lookAccumX = m_lookAccumY = 0.0f;
	m_cursorAccumX = m_cursorAccumY = 0.0f;

	// Everything that was held was just let go, so the analog controls have to
	// come back to rest before they are trusted again.
	m_needsNeutral = true;
}

int CGamepadMapping::Resolve(EGamepadMappedAction action) const
{
	return m_resolver ? m_resolver(int(action)) : 0;
}

void CGamepadMapping::Process(const SGamepadFrame& frame, const SGamepadSettings& settings,
	float dtMs, EGamepadInputContext context)
{
	// A context switch invalidates everything currently held: the receiver on
	// the other side never saw the presses and would never see the releases.
	// This single rule is what keeps actions from being stuck across the
	// gameplay / interface boundary.
	if (!m_initialised || context != m_lastContext || frame.reset)
		ReleaseAll();

	m_lastContext = context;
	m_initialised = true;

	if (!frame.connected)
		return;

	// Somebody else owns input - the console, the tutorial, the imgui editor -
	// so the gamepad stays out of the way. The release above has already let go
	// of anything it was holding.
	if (context == GAMEPAD_CONTEXT_SUPPRESSED)
		return;

	// Release the latch once the controls that could cause a spurious action
	// have come back to rest.
	if (m_needsNeutral)
	{
		const bool triggersUp = frame.lt < m_config.neutralTrigger && frame.rt < m_config.neutralTrigger;
		const float moveMag = std::sqrt(frame.moveX * frame.moveX + frame.moveY * frame.moveY);
		const float lookMag = std::sqrt(frame.lookX * frame.lookX + frame.lookY * frame.lookY);
		if (triggersUp && moveMag < m_config.neutralStick && lookMag < m_config.neutralStick)
			m_needsNeutral = false;
	}

	if (context == GAMEPAD_CONTEXT_UI)
		ApplyUi(frame, settings, dtMs);
	else
		ApplyGameplay(frame, settings, dtMs);
}

// Whether the touchpad's tap and hold combos are allowed to drive the
// interface. The touchpad still reports its button as a plain input either way.
static bool gamepad_touchpad_actions_enabled(const SGamepadSettings& settings)
{
	return settings.touchpadUi;
}

void CGamepadMapping::ApplyGameplay(const SGamepadFrame& frame, const SGamepadSettings& settings,
	float dtMs)
{
	const float dt = dtMs * g_gamepad_ms_to_s;

	// --- movement ---------------------------------------------------------
	// X-Ray movement is a set of cardinal flags, so the stick supplies the
	// eight directions while its magnitude selects between the game's two
	// existing speeds. Walking is the accelerator action, exactly as the walk
	// key drives it, so how movement is simulated is untouched.
	if (!m_needsNeutral)
	{
		frame.moveFwd ? PressAction(GPA_MoveForward) : ReleaseAction(GPA_MoveForward);
		frame.moveBack ? PressAction(GPA_MoveBack) : ReleaseAction(GPA_MoveBack);
		frame.moveLeft ? PressAction(GPA_MoveLeft) : ReleaseAction(GPA_MoveLeft);
		frame.moveRight ? PressAction(GPA_MoveRight) : ReleaseAction(GPA_MoveRight);

		const bool moving = frame.moveFwd || frame.moveBack || frame.moveLeft || frame.moveRight;
		if (moving && frame.moveTravel < settings.moveWalkBand)
			PressAction(GPA_Walk);
		else
			ReleaseAction(GPA_Walk);
	}
	else
	{
		ReleaseAction(GPA_MoveForward);
		ReleaseAction(GPA_MoveBack);
		ReleaseAction(GPA_MoveLeft);
		ReleaseAction(GPA_MoveRight);
		ReleaseAction(GPA_Walk);
	}

	// --- camera -----------------------------------------------------------
	// This is the engine's own relative look path, reached through the mouse
	// move handler. Nothing is generated at the OS level.
	if (!m_needsNeutral)
	{
		float rateX = m_config.lookRatePx * settings.lookSensX;
		float rateY = m_config.lookRatePx * settings.lookSensY;
		if (frame.ads)
		{
			rateX *= settings.adsLookMult;
			rateY *= settings.adsLookMult;
		}

		// The stick is documented with +Y up, the engine's mouse delta with +Y
		// down, so the vertical axis is negated on the way out. Without this a
		// stick pushed up produces exactly what a physical mouse pushed down
		// produces, which is the inverted look players report.
		m_lookAccumX += frame.lookX * rateX * dt;
		m_lookAccumY -= frame.lookY * rateY * dt;

		// Gyro aim: frame-rate independent additive camera delta
		SGyroConfig gyroCfg = m_gyro.Config();
		gyroCfg.mode = static_cast<EGyroMode>(settings.gyroMode);
		gyroCfg.sensX = settings.gyroSens;
		gyroCfg.sensY = settings.gyroSens;
		gyroCfg.invertY = settings.gyroInvertY;
		gyroCfg.smoothing = settings.gyroSmoothing;
		m_gyro.Configure(gyroCfg);

		const SGyroDelta gyroDelta = m_gyro.Process(frame.gyroX, frame.gyroY, frame.gyroZ, frame.ads, dt);
		m_lookAccumX += gyroDelta.dx;
		m_lookAccumY -= gyroDelta.dy;

		const int dx = int(m_lookAccumX);
		const int dy = int(m_lookAccumY);
		m_lookAccumX -= float(dx);
		m_lookAccumY -= float(dy);

		if ((dx || dy) && m_output)
			m_output->GamepadMouseMove(dx, dy);
	}
	else
	{
		m_lookAccumX = m_lookAccumY = 0.0f;
		m_gyro.Reset();
	}

	// --- triggers ---------------------------------------------------------
	if (!m_needsNeutral)
		frame.ads ? PressAction(GPA_Aim) : ReleaseAction(GPA_Aim);
	else
		ReleaseAction(GPA_Aim);

	if (!m_needsNeutral)
		frame.fire ? PressAction(GPA_Fire) : ReleaseAction(GPA_Fire);
	else
		ReleaseAction(GPA_Fire);

	// --- quicksave / quickload chord tracker -------------------------------
	const bool createHeld = (frame.held & GAMEPAD_BIT_BACK) != 0;
	const bool dpadUpHeld = (frame.held & GAMEPAD_BIT_DPAD_UP) != 0;
	const bool dpadDownHeld = (frame.held & GAMEPAD_BIT_DPAD_DOWN) != 0;
	const EGamepadChordAction chord = m_chordTracker.Update(createHeld, dpadUpHeld, dpadDownHeld, dtMs);
	if (chord != CHORD_NONE && m_output)
		m_output->GamepadChordEvent(int(chord));

	const bool chordActive = createHeld && (dpadUpHeld || dpadDownHeld);

	// --- buttons ----------------------------------------------------------
	// Buttons only act on their edges, so one that was already down when the
	// context changed cannot produce a fresh press on its own.
	for (size_t i = 0; i < sizeof(g_gamepad_button_map) / sizeof(g_gamepad_button_map[0]); ++i)
	{
		if (chordActive && (g_gamepad_button_map[i].bit == GAMEPAD_BIT_BACK ||
		                    g_gamepad_button_map[i].bit == GAMEPAD_BIT_DPAD_UP ||
		                    g_gamepad_button_map[i].bit == GAMEPAD_BIT_DPAD_DOWN))
		{
			continue;
		}

		const int dik = Resolve(g_gamepad_button_map[i].action);
		if (!dik)
			continue;

		if (frame.pressed & g_gamepad_button_map[i].bit)
			PressDik(dik);
		if (frame.released & g_gamepad_button_map[i].bit)
			ReleaseDik(dik);
		if (frame.held & g_gamepad_button_map[i].bit)
			HoldDik(dik);
	}

	// --- square: a tap reloads, holding it uses ----------------------------
	if (!m_needsNeutral)
	{
		switch (frame.squareEvent)
		{
		case GAMEPAD_TAP_HOLD_TAP:
			// Press and release inside one frame; the reload is a one shot.
			PressDik(Resolve(GPA_Reload));
			ReleaseDik(Resolve(GPA_Reload));
			break;
		case GAMEPAD_TAP_HOLD_HOLD_STARTED:
			PressAction(GPA_Use);
			break;
		case GAMEPAD_TAP_HOLD_HOLD_RELEASED:
			ReleaseAction(GPA_Use);
			break;
		default:
			break;
		}
		if (frame.squareEvent == GAMEPAD_TAP_HOLD_HOLD_STARTED)
			HoldDik(Resolve(GPA_Use));
	}
	else
	{
		ReleaseAction(GPA_Use);
	}

	// --- touchpad: a tap opens the PDA, holding it opens the inventory -----
	// The combos can be switched off without losing the button itself, which
	// stays available as a plain input.
	if (!m_needsNeutral && gamepad_touchpad_actions_enabled(settings))
	{
		switch (frame.touchpadEvent)
		{
		case GAMEPAD_TAP_HOLD_TAP:
			// The PDA is opened through the action the keyboard uses, so the
			// script side still decides whether it may open.
			PressDik(Resolve(GPA_Pda));
			ReleaseDik(Resolve(GPA_Pda));
			break;
		case GAMEPAD_TAP_HOLD_HOLD_STARTED:
			PressAction(GPA_Inventory);
			break;
		case GAMEPAD_TAP_HOLD_HOLD_RELEASED:
			ReleaseAction(GPA_Inventory);
			break;
		default:
			break;
		}
		if (frame.touchpadEvent == GAMEPAD_TAP_HOLD_HOLD_STARTED)
			HoldDik(Resolve(GPA_Inventory));
	}
	else
	{
		ReleaseAction(GPA_Inventory);
	}

	// Movement, aim and fire have to be re-asserted every frame, exactly as the
	// keyboard path does: the actor clears its wishful movement state at the
	// end of each frame and rebuilds it from the holds it receives, and the
	// weapon path needs the hold to keep firing.
	HoldDik(Resolve(GPA_MoveForward));
	HoldDik(Resolve(GPA_MoveBack));
	HoldDik(Resolve(GPA_MoveLeft));
	HoldDik(Resolve(GPA_MoveRight));
	HoldDik(Resolve(GPA_Walk));
	HoldDik(Resolve(GPA_Aim));
	HoldDik(Resolve(GPA_Fire));
}

void CGamepadMapping::ApplyUi(const SGamepadFrame& frame, const SGamepadSettings& settings, float dtMs)
{
	const float dt = dtMs * g_gamepad_ms_to_s;

	// Toggle fallback pointer controls with R3 only after the analog controls
	// have returned to rest for this UI context. Release the old accept binding
	// first so a Cross held through the toggle cannot become stuck or transfer
	// its hold into the new mode.
	if (!m_needsNeutral && (frame.pressed & GAMEPAD_BIT_RS))
	{
		ReleaseDik(m_config.uiAccept);
		ReleaseDik(m_config.uiPointerClick);
		ToggleFallbackCursor();
	}
	const int accept = m_fallbackCursor ? m_config.uiPointerClick : m_config.uiAccept;

	// Discrete shortcuts and analog triggers are useful only after the neutral
	// latch has established a fresh baseline for this context.
	if (!m_needsNeutral && m_output)
	{
		if (frame.pressed & GAMEPAD_BIT_X)
			m_output->GamepadUiAction(UI_ACTION_CONTEXT_MENU);
		if (frame.pressed & GAMEPAD_BIT_Y)
			m_output->GamepadUiAction(UI_ACTION_TRANSFER);
		if (frame.pressed & GAMEPAD_BIT_LB)
			m_output->GamepadUiAction(UI_ACTION_TAB_PREV);
		if (frame.pressed & GAMEPAD_BIT_RB)
			m_output->GamepadUiAction(UI_ACTION_TAB_NEXT);
		if (frame.adsPressed)
			m_output->GamepadUiAction(UI_ACTION_PANEL_PREV);
		if (frame.firePressed)
			m_output->GamepadUiAction(UI_ACTION_PANEL_NEXT);
	}

	// --- touchpad gestures in UI ------------------------------------------
	STouchGestureResult touch;
	if (!settings.touchpadUi || m_needsNeutral || m_suppressTouchUntilLift)
	{
		// A touch begun under the previous dialog/context must not turn into an
		// accept on release under its new owner.
		m_touchpad.Reset();
		if (!frame.touchActive && settings.touchpadUi && !m_needsNeutral)
			m_suppressTouchUntilLift = false;
	}
	else
	{
		touch = m_touchpad.Process(
			frame.touchActive, frame.touchX, frame.touchY,
			frame.touchSecondaryActive, frame.touchSecondaryX, frame.touchSecondaryY, dtMs);
	}

	if (touch.tap && m_output && (!m_config.uiMapPan || m_fallbackCursor))
	{
		PressDik(accept);
		ReleaseDik(accept);
	}
	if (touch.mode == GESTURE_MODE_PAN && m_output)
	{
		if (m_config.uiMapPan && !m_fallbackCursor)
			m_output->GamepadMapPan(touch.panDx * m_config.touchMapPanScale,
				touch.panDy * m_config.touchMapPanScale);
		else if (m_fallbackCursor)
			m_output->GamepadCursorMove(touch.panDx * m_config.cursorRate, touch.panDy * m_config.cursorRate);
	}
	else if (touch.mode == GESTURE_MODE_PINCH && m_output && touch.zoomDelta != 0.0f)
	{
		if (m_config.uiMapPan && !m_fallbackCursor)
			m_output->GamepadMapZoom(touch.zoomDelta > 0.0f);
		else
			m_output->GamepadMouseWheel(touch.zoomDelta > 0.0f ? 1 : -1);
	}

	// --- navigation -------------------------------------------------------
	// A held direction becomes a stream of arrow key taps, which is what the
	// menus are written to listen for. The keys are never left down between
	// repeats, so a menu that only handles presses still sees every step.
	const bool dirDown[4] = {
		!m_needsNeutral && (frame.moveFwd || (frame.held & GAMEPAD_BIT_DPAD_UP) != 0),
		!m_needsNeutral && (frame.moveBack || (frame.held & GAMEPAD_BIT_DPAD_DOWN) != 0),
		!m_needsNeutral && (frame.moveLeft || (frame.held & GAMEPAD_BIT_DPAD_LEFT) != 0),
		!m_needsNeutral && (frame.moveRight || (frame.held & GAMEPAD_BIT_DPAD_RIGHT) != 0),
	};
	const int dirDik[4] = {m_config.uiNavUp, m_config.uiNavDown, m_config.uiNavLeft, m_config.uiNavRight};

	for (int i = 0; i < 4; ++i)
	{
		SNavState& s = m_nav[i];

		if (!dirDown[i])
		{
			s.wasDown = false;
			s.heldMs = 0.0f;
			s.repeated = false;
			ReleaseDik(dirDik[i]);
			continue;
		}

		bool tap = false;
		if (!s.wasDown)
		{
			s.wasDown = true;
			s.heldMs = 0.0f;
			s.repeated = false;
			tap = true;
		}
		else
		{
			s.heldMs += dtMs;
			const float interval = s.repeated ? m_config.navRepeatMs : m_config.navDelayMs;
			if (s.heldMs >= interval)
			{
				s.heldMs = 0.0f;
				s.repeated = true;
				tap = true;
			}
		}

		if (tap)
		{
			PressDik(dirDik[i]);
			ReleaseDik(dirDik[i]);
		}
	}

	// In focus mode the right stick scrolls by discrete steps instead of
	// displacing the virtual pointer. The same delay/repeat timing as directional
	// navigation keeps a held stick useful without flooding wheel events.
	const bool scrollDown[2] = {
		!m_needsNeutral && !m_fallbackCursor && !m_config.uiMapPan && frame.lookY >= m_config.uiScrollThreshold,
		!m_needsNeutral && !m_fallbackCursor && !m_config.uiMapPan && frame.lookY <= -m_config.uiScrollThreshold,
	};
	const int scrollAction[2] = {UI_ACTION_SCROLL_UP, UI_ACTION_SCROLL_DOWN};
	for (int i = 0; i < 2; ++i)
	{
		SNavState& s = m_scroll[i];
		if (!scrollDown[i])
		{
			s = SNavState();
			continue;
		}

		bool scroll = false;
		if (!s.wasDown)
		{
			s.wasDown = true;
			s.heldMs = 0.0f;
			s.repeated = false;
			scroll = true;
		}
		else
		{
			s.heldMs += dtMs;
			const float interval = s.repeated ? m_config.navRepeatMs : m_config.navDelayMs;
			if (s.heldMs >= interval)
			{
				s.heldMs = 0.0f;
				s.repeated = true;
				scroll = true;
			}
		}
		if (scroll && m_output)
			m_output->GamepadUiAction(scrollAction[i]);
	}

	// --- accept / back ----------------------------------------------------
	// These need no neutral latch: a press edge cannot come from a control that
	// was already held when the context changed.
	if (frame.pressed & GAMEPAD_BIT_A)
		PressDik(accept);
	if (frame.released & GAMEPAD_BIT_A)
		ReleaseDik(accept);
	if (frame.held & GAMEPAD_BIT_A)
		HoldDik(accept);

	if (frame.pressed & GAMEPAD_BIT_B)
		PressAction(GPA_Back);
	if (frame.released & GAMEPAD_BIT_B)
		ReleaseAction(GPA_Back);
	if (frame.pressed & GAMEPAD_BIT_START)
		PressAction(GPA_Back);
	if (frame.released & GAMEPAD_BIT_START)
		ReleaseAction(GPA_Back);

	// --- virtual cursor ---------------------------------------------------
	// Positioned inside X-Ray's own cursor object through the same absolute
	// setter the dialogs already use. No OS level pointer event is produced.
	if (!m_needsNeutral && m_config.uiMapPan && !m_fallbackCursor)
	{
		// The stick moves the view; a finger drag above moves the map content.
		if (m_output && (frame.lookX != 0.0f || frame.lookY != 0.0f))
			m_output->GamepadMapPan(-frame.lookX * m_config.mapPanRate * dt,
				frame.lookY * m_config.mapPanRate * dt);
		m_cursorAccumX = m_cursorAccumY = 0.0f;
		return;
	}
	if (m_needsNeutral || !m_fallbackCursor)
	{
		m_cursorAccumX = m_cursorAccumY = 0.0f;
		return;
	}

	// The cursor lives in screen coordinates too, so its vertical axis is
	// flipped the same way the camera's is.
	m_cursorAccumX += frame.lookX * m_config.cursorRate * dt;
	m_cursorAccumY -= frame.lookY * m_config.cursorRate * dt;

	const int dx = int(m_cursorAccumX);
	const int dy = int(m_cursorAccumY);
	m_cursorAccumX -= float(dx);
	m_cursorAccumY -= float(dy);

	if ((dx || dy) && m_output)
		m_output->GamepadCursorMove(float(dx), float(dy));
}
