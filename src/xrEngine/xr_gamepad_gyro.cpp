#include "xr_gamepad_gyro.h"

#include <cmath>
#include <algorithm>

namespace
{
inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
}

CGyroAimProcessor::CGyroAimProcessor()
{
	Reset();
}

void CGyroAimProcessor::Reset()
{
	m_filteredPitch = 0.0f;
	m_filteredYaw = 0.0f;
	m_hasHistory = false;
}

SGyroDelta CGyroAimProcessor::Process(float gyroX, float gyroY, float gyroZ, bool adsHolding, float dtSec)
{
	if (dtSec <= 0.0f)
		return SGyroDelta(0.0f, 0.0f);

	if (m_config.mode == GYRO_MODE_OFF)
	{
		Reset();
		return SGyroDelta(0.0f, 0.0f);
	}

	if (m_config.mode == GYRO_MODE_ADS_ONLY && !adsHolding)
	{
		Reset();
		return SGyroDelta(0.0f, 0.0f);
	}

	// In controller orientation:
	// Pitch (tilt up/down) is rotation around X axis (gyroX).
	// Yaw (turn left/right) is rotation around Y axis (gyroY) or combined with roll (gyroZ).
	float rawPitch = gyroX;
	float rawYaw = gyroY;

	// Deadzone: eliminate resting sensor noise / drift
	const float mag = std::sqrt(rawPitch * rawPitch + rawYaw * rawYaw);
	if (mag < m_config.deadzone)
	{
		rawPitch = 0.0f;
		rawYaw = 0.0f;
	}

	// Spike clamp: cap extreme sudden jerks
	const float maxVel = m_config.maxVelocity;
	rawPitch = clampf(rawPitch, -maxVel, maxVel);
	rawYaw = clampf(rawYaw, -maxVel, maxVel);

	// Smoothing: exponential moving average
	// alpha = 1.0 -> no smoothing, lower alpha -> more smoothing
	float alpha = 1.0f - clampf(m_config.smoothing, 0.0f, 0.95f);
	if (!m_hasHistory)
	{
		m_filteredPitch = rawPitch;
		m_filteredYaw = rawYaw;
		m_hasHistory = true;
	}
	else
	{
		m_filteredPitch = m_filteredPitch + alpha * (rawPitch - m_filteredPitch);
		m_filteredYaw = m_filteredYaw + alpha * (rawYaw - m_filteredYaw);
	}

	// Integration: dtSec multiplier gives true frame-rate independence!
	// Rotation velocity (rad/s) * dtSec (s) = angle delta (rad)
	float pitchDelta = m_filteredPitch * m_config.rateScale * m_config.sensY * dtSec;
	float yawDelta = m_filteredYaw * m_config.rateScale * m_config.sensX * dtSec;

	if (m_config.invertY)
		pitchDelta = -pitchDelta;
	if (m_config.invertX)
		yawDelta = -yawDelta;

	return SGyroDelta(yawDelta, pitchDelta);
}
