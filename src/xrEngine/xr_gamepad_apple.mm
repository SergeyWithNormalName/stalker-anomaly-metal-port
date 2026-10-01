// Apple GameController backend.
//
// This file talks to the framework and nothing else. It must not include any
// engine header: the engine typedefs BOOL as int32_t, Objective-C typedefs it
// as bool, and one translation unit cannot hold both. Everything crosses the
// boundary declared in xr_gamepad_apple.h.
//
// The framework delivers value changed handlers on the main queue by default,
// but the engine thread is not guaranteed to be the main one, so the shared
// state those handlers touch is guarded by a mutex, and every change to the
// controller selection is made on the polling thread rather than inside a
// notification handler.

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

#include "xr_gamepad_apple.h"
#include "xr_gamepad_apple_input.h"

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace
{
// Mirror of the engine's EGamepadButtonBit. This file cannot include the header
// that defines it, so the two tables are kept in step by hand; the static
// assertion in the C++ glue catches a drift.
enum
{
	kb_a = 1u << 0,
	kb_b = 1u << 1,
	kb_x = 1u << 2,
	kb_y = 1u << 3,
	kb_lb = 1u << 4,
	kb_rb = 1u << 5,
	kb_back = 1u << 6,
	kb_start = 1u << 7,
	kb_guide = 1u << 8,
	kb_ls = 1u << 9,
	kb_rs = 1u << 10,
	kb_dpad_up = 1u << 11,
	kb_dpad_down = 1u << 12,
	kb_dpad_left = 1u << 13,
	kb_dpad_right = 1u << 14,
	kb_touchpad = 1u << 15
};

void copy_nsstring(char* dst, size_t dstSize, NSString* src)
{
	if (!dst || dstSize == 0)
		return;

	dst[0] = 0;
	if (!src)
		return;

	const char* utf8 = [src UTF8String];
	if (!utf8)
		return;

	std::strncpy(dst, utf8, dstSize - 1);
	dst[dstSize - 1] = 0;
}

float axis_value(GCControllerAxisInput* axis)
{
	return axis ? axis.value : 0.0f;
}

int button_pressed(GCControllerButtonInput* button)
{
	return (button && button.isPressed) ? 1 : 0;
}
}

@interface XRAppleGamepadBridge : NSObject
{
@public
	std::mutex m_lock;

	GCController* m_controller;      // strong, the currently selected device
	GCDualSenseGamepad* m_dualsense; // aliases the profile when it is a DualSense

	XrGamepadAppleState m_state;
	XrGamepadAppleLogFn m_log;

	// Presses seen since the last poll. This is what keeps a press and release
	// that both fit inside one frame from being lost when the frame rate dips.
	unsigned int m_latchedButtons;
	unsigned int m_latchedTriggers; // bit0 = L2, bit1 = R2

	CHHapticEngine* m_hapticEngine;

	// Written from the CoreHaptics queue when the engine stops itself and read
	// on the engine thread when a haptic is requested, so it is atomic rather
	// than guarded: taking the lock here would mean the stopped handler could
	// deadlock against a haptic that is already being started.
	std::atomic<bool> m_hapticEngineStarted;

	// Set by the connect and disconnect notifications, acted on by poll, so
	// that selection never races with a frame.
	bool m_deviceSetDirty;

	// Frames left before the next look for a controller while none is attached.
	int m_framesUntilScan;

	bool m_observing;
	CGamepadAppleMotionBasis m_motionBasis;
	double m_lastMotionTime;
}

- (id)initWithLog:(XrGamepadAppleLogFn)logFn;
- (void)logLine:(const char*)format, ...;
- (void)start;
- (void)stop;
- (void)poll;
- (void)snapshot:(XrGamepadAppleState*)out;
- (void)latchButton:(unsigned int)bit;
- (void)latchTrigger:(unsigned int)which value:(float)value;
- (void)markHapticEngineStopped;
- (void)setAdaptiveTrigger:(int)hand effect:(int)effect strength:(float)strength;
- (void)playHaptic:(float)amplitude duration:(float)duration;
- (void)setLightRed:(float)r green:(float)g blue:(float)b;
- (void)resetEffects;
@end

@implementation XRAppleGamepadBridge

