#ifndef __XR_GAMEPAD_RADIAL__
#define __XR_GAMEPAD_RADIAL__
#pragma once

struct SRadialResult
{
	bool active;        // stick deflected past deadzone while open
	int sector;         // currently highlighted sector [0, sectorCount-1], or -1
	float angleRad;     // angle in radians [0, 2*pi), 0 at 12 o'clock, clockwise
	bool committed;     // true on frame when menu was released and a sector was chosen
	int committedSector;
	bool cancelled;     // true when cancelled with back button

	SRadialResult()
		: active(false), sector(-1), angleRad(0.0f), committed(false), committedSector(-1), cancelled(false) {}
};

class CRadialMenuProcessor
{
public:
	CRadialMenuProcessor();

	void Reset();

	// rx, ry: right stick axes (-1..1), deadzone: radial deadzone
	// sectorCount: number of wheel slices
	// openHeld: true while holding the button that keeps radial menu open (e.g. D-pad Down or Triangle)
	// cancelPressed: true on rising edge of cancel button (Circle / Esc)
	SRadialResult Process(float rx, float ry, float deadzone, int sectorCount,
		bool openHeld, bool cancelPressed);

private:
	bool m_wasOpen;
	int m_lastHighlightedSector;
};

#endif // __XR_GAMEPAD_RADIAL__
