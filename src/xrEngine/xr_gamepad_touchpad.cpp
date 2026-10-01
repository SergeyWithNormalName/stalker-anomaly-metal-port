#include "xr_gamepad_touchpad.h"

#include <cmath>

CTouchpadGestureProcessor::CTouchpadGestureProcessor()
{
	m_pinchDistThreshold = 0.015f;
	m_panCentroidThreshold = 0.008f;
	Reset();
}

void CTouchpadGestureProcessor::Reset()
{
	m_primaryWasActive = false;
	m_prevPrimaryX = 0.0f;
	m_prevPrimaryY = 0.0f;

	m_secondaryWasActive = false;
	m_prevSecondaryX = 0.0f;
	m_prevSecondaryY = 0.0f;

	m_twoTouchWasActive = false;
	m_prevDistance = 0.0f;
	m_prevCentroidX = 0.0f;
	m_prevCentroidY = 0.0f;

	m_latchedMode = GESTURE_MODE_NONE;
	m_touchDownTimeMs = 0.0f;
	m_totalMovement = 0.0f;
}

void CTouchpadGestureProcessor::SetHysteresis(float pinchDistThreshold, float panCentroidThreshold)
{
	m_pinchDistThreshold = pinchDistThreshold;
	m_panCentroidThreshold = panCentroidThreshold;
}

STouchGestureResult CTouchpadGestureProcessor::Process(bool primaryActive, float primaryX, float primaryY,
	bool secondaryActive, float secondaryX, float secondaryY, float dtMs)
{
	STouchGestureResult result;

	const bool twoTouch = primaryActive && secondaryActive;

	if (twoTouch)
	{
		// Two-finger pinch / pan mode
		const float dx = primaryX - secondaryX;
		const float dy = primaryY - secondaryY;
		const float dist = std::sqrt(dx * dx + dy * dy);
		const float cx = (primaryX + secondaryX) * 0.5f;
		const float cy = (primaryY + secondaryY) * 0.5f;

		if (!m_twoTouchWasActive)
		{
			// First sample of two-touch: establish baseline with zero delta (no jump!)
			m_twoTouchWasActive = true;
			m_prevDistance = dist;
			m_prevCentroidX = cx;
			m_prevCentroidY = cy;
			m_latchedMode = GESTURE_MODE_NONE;
			result.mode = GESTURE_MODE_NONE;
			result.panDx = 0.0f;
			result.panDy = 0.0f;
			result.zoomDelta = 0.0f;
		}
		else
		{
			const float deltaDist = dist - m_prevDistance;
			const float deltaCx = cx - m_prevCentroidX;
			const float deltaCy = cy - m_prevCentroidY;
			const float centroidMove = std::sqrt(deltaCx * deltaCx + deltaCy * deltaCy);

			// Hysteresis classifier: mode latching to prevent flicker between pinch and pan
			if (m_latchedMode == GESTURE_MODE_NONE)
			{
				if (std::fabs(deltaDist) > m_pinchDistThreshold && std::fabs(deltaDist) > centroidMove * 1.5f)
				{
					m_latchedMode = GESTURE_MODE_PINCH;
				}
				else if (centroidMove > m_panCentroidThreshold)
				{
					m_latchedMode = GESTURE_MODE_PAN;
				}
			}

			if (m_latchedMode == GESTURE_MODE_PINCH)
			{
				result.mode = GESTURE_MODE_PINCH;
				result.zoomDelta = deltaDist;
				result.panDx = 0.0f;
				result.panDy = 0.0f;
			}
			else if (m_latchedMode == GESTURE_MODE_PAN)
			{
				result.mode = GESTURE_MODE_PAN;
				result.panDx = deltaCx;
				result.panDy = deltaCy;
				result.zoomDelta = 0.0f;
			}

			m_prevDistance = dist;
			m_prevCentroidX = cx;
			m_prevCentroidY = cy;
		}

		m_primaryWasActive = true;
		m_prevPrimaryX = primaryX;
		m_prevPrimaryY = primaryY;
		m_secondaryWasActive = true;
		m_prevSecondaryX = secondaryX;
		m_prevSecondaryY = secondaryY;
		return result;
	}

	// Two-touch is not active
	m_twoTouchWasActive = false;
	m_secondaryWasActive = false;

	if (primaryActive)
	{
		m_touchDownTimeMs += dtMs;

		if (!m_primaryWasActive)
		{
			// First sample of new touch: establish baseline with zero delta (no jump!)
			m_primaryWasActive = true;
			m_prevPrimaryX = primaryX;
			m_prevPrimaryY = primaryY;
			m_totalMovement = 0.0f;
			m_touchDownTimeMs = 0.0f;

			result.mode = GESTURE_MODE_PAN;
			result.panDx = 0.0f;
			result.panDy = 0.0f;
		}
		else
		{
			// Relative move
			const float dx = primaryX - m_prevPrimaryX;
			const float dy = primaryY - m_prevPrimaryY;
			m_totalMovement += std::sqrt(dx * dx + dy * dy);

			result.mode = GESTURE_MODE_PAN;
			result.panDx = dx;
			result.panDy = dy;

			m_prevPrimaryX = primaryX;
			m_prevPrimaryY = primaryY;
		}
	}
	else
	{
		if (m_primaryWasActive)
		{
			// Finger release: check if it constitutes a tap
			if (m_touchDownTimeMs < 250.0f && m_totalMovement < 0.02f)
			{
				result.tap = true;
			}
			m_primaryWasActive = false;
			m_touchDownTimeMs = 0.0f;
			m_totalMovement = 0.0f;
		}
	}

	return result;
}
