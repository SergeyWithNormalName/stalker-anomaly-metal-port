#pragma once

#include <cmath>

// The legacy DualSense dpad contacts return (0,0) when lifted on macOS;
// touchpadButton.isTouched is not supplied by every driver. Test raw axes,
// before converting their absent (0,0) into the normalized surface centre.
inline bool GamepadAppleTouchContact(float x, float y, bool touched)
{
	return touched || std::fabs(x) > 0.0001f || std::fabs(y) > 0.0001f;
}

// Right-positive yaw is rotation about down, regardless of controller tilt.
// Some pads expose only total acceleration, so smooth that gravity estimate
// and retain the last plausible direction during strong linear acceleration.
class CGamepadAppleMotionBasis
{
public:
	void Reset() { m_valid = false; }
	void Update(float x, float y, float z, float dt, bool separateGravity)
	{
		const float magnitude = std::sqrt(x * x + y * y + z * z);
		if (!std::isfinite(magnitude) || magnitude < 0.75f || magnitude > 1.25f)
			return;
		x /= magnitude;
		y /= magnitude;
		z /= magnitude;
		const float alpha = separateGravity || !m_valid ? 1.0f :
			1.0f - std::exp(-std::fmax(0.0f, std::fmin(dt, 0.25f)) / 0.20f);
		m_x += alpha * (x - m_x);
		m_y += alpha * (y - m_y);
		m_z += alpha * (z - m_z);
		m_valid = true;
	}
	float Yaw(float x, float y, float z) const
	{
		const float magnitude = std::sqrt(m_x * m_x + m_y * m_y + m_z * m_z);
		return m_valid && magnitude > 0.001f ? (x * m_x + y * m_y + z * m_z) / magnitude : 0.0f;
	}
private:
	bool m_valid = false;
	float m_x = 0.0f, m_y = 0.0f, m_z = -1.0f;
};
