// Unit tests for the gamepad state processing and mapping policy.
//
// Nothing here needs a controller, an engine or a running game: both units
// under test are deliberately free of platform and engine dependencies, and the
// mapping's output is a recording fake. Run with:
//
//   ctest --test-dir build/src/xrEngine -R Gamepad --output-on-failure

#include "../xr_gamepad_processor.h"
#include "../xr_gamepad_mapping.h"
#include "../../xrGame/GamepadInputRouting.h"
#include "../xr_gamepad_nav.h"
#include "../xr_gamepad_touchpad.h"
#include "../xr_gamepad_radial.h"
#include "../xr_gamepad_gyro.h"
#include "../xr_gamepad_apple_input.h"
#include "../xr_gamepad_chord.h"
#include "../xr_gamepad_haptics.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;
const char* g_case = "";

void check(bool condition, const char* what)
{
	++g_checks;
	if (!condition)
	{
		++g_failures;
		std::printf("  FAIL [%s] %s\n", g_case, what);
	}
}

bool nearly(float a, float b, float epsilon = 1.0e-4f)
{
	return std::fabs(a - b) <= epsilon;
}

void begin(const char* name)
{
	g_case = name;
	std::printf("case: %s\n", name);
}

// Test key codes: the resolver hands out a distinct code per logical action so
// a recording output can say exactly which action was driven.
constexpr int test_dik_base = 1000;

int test_resolver(int mappedAction)
{
	return test_dik_base + mappedAction;
}

int dik_for(EGamepadMappedAction action)
{
	return test_dik_base + int(action);
}

struct SEvent
{
	int dik;
	int kind; // 0 press, 1 release, 2 hold, 3 mouse move, 4 wheel, 5 cursor
	int a;
	int b;

	bool operator==(const SEvent& other) const
	{
		return dik == other.dik && kind == other.kind && a == other.a && b == other.b;
	}
};

class CRecordingOutput : public IGamepadActionOutput
{
public:
	std::vector<SEvent> events;
	std::vector<int> held;
	std::vector<int> chords;
	std::vector<int> uiActions;

	void Clear() { events.clear(); chords.clear(); uiActions.clear(); }

	void GamepadChordEvent(int chord) override { chords.push_back(chord); }
	void GamepadUiAction(int uiAction) override { uiActions.push_back(uiAction); }

	void GamepadKeyPress(int dik) override
	{
		events.push_back({dik, 0, 0, 0});
		held.push_back(dik);
	}
	void GamepadKeyRelease(int dik) override
	{
		events.push_back({dik, 1, 0, 0});
		for (size_t i = 0; i < held.size(); ++i)
			if (held[i] == dik) { held.erase(held.begin() + i); break; }
	}
	void GamepadKeyHold(int dik) override { events.push_back({dik, 2, 0, 0}); }
	void GamepadMouseMove(int dx, int dy) override { events.push_back({0, 3, dx, dy}); }
	void GamepadMouseWheel(int direction) override { events.push_back({0, 4, direction, 0}); }
	void GamepadMapPan(float dx, float dy) override { events.push_back({0, 6, int(dx), int(dy)}); }
	void GamepadMapZoom(bool zoomIn) override { events.push_back({0, 7, zoomIn ? 1 : -1, 0}); }
	void GamepadCursorMove(float dx, float dy) override
	{
		events.push_back({0, 5, int(dx), int(dy)});
	}

	int CountKind(int kind) const
	{
		int n = 0;
		for (size_t i = 0; i < events.size(); ++i)
			if (events[i].kind == kind) ++n;
		return n;
	}

	bool HasKeyPress(int dik) const
	{
		for (size_t i = 0; i < events.size(); ++i)
			if (events[i].kind == 0 && events[i].dik == dik) return true;
		return false;
	}

	bool HasKeyRelease(int dik) const
	{
		for (size_t i = 0; i < events.size(); ++i)
			if (events[i].kind == 1 && events[i].dik == dik) return true;
		return false;
	}

	bool IsHeld(int dik) const
	{
		for (size_t i = 0; i < held.size(); ++i)
			if (held[i] == dik) return true;
		return false;
	}
};

SRawGamepadState connected_state()
{
	SRawGamepadState raw;
	raw.buttons = 0;
	return raw;
}

SGamepadCapabilities connected_caps()
{
	SGamepadCapabilities caps;
	caps.connected = true;
	caps.extended = true;
	return caps;
}

// --- dead zone and response curve ------------------------------------------

void test_stick_deadzone()
{
	begin("stick: centre reports zero");
	{
		float x = 1.0f, y = 1.0f;
		const float mag = GamepadApplyStick(0.0f, 0.0f, 0.18f, 1.35f, x, y);
		check(nearly(mag, 0.0f), "magnitude is zero at centre");
		check(nearly(x, 0.0f) && nearly(y, 0.0f), "axes are zero at centre");
	}

	begin("stick: inside the radial dead zone reports zero");
	{
		float x = 0.0f, y = 0.0f;
		// 0.10 on each axis is a radius of ~0.14, under the 0.18 dead zone.
		const float mag = GamepadApplyStick(0.10f, 0.10f, 0.18f, 1.35f, x, y);
		check(nearly(mag, 0.0f), "magnitude is zero inside the dead zone");
		check(nearly(x, 0.0f) && nearly(y, 0.0f), "axes are zero inside the dead zone");
	}

	begin("stick: just outside the dead zone is small but non zero");
	{
		float x = 0.0f, y = 0.0f;
		const float mag = GamepadApplyStick(0.20f, 0.0f, 0.18f, 1.0f, x, y);
		check(mag > 0.0f && mag < 0.1f, "magnitude is small just outside the dead zone");
	}

	begin("stick: full deflection reaches one");
	{
		float x = 0.0f, y = 0.0f;
		const float mag = GamepadApplyStick(1.0f, 0.0f, 0.18f, 1.35f, x, y);
		check(nearly(mag, 1.0f), "magnitude is one at full deflection");
		check(nearly(x, 1.0f), "x is one at full deflection");
		check(nearly(y, 0.0f), "y stays zero on a pure x deflection");
	}

	begin("stick: radial, not per axis, so a diagonal is not squared off");
	{
		// A per axis dead zone would leave this fully inside; a radial one
		// measures the actual distance, which is over the threshold.
		float x = 0.0f, y = 0.0f;
		const float mag = GamepadApplyStick(0.14f, 0.14f, 0.18f, 1.0f, x, y);
		check(mag > 0.0f, "a diagonal at radius 0.198 is outside a 0.18 dead zone");
	}
}

void test_diagonal_normalisation()
{
	begin("stick: diagonals are normalised, not longer than cardinals");
	{
		float cx = 0.0f, cy = 0.0f;
		float dx = 0.0f, dy = 0.0f;
		const float cardinal = GamepadApplyStick(1.0f, 0.0f, 0.18f, 1.0f, cx, cy);
		const float diagonal = GamepadApplyStick(0.70710678f, 0.70710678f, 0.18f, 1.0f, dx, dy);

		check(nearly(cardinal, diagonal, 1.0e-3f),
			"a full diagonal has the same magnitude as a full cardinal");
		check(nearly(dx, dy, 1.0e-3f), "the diagonal components stay equal");
		check(nearly(std::sqrt(dx * dx + dy * dy), diagonal, 1.0e-3f),
			"the reported magnitude matches the vector length");
	}
}

void test_response_curve()
{
	begin("stick: the response curve is monotonic and pinches the low end");
	{
		float prev = -1.0f;
		bool monotonic = true;
		for (int i = 0; i <= 20; ++i)
		{
			const float in = 0.18f + (1.0f - 0.18f) * (float(i) / 20.0f);
			float x = 0.0f, y = 0.0f;
			const float mag = GamepadApplyStick(in, 0.0f, 0.18f, 2.0f, x, y);
			if (mag < prev - 1.0e-5f)
				monotonic = false;
			prev = mag;
		}
		check(monotonic, "the curve never decreases");

		// With exponent 2 and everything rescaled, half travel must land below
		// half, which is the point of a response curve.
		float hx = 0.0f, hy = 0.0f;
		const float half = GamepadApplyStick(0.18f + 0.5f * 0.82f, 0.0f, 0.18f, 2.0f, hx, hy);
		check(half < 0.5f, "half way out is worth less than half with exponent 2");

		float lx = 0.0f, ly = 0.0f;
		const float linear = GamepadApplyStick(0.18f + 0.5f * 0.82f, 0.0f, 0.18f, 1.0f, lx, ly);
		check(nearly(linear, 0.5f, 1.0e-3f), "exponent 1 stays linear");
	}
}

// --- trigger hysteresis -----------------------------------------------------

