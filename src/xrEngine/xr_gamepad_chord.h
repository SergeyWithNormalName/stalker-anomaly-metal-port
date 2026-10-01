#ifndef __XR_GAMEPAD_CHORD__
#define __XR_GAMEPAD_CHORD__
#pragma once

enum EGamepadChordAction
{
	CHORD_NONE = 0,
	CHORD_QUICKSAVE,
	CHORD_QUICKLOAD
};

class CGamepadChordTracker
{
public:
	CGamepadChordTracker(float holdThresholdMs = 500.0f);

	void Reset();
	void SetHoldThreshold(float holdThresholdMs) { m_holdThresholdMs = holdThresholdMs; }
	float HoldThreshold() const { return m_holdThresholdMs; }

	// Evaluates the chord state.
	// createDown: Create/View button (GAMEPAD_BIT_BACK)
	// dpadUpDown: D-pad Up (GAMEPAD_BIT_DPAD_UP)
	// dpadDownDown: D-pad Down (GAMEPAD_BIT_DPAD_DOWN)
	// dtMs: delta time in ms
	EGamepadChordAction Update(bool createDown, bool dpadUpDown, bool dpadDownDown, float dtMs);

private:
	float m_holdThresholdMs;
	bool m_prevQuicksaveActive;
	bool m_quickloadFired;
	float m_quickloadHeldMs;
};

#endif // __XR_GAMEPAD_CHORD__
