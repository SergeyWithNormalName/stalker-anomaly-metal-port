#include "xr_gamepad_nav.h"

#include <limits>

namespace
{
constexpr float g_nav_epsilon = 1.0e-3f;

float CalcOverlap(float minA, float maxA, float minB, float maxB)
{
	float overlap = std::min(maxA, maxB) - std::max(minA, minB);
	return overlap > 0.0f ? overlap : 0.0f;
}
}

int FindBestSpatialNeighbor(const SNavRect& current, const std::vector<SNavCandidate>& candidates,
	ENavDirection dir)
{
	if (dir == NAV_NONE || candidates.empty())
		return -1;

	const float curCx = current.CenterX();
	const float curCy = current.CenterY();

	int bestId = -1;
	float bestScore = std::numeric_limits<float>::max();

	for (size_t i = 0; i < candidates.size(); ++i)
	{
		const SNavCandidate& cand = candidates[i];
		if (!cand.enabled || !cand.focusable)
			continue;

		const SNavRect& r = cand.rect;
		const float candCx = r.CenterX();
		const float candCy = r.CenterY();

		float primaryDist = 0.0f;
		float transverseDist = 0.0f;
		float transverseOverlap = 0.0f;
		bool inHalfPlane = false;

		switch (dir)
		{
		case NAV_RIGHT:
			if (candCx > curCx + g_nav_epsilon && r.x1 >= current.x1)
			{
				inHalfPlane = true;
				primaryDist = candCx - curCx;
				transverseDist = std::fabs(candCy - curCy);
				transverseOverlap = CalcOverlap(current.y1, current.y2, r.y1, r.y2);
			}
			break;

		case NAV_LEFT:
			if (candCx < curCx - g_nav_epsilon && r.x2 <= current.x2)
			{
				inHalfPlane = true;
				primaryDist = curCx - candCx;
				transverseDist = std::fabs(candCy - curCy);
				transverseOverlap = CalcOverlap(current.y1, current.y2, r.y1, r.y2);
			}
			break;

		case NAV_DOWN:
			if (candCy > curCy + g_nav_epsilon && r.y1 >= current.y1)
			{
				inHalfPlane = true;
				primaryDist = candCy - curCy;
				transverseDist = std::fabs(candCx - curCx);
				transverseOverlap = CalcOverlap(current.x1, current.x2, r.x1, r.x2);
			}
			break;

		case NAV_UP:
			if (candCy < curCy - g_nav_epsilon && r.y2 <= current.y2)
			{
				inHalfPlane = true;
				primaryDist = curCy - candCy;
				transverseDist = std::fabs(candCx - curCx);
				transverseOverlap = CalcOverlap(current.x1, current.x2, r.x1, r.x2);
			}
			break;

		default:
			break;
		}

		if (!inHalfPlane)
			continue;

		// Direction cone check: transverse offset should not overwhelmingly exceed primary distance
		// unless there is significant overlap on the transverse axis.
		if (transverseOverlap <= 0.0f && transverseDist > primaryDist * 2.5f)
			continue;

		float score = 0.0f;
		if (transverseOverlap > 0.0f)
		{
			// Candidate is naturally in the same row/column: strong overlap bonus
			score = primaryDist * 1.0f + transverseDist * 0.5f - transverseOverlap * 1.5f;
		}
		else
		{
			// Diagonal or offset candidate: penalize transverse offset
			score = primaryDist * 1.2f + transverseDist * 3.0f;
		}

		if (score < bestScore)
		{
			bestScore = score;
			bestId = cand.id;
		}
	}

	return bestId;
}

int FindGridNeighbor(const std::vector<SGridCell>& grid, int currentId, ENavDirection dir)
{
	if (dir == NAV_NONE || grid.empty())
		return -1;

	const SGridCell* cur = nullptr;
	for (size_t i = 0; i < grid.size(); ++i)
	{
		if (grid[i].id == currentId)
		{
			cur = &grid[i];
			break;
		}
	}

	if (!cur)
		return -1;

	// Convert grid cells into spatial candidates using cell units
	SNavRect curRect(float(cur->col), float(cur->row),
		float(cur->col + cur->colSpan), float(cur->row + cur->rowSpan));

	std::vector<SNavCandidate> candidates;
	candidates.reserve(grid.size());

	for (size_t i = 0; i < grid.size(); ++i)
	{
		if (grid[i].id == currentId)
			continue;

		SNavRect rect(float(grid[i].col), float(grid[i].row),
			float(grid[i].col + grid[i].colSpan), float(grid[i].row + grid[i].rowSpan));

		candidates.push_back(SNavCandidate(grid[i].id, rect, grid[i].enabled, true));
	}

	return FindBestSpatialNeighbor(curRect, candidates, dir);
}

int CycleTab(int currentIdx, int totalTabs, bool forward, bool wrap)
{
	if (totalTabs <= 0)
		return 0;

	if (forward)
	{
		int next = currentIdx + 1;
		if (next >= totalTabs)
			return wrap ? 0 : (totalTabs - 1);
		return next;
	}
	else
	{
		int prev = currentIdx - 1;
		if (prev < 0)
			return wrap ? (totalTabs - 1) : 0;
		return prev;
	}
}

CNavRepeatTracker::CNavRepeatTracker(float delayMs, float repeatMs)
	: m_delayMs(delayMs), m_repeatMs(repeatMs), m_heldMs(0.0f), m_wasDown(false), m_repeated(false)
{
}

void CNavRepeatTracker::Reset()
{
	m_heldMs = 0.0f;
	m_wasDown = false;
	m_repeated = false;
}

void CNavRepeatTracker::SetTiming(float delayMs, float repeatMs)
{
	m_delayMs = delayMs > 0.0f ? delayMs : 350.0f;
	m_repeatMs = repeatMs > 0.0f ? repeatMs : 110.0f;
}

bool CNavRepeatTracker::Update(bool isDown, float dtMs)
{
	if (!isDown)
	{
		m_wasDown = false;
		m_heldMs = 0.0f;
		m_repeated = false;
		return false;
	}

	if (!m_wasDown)
	{
		m_wasDown = true;
		m_heldMs = 0.0f;
		m_repeated = false;
		return true;
	}

	m_heldMs += dtMs;
	const float interval = m_repeated ? m_repeatMs : m_delayMs;
	if (m_heldMs >= interval)
	{
		m_heldMs = 0.0f;
		m_repeated = true;
		return true;
	}

	return false;
}