- (id)initWithLog:(XrGamepadAppleLogFn)logFn
{
	self = [super init];
	if (!self)
		return nil;

	m_controller = nil;
	m_dualsense = nil;
	m_log = logFn;
	m_latchedButtons = 0;
	m_latchedTriggers = 0;
	m_hapticEngine = nil;
	m_hapticEngineStarted = false;
	m_deviceSetDirty = true;
	m_framesUntilScan = 0;
	m_observing = false;
	m_lastMotionTime = 0.0;
	std::memset(&m_state, 0, sizeof(m_state));
	return self;
}

- (void)logLine:(const char*)format, ...
{
	if (!m_log)
		return;

	char buffer[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	m_log(buffer);
}

- (void)dealloc
{
	[self stop];
}

// --- discovery -------------------------------------------------------------

- (void)start
{
	if (m_observing)
		return;

	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
	[center addObserver:self selector:@selector(onControllerConnected:)
		name:GCControllerDidConnectNotification object:nil];
	[center addObserver:self selector:@selector(onControllerDisconnected:)
		name:GCControllerDidDisconnectNotification object:nil];
	m_observing = true;

	// Already paired pads arrive through the observers above, and through the
	// enumeration in poll. This only asks the framework to look for pads that
	// are not paired yet, which is a main thread operation; skipping it off the
	// main thread costs nothing because the user can pair in System Settings.
	if ([NSThread isMainThread] &&
		[GCController respondsToSelector:@selector(startWirelessControllerDiscoveryWithCompletionHandler:)])
	{
		[GCController startWirelessControllerDiscoveryWithCompletionHandler:^{
			// A successful discovery arrives as a connect notification, which
			// is already handled, so there is nothing to do here.
		}];
	}

	[self selectController:[self pickController]];
}

- (void)stop
{
	if (m_observing)
	{
		[[NSNotificationCenter defaultCenter] removeObserver:self];
		m_observing = false;
	}

	[self detachController];
	m_hapticEngineStarted = false;
}

- (void)onControllerConnected:(NSNotification*)note
{
	// Notifications arrive on a framework queue. Only flag the change here; the
	// engine thread acts on it in poll, so selection never races with a frame.
	GCController* controller = note.object;
	if (!controller || !controller.extendedGamepad)
		return;

	std::lock_guard<std::mutex> guard(m_lock);
	m_deviceSetDirty = true;
}

- (void)onControllerDisconnected:(NSNotification*)note
{
	// Simply flag it: poll decides whether the pad that went away was the one in
	// use, by re-reading the framework's own list. That keeps this handler from
	// touching the selection, which belongs to the engine thread alone.
	std::lock_guard<std::mutex> guard(m_lock);
	m_deviceSetDirty = true;
}

- (GCController*)pickController
{
	for (GCController* controller in [GCController controllers])
	{
		if (controller.extendedGamepad)
			return controller;
	}
	return nil;
}

- (void)selectController:(GCController*)controller
{
	[self detachController];
	m_motionBasis.Reset();
	m_lastMotionTime = 0.0;

	if (!controller)
		return;

	m_controller = controller;

	GCExtendedGamepad* pad = controller.extendedGamepad;

	XrGamepadAppleState state;
	std::memset(&state, 0, sizeof(state));
	state.connected = 1;
	state.extended = (pad != nil) ? 1 : 0;

	if (@available(macOS 11.3, *))
	{
		if ([pad isKindOfClass:[GCDualSenseGamepad class]])
		{
			m_dualsense = (GCDualSenseGamepad*)pad;
			state.dualsense = 1;
			// The click and the surface are separate capabilities: a pad can
			// expose the button without reporting finger positions.
			state.touchpad =
				((m_dualsense.touchpadPrimary != nil) || (m_dualsense.touchpadButton != nil)) ? 1 : 0;
			state.adaptiveTriggers =
				((m_dualsense.leftTrigger != nil) && (m_dualsense.rightTrigger != nil)) ? 1 : 0;
		}
	}

	if (@available(macOS 11.0, *))
	{
		state.haptics = (controller.haptics != nil) ? 1 : 0;
		state.light = (controller.light != nil) ? 1 : 0;
	}

	@try
	{
		state.motion = (controller.motion != nil) ? 1 : 0;
	}
	@catch (NSException* e)
	{
		state.motion = 0;
	}

	// Setting the player index is what lights the player LEDs on a DualSense.
	controller.playerIndex = GCControllerPlayerIndex1;

	if (@available(macOS 11.0, *))
	{
		if (state.motion && controller.motion && controller.motion.sensorsRequireManualActivation)
		{
			controller.motion.sensorsActive = YES;
		}
	}

	copy_nsstring(state.vendorName, sizeof(state.vendorName), controller.vendorName);
	copy_nsstring(state.productCategory, sizeof(state.productCategory), controller.productCategory);
	// There is no public transport query. `attachedToDevice` is the closest
	// public signal: true for a pad wired to this machine.
	copy_nsstring(state.transport, sizeof(state.transport),
		controller.isAttachedToDevice ? @"wired" : @"wireless");
	copy_nsstring(state.profileName, sizeof(state.profileName),
		state.dualsense ? @"GCDualSenseGamepad" : @"GCExtendedGamepad");
	state.playerIndex = 0;

	{
		std::lock_guard<std::mutex> guard(m_lock);
		m_state = state;
	}

	[self installHandlers:pad];

	if (state.haptics)
		[self createHapticEngine];

	[self logLine:"[gamepad] connected: %s / %s (%s, %s)", state.vendorName, state.productCategory,
		state.profileName, state.transport];
}

- (void)detachController
{
	if (m_controller)
		[self clearHandlers:m_controller.extendedGamepad];

	m_controller = nil;
	m_dualsense = nil;

	// The haptic engine is bound to the pad that created it. CoreHaptics stops
	// it on disconnect, but the reference has to go too, otherwise a
	// reconnected pad would keep playing through a dead engine. The stop is
	// deliberately outside the lock: stopping can invoke the stopped handler
	// synchronously and that would deadlock against a held lock.
	if (m_hapticEngine)
	{
		[m_hapticEngine stopWithCompletionHandler:nil];
		m_hapticEngine = nil;
	}

	std::lock_guard<std::mutex> guard(m_lock);
	m_hapticEngineStarted = false;
	std::memset(&m_state, 0, sizeof(m_state));
	m_latchedButtons = 0;
	m_latchedTriggers = 0;
}

// --- input handlers --------------------------------------------------------

- (void)latchButton:(unsigned int)bit
{
	std::lock_guard<std::mutex> guard(m_lock);
	m_latchedButtons |= bit;
}

- (void)latchTrigger:(unsigned int)which value:(float)value
{
	std::lock_guard<std::mutex> guard(m_lock);
	m_latchedTriggers |= which;
	if (which & 1u)
		m_state.lt = value;
	if (which & 2u)
		m_state.rt = value;
}

- (void)markHapticEngineStopped
{
	// Called from the CoreHaptics queue. The flag is atomic, so there is nothing
	// to synchronise here.
	m_hapticEngineStarted = false;
}

- (void)installHandlers:(GCExtendedGamepad*)pad
{
	if (!pad)
		return;

	__weak XRAppleGamepadBridge* weakSelf = self;

	pad.buttonA.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_a];
	};
	pad.buttonB.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_b];
	};
	pad.buttonX.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_x];
	};
	pad.buttonY.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_y];
	};

	pad.leftShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_lb];
	};
	pad.rightShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_rb];
	};

	// buttonMenu is Options on a DualSense; buttonOptions is Create.
	pad.buttonMenu.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_start];
	};
	if (pad.buttonOptions)
	{
		pad.buttonOptions.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
		{
			if (pressed) [weakSelf latchButton:kb_back];
		};
	}

	if (pad.leftThumbstickButton)
	{
		pad.leftThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
		{
			if (pressed) [weakSelf latchButton:kb_ls];
		};
	}
	if (pad.rightThumbstickButton)
	{
		pad.rightThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
		{
			if (pressed) [weakSelf latchButton:kb_rs];
		};
	}

	// The d-pad exposes both an analog pair and four buttons. The stick pair is
	// ignored on purpose: on a DualSense the d-pad is digital, and treating a
	// slight membrane wobble as a direction change would be wrong.
	pad.dpad.up.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_dpad_up];
	};
	pad.dpad.down.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_dpad_down];
	};
	pad.dpad.left.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_dpad_left];
	};
	pad.dpad.right.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
	{
		if (pressed) [weakSelf latchButton:kb_dpad_right];
	};

	pad.leftTrigger.valueChangedHandler = ^(GCControllerButtonInput*, float value, BOOL pressed)
	{
		if (pressed) [weakSelf latchTrigger:1u value:value];
	};
	pad.rightTrigger.valueChangedHandler = ^(GCControllerButtonInput*, float value, BOOL pressed)
	{
		if (pressed) [weakSelf latchTrigger:2u value:value];
	};

	if (m_dualsense && m_dualsense.touchpadButton)
	{
		m_dualsense.touchpadButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL pressed)
		{
			if (pressed) [weakSelf latchButton:kb_touchpad];
		};
	}
}

