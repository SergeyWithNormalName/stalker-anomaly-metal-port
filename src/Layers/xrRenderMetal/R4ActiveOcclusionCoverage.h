#pragma once

// Scratch-only, observe-only CPU coverage foundation. No renderer integration.
//
// The caller supplies clip-space values produced by the selected draw's exact
// WVP and vertex-jitter path. clipAbsDotTerms[r] must upper-bound the true
// Σ|M[r][i] * P[i]| (it must not be a potentially rounded-down estimate).
// upstreamAbsError[r] bounds all other shader/caller arithmetic, including
// CPU-vs-GPU WVP evaluation and TAA jitter mul/add. The helper adds a binary32
// dot-product roundoff bound; neither that model nor rasterEdgeGuardPixels is
// proof of Metal equivalence. The edge guard is an explicit empirical
// assumption because Apple does not specify a normative subpixel bound.
// Admission must provide conservative bounds and verified raster conventions.
// Compile with strict finite arithmetic (no fast-math or finite-math-only);
// the CPU reference probe uses -ffp-contract=off. Unknown inputs are skipped /
// visible. The implementation includes an absolute 8*FLT_MIN allowance for
// the bounded four-product/three-sum FP32 WVP dot; upstreamAbsError must also
// cover underflow/flush-to-zero uncertainty from upstream and jitter operations.
// No culling/suppression is implemented.

#include <array>
#include <cstddef>
#include <cstdint>

namespace R4ActiveOcclusion
{
constexpr uint32_t kGridWidth = 64;
constexpr uint32_t kGridHeight = 64;
constexpr size_t kCellCount = size_t(kGridWidth) * kGridHeight;
constexpr uint32_t kMaxTriangles = 32768;
constexpr uint64_t kMaxPrimitiveCellTests = 262'144;
constexpr uint64_t kMaxQueryCellTests = kCellCount;
constexpr uint32_t kNoMate = 0xffffffffu;

enum class FrontWinding : uint8_t
{
    Unknown = 0,
    Clockwise,
    CounterClockwise,
};

enum class PixelAreaWinding : uint8_t
{
    Unknown = 0,
    PositiveIsClockwise,
    PositiveIsCounterClockwise,
};

enum class CullMode : uint8_t
{
    Unknown = 0,
    None,
    Front,
    Back,
};

enum class DepthCompare : uint8_t
{
    Unknown = 0,
    Less,
    LessEqual,
};

enum class ViewportYDirection : uint8_t
{
    Unknown = 0,
    ClipPositiveYMapsToHigherPixelY,
    ClipPositiveYMapsToLowerPixelY,
};

// Helper screen coordinates always use upper-left origin (+x right, +y down).
// The actual relation between signed area and MTLWinding is an explicit required
// input; native equivalence remains unqualified pending offscreen validation.

struct ClipVertex
{
    // Post-WVP and post-jitter clip-space output from the admitted vertex path.
    float clip[4]{};

    // Per-component Σ|WVP-row-term| before jitter. Must be finite and >= 0.
    float clipAbsDotTerms[4]{};

    // Caller-supplied absolute bound for all remaining shader/caller arithmetic
    // (including jitter). Must be finite and >= 0. This is not an arbitrary
    // epsilon; it is an explicit part of the source contract.
    float upstreamAbsError[4]{};
};

struct Triangle
{
    uint64_t drawId = 0;
    uint32_t sourceTriangleId = 0;
    uint32_t sourceVertexId[3]{};
    uint32_t optionalMateTriangle = kNoMate;
    ClipVertex vertices[3]{};

    // Exact state for this triangle's draw. Unknown state makes it ineligible.
    FrontWinding frontWinding = FrontWinding::Unknown;
    CullMode cullMode = CullMode::Unknown;
    DepthCompare depthCompare = DepthCompare::Unknown;
    bool opaque = false;
    bool writesDepth = false;
    bool stencilGuaranteedPass = false;
};

struct BuildInput
{
    // Unique nonzero identifier for the current acquisition/view/projection/
    // jitter observation. A grid cannot be queried by another scope.
    uint64_t observationScopeId = 0;
    const Triangle* triangles = nullptr;
    size_t triangleCount = 0;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    ViewportYDirection viewportY = ViewportYDirection::Unknown;
    PixelAreaWinding pixelAreaWinding = PixelAreaWinding::Unknown;

    // Required positive finite margin, in viewport pixels. This is an explicit
    // empirical assumption, not an Apple-guaranteed raster precision bound.
    // Zero is rejected.
    float rasterEdgeGuardPixels = 0.0f;
};

struct BuildStats
{
    uint32_t trianglesVisited = 0;
    uint32_t trianglesSkipped = 0;
    uint32_t pairedUnionsAccepted = 0;
    uint32_t pairedUnionsRejected = 0;
    uint32_t cellsMarked = 0;
    uint64_t primitiveCellTests = 0;
    bool triangleCapReached = false;
    bool cellTestCapReached = false;
    bool invalidBuildInput = false;
};

enum class QueryResult : uint8_t
{
    VisibleOrUnknown = 0,
    ConservativelyBehindCoverage,
};

struct QueryStats
{
    uint32_t cellsVisited = 0;
    bool invalidInput = false;
    bool cellTestCapReached = false;
};

class CoverageGrid
{
public:
    // Builds a current-observation-only grid. Capacity exhaustion retains only
    // already proven coverage, which can reduce positives but cannot add them.
    bool Build(const BuildInput& input, BuildStats* stats);

    // Input is the eight post-WVP/post-jitter clip-space corners of an already
    // padded world AABB, using the same view/projection and viewport convention
    // as Build. Any invalid or uncertain corner returns VisibleOrUnknown.
    QueryResult QueryPaddedAabb(const ClipVertex corners[8],
        uint32_t viewportWidth, uint32_t viewportHeight,
        ViewportYDirection viewportY, float rasterEdgeGuardPixels,
        uint64_t observationScopeId,
        QueryStats* stats) const;

    void Reset();

private:
    std::array<uint64_t, kGridHeight> m_coveredRows{};
    std::array<float, kCellCount> m_farthestDepth{};
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    uint64_t m_observationScopeId = 0;
    ViewportYDirection m_viewportY = ViewportYDirection::Unknown;
    PixelAreaWinding m_pixelAreaWinding = PixelAreaWinding::Unknown;
    float m_rasterEdgeGuardPixels = 0.0f;
    bool m_built = false;
};
} // namespace R4ActiveOcclusion
