#ifndef __XR_GAMEPAD_NAV__
#define __XR_GAMEPAD_NAV__
#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

enum ENavDirection
{
	NAV_NONE = 0,
	NAV_UP,
	NAV_DOWN,
	NAV_LEFT,
	NAV_RIGHT
};

struct SNavRect
{
	float x1, y1, x2, y2;

	SNavRect() : x1(0.0f), y1(0.0f), x2(0.0f), y2(0.0f) {}
	SNavRect(float left, float top, float right, float bottom)
		: x1(left), y1(top), x2(right), y2(bottom) {}

	float CenterX() const { return (x1 + x2) * 0.5f; }
	float CenterY() const { return (y1 + y2) * 0.5f; }
	float Width() const { return x2 - x1; }
	float Height() const { return y2 - y1; }
};

struct SNavCandidate
{
	int id;
	SNavRect rect;
	bool enabled;
	bool focusable;

	SNavCandidate(int inId = 0, SNavRect inRect = SNavRect(), bool inEnabled = true, bool inFocusable = true)
		: id(inId), rect(inRect), enabled(inEnabled), focusable(inFocusable) {}
};

// Spatial neighbor selection based on half-plane filtering, direction,
// distance, and transverse axis overlap.
int FindBestSpatialNeighbor(const SNavRect& current, const std::vector<SNavCandidate>& candidates,
	ENavDirection dir);

// Grid cell representation for 2D item and cell layouts.
struct SGridCell
{
	int id;
	int row;
	int col;
	int rowSpan;
	int colSpan;
	bool enabled;

	SGridCell(int inId = 0, int inRow = 0, int inCol = 0, int inRowSpan = 1, int inColSpan = 1, bool inEnabled = true)
		: id(inId), row(inRow), col(inCol), rowSpan(inRowSpan), colSpan(inColSpan), enabled(inEnabled) {}
};

int FindGridNeighbor(const std::vector<SGridCell>& grid, int currentId, ENavDirection dir);

// Tab cycling helper
int CycleTab(int currentIdx, int totalTabs, bool forward, bool wrap = true);

// Direction repeat tracker with initial delay and repeat interval.
class CNavRepeatTracker
{
public:
	CNavRepeatTracker(float delayMs = 350.0f, float repeatMs = 110.0f);

	void Reset();
	void SetTiming(float delayMs, float repeatMs);

	// Returns true on rising edge or on timer-triggered repeat.
	bool Update(bool isDown, float dtMs);

private:
	float m_delayMs;
	float m_repeatMs;
	float m_heldMs;
	bool m_wasDown;
	bool m_repeated;
};

#endif // __XR_GAMEPAD_NAV__