- (void)clearHandlers:(GCExtendedGamepad*)pad
{
	if (!pad)
		return;

	pad.buttonA.valueChangedHandler = nil;
	pad.buttonB.valueChangedHandler = nil;
	pad.buttonX.valueChangedHandler = nil;
	pad.buttonY.valueChangedHandler = nil;
	pad.leftShoulder.valueChangedHandler = nil;
	pad.rightShoulder.valueChangedHandler = nil;
	pad.buttonMenu.valueChangedHandler = nil;
	if (pad.buttonOptions)
		pad.buttonOptions.valueChangedHandler = nil;
	if (pad.leftThumbstickButton)
		pad.leftThumbstickButton.valueChangedHandler = nil;
	if (pad.rightThumbstickButton)
		pad.rightThumbstickButton.valueChangedHandler = nil;
	pad.dpad.up.valueChangedHandler = nil;
	pad.dpad.down.valueChangedHandler = nil;
	pad.dpad.left.valueChangedHandler = nil;
	pad.dpad.right.valueChangedHandler = nil;
	pad.leftTrigger.valueChangedHandler = nil;
	pad.rightTrigger.valueChangedHandler = nil;

	if (m_dualsense && m_dualsense.touchpadButton)
		m_dualsense.touchpadButton.valueChangedHandler = nil;
}

