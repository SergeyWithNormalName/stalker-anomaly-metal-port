#include "xr_gamepad_haptics.h"

CShotHapticTracker::CShotHapticTracker(float defaultAmplitude, float defaultDurationSec)
	: m_amplitude(defaultAmplitude), m_durationSec(defaultDurationSec)
{
	Reset();
}

void CShotHapticTracker::Reset()
{
	m_pendingShot = false;
	m_isAutomatic = false;
	m_shotCount = 0;
}

void CShotHapticTracker::OnRealShot(bool isAutomatic)
{
	m_pendingShot = true;
	m_isAutomatic = isAutomatic;
	++m_shotCount;
}

void CShotHapticTracker::OnDryFire()
{
	// Dry fire does not produce a real shot haptic
	m_pendingShot = false;
}

void CShotHapticTracker::OnWeaponSwitch()
{
	// Weapon switch cancels any pending feedback
	m_pendingShot = false;
	m_shotCount = 0;
}

SHapticFeedbackRequest CShotHapticTracker::Process(float dtMs)
{
	SHapticFeedbackRequest req;
	if (m_pendingShot)
	{
		req.triggerHaptic = true;
		req.amplitude = m_amplitude;
		req.durationSec = m_durationSec;
		req.triggerAdaptiveKick = true;
		m_pendingShot = false;
	}
	return req;
}
