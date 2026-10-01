#include "R4ActiveOcclusionCatalogSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace xray::metal::active_occlusion_catalog
{
namespace
{
bool checkedMultiply(const std::size_t a, const std::size_t b, std::size_t& result)
{
    if (a != 0u && b > std::numeric_limits<std::size_t>::max() / a)
        return false;
    result = a * b;
    return true;
}

struct ProjectedFootprint
{
    double area = 0.0;
    double nearDepth = 0.0;
};

struct RankedReceipt
{
    SelectedDraw selected;
    double areaPerTriangle = 0.0;
};

bool finiteBounds(const PositionBounds& bounds)
{
    for (unsigned axis = 0; axis != 3u; ++axis)
        if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) ||
            bounds.minimum[axis] > bounds.maximum[axis])
            return false;
    return true;
}

bool projectBoundsForRanking(const DrawReceipt& receipt,
    const std::uint32_t width, const std::uint32_t height,
    const ViewportYDirection viewportY, ProjectedFootprint& footprint,
    bool& outsideViewport)
{
    outsideViewport = false;
    if (width == 0u || height == 0u || receipt.viewportY != viewportY ||
        viewportY != ViewportYDirection::ClipPositiveYMapsToLowerPixelY ||
        !finiteBounds(receipt.localBounds))
        return false;
    for (const float value : receipt.uploadedWvp)
        if (!std::isfinite(value))
            return false;
    if (!std::isfinite(receipt.uploadedJitter[0]) || !std::isfinite(receipt.uploadedJitter[1]))
        return false;

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double minDepth = std::numeric_limits<double>::infinity();
    double maxDepth = -std::numeric_limits<double>::infinity();
    bool anyPositiveW = false;
    bool anyNonPositiveW = false;
    bool anyZeroW = false;
    for (unsigned corner = 0; corner != 8u; ++corner)
    {
        const double x = receipt.localBounds.minimum[0u] +
            ((corner & 1u) ? receipt.localBounds.maximum[0u] - receipt.localBounds.minimum[0u] : 0.0);
        const double y = receipt.localBounds.minimum[1u] +
            ((corner & 2u) ? receipt.localBounds.maximum[1u] - receipt.localBounds.minimum[1u] : 0.0);
        const double z = receipt.localBounds.minimum[2u] +
            ((corner & 4u) ? receipt.localBounds.maximum[2u] - receipt.localBounds.minimum[2u] : 0.0);
        // Match the backend's column-major uploaded m_WVP and shader jitter
        // transform. This projection ranks work only; it never marks coverage.
        double clipX = double(receipt.uploadedWvp[0]) * x +
            double(receipt.uploadedWvp[4]) * y + double(receipt.uploadedWvp[8]) * z +
            double(receipt.uploadedWvp[12]);
        double clipY = double(receipt.uploadedWvp[1]) * x +
            double(receipt.uploadedWvp[5]) * y + double(receipt.uploadedWvp[9]) * z +
            double(receipt.uploadedWvp[13]);
        const double clipZ = double(receipt.uploadedWvp[2]) * x +
            double(receipt.uploadedWvp[6]) * y + double(receipt.uploadedWvp[10]) * z +
            double(receipt.uploadedWvp[14]);
        const double clipW = double(receipt.uploadedWvp[3]) * x +
            double(receipt.uploadedWvp[7]) * y + double(receipt.uploadedWvp[11]) * z +
            double(receipt.uploadedWvp[15]);
        if (!std::isfinite(clipX) || !std::isfinite(clipY) || !std::isfinite(clipZ) ||
            !std::isfinite(clipW))
            return false;
        if (clipW <= 0.0)
        {
            anyNonPositiveW = true;
            anyZeroW = anyZeroW || clipW == 0.0;
            continue;
        }
        anyPositiveW = true;
        clipX += double(receipt.uploadedJitter[0]) * clipW;
        clipY += double(receipt.uploadedJitter[1]) * clipW;
        const double ndcX = clipX / clipW;
        const double ndcY = clipY / clipW;
        const double ndcZ = clipZ / clipW;
        if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || !std::isfinite(ndcZ))
            return false;
        // Native observer convention: full-target viewport, clip +Y maps to
        // lower pixel Y. A clamped rectangle is only a sorting heuristic.
        const double pixelX = (ndcX * 0.5 + 0.5) * width;
        const double pixelY = (0.5 - ndcY * 0.5) * height;
        minX = std::min(minX, pixelX);
        maxX = std::max(maxX, pixelX);
        minY = std::min(minY, pixelY);
        maxY = std::max(maxY, pixelY);
        minDepth = std::min(minDepth, ndcZ);
        maxDepth = std::max(maxDepth, ndcZ);
    }
    if (!anyPositiveW)
    {
        // A zero-W-only bound is numerically indeterminate; negative-W-only
        // bounds are wholly behind the camera under this ranking convention.
        outsideViewport = !anyZeroW;
        return false;
    }
    if (anyNonPositiveW)
    {
        // The AABB crosses the camera plane. This ranking heuristic cannot
        // bound its projected rectangle, so give it highest priority. Exact
        // source triangles still pass through the strict coverage checks.
        footprint.area = double(width) * double(height);
        footprint.nearDepth = 0.0;
        return true;
    }
    if (maxDepth < 0.0 || minDepth > 1.0)
    {
        outsideViewport = true;
        return false;
    }
    if (minDepth < 0.0 || maxDepth > 1.0)
        minDepth = 0.0; // near/far crossing gets conservative ranking priority
    minX = std::clamp(minX, 0.0, double(width));
    maxX = std::clamp(maxX, 0.0, double(width));
    minY = std::clamp(minY, 0.0, double(height));
    maxY = std::clamp(maxY, 0.0, double(height));
    if (!(maxX > minX) || !(maxY > minY))
    {
        outsideViewport = true;
        return false;
    }
    footprint.area = (maxX - minX) * (maxY - minY);
    footprint.nearDepth = minDepth;
    return std::isfinite(footprint.area) && footprint.area > 0.0 &&
        std::isfinite(footprint.nearDepth);
}

