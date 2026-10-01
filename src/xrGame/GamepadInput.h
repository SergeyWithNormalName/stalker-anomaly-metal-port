#pragma once

#include "../xrEngine/xr_gamepad.h"
#include "../xrEngine/xr_gamepad_mapping.h"
#include "../xrEngine/xr_gamepad_haptics.h"
#include "GamepadUiNavigation.h"

class ENGINE_API IInputReceiver;
class CUIDialogWnd;

// The game side of gamepad support.
//
// It does three things and nothing else: it tells the mapping policy where to
// send events, it translates the policy's logical controls into engine actions
// and then into whatever keys those actions are currently bound to, and it
// drives the DualSense extras. All the state processing lives in
// CGamepadProcessor and CGamepadMapping, which are engine free and covered by
// unit tests.
class CGamepadInput : public IGamepadInputSink, public IGamepadActionOutput
{
public:
	CGamepadInput();
	virtual ~CGamepadInput();

	// IGamepadInputSink
	virtual void OnGamepadFrame(const SGamepadFrame& frame, float dtMs);
	virtual void OnGamepadReset();

	// IGamepadActionOutput
	virtual void GamepadKeyPress(int dik);
	virtual void GamepadKeyRelease(int dik);
	virtual void GamepadKeyHold(int dik);
	virtual void GamepadMouseMove(int dx, int dy);
	virtual void GamepadMouseWheel(int direction);
	virtual void GamepadCursorMove(float dx, float dy);
	virtual void GamepadMapPan(float dx, float dy) override;
	virtual void GamepadMapZoom(bool zoomIn) override;
	virtual void GamepadChordEvent(int chord) override;
	virtual void GamepadUiAction(int uiAction) override;

	// Installs the singleton. Called from CCC_RegisterInput, once the binding
	// table exists, so the mapping can resolve actions from its first frame.
	static void Register();

	// Verified weapon events from CWeaponMagazined
	static void OnWeaponShot(bool isAutomatic);
	static void OnWeaponDryFire();
	// Diagnostic UI replay through the normal mapping. Inert unless
	// g_gamepad_debug is enabled; useful without changing a user's bindings.
	static void DebugUiInput(int control);

private:
	void UpdateEffects(const SGamepadFrame& frame, bool uiActive, float dtMs);

	IInputReceiver* Receiver() const;
	CUIDialogWnd* ActiveDialog() const;
	void UpdateUiFocus(bool active);

	// The receiver the last frame was delivered to. A change means the presses
	// already sent belong to something that is no longer listening, so
	// everything held has to be let go before the new owner sees anything.
	IInputReceiver* m_lastReceiver;
	IInputReceiver* m_lastRouteReceiver;
	CUIDialogWnd* m_lastDialog;
	CGamepadUiNavigation m_uiNavigation;
	bool m_uiFocusActive;
	std::vector<int> m_debugUiInputs;
	int m_debugUiRelease;
	bool m_debugUiReplay;
	float m_debugMs;
	int m_debugContext;

	CGamepadMapping m_mapping;

	// Set once the cursor has been given its default colour, so the light is
	// only ever written when something actually changed.
	bool m_lightApplied;

	// DualSense effect state. None of this touches core input.
	bool m_adaptiveWeaponOn;
	float m_recoilMs;

	CShotHapticTracker m_haptics;
	float m_lightThrottleMs;
	float m_lastLightR;
	float m_lastLightG;
	float m_lastLightB;
};
