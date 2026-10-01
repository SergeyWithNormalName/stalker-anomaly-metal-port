#include "stdafx.h"
#pragma hdrstop

#include "xr_gamepad.h"

#if defined(XR_PLATFORM_APPLE)

#include "xr_gamepad_apple.h"

namespace
{
// The Objective-C++ side carries its own copy of the button bits because it
// cannot include this header. Catching a drift here is much cheaper than
// debugging a mislabelled button.
static_assert(GAMEPAD_BIT_A == (1u << 0), "button bit table drifted from the Apple bridge");
static_assert(GAMEPAD_BIT_TOUCHPAD == (1u << 15), "button bit table drifted from the Apple bridge");
static_assert(int(GAMEPAD_HAND_RIGHT) == int(XR_GAMEPAD_APPLE_HAND_RIGHT),
	"hand table drifted from the Apple bridge");
static_assert(int(GAMEPAD_TRIGGER_RECOIL) == int(XR_GAMEPAD_APPLE_TRIGGER_RECOIL),
	"trigger effect table drifted from the Apple bridge");

void gamepad_apple_log(const char* message)
{
	Msg("* %s", message);
}

class CXAppleGamepadBackend : public IGamepadBackend
{
public:
	CXAppleGamepadBackend()
	{
		m_handle = XrGamepadAppleCreate(&gamepad_apple_log);
	}

	virtual ~CXAppleGamepadBackend()
	{
		if (m_handle)
			XrGamepadAppleDestroy(m_handle);
	}

	virtual bool Start() override
	{
		if (!m_handle)
			return false;

		return XrGamepadAppleStart(m_handle) != 0;
	}

	virtual void Stop() override
	{
		if (m_handle)
			XrGamepadAppleStop(m_handle);
	}

	virtual void Poll() override
	{
		if (m_handle)
			XrGamepadApplePoll(m_handle);
	}

	virtual void Snapshot(SRawGamepadState& raw, SGamepadCapabilities& caps,
		SGamepadDeviceInfo& info) const override
	{
		raw.Reset();
		caps.Reset();
		info.Reset();

		if (!m_handle)
			return;

		XrGamepadAppleState state;
		XrGamepadAppleSnapshot(m_handle, &state);

		raw.lx = state.lx;
		raw.ly = state.ly;
		raw.rx = state.rx;
		raw.ry = state.ry;
		raw.lt = state.lt;
		raw.rt = state.rt;
		raw.buttons = state.buttons & GAMEPAD_BUTTON_MASK;

		raw.touchActive = state.touchActive != 0;
		raw.touchX = state.touchX;
		raw.touchY = state.touchY;
		raw.touchSecondaryActive = state.touchSecondaryActive != 0;
		raw.touchSecondaryX = state.touchSecondaryX;
		raw.touchSecondaryY = state.touchSecondaryY;

		raw.gyroX = state.gyroX;
		raw.gyroY = state.gyroY;
		raw.gyroZ = state.gyroZ;
		raw.accX = state.accX;
		raw.accY = state.accY;
		raw.accZ = state.accZ;

		caps.connected = state.connected != 0;
		caps.extended = state.extended != 0;
		caps.dualsense = state.dualsense != 0;
		caps.haptics = state.haptics != 0;
		caps.adaptiveTriggers = state.adaptiveTriggers != 0;
		caps.light = state.light != 0;
		caps.motion = state.motion != 0;
		caps.touchpad = state.touchpad != 0;

		xr_strcpy(info.vendorName, sizeof(info.vendorName), state.vendorName);
		xr_strcpy(info.productCategory, sizeof(info.productCategory), state.productCategory);
		xr_strcpy(info.profileName, sizeof(info.profileName), state.profileName);
		xr_strcpy(info.transport, sizeof(info.transport), state.transport);
		info.playerIndex = state.playerIndex;
	}

	virtual void SetAdaptiveTrigger(EGamepadHand hand, EGamepadTriggerEffect effect,
		float strength) override
	{
		if (m_handle)
			XrGamepadAppleSetAdaptiveTrigger(m_handle, int(hand), int(effect), strength);
	}

	virtual void PlayHaptic(float amplitude, float durationSec) override
	{
		if (m_handle)
			XrGamepadApplePlayHaptic(m_handle, amplitude, durationSec);
	}

	virtual void SetLight(float r, float g, float b) override
	{
		if (m_handle)
			XrGamepadAppleSetLight(m_handle, r, g, b);
	}

	virtual void ResetEffects() override
	{
		if (m_handle)
			XrGamepadAppleResetEffects(m_handle);
	}

private:
	void* m_handle;
};
}

IGamepadBackend* CreateAppleGamepadBackend()
{
	return xr_new<CXAppleGamepadBackend>();
}

#endif // XR_PLATFORM_APPLE
