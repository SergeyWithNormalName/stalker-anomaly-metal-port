#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xray::metal::active_occlusion_catalog
{

constexpr std::size_t kCatalogBudgetBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t kStartingTriangleBudget = 4096u;
constexpr std::uint32_t kMaximumTriangleBudget = 32768u;
constexpr std::size_t kMaximumVisualRanges = 4096u;
constexpr std::size_t kMaximumDrawReceipts = 4096u;
constexpr std::uint32_t kNoMateTriangle = UINT32_MAX;

struct SourceTriangle
{
    std::uint32_t visualId = 0u;
    std::uint32_t triangleId = 0u;
    std::uint32_t mateTriangleId = kNoMateTriangle;
    std::array<std::uint32_t, 3> sourceVertexIds{};
    std::array<std::array<float, 3>, 3> positions{};
};
static_assert(sizeof(SourceTriangle) == 60u,
    "Keep the V7 full-catalog storage calculation tied to the source layout");

// Mate links are optional hints used only for certified adjacent-triangle
// unions. Keep only final reciprocal links; positions, source indices, and
// triangle order are immutable. This is allocation-free and returns the
// number of fields cleared.
std::size_t NormalizeReciprocalMateHints(SourceTriangle* triangles,
    std::size_t triangleCount);

struct VisualRange
{
    std::uint32_t visualId = 0u;
    std::uint32_t firstTriangle = 0u;
    std::uint32_t triangleCount = 0u;
};
static_assert(sizeof(VisualRange) == 12u);

struct PositionBounds
{
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
};

enum class ViewportYDirection : std::uint8_t
{
    Unknown = 0,
    ClipPositiveYMapsToLowerPixelY = 1,
};

// A bounded copy of the production after-success draw receipt. Its presence is
// proof that the ordinary main G-buffer DrawIndexed succeeded in this scope.
// No selection score or projected AABB is used as an occluder.
struct DrawReceipt
{
    std::uint64_t drawId = 0u;
    std::uint64_t scopeId = 0u;
    std::uint32_t engineFrame = 0u;
    std::uint32_t visualId = 0u;
    std::uint32_t firstTriangle = 0u;
    std::uint32_t triangleCount = 0u;
    std::uint32_t viewportWidth = 0u;
    std::uint32_t viewportHeight = 0u;
    ViewportYDirection viewportY = ViewportYDirection::Unknown;
    bool drawSucceeded = false;
    bool clockwiseFrontFace = false;
    bool backFaceCull = false;
    bool depthLess = false;
    bool opaque = false;
    bool writesDepth = false;
    bool stencilPassGuaranteed = false;
    std::array<float, 16> uploadedWvp{};
    std::array<float, 2> uploadedJitter{};
    PositionBounds localBounds{};
};

struct SelectedDraw
{
    std::uint64_t drawId = 0u;
    std::uint32_t visualId = 0u;
    std::uint32_t firstTriangle = 0u;
    std::uint32_t triangleCount = 0u;
    double projectedPixelArea = 0.0;
    double projectedNearDepth = 0.0;
    std::size_t receiptIndex = 0u;
};

struct SelectionStats
{
    std::size_t receiptsSeen = 0u;
    std::size_t receiptsSelected = 0u;
    std::size_t invalidReceipt = 0u;
    std::size_t duplicateReceipt = 0u;
    std::size_t unknownProjection = 0u;
    std::size_t outsideViewport = 0u;
    std::size_t omittedByTriangleBudget = 0u;
    std::uint32_t selectedTriangles = 0u;
    std::uint32_t effectiveTriangleBudget = 0u;
};

class Catalog
{
public:
    enum class BuildResult : std::uint8_t
    {
        Ready,
        BudgetExceeded,
        TooManyRanges,
        InvalidRange,
        InvalidTriangle,
    };

    // Rvalue ownership avoids a second full-catalog copy in the production
    // loader. Failure leaves this catalog empty; partial prefixes are rejected.
    // A caller may lower, but cannot raise, the 64 MiB hard byte budget.
    BuildResult Build(std::vector<VisualRange>&& ranges,
        std::vector<SourceTriangle>&& triangles,
        std::size_t byteBudget = kCatalogBudgetBytes);
    void Reset();

    const std::vector<VisualRange>& Ranges() const { return m_ranges; }
    const std::vector<SourceTriangle>& Triangles() const { return m_triangles; }
    std::size_t StorageBytes() const;
    const VisualRange* Find(std::uint32_t visualId) const;

private:
    std::vector<VisualRange> m_ranges;
    std::vector<SourceTriangle> m_triangles;
};

struct SelectionResult
{
    std::vector<SelectedDraw> draws;
    SelectionStats stats;
};

// Rank only actual successful draws. Projected bounds are a work-priority
// heuristic: the coverage builder must still use exact source triangles and
// the existing conservative raster/uncertainty rules. Whole visuals are
// selected; no draw is truncated to fill the budget.
bool SelectCurrentView(const Catalog& catalog,
    const DrawReceipt* receipts, std::size_t receiptCount,
    std::uint64_t scopeId, std::uint32_t engineFrame,
    std::uint32_t viewportWidth, std::uint32_t viewportHeight,
    ViewportYDirection viewportY,
    SelectionResult& result,
    std::uint32_t requestedBudget = kStartingTriangleBudget,
    std::size_t receiptCap = kMaximumDrawReceipts);

} // namespace xray::metal::active_occlusion_catalog