// --- per frame -------------------------------------------------------------

- (void)poll
{
	// Act on whatever the notification handlers flagged. Doing the selection
	// here rather than in the handler keeps every change to the controller and
	// its capabilities on a single thread.
	bool deviceSetChanged = false;
	{
		std::lock_guard<std::mutex> guard(m_lock);
		deviceSetChanged = m_deviceSetDirty;
		m_deviceSetDirty = false;
	}

	// Enumerating allocates, so while a pad is attached the list is only
	// re-read when a notification says something changed. With nothing
	// attached it is re-read on a slow tick, which is the safety net for a
	// missed notification.
	bool shouldScan = deviceSetChanged;
	if (!m_controller)
	{
		if (m_framesUntilScan > 0)
			--m_framesUntilScan;
		if (m_framesUntilScan == 0)
		{
			m_framesUntilScan = 60;
			shouldScan = true;
		}
	}

	if (shouldScan)
	{
		GCController* current = m_controller;
		const bool stillThere = (current != nil) && [[GCController controllers] containsObject:current];

		if (!stillThere)
		{
			if (current)
				[self detachController];

			GCController* next = [self pickController];
			if (next)
				[self selectController:next];
		}
	}

	GCController* controller = m_controller;
	if (!controller)
		return;

	GCExtendedGamepad* pad = controller.extendedGamepad;
	if (!pad)
		return;

	float lx = axis_value(pad.leftThumbstick.xAxis);
	float ly = axis_value(pad.leftThumbstick.yAxis);
	float rx = axis_value(pad.rightThumbstick.xAxis);
	float ry = axis_value(pad.rightThumbstick.yAxis);
	float lt = pad.leftTrigger.value;
	float rt = pad.rightTrigger.value;

	unsigned int buttons = 0;
	if (button_pressed(pad.buttonA)) buttons |= kb_a;
	if (button_pressed(pad.buttonB)) buttons |= kb_b;
	if (button_pressed(pad.buttonX)) buttons |= kb_x;
	if (button_pressed(pad.buttonY)) buttons |= kb_y;
	if (button_pressed(pad.leftShoulder)) buttons |= kb_lb;
	if (button_pressed(pad.rightShoulder)) buttons |= kb_rb;
	if (button_pressed(pad.buttonMenu)) buttons |= kb_start;
	if (pad.buttonOptions && button_pressed(pad.buttonOptions)) buttons |= kb_back;
	if (pad.leftThumbstickButton && button_pressed(pad.leftThumbstickButton)) buttons |= kb_ls;
	if (pad.rightThumbstickButton && button_pressed(pad.rightThumbstickButton)) buttons |= kb_rs;
	if (button_pressed(pad.dpad.up)) buttons |= kb_dpad_up;
	if (button_pressed(pad.dpad.down)) buttons |= kb_dpad_down;
	if (button_pressed(pad.dpad.left)) buttons |= kb_dpad_left;
	if (button_pressed(pad.dpad.right)) buttons |= kb_dpad_right;

	int touchActive = 0;
	float touchX = 0.0f;
	float touchY = 0.0f;
	int touchSecondaryActive = 0;
	float touchSecondaryX = 0.0f;
	float touchSecondaryY = 0.0f;

	if (m_dualsense)
	{
		if (m_dualsense.touchpadButton)
		{
			if (button_pressed(m_dualsense.touchpadButton))
				buttons |= kb_touchpad;

			touchActive = m_dualsense.touchpadButton.isTouched ? 1 : 0;
		}

		// This legacy profile has no touchpad collection/contact state on some
		// macOS drivers. Its raw absent contacts return zero; isTouched alone
		// stays false even while the surface reports a moving finger.
		if (m_dualsense.touchpadPrimary)
		{
			const float x = axis_value(m_dualsense.touchpadPrimary.xAxis);
			const float y = axis_value(m_dualsense.touchpadPrimary.yAxis);
			touchActive = GamepadAppleTouchContact(x, y, touchActive ||
				button_pressed(m_dualsense.touchpadButton));
			touchX = (x + 1.0f) * 0.5f;
			touchY = (1.0f - y) * 0.5f;
		}
		if (m_dualsense.touchpadSecondary)
		{
			const float x = axis_value(m_dualsense.touchpadSecondary.xAxis);
			const float y = axis_value(m_dualsense.touchpadSecondary.yAxis);
			touchSecondaryActive = touchActive && GamepadAppleTouchContact(x, y, false);
			touchSecondaryX = (x + 1.0f) * 0.5f;
			touchSecondaryY = (1.0f - y) * 0.5f;
		}

	}

	float gyroX = 0.0f, gyroY = 0.0f, gyroZ = 0.0f;
	float accX = 0.0f, accY = 0.0f, accZ = 0.0f;

	if (controller.motion)
	{
		@try
		{
			if (controller.motion.sensorsActive)
			{
				GCRotationRate rate = controller.motion.rotationRate;
				gyroX = float(rate.x);
				const bool separateGravity = controller.motion.hasGravityAndUserAcceleration;
				const GCAcceleration down = separateGravity ? controller.motion.gravity : controller.motion.acceleration;
				const double now = [NSDate timeIntervalSinceReferenceDate];
				const float dt = m_lastMotionTime > 0.0 ? float(now - m_lastMotionTime) : 0.0f;
				m_lastMotionTime = now;
				m_motionBasis.Update(float(down.x), float(down.y), float(down.z), dt, separateGravity);
				// GCRotationRate follows the right-hand rule; rotation about down
				// is a right turn. Raw Y alone misses flat-grip Z yaw and reverses
				// the sign for an upright controller.
				gyroY = m_motionBasis.Yaw(float(rate.x), float(rate.y), float(rate.z));
				gyroZ = float(rate.z);

				GCAcceleration acc = controller.motion.userAcceleration;
				accX = float(acc.x);
				accY = float(acc.y);
				accZ = float(acc.z);
			}
		}
		@catch (NSException* e)
		{
			gyroX = gyroY = gyroZ = 0.0f;
			accX = accY = accZ = 0.0f;
		}
	}

	{
		std::lock_guard<std::mutex> guard(m_lock);

		// Sub frame presses the poll above cannot see. They are folded into the
		// mask so the state processor still produces a matching release on the
		// following frame.
		buttons |= m_latchedButtons;
		m_latchedButtons = 0;

		if (m_latchedTriggers & 1u)
			lt = 1.0f;
		if (m_latchedTriggers & 2u)
			rt = 1.0f;
		m_latchedTriggers = 0;

		m_state.lx = lx;
		m_state.ly = ly;
		m_state.rx = rx;
		m_state.ry = ry;
		m_state.lt = lt;
		m_state.rt = rt;
		m_state.buttons = buttons;
		m_state.touchActive = touchActive;
		m_state.touchX = touchX;
		m_state.touchY = touchY;
		m_state.touchSecondaryActive = touchSecondaryActive;
		m_state.touchSecondaryX = touchSecondaryX;
		m_state.touchSecondaryY = touchSecondaryY;
		m_state.gyroX = gyroX;
		m_state.gyroY = gyroY;
		m_state.gyroZ = gyroZ;
		m_state.accX = accX;
		m_state.accY = accY;
		m_state.accZ = accZ;
	}
}