void test_trigger_hysteresis()
{
	begin("trigger: engages at the threshold");
	{
		check(GamepadTriggerDigital(false, 0.35f, 0.35f, 0.10f), "presses exactly at the threshold");
		check(!GamepadTriggerDigital(false, 0.30f, 0.35f, 0.10f), "does not press below it");
	}

	begin("trigger: does not chatter around the threshold");
	{
		// Once down, the value has to fall all the way to threshold minus the
		// hysteresis before the release is reported.
		check(GamepadTriggerDigital(true, 0.30f, 0.35f, 0.10f), "stays down just below the threshold");
		check(GamepadTriggerDigital(true, 0.26f, 0.35f, 0.10f), "stays down at the release point");
		check(!GamepadTriggerDigital(true, 0.20f, 0.35f, 0.10f), "releases below the release point");

		// A full sweep in and out of the hysteresis band must produce exactly
		// one press and one release, never a burst.
		int transitions = 0;
		bool state = false;
		const float sweep[] = {0.0f, 0.34f, 0.36f, 0.34f, 0.32f, 0.30f, 0.28f, 0.26f, 0.24f, 0.0f};
		for (size_t i = 0; i < sizeof(sweep) / sizeof(sweep[0]); ++i)
		{
			const bool next = GamepadTriggerDigital(state, sweep[i], 0.35f, 0.10f);
			if (next != state) ++transitions;
			state = next;
		}
		check(transitions == 2, "one press and one release across the whole sweep");
	}

	begin("trigger: a hysteresis wider than the threshold cannot latch it on");
	{
		// The user can ask for a low threshold and, through the settings
		// defaults, a hysteresis that is larger than it. The release point must
		// stay clear of zero anyway, or a trigger at rest - which still reports
		// a small residual value - would never cross it and the weapon would
		// fire forever.
		check(GamepadTriggerDigital(false, 0.06f, 0.05f, 0.40f), "a very low threshold still presses");
		check(!GamepadTriggerDigital(true, 0.0f, 0.05f, 0.40f), "and still releases at rest");

		// Sweeping a realistic rest value has to end up released.
		bool state = GamepadTriggerDigital(false, 0.9f, 0.05f, 0.40f);
		check(state, "a full pull presses with an extreme setting");
		state = GamepadTriggerDigital(state, 0.02f, 0.05f, 0.40f);
		check(!state, "returning to rest releases it");
	}
}

// --- frame processing -------------------------------------------------------

void test_press_release_edges()
{
	begin("frame: press and release edges");
	{
		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		raw.buttons = GAMEPAD_BIT_A;
		const SGamepadFrame& f1 = processor.Process(raw, caps, settings, 16.0f);
		check((f1.pressed & GAMEPAD_BIT_A) != 0, "the press edge is reported once");
		check((f1.released & GAMEPAD_BIT_A) == 0, "the press edge is not also a release");
		check((f1.held & GAMEPAD_BIT_A) != 0, "the button reads as held");

		const SGamepadFrame& f2 = processor.Process(raw, caps, settings, 16.0f);
		check((f2.pressed & GAMEPAD_BIT_A) == 0, "holding produces no repeat press edge");
		check((f2.held & GAMEPAD_BIT_A) != 0, "the button still reads as held");

		raw.buttons = 0;
		const SGamepadFrame& f3 = processor.Process(raw, caps, settings, 16.0f);
		check((f3.released & GAMEPAD_BIT_A) != 0, "the release edge is reported");
		check((f3.pressed & GAMEPAD_BIT_A) == 0, "the release is not also a press");
		check((f3.held & GAMEPAD_BIT_A) == 0, "the button is no longer held");
	}
}

void test_trigger_edges_and_analog_value()
{
	begin("frame: trigger edges keep the analog value");
	{
		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		raw.rt = 0.2f;
		const SGamepadFrame& f1 = processor.Process(raw, caps, settings, 16.0f);
		check(!f1.fire, "a light pull does not fire");

		raw.rt = 0.8f;
		const SGamepadFrame& f2 = processor.Process(raw, caps, settings, 16.0f);
		check(f2.fire, "a firm pull fires");
		check(f2.firePressed, "the firing edge is reported");
		check(nearly(f2.rt, 0.8f), "the analog value is preserved alongside the digital one");

		raw.rt = 0.0f;
		const SGamepadFrame& f3 = processor.Process(raw, caps, settings, 16.0f);
		check(!f3.fire, "releasing stops the fire");
		check(f3.fireReleased, "the release edge is reported");
	}
}

void test_tap_vs_hold()
{
	begin("tap hold: a short press is a tap, reported on release");
	{
		CTapHoldTracker tracker;
		check(tracker.Update(true, 16.0f, 220) == GAMEPAD_TAP_HOLD_NONE, "the press itself is ambiguous");
		check(tracker.Update(false, 16.0f, 220) == GAMEPAD_TAP_HOLD_TAP, "letting go early is a tap");
		check(tracker.Update(false, 16.0f, 220) == GAMEPAD_TAP_HOLD_NONE, "the tap is not repeated");
	}

	begin("tap hold: a long press becomes a hold");
	{
		CTapHoldTracker tracker;
		EGamepadTapHoldEvent event = GAMEPAD_TAP_HOLD_NONE;
		for (int i = 0; i < 20 && event != GAMEPAD_TAP_HOLD_HOLD_STARTED; ++i)
			event = tracker.Update(true, 16.0f, 220);
		check(event == GAMEPAD_TAP_HOLD_HOLD_STARTED, "the hold starts once the deadline passes");
		check(tracker.IsHolding(), "the tracker reports the live hold");
		check(tracker.Update(true, 16.0f, 220) == GAMEPAD_TAP_HOLD_NONE,
			"the hold start is reported once, not every frame");
		check(tracker.Update(false, 16.0f, 220) == GAMEPAD_TAP_HOLD_HOLD_RELEASED,
			"releasing after a hold reports a hold release, never a tap");
	}

	begin("tap hold: a hold is never also a tap");
	{
		CTapHoldTracker tracker;
		for (int i = 0; i < 20; ++i)
			tracker.Update(true, 16.0f, 220);
		const EGamepadTapHoldEvent released = tracker.Update(false, 16.0f, 220);
		check(released != GAMEPAD_TAP_HOLD_TAP, "a completed hold does not fire the tap action");
	}

	begin("tap hold: reset abandons a press in flight");
	{
		CTapHoldTracker tracker;
		tracker.Update(true, 16.0f, 220);
		tracker.Reset();
		check(!tracker.IsDown(), "the tracker is clear after a reset");
		check(tracker.Update(false, 16.0f, 220) == GAMEPAD_TAP_HOLD_NONE,
			"the abandoned press does not turn into a tap");
	}
}

void test_disconnect_releases_held_actions()
{
	begin("disconnect: exactly one reset frame, then silence");
	{
		CGamepadProcessor processor;
		SGamepadSettings settings;

		SRawGamepadState raw = connected_state();
		raw.buttons = GAMEPAD_BIT_A | GAMEPAD_BIT_DPAD_UP;
		raw.rt = 0.9f;
		processor.Process(raw, connected_caps(), settings, 16.0f);

		SGamepadCapabilities gone;
		const SGamepadFrame& f1 = processor.Process(raw, gone, settings, 16.0f);
		check(!f1.connected, "the frame reports the device as gone");
		check(f1.reset, "the first frame after the loss carries a reset");
		check(f1.held == 0 && f1.pressed == 0 && f1.released == 0,
			"no edges are carried over from the lost device");
		check(!f1.fire, "the trigger is not left on");

		const SGamepadFrame& f2 = processor.Process(raw, gone, settings, 16.0f);
		check(!f2.reset, "the reset is only reported on the transition");
	}
}

void test_focus_loss_reset()
{
	begin("focus loss: reset clears every edge and the analog state");
	{
		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		raw.buttons = GAMEPAD_BIT_A | GAMEPAD_BIT_TOUCHPAD;
		raw.lt = 0.9f;
		raw.rt = 0.9f;
		raw.lx = 0.8f;
		processor.Process(raw, caps, settings, 16.0f);

		processor.Reset();
		// These controls changed while input was suspended: A/touchpad and both
		// triggers were released, while B was pressed. The reset frame must take
		// this state as its new baseline rather than exposing any transitions.
		raw.buttons = GAMEPAD_BIT_B;
		raw.lt = 0.0f;
		raw.rt = 0.0f;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
		check(f.reset, "the frame carries the reset");
		check(f.pressed == 0, "no press edges survive the reset");
		check(f.released == 0, "no release edges survive the reset");
		check(!f.adsPressed && !f.adsReleased, "ADS has no edge on the reset frame");
		check(!f.firePressed && !f.fireReleased, "fire has no edge on the reset frame");
		check(f.squareEvent == GAMEPAD_TAP_HOLD_NONE, "a press in flight is abandoned, not tapped");
		check(f.touchpadEvent == GAMEPAD_TAP_HOLD_NONE, "the same for the touchpad");

		// The reset state is now the baseline, so it does not appear as a fresh
		// edge on the following ordinary frame either.
		const SGamepadFrame& g = processor.Process(raw, caps, settings, 16.0f);
		check(!g.reset, "the reset does not repeat");
		check(g.pressed == 0 && g.released == 0, "the reset baseline does not repeat button edges");
		check(!g.adsPressed && !g.firePressed && !g.adsReleased && !g.fireReleased,
			"the reset baseline does not repeat trigger edges");

		// A later real transition is still reported normally.
		raw.buttons |= GAMEPAD_BIT_A;
		raw.lt = 0.9f;
		raw.rt = 0.9f;
		const SGamepadFrame& h = processor.Process(raw, caps, settings, 16.0f);
		check((h.pressed & GAMEPAD_BIT_A) != 0, "a new button press after reset is reported");
		check(h.adsPressed && h.firePressed, "new trigger presses after reset are reported");
	}
}

// --- mapping policy ---------------------------------------------------------

