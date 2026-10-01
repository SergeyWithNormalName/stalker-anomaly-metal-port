#ifndef __XR_GAMEPAD_HAPTICS__
#define __XR_GAMEPAD_HAPTICS__
#pragma once

struct SHapticFeedbackRequest
{
	bool triggerHaptic;
	float amplitude;
	float durationSec;
	bool triggerAdaptiveKick;

	SHapticFeedbackRequest()
		: triggerHaptic(false), amplitude(0.0f), durationSec(0.0f), triggerAdaptiveKick(false) {}
};

class CShotHapticTracker
{
public:
	CShotHapticTracker(float defaultAmplitude = 0.75f, float defaultDurationSec = 0.06f);

	void Reset();

	// Called on a verified weapon shot event
	void OnRealShot(bool isAutomatic);

	// Called on a dry fire attempt (misfire, empty mag)
	void OnDryFire();

	// Called when switching weapons or holstering
	void OnWeaponSwitch();

	// Update tick: returns feedback request if an event needs delivery
	SHapticFeedbackRequest Process(float dtMs);

private:
	float m_amplitude;
	float m_durationSec;
	bool m_pendingShot;
	bool m_isAutomatic;
	int m_shotCount;
};

#endif // __XR_GAMEPAD_HAPTICS__
