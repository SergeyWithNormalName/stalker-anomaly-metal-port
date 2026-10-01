#ifndef __XR_GAMEPAD_APPLE__
#define __XR_GAMEPAD_APPLE__
#pragma once

// C boundary between the Objective-C++ that talks to Apple's GameController
// framework and the rest of the engine.
//
// The split is not cosmetic: the engine typedefs BOOL as int32_t while
// Objective-C typedefs it as bool, and a translation unit cannot contain both.
// The bridge therefore never includes an engine header, and this file stays
// free of engine types in both directions. It uses the compiler's own platform
// macro rather than the engine's, since the Objective-C++ side never sees the
// engine's platform header.

#if defined(__APPLE__)

typedef void (*XrGamepadAppleLogFn)(const char* message);

// Mirrors SRawGamepadState plus SGamepadCapabilities and SGamepadDeviceInfo,
// with every boolean widened to int so the layout cannot depend on how each
// language sizes its own bool.
struct XrGamepadAppleState
{
	float lx, ly;
	float rx, ry;
	float lt, rt;
	unsigned int buttons;

	int touchActive;
	float touchX, touchY;
	int touchSecondaryActive;
	float touchSecondaryX, touchSecondaryY;

	float gyroX, gyroY, gyroZ;
	float accX, accY, accZ;

	int connected;
	int extended;
	int dualsense;
	int haptics;
	int adaptiveTriggers;
	int light;
	int motion;
	int touchpad;

	char vendorName[128];
	char productCategory[128];
	char profileName[64];
	char transport[32];
	int playerIndex;
};

// Effect identifiers, matching EGamepadHand and EGamepadTriggerEffect.
enum XrGamepadAppleHand
{
	XR_GAMEPAD_APPLE_HAND_LEFT = 0,
	XR_GAMEPAD_APPLE_HAND_RIGHT = 1
};

enum XrGamepadAppleTriggerEffect
{
	XR_GAMEPAD_APPLE_TRIGGER_OFF = 0,
	XR_GAMEPAD_APPLE_TRIGGER_RESISTANCE = 1,
	XR_GAMEPAD_APPLE_TRIGGER_RECOIL = 2
};

// All of these are safe to call with a null handle.
void* XrGamepadAppleCreate(XrGamepadAppleLogFn logFn);
void XrGamepadAppleDestroy(void* handle);

// Returns non zero when the framework was set up.
int XrGamepadAppleStart(void* handle);
void XrGamepadAppleStop(void* handle);

// Refresh the device list and drain queued notifications. Engine thread only.
void XrGamepadApplePoll(void* handle);
void XrGamepadAppleSnapshot(void* handle, XrGamepadAppleState* out);

void XrGamepadAppleSetAdaptiveTrigger(void* handle, int hand, int effect, float strength);
void XrGamepadApplePlayHaptic(void* handle, float amplitude, float duration);
void XrGamepadAppleSetLight(void* handle, float r, float g, float b);
void XrGamepadAppleResetEffects(void* handle);

#endif // __APPLE__

#endif // __XR_GAMEPAD_APPLE__