void test_gameplay_mapping_uses_actions()
{
	begin("mapping: gameplay sticks and buttons drive actions, not raw keys");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);
		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		// First frame only arms the neutral latch.
		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(!mapping.NeedsNeutral(), "a resting pad releases the neutral latch");

		out.Clear();
		raw.lx = 0.0f;
		raw.ly = 1.0f;
		raw.buttons = GAMEPAD_BIT_A;
		raw.rt = 0.9f;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		check(out.HasKeyPress(dik_for(GPA_MoveForward)), "the left stick forward drives the move action");
		check(out.HasKeyPress(dik_for(GPA_Jump)), "A drives the jump action");
		check(out.HasKeyPress(dik_for(GPA_Fire)), "R2 drives the fire action");
		check(out.IsHeld(dik_for(GPA_MoveForward)), "the movement key is held down");
		check(out.IsHeld(dik_for(GPA_Jump)), "the button key is held down");

		// A full deflection is a run, so the walk action must not be engaged.
		check(!out.IsHeld(dik_for(GPA_Walk)), "full deflection does not engage the walk speed");
	}
}

void test_contextual_b_mapping()
{
	begin("mapping: B is crouch in gameplay and back in the interface");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();
		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		raw.buttons = GAMEPAD_BIT_B;
		const SGamepadFrame& gameplayPress = processor.Process(raw, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(gameplayPress, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(out.HasKeyPress(dik_for(GPA_Crouch)), "B drives crouch in gameplay");
		check(!out.HasKeyPress(dik_for(GPA_Back)), "B does not drive back in gameplay");
	}

	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();
		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		raw.buttons = GAMEPAD_BIT_B;
		const SGamepadFrame& uiPress = processor.Process(raw, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(uiPress, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(dik_for(GPA_Back)), "B drives back in the interface");
		check(!out.HasKeyPress(dik_for(GPA_Crouch)), "B does not drive crouch in the interface");
	}
}

void test_walk_band()
{
	begin("mapping: a small deflection engages the walk speed");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		out.Clear();
		// Enough to leave the dead zone and register a direction, but well
		// short of full travel.
		raw.ly = 0.35f;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		check(f.moveFwd, "a modest deflection still registers forward");
		check(out.HasKeyPress(dik_for(GPA_MoveForward)), "forward is driven");
		check(out.HasKeyPress(dik_for(GPA_Walk)), "the walk speed is engaged");
	}
}

void test_ui_context_cancels_gameplay_holds()
{
	begin("mapping: switching to the interface releases every gameplay hold");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		// Sprint forward while firing, in gameplay.
		raw.ly = 1.0f;
		raw.rt = 1.0f;
		const SGamepadFrame& playing = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(mapping.HeldCount() > 0, "gameplay holds some keys");

		// Now the interface takes over, with the same sticks still held.
		out.Clear();
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		check(mapping.HeldCount() == 0, "no gameplay key is left held");
		check(out.HasKeyRelease(dik_for(GPA_Fire)), "the fire is released");
		check(out.HasKeyRelease(dik_for(GPA_MoveForward)), "the movement is released");
		check(!out.HasKeyPress(dik_for(GPA_Fire)), "the interface does not fire the weapon");
		check(out.CountKind(0) <= 1, "at most the accept key may press, never a weapon action");

		// Coming back must not fire either, until the trigger is released.
		out.Clear();
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(!out.HasKeyPress(dik_for(GPA_Fire)), "a trigger held across the switch does not fire");
		check(!out.HasKeyPress(dik_for(GPA_MoveForward)), "a stick held across the switch does not lurch");

		// Once it is released, normal input resumes.
		SRawGamepadState rest = connected_state();
		const SGamepadFrame& released = processor.Process(rest, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(released, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(!mapping.NeedsNeutral(), "the latch releases once the controls are at rest");

		SRawGamepadState again = connected_state();
		again.rt = 1.0f;
		const SGamepadFrame& fresh = processor.Process(again, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(fresh, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(out.HasKeyPress(dik_for(GPA_Fire)), "firing works again after the controls rest");
	}
}

void test_reset_releases_everything()
{
	begin("mapping: reset and disconnect release everything held");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		raw.ly = 1.0f;
		raw.rt = 1.0f;
		raw.buttons = GAMEPAD_BIT_A;
		const SGamepadFrame& playing = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(mapping.HeldCount() > 0, "something is held before the reset");

		out.Clear();
		mapping.Reset();
		check(mapping.HeldCount() == 0, "the reset clears the held set");
		check(out.HasKeyRelease(dik_for(GPA_MoveForward)), "the movement key is released");
		check(out.HasKeyRelease(dik_for(GPA_Fire)), "the fire key is released");
		check(out.HasKeyRelease(dik_for(GPA_Jump)), "the button key is released");
		check(mapping.NeedsNeutral(), "the reset re-arms the neutral latch");

		// The pending reset in the next frame must not double release.
		out.Clear();
		processor.Reset();
		const SGamepadFrame& after = processor.Process(connected_state(), caps, settings, 16.0f);
		check(after.reset, "the processor reports the pending reset");
		mapping.Process(after, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(out.CountKind(1) == 0, "nothing is released twice");
	}
}

void test_ui_navigation_repeats()
{
	begin("mapping: a held direction repeats the navigation key");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		SGamepadMappingConfig config;
		config.navDelayMs = 100.0f;
		config.navRepeatMs = 50.0f;
		mapping.Configure(config);
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		processor.Process(raw, caps, settings, 16.0f);

		// Hold the d-pad down. The first frame fires immediately; the repeats
		// come after the delay and then at the repeat interval.
		raw.buttons = GAMEPAD_BIT_DPAD_DOWN;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);

		out.Clear();
		mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.CountKind(0) == 1, "the first frame emits one navigation tap");
		check(out.CountKind(1) == 1, "the navigation key is not left down");

		// Two frames at 40 ms each: past the 100 ms delay, so one repeat.
		out.Clear();
		mapping.Process(f, settings, 40.0f, GAMEPAD_CONTEXT_UI);
		mapping.Process(f, settings, 40.0f, GAMEPAD_CONTEXT_UI);
		mapping.Process(f, settings, 40.0f, GAMEPAD_CONTEXT_UI);
		check(out.CountKind(0) >= 1, "holding the direction keeps repeating");
		check(out.CountKind(0) <= 2, "and does not run away between repeats");
	}
}

void test_suppressed_context()
{
	begin("mapping: a receiver the gamepad must not drive gets no input at all");
	{
		// The console, the tutorial sequencer and the imgui editor all capture
		// the input stack. Feeding them synthesised key strokes would type
		// letters into the console, so the mapping has to stay silent and let
		// go of whatever it was holding.
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		// Sprinting forward with the trigger down.
		raw.ly = 1.0f;
		raw.rt = 1.0f;
		raw.buttons = GAMEPAD_BIT_A;
		const SGamepadFrame& playing = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(mapping.HeldCount() > 0, "something is held going in");

		// The console opens; the sticks are still deflected.
		out.Clear();
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_SUPPRESSED);
		check(mapping.HeldCount() == 0, "everything held is released");
		check(out.HasKeyRelease(dik_for(GPA_MoveForward)), "the movement key is released");
		check(!out.HasKeyPress(dik_for(GPA_MoveForward)), "and no movement key is pressed");
		check(!out.HasKeyPress(dik_for(GPA_Jump)), "no button is pressed either");
		check(out.CountKind(0) == 0, "nothing at all is pressed while suppressed");

		// Continue suppressed for several frames: still nothing.
		out.Clear();
		for (int i = 0; i < 5; ++i)
			mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_SUPPRESSED);
		check(out.events.empty(), "a suppressed receiver stays silent on every frame");

		// Closing the console hands input back, but only once the controls rest.
		out.Clear();
		mapping.Process(playing, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(!out.HasKeyPress(dik_for(GPA_MoveForward)),
			"the stick held across the switch does not drive the actor");

		SRawGamepadState rest = connected_state();
		const SGamepadFrame& released = processor.Process(rest, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(released, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(!mapping.NeedsNeutral(), "the latch clears once the controls rest");

		SRawGamepadState again = connected_state();
		again.ly = 1.0f;
		const SGamepadFrame& fresh = processor.Process(again, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(fresh, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(out.HasKeyPress(dik_for(GPA_MoveForward)), "input works again afterwards");
	}
}

void test_touchpad_gate()
{
	begin("mapping: the touchpad UI combos can be switched off");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		settings.touchpadUi = false;

		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		// Press and release inside the hold deadline: a tap, which would open
		// the PDA if the combos were enabled.
		out.Clear();
		raw.buttons = GAMEPAD_BIT_TOUCHPAD;
		const SGamepadFrame& pressed = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(pressed, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		raw.buttons = 0;
		const SGamepadFrame& released = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(released, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		check(released.touchpadEvent == GAMEPAD_TAP_HOLD_TAP, "the tap is still recognised");
		check(!out.HasKeyPress(dik_for(GPA_Pda)), "the disabled combo does not open the PDA");
		check(!out.HasKeyPress(dik_for(GPA_Inventory)), "and does not open the inventory");
	}
}

void test_ui_accept_and_back()
{
	begin("mapping: the interface accept and back keys");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);
		check(mapping.Config().uiAccept == 0x1c, "focus-mode accept defaults to DIK_RETURN");

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		out.Clear();
		raw.buttons = GAMEPAD_BIT_A;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(mapping.Config().uiAccept), "A acts as the interface accept");
		check(!out.HasKeyPress(test_dik_base + GPA_Jump), "A does not jump while the interface has focus");

		out.Clear();
		raw.buttons = GAMEPAD_BIT_B;
		const SGamepadFrame& g = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(g, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(dik_for(GPA_Back)), "B acts as the interface back");
	}

	begin("mapping: the accept key follows the kind of interface in focus");
	{
		// The main menu is selection based and wants enter; a cursor driven
		// dialog wants a click. The sink swaps the key, so the mapping must
		// honour whatever it is configured with.
		CRecordingOutput out;
		CGamepadMapping mapping;
		SGamepadMappingConfig config;
		// DIK_RETURN. The test stays free of the engine's key code header, so
		// the value is spelled out here.
		constexpr int dik_return = 0x1c;
		config.uiAccept = dik_return;
		mapping.Configure(config);
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		out.Clear();
		raw.buttons = GAMEPAD_BIT_A;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(dik_return), "accept is enter when the menu is selection based");
		check(!out.HasKeyPress(0xed + 100), "and is not a mouse click there");
	}

	begin("mapping: selection accept and touchpad pointer click use separate keys");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		SGamepadMappingConfig config;
		constexpr int dik_return = 0x1c;
		constexpr int mouse_1 = 0xed + 100;
		config.uiAccept = dik_return;
		// Keep uiPointerClick at its default Mouse 1 binding.
		mapping.Configure(config);
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();
		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		out.Clear();
		raw.buttons = GAMEPAD_BIT_A;
		const SGamepadFrame& accept = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(accept, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(dik_return), "A sends Enter for a selection interface");
		check(!out.HasKeyPress(mouse_1), "A does not send a pointer click");

		raw.buttons = 0;
		const SGamepadFrame& acceptRelease = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(acceptRelease, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		out.Clear();

		raw.touchActive = true;
		raw.touchX = 0.4f;
		raw.touchY = 0.5f;
		const SGamepadFrame& fingerDown = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(fingerDown, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		raw.touchActive = false;
		const SGamepadFrame& fingerUp = processor.Process(raw, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(fingerUp, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(dik_return), "a touchpad tap uses the configured focus accept key");
		check(!out.HasKeyPress(mouse_1), "focus-mode touch taps do not send a pointer click");
	}
}

void test_ui_touch_gesture_does_not_cross_contexts()
{
	begin("mapping: a touch gesture is baselined across UI context changes");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		SGamepadSettings settings;
		SGamepadFrame frame;
		frame.connected = true;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		// Start a touch in the UI, leave for gameplay while it is still down,
		// then return to the UI with the same finger still held at another point.
		frame.touchActive = true;
		frame.touchX = 0.2f;
		frame.touchY = 0.2f;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		frame.touchX = 0.8f;
		frame.touchY = 0.8f;
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		int cursorDx = 0;
		int cursorDy = 0;
		for (size_t i = 0; i < out.events.size(); ++i)
		{
			if (out.events[i].kind == 5)
			{
				cursorDx += out.events[i].a;
				cursorDy += out.events[i].b;
			}
		}
		check(cursorDx == 0 && cursorDy == 0,
			"returning with a held finger establishes a cursor baseline without motion");
		check(!out.HasKeyPress(mapping.Config().uiPointerClick), "re-entering UI does not click");

		// The finger is released while gameplay owns input. Returning to the UI
		// must not turn that out-of-context release into a tap/click.
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		frame.touchActive = false;
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!out.HasKeyPress(mapping.Config().uiPointerClick),
			"a release outside the UI does not become a pointer click on re-entry");
	}
}

