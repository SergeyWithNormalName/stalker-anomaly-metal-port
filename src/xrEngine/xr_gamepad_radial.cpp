#include "xr_gamepad_radial.h"

#include <cmath>

namespace
{
constexpr float g_radial_pi = 3.14159265358979323846f;
constexpr float g_radial_two_pi = 2.0f * g_radial_pi;
}

CRadialMenuProcessor::CRadialMenuProcessor()
{
	Reset();
}

void CRadialMenuProcessor::Reset()
{
	m_wasOpen = false;
	m_lastHighlightedSector = -1;
}

SRadialResult CRadialMenuProcessor::Process(float rx, float ry, float deadzone, int sectorCount,
	bool openHeld, bool cancelPressed)
{
	SRadialResult res;

	if (sectorCount <= 0)
		return res;

	if (!openHeld)
	{
		if (m_wasOpen && m_lastHighlightedSector >= 0)
		{
			// Menu button released with a valid selection: commit!
			res.committed = true;
			res.committedSector = m_lastHighlightedSector;
		}
		m_wasOpen = false;
		m_lastHighlightedSector = -1;
		return res;
	}

	m_wasOpen = true;

	if (cancelPressed)
	{
		res.cancelled = true;
		m_lastHighlightedSector = -1;
		return res;
	}

	const float mag = std::sqrt(rx * rx + ry * ry);
	if (mag < deadzone)
	{
		// Inside deadzone: no active sector deflection
		res.active = false;
		res.sector = m_lastHighlightedSector; // preserve previously selected until changed or reset
		return res;
	}

	// Calculate angle in screen-clockwise coordinates where 12 o'clock is 0
	// rx: right positive, ry: up positive in stick convention.
	// 12 o'clock (rx=0, ry=1): angle 0
	// 3 o'clock  (rx=1, ry=0): angle pi/2
	// 6 o'clock  (rx=0, ry=-1): angle pi
	// 9 o'clock  (rx=-1, ry=0): angle 3pi/2
	float angle = std::atan2(rx, ry);
	if (angle < 0.0f)
		angle += g_radial_two_pi;

	res.active = true;
	res.angleRad = angle;

	// Calculate sector: slice width = 2*pi / sectorCount.
	// Sector 0 is centered around 0 ([-slice/2, +slice/2)).
	const float slice = g_radial_two_pi / float(sectorCount);
	float shifted = angle + slice * 0.5f;
	if (shifted >= g_radial_two_pi)
		shifted -= g_radial_two_pi;

	int sector = int(shifted / slice);
	if (sector < 0)
		sector = 0;
	if (sector >= sectorCount)
		sector = sectorCount - 1;

	res.sector = sector;
	m_lastHighlightedSector = sector;

	return res;
}
