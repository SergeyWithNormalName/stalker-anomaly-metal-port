#include "R4ActiveOcclusionCoverage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace R4ActiveOcclusion
{
namespace
{
constexpr double kFloatUnitRoundoff = 0x1p-24;
constexpr double kGamma8 = (8.0 * kFloatUnitRoundoff) / (1.0 - 8.0 * kFloatUnitRoundoff);
constexpr double kGamma2 = (2.0 * kFloatUnitRoundoff) / (1.0 - 2.0 * kFloatUnitRoundoff);
constexpr double kDotUnderflowAllowance = 8.0 * std::numeric_limits<float>::min();

struct ProjectedPoint
{
    double x = 0.0;
    double y = 0.0;
    double screenError = 0.0;
    double depth = 0.0;
    double depthError = 0.0;
};

struct Polygon
{
    ProjectedPoint points[4]{};
    uint32_t count = 0;
    double signedArea = 0.0;
    double farDepth = 0.0;
};

bool FiniteNonnegative(float value)
{
    return std::isfinite(value) && value >= 0.0f;
}

bool Project(const ClipVertex& input, uint32_t width, uint32_t height,
    ViewportYDirection yDirection, ProjectedPoint& output)
{
    double c[4]{};
    double e[4]{};
    for (size_t i = 0; i != 4; ++i)
    {
        const float clip = input.clip[i];
        const float terms = input.clipAbsDotTerms[i];
        const float extra = input.upstreamAbsError[i];
        if (!std::isfinite(clip) || !FiniteNonnegative(terms) || !FiniteNonnegative(extra))
            return false;

        c[i] = static_cast<double>(clip);
        // Bound a four-term dot product rounded in binary32, plus the caller's
        // explicit bound for all other shader/caller arithmetic.
        e[i] = kGamma8 * static_cast<double>(terms) + kDotUnderflowAllowance +
            static_cast<double>(extra);
        if (!std::isfinite(e[i]))
            return false;
    }

    // The helper only handles wholly in-front primitives/query corners. It
    // deliberately does not clip uncertain W, near, or far crossings.
    if (!(c[3] > e[3]))
        return false;
    if (!(c[2] - e[2] > 0.0 && c[2] + e[2] < c[3] - e[3]))
        return false;

    const double wSafe = c[3] - e[3];
    const double ndcX = c[0] / c[3];
    const double ndcY = c[1] / c[3];
    const double ndcDepth = c[2] / c[3];
    const double xError = (e[0] + std::abs(ndcX) * e[3]) / wSafe +
        kGamma2 * std::abs(ndcX) + kDotUnderflowAllowance;
    const double yError = (e[1] + std::abs(ndcY) * e[3]) / wSafe +
        kGamma2 * std::abs(ndcY) + kDotUnderflowAllowance;
    const double zError = (e[2] + std::abs(ndcDepth) * e[3]) / wSafe +
        kGamma2 * std::abs(ndcDepth) + kDotUnderflowAllowance;

    const double halfWidth = static_cast<double>(width) * 0.5;
    const double halfHeight = static_cast<double>(height) * 0.5;
    output.x = (ndcX + 1.0) * halfWidth;
    output.y = yDirection == ViewportYDirection::ClipPositiveYMapsToHigherPixelY
        ? (ndcY + 1.0) * halfHeight
        : (1.0 - ndcY) * halfHeight;
    output.screenError = std::max(halfWidth * xError + kGamma2 * std::abs(output.x),
        halfHeight * yError + kGamma2 * std::abs(output.y)) + kDotUnderflowAllowance;
    output.depth = ndcDepth;
    output.depthError = zError;
    return std::isfinite(output.x) && std::isfinite(output.y) &&
        std::isfinite(output.screenError) && std::isfinite(output.depth) &&
        std::isfinite(output.depthError);
}

double Cross(double ax, double ay, double bx, double by)
{
    return ax * by - ay * bx;
}

double Orient2D(const ProjectedPoint& a, const ProjectedPoint& b, const ProjectedPoint& c)
{
    return Cross(b.x - a.x, b.y - a.y, c.x - a.x, c.y - a.y);
}

double Orient2DError(const ProjectedPoint& a, const ProjectedPoint& b, const ProjectedPoint& c,
    double ea, double eb, double ec)
{
    const double u = b.x - a.x;
    const double v = c.y - a.y;
    const double r = b.y - a.y;
    const double s = c.x - a.x;
    const double du = ea + eb;
    const double dv = ea + ec;
    const double dr = ea + eb;
    const double ds = ea + ec;
    double error = std::abs(u) * dv + std::abs(v) * du + du * dv +
        std::abs(r) * ds + std::abs(s) * dr + dr * ds;
    error += kGamma2 * (std::abs(u * v) + std::abs(r * s));
    return std::nextafter(error, std::numeric_limits<double>::infinity());
}

bool EligibleState(const Triangle& t)
{
    const bool frontKnown = t.frontWinding == FrontWinding::Clockwise ||
        t.frontWinding == FrontWinding::CounterClockwise;
    const bool cullKnown = t.cullMode == CullMode::None || t.cullMode == CullMode::Front ||
        t.cullMode == CullMode::Back;
    const bool depthKnown = t.depthCompare == DepthCompare::Less ||
        t.depthCompare == DepthCompare::LessEqual;
    return t.drawId != 0 && t.opaque && t.writesDepth && t.stencilGuaranteedPass &&
        frontKnown && cullKnown && depthKnown;
}

bool IsFrontFacing(double area, PixelAreaWinding areaWinding, FrontWinding front)
{
    if (area == 0.0)
        return false;
    if (areaWinding == PixelAreaWinding::Unknown)
        return false;
    const bool positiveIsClockwise = areaWinding == PixelAreaWinding::PositiveIsClockwise;
    const bool clockwise = (area > 0.0) == positiveIsClockwise;
    return clockwise == (front == FrontWinding::Clockwise);
}

bool StateMatches(const Triangle& a, const Triangle& b)
{
    return a.drawId == b.drawId && a.frontWinding == b.frontWinding &&
        a.cullMode == b.cullMode && a.depthCompare == b.depthCompare &&
        a.opaque == b.opaque && a.writesDepth == b.writesDepth &&
        a.stencilGuaranteedPass == b.stencilGuaranteedPass;
}

bool SameClipVertex(const ClipVertex& a, const ClipVertex& b)
{
    for (size_t i = 0; i != 4; ++i)
    {
        if (a.clip[i] != b.clip[i] || a.clipAbsDotTerms[i] != b.clipAbsDotTerms[i] ||
            a.upstreamAbsError[i] != b.upstreamAbsError[i])
            return false;
    }
    return true;
}

bool PrepareTriangle(const Triangle& t, uint32_t width, uint32_t height,
    ViewportYDirection yDirection, PixelAreaWinding areaWinding, Polygon& out)
{
    if (!EligibleState(t))
        return false;
    if (t.sourceVertexId[0] == t.sourceVertexId[1] ||
        t.sourceVertexId[1] == t.sourceVertexId[2] ||
        t.sourceVertexId[2] == t.sourceVertexId[0])
        return false;

    out = {};
    out.count = 3;
    for (uint32_t i = 0; i != 3; ++i)
        if (!Project(t.vertices[i], width, height, yDirection, out.points[i]))
            return false;

    const double orientation = Orient2D(out.points[0], out.points[1], out.points[2]);
    const double orientationError = Orient2DError(out.points[0], out.points[1], out.points[2],
        out.points[0].screenError, out.points[1].screenError, out.points[2].screenError);
    if (!std::isfinite(orientation) || !std::isfinite(orientationError) ||
        !(std::abs(orientation) > orientationError))
        return false;
    out.signedArea = orientation;

    const bool front = IsFrontFacing(out.signedArea, areaWinding, t.frontWinding);
    if ((t.cullMode == CullMode::Back && !front) || (t.cullMode == CullMode::Front && front))
        return false;

    out.farDepth = 0.0;
    for (uint32_t i = 0; i != out.count; ++i)
        out.farDepth = std::max(out.farDepth, out.points[i].depth + out.points[i].depthError);
    return std::isfinite(out.farDepth) && out.farDepth <= 1.0;
}

bool SameIdsAndClip(const Triangle& a, uint32_t ai, const Triangle& b, uint32_t bi)
{
    if (a.sourceVertexId[ai] != b.sourceVertexId[bi])
        return false;
    return SameClipVertex(a.vertices[ai], b.vertices[bi]);
}

bool BuildPair(const Triangle& a, const Polygon& pa, const Triangle& b, const Polygon& pb,
    Polygon& out)
{
    if (!StateMatches(a, b) || a.sourceTriangleId == b.sourceTriangleId)
        return false;

    uint32_t sharedA[2]{};
    uint32_t sharedCount = 0;
    uint32_t onlyA = 0;
    uint32_t onlyB = 0;
    for (uint32_t i = 0; i != 3; ++i)
    {
        bool found = false;
        for (uint32_t j = 0; j != 3; ++j)
        {
            if (a.sourceVertexId[i] == b.sourceVertexId[j])
            {
                if (sharedCount >= 2 || !SameIdsAndClip(a, i, b, j))
                    return false;
                sharedA[sharedCount] = i;
                ++sharedCount;
                found = true;
                break;
            }
        }
        if (!found)
            onlyA = i;
    }
    if (sharedCount != 2)
        return false;
    for (uint32_t j = 0; j != 3; ++j)
        if (b.sourceVertexId[j] != a.sourceVertexId[sharedA[0]] &&
            b.sourceVertexId[j] != a.sourceVertexId[sharedA[1]])
            onlyB = j;

    // Find the directed shared edge in each input triangle's original order.
    auto directedEdge = [](const Triangle& t, uint32_t id0, uint32_t id1, uint32_t& start, uint32_t& end) {
        for (uint32_t i = 0; i != 3; ++i)
        {
            const uint32_t j = (i + 1) % 3;
            if (t.sourceVertexId[i] == id0 && t.sourceVertexId[j] == id1)
            {
                start = i;
                end = j;
                return true;
            }
        }
        return false;
    };
    uint32_t aStart = 0, aEnd = 0, bStart = 0, bEnd = 0;
    const uint32_t id0 = a.sourceVertexId[sharedA[0]];
    const uint32_t id1 = a.sourceVertexId[sharedA[1]];
    if (!directedEdge(a, id0, id1, aStart, aEnd) && !directedEdge(a, id1, id0, aStart, aEnd))
        return false;
    const uint32_t startId = a.sourceVertexId[aStart];
    const uint32_t endId = a.sourceVertexId[aEnd];
    if (!directedEdge(b, endId, startId, bStart, bEnd))
        return false;
    if (b.sourceVertexId[bStart] != endId || b.sourceVertexId[bEnd] != startId)
        return false;

    const ProjectedPoint& edgeA = pa.points[aStart];
    const ProjectedPoint& edgeB = pa.points[aEnd];
    const ProjectedPoint& oppositeA = pa.points[onlyA];
    const ProjectedPoint& oppositeB = pb.points[onlyB];
    const double sideA = Orient2D(edgeA, edgeB, oppositeA);
    const double sideB = Orient2D(edgeA, edgeB, oppositeB);
    const double sideErrorA = Orient2DError(edgeA, edgeB, oppositeA,
        edgeA.screenError, edgeB.screenError, oppositeA.screenError);
    const double sideErrorB = Orient2DError(edgeA, edgeB, oppositeB,
        edgeA.screenError, edgeB.screenError, oppositeB.screenError);
    if (!std::isfinite(sideA) || !std::isfinite(sideB) ||
        !std::isfinite(sideErrorA) || !std::isfinite(sideErrorB) ||
        !(std::abs(sideA) > sideErrorA) || !(std::abs(sideB) > sideErrorB) ||
        ((sideA < 0.0) == (sideB < 0.0)))
        return false;

    // Boundary order around the union. The shared diagonal is omitted.
    out = {};
    out.count = 4;
    out.points[0] = pa.points[aStart];
    out.points[1] = pb.points[onlyB];
    out.points[2] = pa.points[aEnd];
    out.points[3] = pa.points[onlyA];
    const double quadTurn0 = Orient2D(out.points[0], out.points[1], out.points[2]);
    const double quadTurn1 = Orient2D(out.points[1], out.points[2], out.points[3]);
    const double quadTurn2 = Orient2D(out.points[2], out.points[3], out.points[0]);
    const double quadTurn3 = Orient2D(out.points[3], out.points[0], out.points[1]);
    const double quadError0 = Orient2DError(out.points[0], out.points[1], out.points[2],
        out.points[0].screenError, out.points[1].screenError, out.points[2].screenError);
    const double quadError1 = Orient2DError(out.points[1], out.points[2], out.points[3],
        out.points[1].screenError, out.points[2].screenError, out.points[3].screenError);
    const double quadError2 = Orient2DError(out.points[2], out.points[3], out.points[0],
        out.points[2].screenError, out.points[3].screenError, out.points[0].screenError);
    const double quadError3 = Orient2DError(out.points[3], out.points[0], out.points[1],
        out.points[3].screenError, out.points[0].screenError, out.points[1].screenError);
    const double turns[4] = {quadTurn0, quadTurn1, quadTurn2, quadTurn3};
    const double errors[4] = {quadError0, quadError1, quadError2, quadError3};
    for (size_t i = 0; i != 4; ++i)
        if (!std::isfinite(turns[i]) || !std::isfinite(errors[i]) ||
            !((turns[i] > 0.0) == (turns[0] > 0.0)) || !(std::abs(turns[i]) > errors[i]))
            return false;
    out.signedArea = quadTurn0;
    // Both original triangles already passed the actual draw's cull-state
    // test in PrepareTriangle. The convex union must not impose a new winding
    // restriction when the admitted state is CullNone or CullFront.
    out.farDepth = 0.0;
    for (uint32_t i = 0; i != out.count; ++i)
        out.farDepth = std::max(out.farDepth, out.points[i].depth + out.points[i].depthError);
    return std::isfinite(out.farDepth) && out.farDepth <= 1.0;
}

bool CellInside(const Polygon& polygon, double x0, double y0, double x1, double y1,
    double edgeGuard)
{
    const double orientation = polygon.signedArea > 0.0 ? 1.0 : -1.0;
    const double cx[4] = {x0, x1, x1, x0};
    const double cy[4] = {y0, y0, y1, y1};
    for (uint32_t edge = 0; edge != polygon.count; ++edge)
    {
        const ProjectedPoint& a = polygon.points[edge];
        const ProjectedPoint& b = polygon.points[(edge + 1) % polygon.count];
        for (uint32_t corner = 0; corner != 4; ++corner)
        {
            ProjectedPoint cellCorner{};
            cellCorner.x = cx[corner];
            cellCorner.y = cy[corner];
            cellCorner.screenError = edgeGuard;
            const double cross = Orient2D(a, b, cellCorner);
            const double crossError = Orient2DError(a, b, cellCorner,
                a.screenError + edgeGuard, b.screenError + edgeGuard, edgeGuard);
            if (!std::isfinite(cross) || !std::isfinite(crossError) || !(orientation * cross > crossError))
                return false;
        }
    }
    return true;
}

float RoundUpFloat(double value)
{
    float result = static_cast<float>(value);
    if (static_cast<double>(result) < value)
        result = std::nextafter(result, std::numeric_limits<float>::infinity());
    return result;
}

float RoundDownFloat(double value)
{
    float result = static_cast<float>(value);
    if (static_cast<double>(result) > value)
        result = std::nextafter(result, -std::numeric_limits<float>::infinity());
    return result;
}
} // namespace

void CoverageGrid::Reset()
{
    m_coveredRows.fill(0);
    m_farthestDepth.fill(1.0f);
    m_viewportWidth = 0;
    m_viewportHeight = 0;
    m_observationScopeId = 0;
    m_viewportY = ViewportYDirection::Unknown;
    m_pixelAreaWinding = PixelAreaWinding::Unknown;
    m_rasterEdgeGuardPixels = 0.0f;
    m_built = false;
}

bool CoverageGrid::Build(const BuildInput& input, BuildStats* statsOut)
{
    BuildStats local{};
    BuildStats& stats = statsOut ? *statsOut : local;
    stats = {};
    Reset();

    if (input.observationScopeId == 0 || (input.triangleCount != 0 && input.triangles == nullptr) ||
        input.viewportWidth == 0 || input.viewportHeight == 0 ||
        (input.viewportY != ViewportYDirection::ClipPositiveYMapsToHigherPixelY &&
            input.viewportY != ViewportYDirection::ClipPositiveYMapsToLowerPixelY) ||
        (input.pixelAreaWinding != PixelAreaWinding::PositiveIsClockwise &&
            input.pixelAreaWinding != PixelAreaWinding::PositiveIsCounterClockwise) ||
        !std::isfinite(input.rasterEdgeGuardPixels) || input.rasterEdgeGuardPixels <= 0.0f)
    {
        stats.invalidBuildInput = true;
        return false;
    }

    m_viewportWidth = input.viewportWidth;
    m_viewportHeight = input.viewportHeight;
    m_observationScopeId = input.observationScopeId;
    m_viewportY = input.viewportY;
    m_pixelAreaWinding = input.pixelAreaWinding;
    m_rasterEdgeGuardPixels = input.rasterEdgeGuardPixels;
    m_built = true;

    const size_t count = std::min<size_t>(input.triangleCount, kMaxTriangles);
    stats.triangleCapReached = input.triangleCount > kMaxTriangles;
    std::array<bool, kMaxTriangles> consumed{};
    uint64_t cellTests = 0;

    auto markPolygon = [&](const Polygon& polygon) {
        double minX = std::numeric_limits<double>::infinity();
        double minY = std::numeric_limits<double>::infinity();
        double maxX = -std::numeric_limits<double>::infinity();
        double maxY = -std::numeric_limits<double>::infinity();
        double maxPointError = 0.0;
        for (uint32_t i = 0; i != polygon.count; ++i)
        {
            const ProjectedPoint& p = polygon.points[i];
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
            maxPointError = std::max(maxPointError, p.screenError);
        }
        const double expansion = maxPointError + input.rasterEdgeGuardPixels;
        minX -= expansion;
        minY -= expansion;
        maxX += expansion;
        maxY += expansion;
        if (maxX < 0.0 || maxY < 0.0 || minX > m_viewportWidth || minY > m_viewportHeight)
            return;

        const double cellWidth = static_cast<double>(m_viewportWidth) / kGridWidth;
        const double cellHeight = static_cast<double>(m_viewportHeight) / kGridHeight;
        const int x0 = std::clamp(static_cast<int>(std::floor(std::max(0.0, minX) / cellWidth)), 0, int(kGridWidth) - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(std::max(0.0, minY) / cellHeight)), 0, int(kGridHeight) - 1);
        const int x1 = std::clamp(static_cast<int>(std::floor(std::min<double>(m_viewportWidth, maxX) / cellWidth)), 0, int(kGridWidth) - 1);
        const int y1 = std::clamp(static_cast<int>(std::floor(std::min<double>(m_viewportHeight, maxY) / cellHeight)), 0, int(kGridHeight) - 1);
        const float farDepth = RoundUpFloat(polygon.farDepth);
        if (!(farDepth <= 1.0f))
            return;

        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                if (cellTests == kMaxPrimitiveCellTests)
                {
                    stats.cellTestCapReached = true;
                    return;
                }
                ++cellTests;
                ++stats.primitiveCellTests;
                const double px0 = x * cellWidth;
                const double py0 = y * cellHeight;
                if (!CellInside(polygon, px0, py0, px0 + cellWidth, py0 + cellHeight,
                        input.rasterEdgeGuardPixels))
                    continue;

                const uint64_t mask = uint64_t(1) << x;
                const bool wasCovered = (m_coveredRows[size_t(y)] & mask) != 0;
                const size_t index = size_t(y) * kGridWidth + size_t(x);
                // Among complete-cell covers, retain the nearest proven cover's
                // conservative far bound (smaller normal depth is closer).
                if (!wasCovered || farDepth < m_farthestDepth[index])
                    m_farthestDepth[index] = farDepth;
                if (!wasCovered)
                {
                    m_coveredRows[size_t(y)] |= mask;
                    ++stats.cellsMarked;
                }
            }
        }
    };

    for (size_t i = 0; i != count; ++i)
    {
        if (consumed[i])
            continue;
        const Triangle& a = input.triangles[i];
        Polygon pa{};
        const bool validA = PrepareTriangle(a, m_viewportWidth, m_viewportHeight,
            m_viewportY, m_pixelAreaWinding, pa);

        bool paired = false;
        const uint32_t mate = a.optionalMateTriangle;
        if (validA && mate != kNoMate && mate > i && mate < count &&
            input.triangles[mate].optionalMateTriangle == i && !consumed[mate])
        {
            Polygon pb{};
            if (PrepareTriangle(input.triangles[mate], m_viewportWidth, m_viewportHeight,
                    m_viewportY, m_pixelAreaWinding, pb))
            {
                Polygon unionPolygon{};
                if (BuildPair(a, pa, input.triangles[mate], pb, unionPolygon))
                {
                    markPolygon(unionPolygon);
                    consumed[mate] = true;
                    paired = true;
                    ++stats.pairedUnionsAccepted;
                    stats.trianglesVisited += 2;
                    if (stats.cellTestCapReached)
                        break;
                }
                else
                {
                    ++stats.pairedUnionsRejected;
                }
            }
            else
            {
                ++stats.pairedUnionsRejected;
            }
        }
        if (paired)
            continue;

        if (validA)
            markPolygon(pa);
        else
            ++stats.trianglesSkipped;
        ++stats.trianglesVisited;

        if (stats.cellTestCapReached)
            break;
    }
    return true;
}

