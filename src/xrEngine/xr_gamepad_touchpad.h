#ifndef __XR_GAMEPAD_TOUCHPAD__
#define __XR_GAMEPAD_TOUCHPAD__
#pragma once

enum ETouchGestureMode
{
	GESTURE_MODE_NONE = 0,
	GESTURE_MODE_PAN,
	GESTURE_MODE_PINCH
};

struct STouchGestureResult
{
	ETouchGestureMode mode;
	float panDx;
	float panDy;
	float zoomDelta; // positive = zoom in, negative = zoom out
	bool tap;

	STouchGestureResult()
		: mode(GESTURE_MODE_NONE), panDx(0.0f), panDy(0.0f), zoomDelta(0.0f), tap(false) {}
};

class CTouchpadGestureProcessor
{
public:
	CTouchpadGestureProcessor();

	void Reset();

	// Process one frame of primary and secondary touch samples.
	// touchX, touchY are 0..1 coordinates with origin at top-left.
	STouchGestureResult Process(bool primaryActive, float primaryX, float primaryY,
		bool secondaryActive, float secondaryX, float secondaryY, float dtMs);

	void SetHysteresis(float pinchDistThreshold, float panCentroidThreshold);

private:
	bool m_primaryWasActive;
	float m_prevPrimaryX;
	float m_prevPrimaryY;

	bool m_secondaryWasActive;
	float m_prevSecondaryX;
	float m_prevSecondaryY;

	bool m_twoTouchWasActive;
	float m_prevDistance;
	float m_prevCentroidX;
	float m_prevCentroidY;

	ETouchGestureMode m_latchedMode;

	float m_touchDownTimeMs;
	float m_totalMovement;
	float m_pinchDistThreshold;
	float m_panCentroidThreshold;
};

#endif // __XR_GAMEPAD_TOUCHPAD__