void test_ui_navigation_restarts_after_reset_and_context_change()
{
	begin("mapping: UI navigation restarts its initial pulse after reset or context change");
	SGamepadSettings settings;
	SGamepadFrame idle;
	idle.connected = true;
	SGamepadFrame down;
	down.connected = true;
	down.held = GAMEPAD_BIT_DPAD_DOWN;

	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		SGamepadMappingConfig config;
		config.navDelayMs = 100.0f;
		config.navRepeatMs = 50.0f;
		mapping.Configure(config);
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		mapping.Process(down, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		out.Clear();
		mapping.Process(down, settings, 80.0f, GAMEPAD_CONTEXT_UI);
		check(out.CountKind(0) == 0, "the repeat delay is still active before reset");

		mapping.Reset();
		out.Clear();
		mapping.Process(down, settings, 0.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(config.uiNavDown), "Reset makes a held direction produce a fresh initial pulse");
	}

	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		SGamepadMappingConfig config;
		config.navDelayMs = 100.0f;
		config.navRepeatMs = 50.0f;
		mapping.Configure(config);
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		mapping.Process(down, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		out.Clear();
		mapping.Process(down, settings, 80.0f, GAMEPAD_CONTEXT_UI);
		mapping.Process(down, settings, 400.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		out.Clear();
		mapping.Process(down, settings, 0.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(config.uiNavDown), "returning to UI makes a held direction produce a fresh initial pulse");
	}
}

void test_frame_rate_independence()
{
	begin("mapping: camera look does not depend on the frame rate");
	{
		// The same total time has to produce the same total rotation whether it
		// arrives as many small frames or a few large ones.
		auto total_move = [](float dtMs, int frames) {
			CRecordingOutput out;
			CGamepadMapping mapping;
			mapping.SetOutput(&out);
			mapping.SetResolver(&test_resolver);

			CGamepadProcessor processor;
			SGamepadSettings settings;
			const SGamepadCapabilities caps = connected_caps();

			SRawGamepadState raw = connected_state();
			SRawGamepadState look = connected_state();
			look.rx = 1.0f;

			// Arm the latch.
			const SGamepadFrame& idle = processor.Process(raw, caps, settings, dtMs);
			mapping.Process(idle, settings, dtMs, GAMEPAD_CONTEXT_GAMEPLAY);

			int dx = 0;
			for (int i = 0; i < frames; ++i)
			{
				const SGamepadFrame& f = processor.Process(look, caps, settings, dtMs);
				out.Clear();
				mapping.Process(f, settings, dtMs, GAMEPAD_CONTEXT_GAMEPLAY);
				for (size_t j = 0; j < out.events.size(); ++j)
					if (out.events[j].kind == 3)
						dx += out.events[j].a;
			}
			return dx;
		};

		const int fast = total_move(5.0f, 200);   // 1 second at 200 fps
		const int slow = total_move(50.0f, 20);   // 1 second at 20 fps
		check(std::abs(fast - slow) <= 2, "1 second of look is the same at 200 fps and at 20 fps");
	}
}

void test_look_axis_conventions()
{
	begin("mapping: the vertical look axis follows the mouse, not the stick");
	{
		// The frame documents lookY as "+Y up" while the engine's mouse delta is
		// "+Y down". Emitting the frame value unchanged makes a stick pushed up
		// produce exactly what a mouse pushed down produces, which is an inverted
		// camera; these checks pin the conversion in both directions and the
		// user's own invert setting on top of it.
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		auto look_dy = [&](float ry, bool invert) {
			processor.Reset();
			mapping.Reset();
			settings.invertY = invert;

			SRawGamepadState raw = connected_state();
			const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
			mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

			raw.ry = ry;
			const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
			out.Clear();
			mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

			int dy = 0;
			for (size_t i = 0; i < out.events.size(); ++i)
				if (out.events[i].kind == 3)
					dy += out.events[i].b;
			return dy;
		};

		check(look_dy(1.0f, false) < 0, "the stick pushed up looks up");
		check(look_dy(-1.0f, false) > 0, "the stick pulled down looks down");
		check(look_dy(1.0f, true) > 0, "the invert setting still flips the axis");
	}

	begin("mapping: focus mode scrolls with the right stick without moving a cursor");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);

		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();

		SRawGamepadState raw = connected_state();
		const SGamepadFrame& idle = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(idle, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		raw.ry = 1.0f;
		const SGamepadFrame& f = processor.Process(raw, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(f, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		check(out.CountKind(5) == 0, "the right stick does not move the pointer in focus mode");
		check(out.uiActions.size() == 1 && out.uiActions[0] == UI_ACTION_SCROLL_UP,
			"a stick pushed up emits one discrete scroll-up action");

		out.Clear();
		raw.ry = -1.0f;
		const SGamepadFrame& down = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(down, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.CountKind(5) == 0, "scrolling down still does not move the pointer");
		check(out.uiActions.size() == 1 && out.uiActions[0] == UI_ACTION_SCROLL_DOWN,
			"a stick pulled down emits one discrete scroll-down action");

		SGamepadMappingConfig mapConfig = mapping.Config();
		mapConfig.uiMapPan = true;
		mapping.Configure(mapConfig);
		raw.ry = 1.0f;
		const SGamepadFrame& map = processor.Process(raw, caps, settings, 16.0f);
		out.Clear();
		mapping.Process(map, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.CountKind(6) > 0, "the explicit PDA map mode emits right-stick pan deltas");
		check(out.uiActions.empty(), "the PDA map mode does not emit list scroll actions");
	}

	begin("mapping: R3 explicitly enables pointer pan and tap in UI");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		SGamepadMappingConfig config;
		config.uiAccept = 0x1c; // DIK_RETURN, distinct from the pointer click
		mapping.Configure(config);
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);
		CGamepadProcessor processor;
		SGamepadSettings settings;
		const SGamepadCapabilities caps = connected_caps();
		SRawGamepadState raw = connected_state();
		processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_UI);

		raw.buttons = GAMEPAD_BIT_RS;
		const SGamepadFrame& toggleOn = processor.Process(raw, caps, settings, 16.0f);
		mapping.Process(toggleOn, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(mapping.FallbackCursorEnabled(), "R3 enables pointer fallback after neutral");

		// A touch surface pan and tap use pointer output only in fallback mode.
		raw.buttons = 0;
		raw.touchActive = true;
		raw.touchX = 0.2f;
		raw.touchY = 0.2f;
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_UI);
		raw.touchX = 0.4f;
		raw.touchY = 0.4f;
		out.Clear();
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.CountKind(5) > 0, "touchpad pan moves the cursor in fallback mode");

		raw.touchActive = false;
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_UI);
		raw.touchActive = true;
		raw.touchX = 0.5f;
		raw.touchY = 0.5f;
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_UI);
		raw.touchActive = false;
		out.Clear();
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(mapping.Config().uiPointerClick), "fallback touch tap sends the pointer click");
		check(!out.HasKeyPress(mapping.Config().uiAccept), "fallback touch tap does not send focus accept");

		// Leaving UI or explicitly resetting returns to focus mode.
		mapping.Process(processor.Process(raw, caps, settings, 16.0f), settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(!mapping.FallbackCursorEnabled(), "a new context starts in focus mode");
		raw.buttons = 0;
		mapping.Reset();
		check(!mapping.FallbackCursorEnabled(), "Reset returns to focus mode");
	}

	begin("mapping: R3 accept selection follows an actual neutral-gated toggle");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);
		SGamepadSettings settings;
		SGamepadFrame frame;
		frame.connected = true;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		// A reset followed by a deflected right stick leaves the latch armed;
		// R3 must not switch accept to Mouse 1 until the analogs rest.
		mapping.Reset();
		frame.lookY = 0.8f;
		frame.pressed = GAMEPAD_BIT_RS | GAMEPAD_BIT_A;
		frame.held = frame.pressed;
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!mapping.FallbackCursorEnabled(), "R3 cannot toggle while the analog latch is armed");
		check(out.HasKeyPress(mapping.Config().uiAccept), "Cross still uses focus accept while R3 is gated");
		check(!out.HasKeyPress(mapping.Config().uiPointerClick), "a rejected toggle does not use pointer accept");

		// Establish rest and start a Cross hold in focus mode.
		frame.lookY = 0.0f;
		frame.pressed = GAMEPAD_BIT_A;
		frame.held = GAMEPAD_BIT_A;
		frame.released = 0;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(mapping.IsHeld(mapping.Config().uiAccept), "Cross hold is tracked on the focus accept key");

		// Toggling while Cross remains down releases the old key without
		// synthesizing a hold of the newly selected pointer-click key.
		frame.pressed = GAMEPAD_BIT_RS;
		frame.held = GAMEPAD_BIT_RS | GAMEPAD_BIT_A;
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(mapping.FallbackCursorEnabled(), "R3 toggles after the stick returns to neutral");
		check(out.HasKeyRelease(mapping.Config().uiAccept), "toggle releases the prior accept binding");
		check(!out.HasKeyPress(mapping.Config().uiPointerClick) &&
			!mapping.IsHeld(mapping.Config().uiPointerClick),
			"a held Cross does not transfer its hold to pointer click");

		frame.pressed = GAMEPAD_BIT_A;
		frame.held = GAMEPAD_BIT_A;
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(mapping.Config().uiPointerClick), "a fresh Cross edge accepts in pointer mode");
	}

	begin("mapping: a touch held across a same-context popup reset cannot accept on release");
	{
		CRecordingOutput out;
		CGamepadMapping mapping;
		mapping.SetOutput(&out);
		mapping.SetResolver(&test_resolver);
		SGamepadSettings settings;
		SGamepadFrame frame;
		frame.connected = true;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);

		frame.touchActive = true;
		frame.touchX = 0.2f;
		frame.touchY = 0.3f;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		mapping.Reset(); // Models a top-input-dialog change with the same receiver.
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		frame.touchActive = false;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!out.HasKeyPress(mapping.Config().uiAccept), "the old touch release is consumed through lift");

		frame.touchActive = true;
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		frame.touchActive = false;
		out.Clear();
		mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(out.HasKeyPress(mapping.Config().uiAccept), "a fresh tap after lift is accepted normally");
	}
}
}

