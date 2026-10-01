#ifndef __XR_GAMEPAD_TYPES__
#define __XR_GAMEPAD_TYPES__
#pragma once

// Platform independent value types for the gamepad layer.
//
// This header deliberately depends on nothing but the C++ standard library so
// the state processing built on top of it can be unit tested without an engine
// build and without a physically connected controller.

#include <cstdint>
#include <cstddef>

// Physical button bits. The layout is a stable wire format between the platform
// backend, the state processor and the game side action mapping.
enum EGamepadButtonBit : std::uint32_t
{
	GAMEPAD_BIT_A = 1u << 0, // Cross on DualSense
	GAMEPAD_BIT_B = 1u << 1, // Circle
	GAMEPAD_BIT_X = 1u << 2, // Square
	GAMEPAD_BIT_Y = 1u << 3, // Triangle
	GAMEPAD_BIT_LB = 1u << 4,
	GAMEPAD_BIT_RB = 1u << 5,
	GAMEPAD_BIT_BACK = 1u << 6, // Create / View
	GAMEPAD_BIT_START = 1u << 7, // Options / Menu
	GAMEPAD_BIT_GUIDE = 1u << 8, // PS button; frequently reserved by the OS
	GAMEPAD_BIT_LS = 1u << 9,
	GAMEPAD_BIT_RS = 1u << 10,
	GAMEPAD_BIT_DPAD_UP = 1u << 11,
	GAMEPAD_BIT_DPAD_DOWN = 1u << 12,
	GAMEPAD_BIT_DPAD_LEFT = 1u << 13,
	GAMEPAD_BIT_DPAD_RIGHT = 1u << 14,
	GAMEPAD_BIT_TOUCHPAD = 1u << 15, // touchpad click
	GAMEPAD_BIT_COUNT = 16,
	GAMEPAD_BUTTON_MASK = (1u << GAMEPAD_BIT_COUNT) - 1u
};

enum EGamepadHand
{
	GAMEPAD_HAND_LEFT = 0,
	GAMEPAD_HAND_RIGHT = 1
};

// Adaptive trigger effect requests. A backend that cannot support adaptive
// triggers must silently ignore everything except GAMEPAD_TRIGGER_OFF.
enum EGamepadTriggerEffect
{
	GAMEPAD_TRIGGER_OFF = 0,
	GAMEPAD_TRIGGER_RESISTANCE, // constant resistance while held
	GAMEPAD_TRIGGER_RECOIL      // short kick, then back to resistance
};

// A snapshot of the raw device state. Stick axes are normalised to
// [-1, +1] with +X right and +Y up; triggers to [0, 1] with 0 released.
// Backends are responsible for converting their own conventions.
struct SRawGamepadState
{
	SRawGamepadState()
	{
		Reset();
	}

	void Reset()
	{
		lx = ly = rx = ry = 0.0f;
		lt = rt = 0.0f;
		buttons = 0;
		touchActive = false;
		touchX = touchY = 0.0f;
		touchSecondaryActive = false;
		touchSecondaryX = touchSecondaryY = 0.0f;
		gyroX = gyroY = gyroZ = 0.0f;
		accX = accY = accZ = 0.0f;
	}

	float lx, ly; // left stick
	float rx, ry; // right stick
	float lt, rt; // analog triggers, 0..1

	std::uint32_t buttons; // bitmask of EGamepadButtonBit

	bool touchActive;      // primary finger down on the touchpad surface
	float touchX, touchY;  // 0..1, origin top-left

	bool touchSecondaryActive;      // secondary finger down on the touchpad surface
	float touchSecondaryX, touchSecondaryY;

	float gyroX, gyroY, gyroZ; // rad/s
	float accX, accY, accZ;    // g
};

// What the active controller actually supports. Every optional feature must be
// gated on these flags; a missing capability must never disable core input.
struct SGamepadCapabilities
{
	SGamepadCapabilities()
	{
		Reset();
	}

	void Reset()
	{
		connected = false;
		extended = false;
		dualsense = false;
		haptics = false;
		adaptiveTriggers = false;
		light = false;
		motion = false;
		touchpad = false;
	}

	bool connected;
	bool extended;         // GCExtendedGamepad profile available
	bool dualsense;        // GCDualSenseGamepad profile
	bool haptics;          // GCDeviceHaptics
	bool adaptiveTriggers; // GCDualSenseAdaptiveTrigger
	bool light;            // GCDeviceLight
	bool motion;           // GCMotion sensors
	bool touchpad;         // GCControllerTouchpad
};

struct SGamepadDeviceInfo
{
	SGamepadDeviceInfo()
	{
		Reset();
	}

	void Reset()
	{
		vendorName[0] = 0;
		productCategory[0] = 0;
		profileName[0] = 0;
		transport[0] = 0;
		playerIndex = -1;
	}

	char vendorName[128];
	char productCategory[128];
	char profileName[64]; // "GCExtendedGamepad" / "GCDualSenseGamepad" / ...
	char transport[32];   // "USB" / "Bluetooth" / "unknown"
	int playerIndex;
};

#endif // __XR_GAMEPAD_TYPES__
