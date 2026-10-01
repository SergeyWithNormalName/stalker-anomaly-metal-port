#ifndef __XR_GAMEPAD_GYRO__
#define __XR_GAMEPAD_GYRO__
#pragma once

enum EGyroMode
{
	GYRO_MODE_OFF = 0,
	GYRO_MODE_ADS_ONLY = 1,
	GYRO_MODE_ALWAYS = 2
};

struct SGyroConfig
{
	EGyroMode mode;
	float sensX;
	float sensY;
	bool invertY;
	bool invertX;
	float smoothing;     // 0 = no smoothing (instant), 1 = max smoothing
	float deadzone;      // rad/s below which input is considered zero (no drift)
	float maxVelocity;   // rad/s above which input is clamped (spike clamp)
	float rateScale;     // degrees/pixels scale multiplier

	SGyroConfig()
	{
		mode = GYRO_MODE_ADS_ONLY;
		sensX = 1.0f;
		sensY = 1.0f;
		invertY = false;
		invertX = false;
		smoothing = 0.35f;
		deadzone = 0.015f;
		maxVelocity = 15.0f;
		rateScale = 800.0f;
	}
};

struct SGyroDelta
{
	float dx; // horizontal camera delta (right positive)
	float dy; // vertical camera delta (up positive in raw, down positive in mouse convention)

	SGyroDelta() : dx(0.0f), dy(0.0f) {}
	SGyroDelta(float inX, float inY) : dx(inX), dy(inY) {}
};

class CGyroAimProcessor
{
public:
	CGyroAimProcessor();

	void Reset();
	void Configure(const SGyroConfig& config) { m_config = config; }
	const SGyroConfig& Config() const { return m_config; }

	// Process raw angular velocity (rad/s), ADS holding state, and frame delta time (seconds).
	// Returns relative camera look delta for the frame.
	SGyroDelta Process(float gyroX, float gyroY, float gyroZ, bool adsHolding, float dtSec);

private:
	SGyroConfig m_config;
	float m_filteredPitch;
	float m_filteredYaw;
	bool m_hasHistory;
};

#endif // __XR_GAMEPAD_GYRO__
