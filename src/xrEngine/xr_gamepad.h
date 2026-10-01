#ifndef __XR_GAMEPAD__
#define __XR_GAMEPAD__
#pragma once

// Engine side gamepad support.
//
// Layout: a platform backend (Apple GameController on macOS) fills a raw
// snapshot on the engine thread; CGamepadProcessor turns that into a frame of
// digital button edges and analog values; a single IGamepadInputSink supplied
// by the game layer maps the frame onto X-Ray actions. Nothing here talks to
// the game layer directly, so the engine keeps no dependency on xrGame.

#include "xr_gamepad_types.h"
#include "xr_gamepad_processor.h"

class ENGINE_API IGamepadBackend
{
public:
	virtual ~IGamepadBackend() {}

	// One time setup. Returns false when the platform offers no usable API.
	virtual bool Start() = 0;
	virtual void Stop() = 0;

	// Refresh the device list and drain queued native events. Called once per
	// frame on the engine thread; must be cheap and must never block.
	virtual void Poll() = 0;

	// Copy the latest state. Called on the engine thread immediately after
	// Poll(), so the snapshot and the events describe the same moment.
	virtual void Snapshot(SRawGamepadState& raw, SGamepadCapabilities& caps,
		SGamepadDeviceInfo& info) const = 0;

	// Optional effects. Every one of these must be a safe no-op on a device or
	// transport that does not support it.
	virtual void SetAdaptiveTrigger(EGamepadHand hand, EGamepadTriggerEffect effect, float strength) = 0;
	virtual void PlayHaptic(float amplitude, float durationSec) = 0;
	virtual void SetLight(float r, float g, float b) = 0;
	virtual void ResetEffects() = 0;
};

// Implemented by the game layer. Called from CInput::OnFrame, so the receiver
// stack, the UI and the scripting callbacks are all in their normal state.
class ENGINE_API IGamepadInputSink
{
public:
	virtual ~IGamepadInputSink() {}

	// `dtMs` is the frame delta used to process the frame, from a monotonic
	// clock. Implementations must scale their per-second rates by it.
	virtual void OnGamepadFrame(const SGamepadFrame& frame, float dtMs) = 0;

	// Called synchronously when focus is lost or the pad goes away, while the
	// receiver stack is still intact. Implementations must let go of every
	// action they hold. The next frame also carries a reset, so this only
	// exists to make the release immediate rather than one frame late.
	virtual void OnGamepadReset() = 0;
};

class ENGINE_API CGamepadManager
{
public:
	CGamepadManager();
	~CGamepadManager();

	// The game layer installs its sink once, during start up.
	void SetSink(IGamepadInputSink* sink);

	// Called every frame from CInput::OnFrame, on the engine thread.
	void OnFrame(float dtMs);

	// Focus loss, pause, disconnect. Releases everything the sink holds and
	// switches every device effect off so nothing is left running.
	void OnFocusLost();

	bool IsSupported() const { return m_backend != 0; }

	const SGamepadFrame& Frame() const { return m_processor.Frame(); }
	const SGamepadCapabilities& Capabilities() const { return m_caps; }
	const SGamepadDeviceInfo& DeviceInfo() const { return m_info; }
	const SGamepadSettings& Settings() const { return m_settings; }

	// Pulls the console variable values into the settings block. Cheap.
	void RefreshSettings();

	// Effects, gated on capabilities. Safe to call at any time.
	void SetAdaptiveTrigger(EGamepadHand hand, EGamepadTriggerEffect effect, float strength);
	void PlayHaptic(float amplitude, float durationSec);
	void SetLight(float r, float g, float b);
	void ResetEffects();

	// One line summary, emitted at connect and when the diagnostic variable is
	// enabled. Never called per frame during normal play.
	void LogDiagnostics(bool verbose) const;

	bool IsConnected() const { return m_caps.connected; }

private:
	void DetectDevice();

	IGamepadBackend* m_backend;
	IGamepadInputSink* m_sink;
	CGamepadProcessor m_processor;
	SGamepadSettings m_settings;
	SGamepadCapabilities m_caps;
	SGamepadDeviceInfo m_info;
	SRawGamepadState m_raw;

	bool m_started;
	bool m_wasConnected;
	bool m_loggedFirstConnect;
	int m_lastAdaptiveState; // -1 unknown, 0 off, 1 on
};

// Null until the first frame, so the engine can boot without a controller.
extern ENGINE_API CGamepadManager* pGamepad;

ENGINE_API CGamepadManager* Gamepad();

// Console variables. Registered by CCC_Register() in xr_ioc_cmd.cpp.
extern ENGINE_API BOOL psGamepadEnabled;
extern ENGINE_API float psGamepadMoveDeadzone;
extern ENGINE_API float psGamepadLookDeadzone;
extern ENGINE_API float psGamepadLookSens;
extern ENGINE_API float psGamepadLookSensVertical;
extern ENGINE_API float psGamepadAdsLookMult;
extern ENGINE_API BOOL psGamepadInvertY;
extern ENGINE_API float psGamepadTriggerThreshold;
extern ENGINE_API float psGamepadResponse;
extern ENGINE_API float psGamepadHapticStrength;
extern ENGINE_API float psGamepadAdaptiveStrength;
extern ENGINE_API BOOL psGamepadTouchpadUi;
extern ENGINE_API BOOL psGamepadDebug;

#endif // __XR_GAMEPAD__
