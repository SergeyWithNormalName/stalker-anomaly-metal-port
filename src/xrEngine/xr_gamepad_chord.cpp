#include "xr_gamepad_chord.h"

CGamepadChordTracker::CGamepadChordTracker(float holdThresholdMs)
	: m_holdThresholdMs(holdThresholdMs)
{
	Reset();
}

void CGamepadChordTracker::Reset()
{
	m_prevQuicksaveActive = false;
	m_quickloadFired = false;
	m_quickloadHeldMs = 0.0f;
}

EGamepadChordAction CGamepadChordTracker::Update(bool createDown, bool dpadUpDown, bool dpadDownDown, float dtMs)
{
	if (!createDown)
	{
		// Create is not down: reset all chord states
		m_prevQuicksaveActive = false;
		m_quickloadFired = false;
		m_quickloadHeldMs = 0.0f;
		return CHORD_NONE;
	}

	// 1. Quicksave: Create + D-pad Up (immediate on chord engagement)
	const bool quicksaveActive = createDown && dpadUpDown;
	if (quicksaveActive)
	{
		if (!m_prevQuicksaveActive)
		{
			m_prevQuicksaveActive = true;
			return CHORD_QUICKSAVE;
		}
	}
	else
	{
		m_prevQuicksaveActive = false;
	}

	// 2. Quickload: Create + D-pad Down with hold threshold protection (~500ms)
	const bool quickloadActive = createDown && dpadDownDown;
	if (quickloadActive)
	{
		if (!m_quickloadFired)
		{
			m_quickloadHeldMs += dtMs;
			if (m_quickloadHeldMs >= m_holdThresholdMs)
			{
				m_quickloadFired = true;
				return CHORD_QUICKLOAD;
			}
		}
	}
	else
	{
		m_quickloadFired = false;
		m_quickloadHeldMs = 0.0f;
	}

	return CHORD_NONE;
}
