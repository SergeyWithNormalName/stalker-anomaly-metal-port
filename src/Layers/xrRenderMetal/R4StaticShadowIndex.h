#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace xray::metal
{
// Generic static world-space broad phase for shadow casters. Inputs use
// column-major 4x4 matrices and carry no renderer or engine ownership.
class R4StaticShadowIndex
{
public:
    struct Box { std::array<double, 3> lo{}, hi{}; };
    struct Entry { Box box; std::size_t draw; };
    struct Node { Box box; std::size_t begin = 0, end = 0, left = 0, right = 0; };
    struct Plane { std::array<double, 4> coefficients{}, roundoff{}; };
    using Planes = std::array<Plane, 6>;

    std::vector<Entry> entries;
    std::vector<std::size_t> unbounded;
    std::vector<Node> nodes;
    std::vector<std::size_t> candidates;

    void Add(bool hasBounds, const std::array<float, 3>& lo,
        const std::array<float, 3>& hi, const float* model, std::size_t draw)
    {
        if (!model)
        {
            unbounded.push_back(draw);
            return;
        }
        bool valid = hasBounds && model[3] == 0 && model[7] == 0 &&
            model[11] == 0 && model[15] == 1;
        for (std::size_t i = 0; i < 16; ++i)
            valid &= std::isfinite(model[i]) && std::abs(model[i]) < 1e12f;
        for (std::size_t a = 0; a < 3; ++a)
            valid &= std::isfinite(lo[a]) && std::isfinite(hi[a]) && lo[a] <= hi[a] &&
                std::abs(lo[a]) < 1e12f && std::abs(hi[a]) < 1e12f;
        if (!valid) { unbounded.push_back(draw); return; }
        Box b;
        for (std::size_t a = 0; a < 3; ++a)
        {
            b.lo[a] = b.hi[a] = model[12 + a];
            double magnitude = std::abs(b.lo[a]);
            for (std::size_t k = 0; k < 3; ++k)
            {
                const double v0 = double(model[k * 4 + a]) * lo[k];
                const double v1 = double(model[k * 4 + a]) * hi[k];
                b.lo[a] += std::min(v0, v1); b.hi[a] += std::max(v0, v1);
                magnitude += std::max(std::abs(v0), std::abs(v1));
            }
            // Cover float multiply/add reassociation in the original WVP path,
            // including cancellation of large model terms. Far wider than its
            // rounding error; boundary/uncertain cases reach the exact test.
            const double padding = 1e-4 * (1 + magnitude);
            b.lo[a] -= padding; b.hi[a] += padding;
        }
        entries.push_back({b, draw});
    }

    std::size_t Build(std::size_t begin, std::size_t end)
    {
        Node n; n.begin = begin; n.end = end; n.box = entries[begin].box;
        for (std::size_t i = begin + 1; i < end; ++i)
            for (std::size_t a = 0; a < 3; ++a)
            { n.box.lo[a] = std::min(n.box.lo[a], entries[i].box.lo[a]);
              n.box.hi[a] = std::max(n.box.hi[a], entries[i].box.hi[a]); }
        const std::size_t id = nodes.size(); nodes.push_back(n);
        if (end - begin > 8)
        {
            std::size_t axis = 0;
            for (std::size_t a = 1; a < 3; ++a)
                if (n.box.hi[a] - n.box.lo[a] > n.box.hi[axis] - n.box.lo[axis]) axis = a;
            const std::size_t mid = begin + (end - begin) / 2;
            std::nth_element(entries.begin() + begin, entries.begin() + mid, entries.begin() + end,
                [axis](const Entry& a, const Entry& b) {
                    return a.box.lo[axis] + a.box.hi[axis] < b.box.lo[axis] + b.box.hi[axis]; });
            const std::size_t left = Build(begin, mid), right = Build(mid, end);
            nodes[id].left = left; nodes[id].right = right;
        }
        return id;
    }

    void Finish()
    {
        nodes.clear(); nodes.reserve(entries.size() * 2);
        if (!entries.empty()) Build(0, entries.size());
        candidates.reserve(entries.size() + unbounded.size());
    }

    static Planes MakePlanes(const float* vp)
    {
        Planes p{};
        if (!vp) return p;
        for (std::size_t k = 0; k < 4; ++k)
        {
            const double x = vp[k * 4], y = vp[k * 4 + 1];
            const double z = vp[k * 4 + 2], w = vp[k * 4 + 3];
            p[0].coefficients[k] = w + x; p[1].coefficients[k] = w - x;
            p[2].coefficients[k] = w + y; p[3].coefficients[k] = w - y;
            p[4].coefficients[k] = z; p[5].coefficients[k] = w - z;
            p[0].roundoff[k] = p[1].roundoff[k] = std::abs(w) + std::abs(x);
            p[2].roundoff[k] = p[3].roundoff[k] = std::abs(w) + std::abs(y);
            p[4].roundoff[k] = std::abs(z);
            p[5].roundoff[k] = std::abs(w) + std::abs(z);
        }
        return p;
    }

    static bool Outside(const Box& b, const Planes& planes)
    {
        for (const auto& plane : planes)
        {
            const auto& p = plane.coefficients;
            double upper = p[3], magnitude = plane.roundoff[3];
            for (std::size_t a = 0; a < 3; ++a)
            {
                upper += p[a] * (p[a] >= 0 ? b.hi[a] : b.lo[a]);
                magnitude += plane.roundoff[a] * std::max(std::abs(b.lo[a]), std::abs(b.hi[a]));
            }
            if (upper < -1e-4 * (1 + magnitude)) return true;
        }
        return false;
    }

    void Visit(std::size_t id, const Planes& planes)
    {
        const Node& n = nodes[id];
        if (Outside(n.box, planes)) return;
        if (n.left)
        { Visit(n.left, planes); Visit(n.right, planes); }
        else
            for (std::size_t i = n.begin; i < n.end; ++i) candidates.push_back(entries[i].draw);
    }

    const std::vector<std::size_t>& Query(const float* vp)
    {
        candidates.assign(unbounded.begin(), unbounded.end());
        bool valid = vp != nullptr;
        if (valid)
            for (std::size_t i = 0; i < 16; ++i)
                valid &= std::isfinite(vp[i]) && std::abs(vp[i]) < 1e12f;
        if (valid) { if (!nodes.empty()) Visit(0, MakePlanes(vp)); }
        else for (const Entry& e : entries) candidates.push_back(e.draw);
        // Retain original draw order and all source transparency/state semantics.
        std::sort(candidates.begin(), candidates.end());
        return candidates;
    }
};
} // namespace xray::metal