bool receiptRasterStateSupported(const DrawReceipt& r)
{
    return r.drawSucceeded && r.clockwiseFrontFace && r.backFaceCull && r.depthLess &&
        r.opaque && r.writesDepth && r.stencilPassGuaranteed;
}
} // namespace

std::size_t NormalizeReciprocalMateHints(SourceTriangle* triangles,
    const std::size_t triangleCount)
{
    if (!triangles)
        return 0u;
    std::size_t cleared = 0u;
    for (std::size_t i = 0u; i < triangleCount; ++i)
    {
        const std::uint32_t mate = triangles[i].mateTriangleId;
        if (mate == kNoMateTriangle)
            continue;
        if (mate >= triangleCount || mate == i || triangles[mate].mateTriangleId != i)
        {
            triangles[i].mateTriangleId = kNoMateTriangle;
            ++cleared;
        }
    }
    return cleared;
}

Catalog::BuildResult Catalog::Build(std::vector<VisualRange>&& ranges,
    std::vector<SourceTriangle>&& triangles, const std::size_t byteBudget)
{
    Reset();
    const std::size_t effectiveByteBudget = std::min(byteBudget, kCatalogBudgetBytes);
    if (ranges.size() > kMaximumVisualRanges)
        return BuildResult::TooManyRanges;
    if (triangles.size() > std::numeric_limits<std::uint32_t>::max())
        return BuildResult::BudgetExceeded;
    std::size_t rangeBytes = 0u;
    std::size_t triangleBytes = 0u;
    if (!checkedMultiply(ranges.capacity(), sizeof(VisualRange), rangeBytes) ||
        !checkedMultiply(triangles.capacity(), sizeof(SourceTriangle), triangleBytes))
        return BuildResult::BudgetExceeded;
    if (triangleBytes > effectiveByteBudget ||
        rangeBytes > effectiveByteBudget - triangleBytes)
        return BuildResult::BudgetExceeded;

    std::size_t expectedFirst = 0u;
    for (std::size_t rangeIndex = 0; rangeIndex < ranges.size(); ++rangeIndex)
    {
        const VisualRange& range = ranges[rangeIndex];
        if (range.triangleCount == 0u || range.firstTriangle != expectedFirst ||
            expectedFirst > triangles.size() ||
            range.triangleCount > triangles.size() - expectedFirst ||
            (rangeIndex != 0u && ranges[rangeIndex - 1u].visualId >= range.visualId))
            return BuildResult::InvalidRange;
        for (std::uint32_t localId = 0u; localId < range.triangleCount; ++localId)
        {
            const SourceTriangle& t = triangles[range.firstTriangle + localId];
            if (t.visualId != range.visualId || t.triangleId != localId ||
                (t.mateTriangleId != kNoMateTriangle &&
                    (t.mateTriangleId >= range.triangleCount || t.mateTriangleId == localId)))
                return BuildResult::InvalidTriangle;
            for (const std::uint32_t sourceVertexId : t.sourceVertexIds)
                if (sourceVertexId > UINT16_MAX)
                    return BuildResult::InvalidTriangle;
            for (const auto& position : t.positions)
                for (const float component : position)
                    if (!std::isfinite(component))
                        return BuildResult::InvalidTriangle;
            if (t.mateTriangleId != kNoMateTriangle &&
                triangles[range.firstTriangle + t.mateTriangleId].mateTriangleId != localId)
                return BuildResult::InvalidTriangle;
        }
        expectedFirst += range.triangleCount;
    }
    if (expectedFirst != triangles.size())
        return BuildResult::InvalidRange;

    m_ranges = std::move(ranges);
    m_triangles = std::move(triangles);
    return BuildResult::Ready;
}

void Catalog::Reset()
{
    std::vector<VisualRange>().swap(m_ranges);
    std::vector<SourceTriangle>().swap(m_triangles);
}