- (void)snapshot:(XrGamepadAppleState*)out
{
	if (!out)
		return;

	std::lock_guard<std::mutex> guard(m_lock);
	*out = m_state;
}

// --- haptics ---------------------------------------------------------------

- (void)createHapticEngine
{
	GCController* controller = m_controller;
	if (!controller || !controller.haptics)
		return;

	GCDeviceHaptics* haptics = controller.haptics;

	@try
	{
		// GCHapticsLocalityDefault is the one locality the framework guarantees.
		// createEngineWithLocality: has no error out; a nil engine is the whole
		// failure signal.
		CHHapticEngine* engine = [haptics createEngineWithLocality:GCHapticsLocalityDefault];
		if (!engine)
		{
			[self logLine:"[gamepad] haptics unavailable on this device"];
			std::lock_guard<std::mutex> guard(m_lock);
			m_state.haptics = 0;
			return;
		}

		__weak XRAppleGamepadBridge* weakSelf = self;
		engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason)
		{
			// The engine idles itself out and stops outright when the
			// controller disconnects. Flag it so the next request restarts the
			// engine rather than silently dropping the effect.
			[weakSelf markHapticEngineStopped];
		};

		NSError* startError = nil;
		if (![engine startAndReturnError:&startError])
		{
			[self logLine:"[gamepad] haptic engine start failed: %s",
				startError ? [[startError description] UTF8String] : "unknown"];
			std::lock_guard<std::mutex> guard(m_lock);
			m_state.haptics = 0;
			return;
		}

		m_hapticEngine = engine;
		m_hapticEngineStarted = true;
	}
	@catch (NSException* e)
	{
		[self logLine:"[gamepad] haptic setup raised: %s", [[e reason] UTF8String]];
		std::lock_guard<std::mutex> guard(m_lock);
		m_state.haptics = 0;
		m_hapticEngine = nil;
		m_hapticEngineStarted = false;
	}
}