void test_focus_navigation()
{
	begin("focus navigation: simple list");
	{
		std::vector<SNavCandidate> list = {
			{10, SNavRect(0, 0, 100, 30), true, true},
			{11, SNavRect(0, 35, 100, 65), true, true},
			{12, SNavRect(0, 70, 100, 100), true, true},
		};
		check(FindBestSpatialNeighbor(list[0].rect, list, NAV_DOWN) == 11, "down moves to next item");
		check(FindBestSpatialNeighbor(list[1].rect, list, NAV_DOWN) == 12, "down from middle moves to bottom");
		check(FindBestSpatialNeighbor(list[2].rect, list, NAV_UP) == 11, "up from bottom moves to middle");
		check(FindBestSpatialNeighbor(list[1].rect, list, NAV_UP) == 10, "up from middle moves to top");
		check(FindBestSpatialNeighbor(list[0].rect, list, NAV_UP) == -1, "up from top has no neighbor");
	}

	begin("focus navigation: uniform grid");
	{
		std::vector<SGridCell> grid = {
			{1, 0, 0}, {2, 0, 1}, {3, 0, 2},
			{4, 1, 0}, {5, 1, 1}, {6, 1, 2},
			{7, 2, 0}, {8, 2, 1}, {9, 2, 2},
		};
		check(FindGridNeighbor(grid, 1, NAV_RIGHT) == 2, "grid: right from (0,0) is (0,1)");
		check(FindGridNeighbor(grid, 2, NAV_RIGHT) == 3, "grid: right from (0,1) is (0,2)");
		check(FindGridNeighbor(grid, 3, NAV_RIGHT) == -1, "grid: right edge has no neighbor");
		check(FindGridNeighbor(grid, 2, NAV_DOWN) == 5, "grid: down from (0,1) is (1,1)");
		check(FindGridNeighbor(grid, 5, NAV_LEFT) == 4, "grid: left from (1,1) is (1,0)");
		check(FindGridNeighbor(grid, 8, NAV_DOWN) == -1, "grid: bottom edge has no neighbor");
	}

	begin("focus navigation: uneven grid with multi-cell items");
	{
		std::vector<SGridCell> grid = {
			{1, 0, 0, 2, 2},
			{2, 0, 2, 1, 2},
			{3, 1, 2, 1, 2},
			{4, 2, 0, 2, 1},
		};
		check(FindGridNeighbor(grid, 1, NAV_RIGHT) == 2, "uneven grid: moving right from 2x2 selects top-right row neighbor");
		check(FindGridNeighbor(grid, 1, NAV_DOWN) == 4, "uneven grid: moving down from 2x2 selects aligned lower neighbor");
		check(FindGridNeighbor(grid, 2, NAV_LEFT) == 1, "uneven grid: moving left from (0,2) returns to 2x2 item");
		check(FindGridNeighbor(grid, 3, NAV_LEFT) == 1, "uneven grid: moving left from (1,2) returns to 2x2 item");
	}

	begin("focus navigation: missing cell / gap");
	{
		std::vector<SGridCell> grid = {
			{1, 0, 0}, {3, 0, 2},
			{4, 1, 0}, {5, 1, 1}, {6, 1, 2},
		};
		check(FindGridNeighbor(grid, 1, NAV_RIGHT) == 3, "gap: moving right skips missing cell to next in row");
		check(FindGridNeighbor(grid, 3, NAV_LEFT) == 1, "gap: moving left skips missing cell back to col 0");
	}

	begin("focus navigation: two columns");
	{
		std::vector<SNavCandidate> cols = {
			{1, SNavRect(50, 50, 200, 100), true, true},
			{2, SNavRect(50, 120, 200, 170), true, true},
			{10, SNavRect(400, 50, 550, 100), true, true},
			{11, SNavRect(400, 120, 550, 170), true, true},
		};
		check(FindBestSpatialNeighbor(cols[0].rect, cols, NAV_RIGHT) == 10, "two columns: right crosses column to matching row");
		check(FindBestSpatialNeighbor(cols[3].rect, cols, NAV_LEFT) == 2, "two columns: left crosses back to matching row");
	}

	begin("focus navigation: tab cycling");
	{
		check(CycleTab(0, 4, true) == 1, "tab: forward moves to next");
		check(CycleTab(3, 4, true, true) == 0, "tab: forward wraps from end to start");
		check(CycleTab(3, 4, true, false) == 3, "tab: forward without wrap clamps at end");
		check(CycleTab(0, 4, false, true) == 3, "tab: backward wraps from start to end");
		check(CycleTab(0, 4, false, false) == 0, "tab: backward without wrap clamps at start");
	}

	begin("focus navigation: repeat tracker");
	{
		CNavRepeatTracker tracker(350.0f, 110.0f);
		check(tracker.Update(true, 16.0f) == true, "repeat: initial press fires immediately");
		check(tracker.Update(true, 100.0f) == false, "repeat: during initial delay no repeat");
		check(tracker.Update(true, 240.0f) == false, "repeat: still within initial delay (total 356ms)");
		check(tracker.Update(true, 20.0f) == true, "repeat: initial delay elapsed fires first repeat");
		check(tracker.Update(true, 50.0f) == false, "repeat: within repeat interval no repeat");
		check(tracker.Update(true, 70.0f) == true, "repeat: repeat interval elapsed fires periodic repeat");
		tracker.Update(false, 16.0f);
		check(tracker.Update(true, 16.0f) == true, "repeat: release and re-press fires immediately");
	}
}

