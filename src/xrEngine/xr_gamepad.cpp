#include "stdafx.h"
#pragma hdrstop

#include "xr_gamepad.h"
#include "device.h"

CGamepadManager* pGamepad = NULL;

ENGINE_API BOOL psGamepadEnabled = TRUE;
ENGINE_API float psGamepadMoveDeadzone = 0.18f;
ENGINE_API float psGamepadLookDeadzone = 0.15f;
ENGINE_API float psGamepadLookSens = 1.0f;
ENGINE_API float psGamepadLookSensVertical = 1.0f;
ENGINE_API float psGamepadAdsLookMult = 0.45f;
ENGINE_API BOOL psGamepadInvertY = FALSE;
ENGINE_API float psGamepadTriggerThreshold = 0.35f;
ENGINE_API float psGamepadResponse = 1.35f;
ENGINE_API float psGamepadHapticStrength = 1.0f;
ENGINE_API float psGamepadAdaptiveStrength = 0.6f;
ENGINE_API BOOL psGamepadTouchpadUi = TRUE;
ENGINE_API BOOL psGamepadDebug = FALSE;

#if defined(XR_PLATFORM_APPLE)
extern IGamepadBackend* CreateAppleGamepadBackend();
#endif

namespace
{
float gamepad_debug_accum_ms = 0.0f;
constexpr float gamepad_debug_interval_ms = 500.0f;
}

CGamepadManager::CGamepadManager()
{
	m_backend = NULL;
	m_sink = NULL;
	m_started = false;
	m_wasConnected = false;
	m_loggedFirstConnect = false;
	m_lastAdaptiveState = -1;

	RefreshSettings();
}

CGamepadManager::~CGamepadManager()
{
	if (m_backend)
	{
		m_backend->ResetEffects();
		if (m_started)
			m_backend->Stop();
		xr_delete(m_backend);
	}
}

CGamepadManager* Gamepad()
{
	if (!pGamepad)
		pGamepad = xr_new<CGamepadManager>();
	return pGamepad;
}

void CGamepadManager::RefreshSettings()
{
	SGamepadSettings& s = m_settings;

	s.enabled = !!psGamepadEnabled;

	s.moveDeadzone = psGamepadMoveDeadzone;
	s.moveResponse = psGamepadResponse;

	s.lookDeadzone = psGamepadLookDeadzone;
	s.lookResponse = psGamepadResponse;
	s.lookSensX = psGamepadLookSens;
	s.lookSensY = psGamepadLookSensVertical;
	s.adsLookMult = psGamepadAdsLookMult;
	s.invertY = !!psGamepadInvertY;

	s.triggerThreshold = psGamepadTriggerThreshold;
	s.triggerHysteresis = 0.10f;

	s.hapticsEnabled = psGamepadHapticStrength > 0.0f;
	s.hapticsStrength = psGamepadHapticStrength;
	s.adaptiveEnabled = psGamepadAdaptiveStrength > 0.0f;
	s.adaptiveStrength = psGamepadAdaptiveStrength;

	s.touchpadUi = !!psGamepadTouchpadUi;
}

void CGamepadManager::SetSink(IGamepadInputSink* sink)
{
	m_sink = sink;
}

void CGamepadManager::DetectDevice()
{
	const bool connected = m_caps.connected;

	if (connected && !m_wasConnected)
	{
		m_processor.Reset();
		if (!m_loggedFirstConnect)
		{
			m_loggedFirstConnect = true;
			LogDiagnostics(false);
		}
	}
	else if (!connected && m_wasConnected)
	{
		// The pads are gone. Tell the game side to let go of everything before
		// anything else happens, otherwise the actor keeps running into a wall.
		m_processor.Reset();
		ResetEffects();
		Msg("~ [gamepad] disconnected");
	}

	m_wasConnected = connected;
}