- (void)playHaptic:(float)amplitude duration:(float)duration
{
	if (!m_hapticEngine)
		return;

	{
		std::lock_guard<std::mutex> guard(m_lock);
		if (!m_state.haptics)
			return;
	}

	@try
	{
		NSError* error = nil;
		if (!m_hapticEngineStarted)
		{
			if (![m_hapticEngine startAndReturnError:&error])
				return;
			m_hapticEngineStarted = true;
		}

		const float clamped = amplitude < 0.0f ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);
		const float dur = duration < 0.01f ? 0.01f : duration;

		CHHapticEventParameter* intensity = [[CHHapticEventParameter alloc]
			initWithParameterID:CHHapticEventParameterIDHapticIntensity value:clamped];
		CHHapticEventParameter* sharpness = [[CHHapticEventParameter alloc]
			initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];

		CHHapticEvent* event = [[CHHapticEvent alloc]
			initWithEventType:CHHapticEventTypeHapticContinuous
			parameters:@[intensity, sharpness]
			relativeTime:0.0
			duration:double(dur)];

		CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
			parameters:@[] error:&error];
		if (!pattern)
			return;

		id<CHHapticPatternPlayer> player = [m_hapticEngine createPlayerWithPattern:pattern error:&error];
		if (!player)
			return;

		[player startAtTime:CHHapticTimeImmediate error:&error];
	}
	@catch (NSException* e)
	{
		// A haptic that cannot be delivered must never disturb normal input.
		std::lock_guard<std::mutex> guard(m_lock);
		m_state.haptics = 0;
	}
}

// --- adaptive triggers -----------------------------------------------------