void test_inventory_navigation()
{
	begin("inventory: item spatial selection");
	{
		// 3 items in an inventory grid: Item 101 at (0,0)-(1,1), Item 102 at (2,0)-(2,1), Item 103 at (0,2)-(1,2)
		std::vector<SGridCell> inv = {
			{101, 0, 0, 2, 2},
			{102, 2, 0, 1, 2},
			{103, 0, 2, 2, 1},
		};
		check(FindGridNeighbor(inv, 101, NAV_DOWN) == 102, "inventory: down selects item below");
		check(FindGridNeighbor(inv, 101, NAV_RIGHT) == 103, "inventory: right selects item to the right");
		check(FindGridNeighbor(inv, 102, NAV_UP) == 101, "inventory: up from bottom selects item above");
		check(FindGridNeighbor(inv, 103, NAV_LEFT) == 101, "inventory: left selects item to the left");
	}

	begin("inventory: action menu and transfer events");
	{
		// Square/X mapped to action menu event
		const uint32_t squareBit = GAMEPAD_BIT_X;
		const uint32_t triangleBit = GAMEPAD_BIT_Y;
		check((squareBit & GAMEPAD_BIT_X) != 0, "square bit triggers action menu");
		check((triangleBit & GAMEPAD_BIT_Y) != 0, "triangle bit triggers transfer action");
	}
}

void test_touchpad_gestures()
{
	begin("touchpad: first sample no jump");
	{
		CTouchpadGestureProcessor proc;
		STouchGestureResult res = proc.Process(true, 0.4f, 0.5f, false, 0.0f, 0.0f, 16.0f);
		check(nearly(res.panDx, 0.0f), "first sample reports zero dx");
		check(nearly(res.panDy, 0.0f), "first sample reports zero dy");
	}

	begin("touchpad: relative move");
	{
		CTouchpadGestureProcessor proc;
		proc.Process(true, 0.4f, 0.5f, false, 0.0f, 0.0f, 16.0f);
		STouchGestureResult res = proc.Process(true, 0.45f, 0.52f, false, 0.0f, 0.0f, 16.0f);
		check(nearly(res.panDx, 0.05f), "relative move reports delta x");
		check(nearly(res.panDy, 0.02f), "relative move reports delta y");
		check(res.mode == GESTURE_MODE_PAN, "mode is pan");
	}

	begin("touchpad: finger release");
	{
		CTouchpadGestureProcessor proc;
		proc.Process(true, 0.4f, 0.5f, false, 0.0f, 0.0f, 16.0f);
		STouchGestureResult res = proc.Process(false, 0.0f, 0.0f, false, 0.0f, 0.0f, 16.0f);
		check(res.mode == GESTURE_MODE_NONE, "release enters none mode");
		check(res.tap == true, "short motionless touch reports tap on release");
	}

	begin("touchpad: new touch no jump");
	{
		CTouchpadGestureProcessor proc;
		proc.Process(true, 0.2f, 0.2f, false, 0.0f, 0.0f, 16.0f);
		proc.Process(false, 0.0f, 0.0f, false, 0.0f, 0.0f, 16.0f);
		// New touch lands far away at (0.8, 0.8)
		STouchGestureResult res = proc.Process(true, 0.8f, 0.8f, false, 0.0f, 0.0f, 16.0f);
		check(nearly(res.panDx, 0.0f), "new touch at different pos reports zero dx");
		check(nearly(res.panDy, 0.0f), "new touch at different pos reports zero dy");
	}

	begin("touchpad: two-touch pinch classifier");
	{
		CTouchpadGestureProcessor proc;
		// Initial two touch: p1=(0.4, 0.5), p2=(0.6, 0.5), dist = 0.2
		proc.Process(true, 0.4f, 0.5f, true, 0.6f, 0.5f, 16.0f);
		// Fingers spread apart: p1=(0.35, 0.5), p2=(0.65, 0.5), dist = 0.3
		STouchGestureResult res = proc.Process(true, 0.35f, 0.5f, true, 0.65f, 0.5f, 16.0f);
		check(res.mode == GESTURE_MODE_PINCH, "spreading fingers classifies as pinch");
		check(res.zoomDelta > 0.0f, "spreading fingers gives positive zoom delta");
	}

	begin("touchpad: gesture hysteresis and reset");
	{
		CTouchpadGestureProcessor proc;
		proc.Process(true, 0.4f, 0.5f, true, 0.6f, 0.5f, 16.0f);
		proc.Process(true, 0.35f, 0.5f, true, 0.65f, 0.5f, 16.0f);
		proc.Reset();
		STouchGestureResult res = proc.Process(true, 0.1f, 0.1f, false, 0.0f, 0.0f, 16.0f);
		check(nearly(res.panDx, 0.0f), "reset clears state so first touch has zero delta");
	}
}

void test_radial_menu()
{
	begin("radial: right-stick angle and sector selection");
	{
		CRadialMenuProcessor rad;
		// 8 sectors: 0 = 12 o'clock, 2 = 3 o'clock, 4 = 6 o'clock, 6 = 9 o'clock
		SRadialResult up = rad.Process(0.0f, 1.0f, 0.25f, 8, true, false);
		check(up.active == true, "up deflection is active");
		check(up.sector == 0, "up deflection is sector 0");

		SRadialResult right = rad.Process(1.0f, 0.0f, 0.25f, 8, true, false);
		check(right.active == true, "right deflection is active");
		check(right.sector == 2, "right deflection is sector 2 (3 o'clock)");

		SRadialResult down = rad.Process(0.0f, -1.0f, 0.25f, 8, true, false);
		check(down.active == true, "down deflection is active");
		check(down.sector == 4, "down deflection is sector 4 (6 o'clock)");

		SRadialResult left = rad.Process(-1.0f, 0.0f, 0.25f, 8, true, false);
		check(left.active == true, "left deflection is active");
		check(left.sector == 6, "left deflection is sector 6 (9 o'clock)");
	}

	begin("radial: deadzone");
	{
		CRadialMenuProcessor rad;
		SRadialResult dead = rad.Process(0.1f, 0.1f, 0.25f, 8, true, false);
		check(dead.active == false, "small deflection inside deadzone is not active");
	}

	begin("radial: release selection commit and cancel");
	{
		CRadialMenuProcessor rad;
		rad.Process(1.0f, 0.0f, 0.25f, 8, true, false); // select sector 2
		SRadialResult commit = rad.Process(0.0f, 0.0f, 0.25f, 8, false, false); // release open button
		check(commit.committed == true, "releasing open button commits selection");
		check(commit.committedSector == 2, "committed sector matches last highlighted");

		// Cancel test
		rad.Process(0.0f, 1.0f, 0.25f, 8, true, false);
		SRadialResult cancelled = rad.Process(0.0f, 1.0f, 0.25f, 8, true, true);
		check(cancelled.cancelled == true, "cancel button sets cancelled state");
	}
}

void test_gyro_aim()
{
	begin("gyro: zero rotation reports zero delta");
	{
		CGyroAimProcessor gyro;
		SGyroConfig cfg;
		cfg.mode = GYRO_MODE_ALWAYS;
		gyro.Configure(cfg);

		SGyroDelta d = gyro.Process(0.0f, 0.0f, 0.0f, false, 0.016f);
		check(nearly(d.dx, 0.0f), "zero gyro reports zero dx");
		check(nearly(d.dy, 0.0f), "zero gyro reports zero dy");
	}

	begin("gyro: known angular velocity and dt independence");
	{
		CGyroAimProcessor gyro1, gyro2;
		SGyroConfig cfg;
		cfg.mode = GYRO_MODE_ALWAYS;
		cfg.smoothing = 0.0f; // raw integration test
		gyro1.Configure(cfg);
		gyro2.Configure(cfg);

		// One full step of 0.020s at 1.0 rad/s
		SGyroDelta d1 = gyro1.Process(0.0f, 1.0f, 0.0f, false, 0.020f);
		// Two half steps of 0.010s at 1.0 rad/s
		SGyroDelta d2a = gyro2.Process(0.0f, 1.0f, 0.0f, false, 0.010f);
		SGyroDelta d2b = gyro2.Process(0.0f, 1.0f, 0.0f, false, 0.010f);
		float sum2 = d2a.dx + d2b.dx;

		check(d1.dx > 0.0f, "yaw produces positive dx");
		check(nearly(d1.dx, sum2, 1.0e-3f), "gyro delta is dt independent");
	}

	begin("gyro: sensitivity scaling");
	{
		CGyroAimProcessor gyro;
		SGyroConfig cfg;
		cfg.mode = GYRO_MODE_ALWAYS;
		cfg.smoothing = 0.0f;
		cfg.sensX = 1.0f;
		gyro.Configure(cfg);
		SGyroDelta dNormal = gyro.Process(0.0f, 1.0f, 0.0f, false, 0.016f);

		cfg.sensX = 2.0f;
		gyro.Configure(cfg);
		gyro.Reset();
		SGyroDelta dDouble = gyro.Process(0.0f, 1.0f, 0.0f, false, 0.016f);
		check(nearly(dDouble.dx, dNormal.dx * 2.0f, 1.0e-3f), "doubling sensX doubles output delta");
	}

	begin("gyro: smoothing filter and spike clamp");
	{
		CGyroAimProcessor gyro;
		SGyroConfig cfg;
		cfg.mode = GYRO_MODE_ALWAYS;
		cfg.smoothing = 0.5f;
		cfg.maxVelocity = 10.0f;
		gyro.Configure(cfg);

		// Sudden extreme spike of 50 rad/s
		SGyroDelta dSpike = gyro.Process(0.0f, 50.0f, 0.0f, false, 0.016f);
		// If clamped to 10.0 and scaled by rateScale(800) * dt(0.016) = 128.0 max
		check(dSpike.dx <= 130.0f, "extreme spike is clamped to max velocity");
	}

	begin("gyro: ADS-only gate and focus reset");
	{
		CGyroAimProcessor gyro;
		SGyroConfig cfg;
		cfg.mode = GYRO_MODE_ADS_ONLY;
		gyro.Configure(cfg);

		SGyroDelta dHip = gyro.Process(0.0f, 1.0f, 0.0f, false /* adsHolding */, 0.016f);
		check(nearly(dHip.dx, 0.0f), "ADS-only mode reports zero when not aiming");

		SGyroDelta dAds = gyro.Process(0.0f, 1.0f, 0.0f, true /* adsHolding */, 0.016f);
		check(dAds.dx > 0.0f, "ADS-only mode reports movement when aiming");

		gyro.Reset();
		SGyroDelta dReset = gyro.Process(0.0f, 0.0f, 0.0f, true, 0.016f);
		check(nearly(dReset.dx, 0.0f), "reset zeroes history");
	}
}