std::size_t Catalog::StorageBytes() const
{
    std::size_t rangeBytes = 0u;
    std::size_t triangleBytes = 0u;
    if (!checkedMultiply(m_ranges.capacity(), sizeof(VisualRange), rangeBytes) ||
        !checkedMultiply(m_triangles.capacity(), sizeof(SourceTriangle), triangleBytes) ||
        rangeBytes > std::numeric_limits<std::size_t>::max() - triangleBytes)
        return std::numeric_limits<std::size_t>::max();
    return rangeBytes + triangleBytes;
}

const VisualRange* Catalog::Find(const std::uint32_t visualId) const
{
    const auto found = std::lower_bound(m_ranges.begin(), m_ranges.end(), visualId,
        [](const VisualRange& range, const std::uint32_t id) { return range.visualId < id; });
    return found != m_ranges.end() && found->visualId == visualId ? &*found : nullptr;
}

bool SelectCurrentView(const Catalog& catalog,
    const DrawReceipt* receipts, const std::size_t receiptCount,
    const std::uint64_t scopeId, const std::uint32_t engineFrame,
    const std::uint32_t viewportWidth, const std::uint32_t viewportHeight,
    const ViewportYDirection viewportY, SelectionResult& result,
    const std::uint32_t requestedBudget, const std::size_t receiptCap)
{
    result = {};
    result.stats.receiptsSeen = receiptCount;
    result.stats.effectiveTriangleBudget = std::min(requestedBudget, kMaximumTriangleBudget);
    if ((receiptCount != 0u && !receipts) || receiptCount > receiptCap ||
        receiptCap > kMaximumDrawReceipts)
        return false;
    if (!scopeId || !viewportWidth || !viewportHeight ||
        viewportY != ViewportYDirection::ClipPositiveYMapsToLowerPixelY)
        return false;

    try
    {
        std::vector<RankedReceipt> ranked;
        ranked.reserve(receiptCount);
        std::unordered_map<std::uint64_t, std::size_t> drawIdCounts;
        std::unordered_map<std::uint32_t, std::size_t> visualIdCounts;
        drawIdCounts.reserve(receiptCount);
        visualIdCounts.reserve(receiptCount);
        for (std::size_t i = 0; i < receiptCount; ++i)
        {
            const DrawReceipt& r = receipts[i];
            if (r.scopeId != scopeId || r.engineFrame != engineFrame ||
                r.viewportWidth != viewportWidth || r.viewportHeight != viewportHeight ||
                !receiptRasterStateSupported(r) || r.triangleCount == 0u)
            {
                ++result.stats.invalidReceipt;
                continue;
            }
            const VisualRange* range = catalog.Find(r.visualId);
            if (!range || range->firstTriangle != r.firstTriangle ||
                range->triangleCount != r.triangleCount)
            {
                ++result.stats.invalidReceipt;
                continue;
            }
            ProjectedFootprint footprint;
            bool outsideViewport = false;
            if (!projectBoundsForRanking(r, viewportWidth, viewportHeight, viewportY,
                    footprint, outsideViewport))
            {
                // Unknown/outside bounds are omitted. They are never converted
                // into coverage and cannot make another draw more occluding.
                if (outsideViewport)
                    ++result.stats.outsideViewport;
                else
                    ++result.stats.unknownProjection;
                continue;
            }
            ranked.push_back({{r.drawId, r.visualId, r.firstTriangle, r.triangleCount,
                footprint.area, footprint.nearDepth, i},
                footprint.area / double(r.triangleCount)});
            ++drawIdCounts[r.drawId];
            ++visualIdCounts[r.visualId];
        }
        ranked.erase(std::remove_if(ranked.begin(), ranked.end(), [&](const RankedReceipt& r) {
            const bool duplicate = drawIdCounts[r.selected.drawId] != 1u ||
                visualIdCounts[r.selected.visualId] != 1u;
            if (duplicate)
                ++result.stats.duplicateReceipt;
            return duplicate;
        }), ranked.end());
        std::sort(ranked.begin(), ranked.end(), [](const RankedReceipt& a, const RankedReceipt& b) {
            if (a.areaPerTriangle != b.areaPerTriangle)
                return a.areaPerTriangle > b.areaPerTriangle;
            if (a.selected.projectedNearDepth != b.selected.projectedNearDepth)
                return a.selected.projectedNearDepth < b.selected.projectedNearDepth;
            if (a.selected.drawId != b.selected.drawId)
                return a.selected.drawId < b.selected.drawId;
            return a.selected.visualId < b.selected.visualId;
        });

        result.draws.reserve(ranked.size());
        for (const RankedReceipt& candidate : ranked)
        {
            if (candidate.selected.triangleCount > result.stats.effectiveTriangleBudget -
                    result.stats.selectedTriangles)
            {
                ++result.stats.omittedByTriangleBudget;
                continue;
            }
            result.stats.selectedTriangles += candidate.selected.triangleCount;
            result.draws.push_back(candidate.selected);
        }
        result.stats.receiptsSelected = result.draws.size();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result = {};
        return false;
    }
}

} // namespace xray::metal::active_occlusion_catalog