QueryResult CoverageGrid::QueryPaddedAabb(const ClipVertex corners[8],
    uint32_t viewportWidth, uint32_t viewportHeight, ViewportYDirection viewportY,
    float rasterEdgeGuardPixels, uint64_t observationScopeId, QueryStats* statsOut) const
{
    QueryStats local{};
    QueryStats& stats = statsOut ? *statsOut : local;
    stats = {};
    if (!m_built || corners == nullptr || observationScopeId == 0 ||
        observationScopeId != m_observationScopeId || viewportWidth != m_viewportWidth ||
        viewportHeight != m_viewportHeight || viewportY != m_viewportY ||
        rasterEdgeGuardPixels != m_rasterEdgeGuardPixels ||
        !std::isfinite(rasterEdgeGuardPixels) || rasterEdgeGuardPixels <= 0.0f)
    {
        stats.invalidInput = true;
        return QueryResult::VisibleOrUnknown;
    }

    ProjectedPoint projected[8]{};
    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double nearestDepth = 1.0;
    double maxPointError = 0.0;
    for (size_t i = 0; i != 8; ++i)
    {
        if (!Project(corners[i], m_viewportWidth, m_viewportHeight, m_viewportY, projected[i]))
        {
            stats.invalidInput = true;
            return QueryResult::VisibleOrUnknown;
        }
        minX = std::min(minX, projected[i].x);
        minY = std::min(minY, projected[i].y);
        maxX = std::max(maxX, projected[i].x);
        maxY = std::max(maxY, projected[i].y);
        nearestDepth = std::min(nearestDepth, projected[i].depth - projected[i].depthError);
        maxPointError = std::max(maxPointError, projected[i].screenError);
    }
    if (!std::isfinite(nearestDepth) || nearestDepth <= 0.0 || nearestDepth >= 1.0)
    {
        stats.invalidInput = true;
        return QueryResult::VisibleOrUnknown;
    }
    const float nearestDepthDown = RoundDownFloat(nearestDepth);
    const double expansion = maxPointError + rasterEdgeGuardPixels;
    minX -= expansion;
    minY -= expansion;
    maxX += expansion;
    maxY += expansion;
    if (maxX <= 0.0 || maxY <= 0.0 || minX >= m_viewportWidth || minY >= m_viewportHeight)
        return QueryResult::VisibleOrUnknown;

    const double cellWidth = static_cast<double>(m_viewportWidth) / kGridWidth;
    const double cellHeight = static_cast<double>(m_viewportHeight) / kGridHeight;
    const int x0 = std::clamp(static_cast<int>(std::floor(std::max(0.0, minX) / cellWidth)), 0, int(kGridWidth) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(std::max(0.0, minY) / cellHeight)), 0, int(kGridHeight) - 1);
    const int x1 = std::clamp(static_cast<int>(std::floor(std::min<double>(m_viewportWidth, maxX) / cellWidth)), 0, int(kGridWidth) - 1);
    const int y1 = std::clamp(static_cast<int>(std::floor(std::min<double>(m_viewportHeight, maxY) / cellHeight)), 0, int(kGridHeight) - 1);

    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            if (stats.cellsVisited == kMaxQueryCellTests)
            {
                stats.cellTestCapReached = true;
                return QueryResult::VisibleOrUnknown;
            }
            ++stats.cellsVisited;
            if ((m_coveredRows[size_t(y)] & (uint64_t(1) << x)) == 0)
                return QueryResult::VisibleOrUnknown;
            const float farDepth = m_farthestDepth[size_t(y) * kGridWidth + size_t(x)];
            if (!(farDepth < nearestDepthDown))
                return QueryResult::VisibleOrUnknown;
        }
    }
    return QueryResult::ConservativelyBehindCoverage;
}
} // namespace R4ActiveOcclusion