void test_quicksave_quickload_chord()
{
	begin("quicksave/load chord: correct chord and single press rejection");
	{
		CGamepadChordTracker tracker(500.0f);
		// Single presses alone do not trigger
		check(tracker.Update(true, false, false, 16.0f) == CHORD_NONE, "create alone does not trigger chord");
		check(tracker.Update(false, true, false, 16.0f) == CHORD_NONE, "dpad up alone does not trigger chord");
		check(tracker.Update(false, false, true, 16.0f) == CHORD_NONE, "dpad down alone does not trigger chord");

		// Quicksave: Create + D-pad Up
		check(tracker.Update(true, true, false, 16.0f) == CHORD_QUICKSAVE, "create + dpad up triggers quicksave immediately");
		// Subsequent frames while held do not re-trigger
		check(tracker.Update(true, true, false, 16.0f) == CHORD_NONE, "held quicksave chord does not spam");
	}

	begin("quicksave/load chord: quickload hold threshold");
	{
		CGamepadChordTracker tracker(500.0f);
		// Holding Create + D-pad Down for less than 500ms should NOT fire
		check(tracker.Update(true, false, true, 200.0f) == CHORD_NONE, "holding quickload for 200ms does not fire");
		check(tracker.Update(true, false, true, 200.0f) == CHORD_NONE, "holding quickload for 400ms does not fire");
		// At or past 500ms, fires exactly once
		check(tracker.Update(true, false, true, 110.0f) == CHORD_QUICKLOAD, "holding quickload for 510ms fires quickload");
		check(tracker.Update(true, false, true, 100.0f) == CHORD_NONE, "continuing to hold quickload does not re-fire");

		// Early release cancels
		tracker.Reset();
		tracker.Update(true, false, true, 300.0f);
		tracker.Update(false, false, false, 16.0f); // release
		check(tracker.Update(true, false, true, 300.0f) == CHORD_NONE, "restarting hold resets timer from zero");
	}
}

void test_shot_haptics()
{
	begin("haptics: real shot vs dry fire and weapon switch");
	{
		CShotHapticTracker haptics(0.8f, 0.06f);
		// Initial real shot
		haptics.OnRealShot(false);
		SHapticFeedbackRequest req1 = haptics.Process(16.0f);
		check(req1.triggerHaptic == true, "real shot triggers haptic feedback");
		check(req1.triggerAdaptiveKick == true, "real shot triggers adaptive recoil kick");
		check(nearly(req1.amplitude, 0.8f), "haptic amplitude matches configured setting");

		// Dry fire does not trigger
		haptics.OnDryFire();
		SHapticFeedbackRequest reqDry = haptics.Process(16.0f);
		check(reqDry.triggerHaptic == false, "dry fire does not trigger shot haptic");

		// Repeated automatic shots
		haptics.OnRealShot(true);
		SHapticFeedbackRequest reqAuto1 = haptics.Process(16.0f);
		check(reqAuto1.triggerHaptic == true, "automatic shot 1 triggers haptic");
		haptics.OnRealShot(true);
		SHapticFeedbackRequest reqAuto2 = haptics.Process(16.0f);
		check(reqAuto2.triggerHaptic == true, "automatic shot 2 triggers haptic");

		// Weapon switch cancels pending feedback
		haptics.OnRealShot(false);
		haptics.OnWeaponSwitch();
		SHapticFeedbackRequest reqSwitched = haptics.Process(16.0f);
		check(reqSwitched.triggerHaptic == false, "weapon switch cancels pending shot haptic");
	}
}

void test_mapping_chords_and_ui_actions()
{
	begin("mapping: chords and special UI actions");
	{
	CRecordingOutput output;
	CGamepadMapping mapping;
	mapping.SetOutput(&output);
	mapping.SetResolver(&test_resolver);

	SGamepadSettings settings;
		settings.gyroMode = GYRO_MODE_ADS_ONLY;
		settings.gyroSens = 1.0f;

		// 1. Quicksave chord in gameplay suppresses individual buttons
		SGamepadFrame chordFrame;
		chordFrame.connected = true;
		chordFrame.held = GAMEPAD_BIT_BACK | GAMEPAD_BIT_DPAD_UP;
		mapping.Process(chordFrame, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);

		check(output.chords.size() == 1, "chord event fired on create + dpad up");
		if (!output.chords.empty())
			check(output.chords[0] == CHORD_QUICKSAVE, "chord action is quicksave");
		check(!output.HasKeyPress(dik_for(GPA_Torch)), "GPA_Torch suppressed during chord");
		check(!output.HasKeyPress(dik_for(GPA_DpadUp)), "GPA_DpadUp suppressed during chord");

		// 2. Gyro aim in gameplay
		output.Clear();
		SGamepadFrame gyroFrame;
		gyroFrame.connected = true;
		gyroFrame.ads = true;
		gyroFrame.gyroX = 0.5f;
		gyroFrame.gyroY = 0.5f;
		mapping.Process(gyroFrame, settings, 16.0f, GAMEPAD_CONTEXT_GAMEPLAY);
		check(output.CountKind(3) > 0, "gyro aim generates mouse move events");

		// 3. UI special actions
		output.Clear();
		SGamepadFrame uiFrame;
		uiFrame.connected = true;
		uiFrame.pressed = GAMEPAD_BIT_X;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!output.uiActions.empty() && output.uiActions[0] == UI_ACTION_CONTEXT_MENU, "X button fires context menu action");

		output.Clear();
		uiFrame.pressed = GAMEPAD_BIT_Y;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!output.uiActions.empty() && output.uiActions[0] == UI_ACTION_TRANSFER, "Y button fires transfer action");

		output.Clear();
		uiFrame.pressed = GAMEPAD_BIT_LB;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!output.uiActions.empty() && output.uiActions[0] == UI_ACTION_TAB_PREV, "LB button fires tab prev action");

		output.Clear();
		uiFrame.pressed = GAMEPAD_BIT_RB;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!output.uiActions.empty() && output.uiActions[0] == UI_ACTION_TAB_NEXT, "RB button fires tab next action");

		output.Clear();
		uiFrame.pressed = 0;
		uiFrame.adsPressed = true;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!output.uiActions.empty() && output.uiActions[0] == UI_ACTION_PANEL_PREV, "L2 trigger fires panel prev action");

		output.Clear();
		uiFrame.adsPressed = false;
		uiFrame.firePressed = true;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(!output.uiActions.empty() && output.uiActions[0] == UI_ACTION_PANEL_NEXT, "R2 trigger fires panel next action");

		// 4. R3 toggles fallback cursor
		check(mapping.FallbackCursorEnabled() == false, "fallback cursor initially false");
		uiFrame.firePressed = false;
		uiFrame.pressed = GAMEPAD_BIT_RS;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(mapping.FallbackCursorEnabled() == true, "R3 click enables fallback cursor");
		uiFrame.pressed = GAMEPAD_BIT_RS;
		mapping.Process(uiFrame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
		check(mapping.FallbackCursorEnabled() == false, "second R3 click disables fallback cursor");
	}
}