void CGamepadManager::OnFrame(float dtMs)
{
	if (!m_backend)
	{
#if defined(XR_PLATFORM_APPLE)
		m_backend = CreateAppleGamepadBackend();
#endif
		if (!m_backend)
			return;

		if (!m_backend->Start())
		{
			Msg("! [gamepad] Apple GameController init failed; controller input unavailable");
			xr_delete(m_backend);
			m_backend = NULL;
			return;
		}
		m_started = true;
	}

	RefreshSettings();

	m_backend->Poll();
	m_backend->Snapshot(m_raw, m_caps, m_info);
	DetectDevice();

	SGamepadCapabilities process_caps = m_caps;

	// Reasons to hand the processor an absent device instead of the real one:
	// the feature has been switched off, the application is in the background,
	// or a level is precaching. A backgrounded application gets no controller
	// input on modern macOS, so the last polled values would otherwise stay
	// frozen with the stick held and the actor would keep running; and the
	// keyboard and mouse paths both drop input during precache, so the gamepad
	// must not be the one device that acts on a half built level.
	//
	// Either way the device is still polled above so the backend never
	// accumulates a backlog, and the processor emits exactly one frame carrying
	// the release edges.
	if (!m_settings.enabled || Device.b_is_Active == FALSE || Device.dwPrecacheFrame != 0)
		process_caps.Reset();

	const SGamepadFrame& frame = m_processor.Process(m_raw, process_caps, m_settings, dtMs);

	if (m_sink && (frame.connected || frame.reset || psGamepadDebug))
		m_sink->OnGamepadFrame(frame, dtMs);

	if (psGamepadDebug)
	{
		gamepad_debug_accum_ms += dtMs;
		if (gamepad_debug_accum_ms >= gamepad_debug_interval_ms)
		{
			gamepad_debug_accum_ms = 0.0f;
			LogDiagnostics(true);
		}
	}
}

void CGamepadManager::OnFocusLost()
{
	m_processor.Reset();
	ResetEffects();

	if (m_sink)
		m_sink->OnGamepadReset();
}

void CGamepadManager::SetAdaptiveTrigger(EGamepadHand hand, EGamepadTriggerEffect effect, float strength)
{
	if (!m_backend)
		return;

	if (!m_caps.adaptiveTriggers || !m_settings.adaptiveEnabled)
	{
		if (m_lastAdaptiveState != 0)
		{
			m_lastAdaptiveState = 0;
			m_backend->SetAdaptiveTrigger(GAMEPAD_HAND_LEFT, GAMEPAD_TRIGGER_OFF, 0.0f);
			m_backend->SetAdaptiveTrigger(GAMEPAD_HAND_RIGHT, GAMEPAD_TRIGGER_OFF, 0.0f);
		}
		return;
	}

	m_lastAdaptiveState = 1;
	m_backend->SetAdaptiveTrigger(hand, effect, strength);
}

void CGamepadManager::PlayHaptic(float amplitude, float durationSec)
{
	if (!m_backend || !m_caps.haptics || !m_settings.hapticsEnabled)
		return;

	const float a = amplitude * m_settings.hapticsStrength;
	if (a <= 0.0f || durationSec <= 0.0f)
		return;

	m_backend->PlayHaptic(a > 1.0f ? 1.0f : a, durationSec);
}

void CGamepadManager::SetLight(float r, float g, float b)
{
	if (!m_backend || !m_caps.light)
		return;

	m_backend->SetLight(r, g, b);
}

void CGamepadManager::ResetEffects()
{
	m_lastAdaptiveState = 0;
	if (m_backend)
		m_backend->ResetEffects();
}

void CGamepadManager::LogDiagnostics(bool verbose) const
{
	if (!m_caps.connected)
	{
		Msg("* [gamepad] no controller connected");
		return;
	}

	Msg("* [gamepad] controller: vendor=\"%s\" category=\"%s\" profile=\"%s\" transport=\"%s\" index=%d",
		m_info.vendorName, m_info.productCategory, m_info.profileName, m_info.transport, m_info.playerIndex);
	Msg("* [gamepad] extended=%d dualsense=%d haptics=%d adaptive=%d light=%d motion=%d touchpad=%d",
		m_caps.extended ? 1 : 0, m_caps.dualsense ? 1 : 0, m_caps.haptics ? 1 : 0,
		m_caps.adaptiveTriggers ? 1 : 0, m_caps.light ? 1 : 0, m_caps.motion ? 1 : 0,
		m_caps.touchpad ? 1 : 0);

	if (verbose)
	{
		Msg("* [gamepad] l=(%.3f, %.3f) r=(%.3f, %.3f) lt=%.3f rt=%.3f buttons=0x%04x",
			m_raw.lx, m_raw.ly, m_raw.rx, m_raw.ry, m_raw.lt, m_raw.rt, unsigned(m_raw.buttons));
	}
}