- (void)setAdaptiveTrigger:(int)hand effect:(int)effect strength:(float)strength
{
	if (!m_dualsense)
		return;

	{
		std::lock_guard<std::mutex> guard(m_lock);
		if (!m_state.adaptiveTriggers)
			return;
	}

	// The framework names the triggers from the player's point of view, the same
	// way this layer does, so the hand maps straight through: left is the
	// physical L2, which is the aiming trigger.
	GCDualSenseAdaptiveTrigger* trigger =
		(hand == XR_GAMEPAD_APPLE_HAND_LEFT) ? m_dualsense.leftTrigger : m_dualsense.rightTrigger;
	if (!trigger)
		return;

	const float s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);

	@try
	{
		switch (effect)
		{
		case XR_GAMEPAD_APPLE_TRIGGER_RESISTANCE:
			// A little travel, then a wall: enough to feel the trigger without
			// making a long pull unusable.
			[trigger setModeFeedbackWithStartPosition:0.25f resistiveStrength:s];
			break;

		case XR_GAMEPAD_APPLE_TRIGGER_RECOIL:
			[trigger setModeWeaponWithStartPosition:0.35f endPosition:0.8f resistiveStrength:s];
			break;

		case XR_GAMEPAD_APPLE_TRIGGER_OFF:
		default:
			[trigger setModeOff];
			break;
		}
	}
	@catch (NSException* e)
	{
		// The effect API is not implemented on every transport. Losing it must
		// not cost the user the trigger as an analog input.
		std::lock_guard<std::mutex> guard(m_lock);
		m_state.adaptiveTriggers = 0;
	}
}

// --- light -----------------------------------------------------------------

- (void)setLightRed:(float)r green:(float)g blue:(float)b
{
	GCController* controller = m_controller;
	if (!controller)
		return;

	if (@available(macOS 11.0, *))
	{
		if (!controller.light)
			return;

		@try
		{
			controller.light.color = [[GCColor alloc] initWithRed:r green:g blue:b];
		}
		@catch (NSException* e)
		{
			std::lock_guard<std::mutex> guard(m_lock);
			m_state.light = 0;
		}
	}
}

- (void)resetEffects
{
	[self setAdaptiveTrigger:XR_GAMEPAD_APPLE_HAND_LEFT effect:XR_GAMEPAD_APPLE_TRIGGER_OFF strength:0.0f];
	[self setAdaptiveTrigger:XR_GAMEPAD_APPLE_HAND_RIGHT effect:XR_GAMEPAD_APPLE_TRIGGER_OFF strength:0.0f];
}

@end

// --- C boundary ------------------------------------------------------------

namespace
{
XRAppleGamepadBridge* as_bridge(void* handle)
{
	return (__bridge XRAppleGamepadBridge*)handle;
}
}

// These run on the engine's input thread, which in this engine is the process
// main thread. They deliberately do not hop to the main thread:
// performSelectorOnMainThread:waitUntilDone: needs a running NSRunLoop, and the
// engine's loop only dequeues NSEvents, so waiting on it would hang. Adding and
// removing notification observers is thread safe, and enumerating
// [GCController controllers] is itself how a pad that was already connected at
// start up gets picked up.

void* XrGamepadAppleCreate(XrGamepadAppleLogFn logFn)
{
	XRAppleGamepadBridge* bridge = [[XRAppleGamepadBridge alloc] initWithLog:logFn];
	return (__bridge_retained void*)bridge;
}

void XrGamepadAppleDestroy(void* handle)
{
	if (!handle)
		return;

	[as_bridge(handle) stop];
	// Creation transferred ownership to the caller, so balance it here.
	CFBridgingRelease(handle);
}

int XrGamepadAppleStart(void* handle)
{
	if (!handle)
		return 0;

	[as_bridge(handle) start];
	return 1;
}

void XrGamepadAppleStop(void* handle)
{
	if (!handle)
		return;

	[as_bridge(handle) stop];
}

void XrGamepadApplePoll(void* handle)
{
	if (!handle)
		return;

	[as_bridge(handle) poll];
}

void XrGamepadAppleSnapshot(void* handle, XrGamepadAppleState* out)
{
	if (!out)
		return;

	std::memset(out, 0, sizeof(*out));
	if (!handle)
		return;

	[as_bridge(handle) snapshot:out];
}

void XrGamepadAppleSetAdaptiveTrigger(void* handle, int hand, int effect, float strength)
{
	if (!handle)
		return;

	[as_bridge(handle) setAdaptiveTrigger:hand effect:effect strength:strength];
}

void XrGamepadApplePlayHaptic(void* handle, float amplitude, float duration)
{
	if (!handle)
		return;

	[as_bridge(handle) playHaptic:amplitude duration:duration];
}

void XrGamepadAppleSetLight(void* handle, float r, float g, float b)
{
	if (!handle)
		return;

	[as_bridge(handle) setLightRed:r green:g blue:b];
}

void XrGamepadAppleResetEffects(void* handle)
{
	if (!handle)
		return;

	[as_bridge(handle) resetEffects];
}
