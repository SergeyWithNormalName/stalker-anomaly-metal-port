// Smoke probe for the Apple GameController bridge.
//
// It links the real bridge and its frameworks and drives the whole lifecycle -
// create, start, poll, snapshot, effects, stop, destroy - but it does not
// include any engine header and it asserts nothing about whether a controller
// is attached: that depends on the machine. With a DualSense plugged in it
// prints the detected capabilities, which is the quickest way to confirm the
// extras are reported as available on a given macOS version.
//
// Build and run:
//   cmake --build build --target XRay.Gamepad.AppleBridgeProbe
//   build/src/xrEngine/XRay.Gamepad.AppleBridgeProbe

#include "xr_gamepad_apple.h"

#include <cstdio>

namespace
{
void probe_log(const char* message)
{
	std::printf("  log: %s\n", message);
}

void print_state(const char* label, const XrGamepadAppleState& state)
{
	std::printf(
		"%s: connected=%d extended=%d dualsense=%d haptics=%d adaptive=%d light=%d motion=%d touchpad=%d\n",
		label, state.connected, state.extended, state.dualsense, state.haptics,
		state.adaptiveTriggers, state.light, state.motion, state.touchpad);

	if (state.connected)
	{
		std::printf(
			"    vendor=\"%s\" category=\"%s\" profile=\"%s\" transport=\"%s\"\n"
			"    l=(%.3f, %.3f) r=(%.3f, %.3f) lt=%.3f rt=%.3f buttons=0x%04x touch=%d (%.3f, %.3f)\n",
			state.vendorName, state.productCategory, state.profileName, state.transport,
			state.lx, state.ly, state.rx, state.ry, state.lt, state.rt,
			state.buttons, state.touchActive, state.touchX, state.touchY);
	}
}
}

int main()
{
	std::printf("apple gamepad bridge probe\n");

	void* handle = XrGamepadAppleCreate(&probe_log);
	if (!handle)
	{
		std::printf("FAIL: create returned null\n");
		return 1;
	}

	if (!XrGamepadAppleStart(handle))
	{
		std::printf("FAIL: start reported failure\n");
		XrGamepadAppleDestroy(handle);
		return 1;
	}

	// Two seconds at 60 Hz. Long enough to settle discovery, and enough to
	// notice a crash or a stall in the polling path.
	for (int i = 0; i < 120; ++i)
	{
		XrGamepadApplePoll(handle);

		if (i == 0 || i == 119)
		{
			XrGamepadAppleState state;
			XrGamepadAppleSnapshot(handle, &state);
			print_state(i == 0 ? "first frame" : "last frame", state);
		}
	}

	// Every effect must be a safe no-op on a machine with no pad, and on a pad
	// that does not support it.
	XrGamepadAppleSetAdaptiveTrigger(handle, XR_GAMEPAD_APPLE_HAND_RIGHT,
		XR_GAMEPAD_APPLE_TRIGGER_RESISTANCE, 0.6f);
	XrGamepadAppleSetAdaptiveTrigger(handle, XR_GAMEPAD_APPLE_HAND_LEFT,
		XR_GAMEPAD_APPLE_TRIGGER_RECOIL, 0.6f);
	XrGamepadApplePlayHaptic(handle, 0.5f, 0.05f);
	XrGamepadAppleSetLight(handle, 0.1f, 0.2f, 0.3f);
	XrGamepadAppleResetEffects(handle);

	XrGamepadAppleStop(handle);
	XrGamepadAppleDestroy(handle);

	// A null handle has to be inert rather than fatal.
	XrGamepadApplePoll(0);
	XrGamepadAppleState unused;
	XrGamepadAppleSnapshot(0, &unused);
	XrGamepadAppleSnapshot(handle, 0);
	XrGamepadAppleSetAdaptiveTrigger(0, 0, 0, 0.0f);
	XrGamepadApplePlayHaptic(0, 1.0f, 1.0f);
	XrGamepadAppleSetLight(0, 0.0f, 0.0f, 0.0f);
	XrGamepadAppleResetEffects(0);
	XrGamepadAppleStop(0);
	XrGamepadAppleDestroy(0);

	std::printf("PROBE PASS\n");
	return 0;
}