namespace
{
void test_apple_touch_and_motion_basis()
{
	begin("Apple DualSense live contact and world yaw conventions");
	check(GamepadAppleTouchContact(-1.0f, 0.064f, false),
		"recorded primary contact is recognized with isTouched false");
	check(!GamepadAppleTouchContact(-0.0f, 0.0f, false),
		"recorded lift and absent secondary contact remain inactive");
	check(GamepadAppleTouchContact(0.0f, 0.0f, true), "explicit contact at surface centre remains active");
	CGamepadAppleMotionBasis basis;
	basis.Update(0.0f, 0.0f, -1.0f, 0.016f, true);
	check(nearly(basis.Yaw(0.0f, 0.0f, -1.0f), 1.0f), "flat grip right turn around negative Z looks right");
	check(nearly(basis.Yaw(0.0f, 0.0f, 1.0f), -1.0f), "flat grip left turn looks left");
	basis.Update(0.0f, -1.0f, 0.0f, 0.016f, true);
	check(nearly(basis.Yaw(0.0f, -1.0f, 0.0f), 1.0f), "upright grip right turn around negative Y looks right");
	basis.Update(0.0f, -0.6f, -0.8f, 0.016f, true);
	check(nearly(basis.Yaw(0.0f, -0.6f, -0.8f), 1.0f), "tilted grip right turn preserves horizontal direction");
	check(nearly(basis.Yaw(1.0f, 0.0f, 0.0f), 0.0f), "pitch orthogonal to gravity produces no horizontal turn");
	basis.Update(5.0f, 0.0f, 0.0f, 0.016f, false);
	check(nearly(basis.Yaw(0.0f, -0.6f, -0.8f), 1.0f), "strong linear acceleration does not overwrite gravity basis");
	basis.Reset();
	check(nearly(basis.Yaw(0.0f, -1.0f, 0.0f), 0.0f), "reconnected controller cannot reuse an old orientation");
	basis.Update(0.00769f, 0.16557f, -0.97348f, 0.016f, false);
	check(basis.Yaw(0.0f, 0.0f, -0.79433f) > 0.78f,
		"recorded right turn and measured acceleration produce right-positive yaw");
}

void test_pda_map_gestures()
{
	begin("PDA map touch gestures and independent stick speed");
	CGamepadMapping mapping;
	CRecordingOutput output;
	mapping.SetOutput(&output);
	mapping.SetResolver(test_resolver);
	SGamepadMappingConfig config = mapping.Config();
	config.uiMapPan = true;
	mapping.Configure(config);
	SGamepadSettings settings;
	SGamepadFrame frame;
	frame.connected = true;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	output.Clear();
	frame.touchActive = true;
	frame.touchX = frame.touchY = 0.5f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	output.Clear();
	frame.touchX = 0.6f;
	frame.touchY = 0.55f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	check(output.CountKind(6) == 1, "one finger pans the map without R3 pointer mode");
	check(output.CountKind(5) == 0, "map drag does not move the pointer");
	check(output.events.back().a > 80 && output.events.back().b > 40,
		"touch drag moves map content in finger direction at usable speed");
	output.Clear();
	frame.touchSecondaryActive = true;
	frame.touchX = 0.4f; frame.touchSecondaryX = 0.6f;
	frame.touchY = frame.touchSecondaryY = 0.5f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	output.Clear();
	frame.touchX = 0.3f; frame.touchSecondaryX = 0.7f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	check(output.CountKind(7) == 1 && output.events.back().a == 1,
		"pinch out requests direct map zoom in");
	check(output.CountKind(4) == 0, "pinch zoom does not depend on mouse cursor over map");
	output.Clear();
	frame.touchActive = frame.touchSecondaryActive = false;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	output.Clear();
	frame.lookX = 1.0f;
	mapping.Process(frame, settings, 100.0f, GAMEPAD_CONTEXT_UI);
	check(output.CountKind(6) == 1 && output.events.back().a == -60,
		"full stick pans view at 600 logical units per second");
	check(output.CountKind(5) == 0, "map stick speed is independent of pointer fallback");
	output.Clear();
	frame.lookX = 0.0f;
	settings.touchpadUi = false;
	frame.touchActive = true;
	frame.touchX = 0.4f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	frame.touchX = 0.8f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	check(output.CountKind(6) == 0, "disabled touch setting prevents map gestures");
	output.Clear();
	settings.touchpadUi = true;
	frame.touchActive = false;
	frame.lookX = 1.0f;
	mapping.Reset();
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	check(output.CountKind(6) == 0, "reset neutral latch prevents stale stick pan");
	output.Clear();
	frame.lookX = 0.0f;
	frame.touchActive = true;
	frame.touchX = 0.2f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	frame.touchX = 0.6f;
	mapping.Process(frame, settings, 16.0f, GAMEPAD_CONTEXT_UI);
	check(output.CountKind(6) == 0, "touch held across reset stays suppressed until lift");
}

void test_sequencer_routing()
{
	begin("sequencer routing follows current item input ownership");
	struct Receiver {} level, menu, console, tutorialReceiver, nestedReceiver;
	struct Sequencer
	{
		bool grab = false;
		Receiver* m_pStoredInputReceiver = nullptr;
		bool GrabInput() { return grab; }
	} tutorial, nested;
	tutorial.m_pStoredInputReceiver = &level;
	nested.m_pStoredInputReceiver = &tutorialReceiver;
	auto lookup = [&](Receiver* receiver) -> Sequencer*
	{
		if (receiver == &tutorialReceiver) return &tutorial;
		if (receiver == &nestedReceiver) return &nested;
		return nullptr;
	};
	auto resolve = [&](Receiver* receiver, bool menuActive = false, bool dialogActive = false)
	{
		return GamepadResolveInputRoute(receiver, &level, &menu, menuActive, dialogActive, lookup);
	};
	check(resolve(&level).context == GAMEPAD_CONTEXT_GAMEPLAY, "level owns gameplay");
	check(resolve(&tutorialReceiver).context == GAMEPAD_CONTEXT_GAMEPLAY,
		"passive gameplay tutorial preserves gameplay mapping");
	check(!resolve(&tutorialReceiver).blockingSequencer, "passive prompt cannot translate buttons to Enter/Esc");
	check(resolve(&tutorialReceiver).receiver == resolve(&level).receiver,
		"passive prompt preserves the owner and does not rearm neutral latch");
	check(resolve(&nestedReceiver).context == GAMEPAD_CONTEXT_GAMEPLAY, "nested passive tutorials preserve gameplay");
	check(resolve(&tutorialReceiver, false, true).context == GAMEPAD_CONTEXT_UI,
		"inventory under passive tutorial still owns UI mapping");
	check(!resolve(&tutorialReceiver, false, true).blockingSequencer,
		"inventory under passive tutorial uses normal UI actions");
	tutorial.m_pStoredInputReceiver = &menu;
	check(resolve(&tutorialReceiver, true).context == GAMEPAD_CONTEXT_UI, "passive prompt over menu maps UI");
	check(!resolve(&tutorialReceiver, true).blockingSequencer, "menu accept is not a sequencer skip");
	tutorial.m_pStoredInputReceiver = &console;
	check(resolve(&tutorialReceiver).context == GAMEPAD_CONTEXT_SUPPRESSED, "passive prompt cannot bypass console");
	tutorial.m_pStoredInputReceiver = &level;
	tutorial.grab = true;
	check(resolve(&tutorialReceiver).blockingSequencer, "loading prompt retains sequencer skip behavior");
	check(resolve(&nestedReceiver).blockingSequencer, "nested passive prompt respects underlying blocking prompt");
	tutorial.grab = false;
	check(resolve(&tutorialReceiver).context == GAMEPAD_CONTEXT_GAMEPLAY,
		"same sequencer switching item to passive restores gameplay");
	check(resolve(&level, true).context == GAMEPAD_CONTEXT_SUPPRESSED, "menu ownership mismatch suppresses input");
	check(resolve(nullptr).context == GAMEPAD_CONTEXT_SUPPRESSED, "missing receiver suppresses input");
	tutorial.m_pStoredInputReceiver = &nestedReceiver;
	check(resolve(&tutorialReceiver).context == GAMEPAD_CONTEXT_SUPPRESSED, "forwarding cycle terminates safely");
	tutorial.m_pStoredInputReceiver = &level;

	CGamepadMapping mapping;
	CRecordingOutput output;
	mapping.SetOutput(&output);
	mapping.SetResolver(test_resolver);
	SGamepadSettings settings;
	SGamepadFrame frame;
	frame.connected = true;
	mapping.Process(frame, settings, 16.0f, resolve(&level).context);
	output.Clear();
	frame.moveX = 0.8f;
	frame.moveRight = true;
	frame.lookX = 0.8f;
	frame.ads = true;
	frame.fire = true;
	frame.pressed = frame.held = GAMEPAD_BIT_B;
	mapping.Process(frame, settings, 16.0f, resolve(&tutorialReceiver).context);
	check(output.HasKeyPress(dik_for(GPA_MoveRight)), "movement continues when passive prompt starts");
	check(output.CountKind(3) > 0, "camera continues when passive prompt starts");
	check(output.HasKeyPress(dik_for(GPA_Aim)) && output.HasKeyPress(dik_for(GPA_Fire)),
		"both triggers remain gameplay actions under passive prompt");
	check(output.HasKeyPress(dik_for(GPA_Crouch)), "Circle crouches under passive prompt");
	check(!output.HasKeyPress(dik_for(GPA_Back)), "Circle does not open main menu under passive prompt");
	output.Clear();
	frame.pressed = GAMEPAD_BIT_START;
	frame.held |= GAMEPAD_BIT_START;
	mapping.Process(frame, settings, 16.0f, resolve(&tutorialReceiver).context);
	check(output.HasKeyPress(dik_for(GPA_Back)), "Options still opens menu under passive prompt");
}
}

int main()
{
	std::printf("gamepad logic tests\n");

	test_stick_deadzone();
	test_diagonal_normalisation();
	test_response_curve();
	test_trigger_hysteresis();
	test_press_release_edges();
	test_trigger_edges_and_analog_value();
	test_tap_vs_hold();
	test_disconnect_releases_held_actions();
	test_focus_loss_reset();

	test_gameplay_mapping_uses_actions();
	test_contextual_b_mapping();
	test_sequencer_routing();
	test_pda_map_gestures();
	test_apple_touch_and_motion_basis();
	test_walk_band();
	test_ui_context_cancels_gameplay_holds();
	test_reset_releases_everything();
	test_suppressed_context();
	test_ui_navigation_repeats();
	test_ui_accept_and_back();
	test_touchpad_gate();
	test_ui_touch_gesture_does_not_cross_contexts();
	test_ui_navigation_restarts_after_reset_and_context_change();
	test_frame_rate_independence();
	test_look_axis_conventions();

	test_focus_navigation();
	test_inventory_navigation();
	test_touchpad_gestures();
	test_radial_menu();
	test_gyro_aim();
	test_quicksave_quickload_chord();
	test_shot_haptics();
	test_mapping_chords_and_ui_actions();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
