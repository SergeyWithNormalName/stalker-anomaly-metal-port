// Standalone scratch CPU safety oracle for R4ActiveOcclusion::CoverageGrid.
// Deliberately independent reference path: full-resolution double-precision
// barycentric raster over original input triangles, never helper cell data.
#include "../R4ActiveOcclusionCoverage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace R4ActiveOcclusion;
namespace
{
constexpr uint32_t W = 128;
constexpr uint32_t H = 128;
constexpr float kEdgeGuard = 0.125f;
// Fixture pixels use upper-left origin (x right, numeric y down). With this
// explicit direct clip-Y mapping, positive pixel-space area is visually CW.
// This is a logical oracle convention, not a claim about native R4/Metal state.
constexpr ViewportYDirection kY = ViewportYDirection::ClipPositiveYMapsToHigherPixelY;
constexpr uint64_t kScope = 0x4f5241434c450001ull;
constexpr uint64_t kAutoScope = ~uint64_t{0};
int failures = 0;
int hiddenCount = 0;
int visibleCount = 0;

struct P2 { double x, y; };
struct P3 { double x, y, z; };
struct Rect { double x0, y0, x1, y1; };

void expect(bool ok, const char* label)
{
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}

ClipVertex vertex(double px, double py, double depth, uint32_t width = W, uint32_t height = H)
{
    ClipVertex v{};
    const double x = 2.0 * px / width - 1.0;
    const double y = 2.0 * py / height - 1.0; // +clipY maps to +pixelY in this fixture.
    v.clip[0] = static_cast<float>(x);
    v.clip[1] = static_cast<float>(y);
    v.clip[2] = static_cast<float>(depth);
    v.clip[3] = 1.0f;
    // Identity WVP: these are exact upper bounds for sum(abs(row terms)).
    for (int i = 0; i < 4; ++i)
    {
        v.clipAbsDotTerms[i] = std::abs(v.clip[i]);
        v.upstreamAbsError[i] = 0.0f;
    }
    return v;
}

Triangle tri(P2 a, P2 b, P2 c, float z, uint32_t sourceId = 0, uint32_t width=W, uint32_t height=H)
{
    Triangle t{};
    t.drawId = 1;
    t.sourceTriangleId = sourceId;
    t.sourceVertexId[0] = sourceId * 3 + 1;
    t.sourceVertexId[1] = sourceId * 3 + 2;
    t.sourceVertexId[2] = sourceId * 3 + 3;
    t.vertices[0] = vertex(a.x, a.y, z, width, height);
    t.vertices[1] = vertex(b.x, b.y, z, width, height);
    t.vertices[2] = vertex(c.x, c.y, z, width, height);
    t.frontWinding = FrontWinding::Clockwise;
    t.cullMode = CullMode::None;
    t.depthCompare = DepthCompare::Less;
    t.opaque = true;
    t.writesDepth = true;
    t.stencilGuaranteedPass = true;
    return t;
}

void addRect(std::vector<Triangle>& out, Rect r, float z, uint32_t idBase, uint32_t width=W, uint32_t height=H)
{
    // Quad split along a shared diagonal; IDs/clip intervals describe that seam.
    const P2 p[4] = {{r.x0,r.y0},{r.x1,r.y0},{r.x1,r.y1},{r.x0,r.y1}};
    Triangle a = tri(p[0], p[1], p[2], z, idBase, width, height);
    Triangle b = tri(p[0], p[2], p[3], z, idBase + 1, width, height);
    const uint32_t vertexBase = idBase * 4u + 1u;
    a.sourceVertexId[0] = vertexBase; a.sourceVertexId[1] = vertexBase + 1u; a.sourceVertexId[2] = vertexBase + 2u;
    b.sourceVertexId[0] = vertexBase; b.sourceVertexId[1] = vertexBase + 2u; b.sourceVertexId[2] = vertexBase + 3u;
    const uint32_t first = static_cast<uint32_t>(out.size());
    a.optionalMateTriangle = first + 1u;
    b.optionalMateTriangle = first;
    out.push_back(a); out.push_back(b);
}

P3 project(const ClipVertex& v, uint32_t width, uint32_t height, ViewportYDirection ydir)
{
    const double x = static_cast<double>(v.clip[0]) / static_cast<double>(v.clip[3]);
    const double y = static_cast<double>(v.clip[1]) / static_cast<double>(v.clip[3]);
    const double px = (x + 1.0) * 0.5 * width;
    const double yn = (y + 1.0) * 0.5;
    const double py = ydir == ViewportYDirection::ClipPositiveYMapsToHigherPixelY
        ? yn * height : (1.0 - yn) * height;
    return {px, py, double(v.clip[2]) / v.clip[3]};
}

double edge(P2 a, P2 b, P2 p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Independent barycentric top-left pixel-center raster reference. In this
// upper-left-origin, y-down fixture, positive signed area is visually clockwise;
// the rule includes its top/left edges and is independent of the helper code.
bool topLeft(P2 a, P2 b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return dy < 0.0 || (dy == 0.0 && dx > 0.0);
}

bool coversTriangle(const Triangle& t, P2 p, uint32_t width, uint32_t height,
    ViewportYDirection ydir, PixelAreaWinding areaWinding, double& depth)
{
    if (!t.opaque || !t.writesDepth || !t.stencilGuaranteedPass ||
        t.cullMode == CullMode::Unknown || t.frontWinding == FrontWinding::Unknown ||
        (t.depthCompare != DepthCompare::Less && t.depthCompare != DepthCompare::LessEqual))
        return false;
    P3 q[3];
    for (int i = 0; i < 3; ++i)
    {
        for (float x : t.vertices[i].clip) if (!std::isfinite(x)) return false;
        if (!(t.vertices[i].clip[3] > 0.0f)) return false;
        q[i] = project(t.vertices[i], width, height, ydir);
        if (!std::isfinite(q[i].x) || !std::isfinite(q[i].y) ||
            !std::isfinite(q[i].z) || q[i].z < 0.0 || q[i].z > 1.0)
            return false;
    }
    P2 a{q[0].x,q[0].y}, b{q[1].x,q[1].y}, c{q[2].x,q[2].y};
    double area = edge(a,b,c);
    if (area == 0.0) return false;
    const bool positive = area > 0.0;
    const bool front = areaWinding == PixelAreaWinding::PositiveIsClockwise
        ? (positive == (t.frontWinding == FrontWinding::Clockwise))
        : areaWinding == PixelAreaWinding::PositiveIsCounterClockwise
            ? (positive == (t.frontWinding == FrontWinding::CounterClockwise)) : false;
    if (t.cullMode == CullMode::Unknown || t.frontWinding == FrontWinding::Unknown ||
        areaWinding == PixelAreaWinding::Unknown) return false;
    if ((t.cullMode == CullMode::Back && !front) || (t.cullMode == CullMode::Front && front)) return false;
    if (area < 0.0)
    {
        std::swap(q[1], q[2]);
        std::swap(b, c);
        area = -area;
    }
    const double e0 = edge(a,b,p), e1 = edge(b,c,p), e2 = edge(c,a,p);
    auto accepted = [](double e, P2 u, P2 v) { return e > 0.0 || (e == 0.0 && topLeft(u,v)); };
    if (!accepted(e0,a,b) || !accepted(e1,b,c) || !accepted(e2,c,a)) return false;
    const double l0 = e1 / area, l1 = e2 / area, l2 = e0 / area;
    depth = l0*q[0].z + l1*q[1].z + l2*q[2].z;
    return true;
}

struct RefStats { uint32_t samples=0, covered=0; double maxFrontDepth=0.0; uint32_t subSamples=0, subCovered=0; double maxSubFrontDepth=0.0; };
RefStats reference(const std::vector<Triangle>& triangles, Rect rect, float queryNear,
    bool includeSubpixels, PixelAreaWinding areaWinding, uint32_t width, uint32_t height)
{
    RefStats r{};
    for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x)
    {
        P2 p{double(x)+0.5,double(y)+0.5};
        if (p.x < rect.x0 || p.x > rect.x1 || p.y < rect.y0 || p.y > rect.y1) continue;
        ++r.samples;
        double front = std::numeric_limits<double>::infinity();
        for (const auto& t : triangles)
        {
            double z;
            if (coversTriangle(t,p,width,height,kY,areaWinding,z)) front = std::min(front,z);
        }
        if (std::isfinite(front)) { ++r.covered; r.maxFrontDepth = std::max(r.maxFrontDepth,front); }
    }
    if (includeSubpixels)
    {
        for (uint32_t y=0; y<height; ++y) for (uint32_t x=0; x<width; ++x)
        for (int sy=0; sy<4; ++sy) for (int sx=0; sx<4; ++sx)
        {
            P2 p{double(x)+(sx+0.5)/4.0,double(y)+(sy+0.5)/4.0};
            if (p.x < rect.x0 || p.x > rect.x1 || p.y < rect.y0 || p.y > rect.y1) continue;
            ++r.subSamples;
            double front = std::numeric_limits<double>::infinity();
            for (const auto& t : triangles)
            {
                double z;
                if (coversTriangle(t,p,width,height,kY,areaWinding,z)) front = std::min(front,z);
            }
            if (std::isfinite(front))
            {
                ++r.subCovered;
                r.maxSubFrontDepth=std::max(r.maxSubFrontDepth,front);
            }
        }
    }
    (void)queryNear;
    return r;
}

std::array<ClipVertex,8> queryCorners(Rect r, float nearZ, float farZ, uint32_t width=W, uint32_t height=H)
{
    std::array<ClipVertex,8> v{};
    int i=0;
    for (float z : {nearZ,farZ})
        for (double y : {r.y0,r.y1})
            for (double x : {r.x0,r.x1}) v[i++]=vertex(x,y,z,width,height);
    return v;
}

struct CaseResult { QueryResult result; BuildStats build; QueryStats query; RefStats ref; };
CaseResult runCase(const char* name, const std::vector<Triangle>& triangles, Rect rect,
    float queryNear=0.8f, float queryFar=0.9f, bool subpixels=false,
    uint64_t buildScope=kAutoScope, uint64_t queryScope=kAutoScope,
    uint32_t queryWidth=W, uint32_t queryHeight=H, ViewportYDirection queryY=kY,
    PixelAreaWinding areaWinding=PixelAreaWinding::PositiveIsClockwise,
    uint32_t buildWidth=W, uint32_t buildHeight=H)
{
    static uint64_t nextScope=0x100000000ull;
    if (buildScope==kAutoScope) buildScope=nextScope++;
    if (queryScope==kAutoScope) queryScope=buildScope;
    CoverageGrid grid;
    BuildInput input{};
    input.observationScopeId = buildScope;
    input.triangles = triangles.empty() ? nullptr : triangles.data();
    input.triangleCount = triangles.size();
    input.viewportWidth = buildWidth; input.viewportHeight = buildHeight; input.viewportY = kY;
    input.pixelAreaWinding = areaWinding;
    input.rasterEdgeGuardPixels = kEdgeGuard;
    CaseResult r{};
    const bool built = grid.Build(input,&r.build);
    auto corners=queryCorners(rect,queryNear,queryFar,buildWidth,buildHeight);
    r.result = grid.QueryPaddedAabb(corners.data(),queryWidth,queryHeight,queryY,kEdgeGuard,queryScope,&r.query);
    r.ref = reference(triangles,rect,queryNear,subpixels,areaWinding,buildWidth,buildHeight);
    const bool hidden = r.result == QueryResult::ConservativelyBehindCoverage;
    expect(r.query.cellsVisited<=kMaxQueryCellTests,"query traversal never exceeds fixed unique-cell budget");
    if (hidden) ++hiddenCount; else ++visibleCount;
    const bool allPixelsCovered = r.ref.samples > 0 && r.ref.covered == r.ref.samples;
    const bool allSubCovered = !subpixels || (r.ref.subSamples > 0 && r.ref.subCovered == r.ref.subSamples);
    const bool subDepthSafe = !subpixels || r.ref.maxSubFrontDepth < queryNear;
    const bool depthSafe = allPixelsCovered && r.ref.maxFrontDepth < queryNear;
    if (hidden && (!built || !allPixelsCovered || !allSubCovered || !depthSafe || !subDepthSafe))
    {
        ++failures;
        std::fprintf(stderr,"FALSE-HIDDEN %s covered=%u/%u sub=%u/%u front=%.9g subfront=%.9g qnear=%.9g\n",
            name,r.ref.covered,r.ref.samples,r.ref.subCovered,r.ref.subSamples,r.ref.maxFrontDepth,r.ref.maxSubFrontDepth,queryNear);
    }
    std::printf("case=%s result=%s pixels=%u/%u subpixels=%u/%u front=%.9g subfront=%.9g qnear=%.9g triangles=%u skipped=%u build_cap=%d cell_cap=%d query_cap=%d invalid=%d\n",
        name,hidden?"hidden":"visible",r.ref.covered,r.ref.samples,r.ref.subCovered,r.ref.subSamples,
        r.ref.maxFrontDepth,r.ref.maxSubFrontDepth,queryNear,r.build.trianglesVisited,r.build.trianglesSkipped,
        int(r.build.triangleCapReached),int(r.build.cellTestCapReached),int(r.query.cellTestCapReached),
        int(r.query.invalidInput));
    return r;
}

void testBasicAndDepth()
{
    std::vector<Triangle> quad; addRect(quad,{8,8,120,120},0.2f,1);
    auto r=runCase("covered-interior-paired-quad",quad,{50,50,74,74});
    expect(r.result==QueryResult::ConservativelyBehindCoverage,"interior must produce a useful positive");
    std::vector<Triangle> single{tri({8,8},{120,8},{64,120},0.2f,90)};
    auto one=runCase("covered-interior-unpaired-triangle",single,{55,24,73,40});
    expect(one.result==QueryResult::ConservativelyBehindCoverage,"unpaired triangle must independently produce a useful positive");
    runCase("foreground-object",quad,{50,50,74,74},0.1f,0.15f);
    runCase("depth-equality",quad,{50,50,74,74},0.2f,0.25f);
    std::vector<Triangle> sloped;
    Triangle top=tri({8,8},{120,8},{120,120},0.2f,1);
    Triangle bottom=tri({8,8},{120,120},{8,120},0.2f,2);
    auto setZ=[](ClipVertex& v,float z) { v.clip[2]=z; v.clipAbsDotTerms[2]=std::abs(z); };
    setZ(top.vertices[0],0.1f); setZ(top.vertices[1],0.9f); setZ(top.vertices[2],0.9f);
    setZ(bottom.vertices[0],0.1f); setZ(bottom.vertices[1],0.9f); setZ(bottom.vertices[2],0.1f);
    sloped.push_back(top); sloped.push_back(bottom);
    runCase("depth-crossing-inside-query",sloped,{44,44,82,82},0.5f,0.9f);
}

void testSeamsAndNegativeSpace()
{
    std::vector<Triangle> quad; addRect(quad,{8,8,120,120},0.2f,1);
    // Reciprocal mate declaration is optional; reference always keeps originals.
    quad[0].optionalMateTriangle=1; quad[1].optionalMateTriangle=0;
    auto seam=runCase("paired-shared-diagonal",quad,{50,50,74,74},0.8f,0.9f,true);
    expect(seam.result==QueryResult::ConservativelyBehindCoverage,"shared quad interior may remain useful");
    expect(seam.build.pairedUnionsAccepted>0,"exact reciprocal adjacent pair must exercise accepted-union path");
    std::vector<Triangle> reversedOrder{quad[1],quad[0]};
    reversedOrder[0].optionalMateTriangle=1; reversedOrder[1].optionalMateTriangle=0;
    auto reversedPair=runCase("paired-submission-order-reversed",reversedOrder,{50,50,74,74},0.8f,0.9f,true);
    expect(reversedPair.result==QueryResult::ConservativelyBehindCoverage &&
        reversedPair.build.pairedUnionsAccepted>0,"pair result must not depend on submission order");
    auto reversedWinding=quad;
    for (auto& t : reversedWinding)
    {
        std::swap(t.vertices[1],t.vertices[2]);
        std::swap(t.sourceVertexId[1],t.sourceVertexId[2]);
    }
    auto windingPair=runCase("paired-winding-reversed",reversedWinding,{50,50,74,74},0.8f,0.9f,true);
    expect(windingPair.result==QueryResult::ConservativelyBehindCoverage &&
        windingPair.build.pairedUnionsAccepted>0,"pair result must accept reversed input winding when culling is disabled");
    std::vector<Triangle> frame;
    addRect(frame,{16,16,112,34},0.2f,10); addRect(frame,{16,94,112,112},0.2f,20);
    addRect(frame,{16,34,34,94},0.2f,30); addRect(frame,{94,34,112,94},0.2f,40);
    runCase("window-hole",frame,{54,54,72,72},0.8f,0.9f,true);
    std::vector<Triangle> gap;
    addRect(gap,{8,8,63.75,120},0.2f,50); addRect(gap,{64.25,8,120,120},0.2f,60);
    runCase("subpixel-seam-gap",gap,{60,52,68,76},0.8f,0.9f,true);
    std::vector<Triangle> oneSide; oneSide.push_back(tri({8,8},{120,8},{8,120},0.2f,70));
    runCase("partial-cell-edge",oneSide,{70,70,96,96},0.8f,0.9f,true);
}

void testEligibilityAndScope()
{
    std::vector<Triangle> base; addRect(base,{8,8,120,120},0.2f,1);
    auto invalid=base; invalid[0].opaque=false; invalid[1].opaque=false;
    runCase("nonopaque-ineligible",invalid,{50,50,74,74});
    invalid=base; invalid[0].stencilGuaranteedPass=false; invalid[1].stencilGuaranteedPass=false;
    runCase("unknown-stencil-ineligible",invalid,{50,50,74,74});

    auto cull=base;
    for (auto& t : cull) { t.frontWinding=FrontWinding::Clockwise; t.cullMode=CullMode::Back; }
    auto cw=runCase("positive-screen-area-is-clockwise-front",cull,{50,50,74,74},0.8f,0.9f,false,
        kAutoScope,kAutoScope,W,H,kY,PixelAreaWinding::PositiveIsClockwise);
    expect(cw.result==QueryResult::ConservativelyBehindCoverage,"declared positive clockwise must admit CW back-cull face");
    for (auto& t : cull) t.frontWinding=FrontWinding::CounterClockwise;
    auto ccwRejected=runCase("wrong-front-winding-is-culled",cull,{50,50,74,74},0.8f,0.9f,false,
        kAutoScope,kAutoScope,W,H,kY,PixelAreaWinding::PositiveIsClockwise);
    expect(ccwRejected.result==QueryResult::VisibleOrUnknown,"wrong front winding must not occlude under back cull");
    for (auto& t : cull) t.frontWinding=FrontWinding::CounterClockwise;
    auto opposite=runCase("positive-screen-area-is-ccw-front",cull,{50,50,74,74},0.8f,0.9f,false,
        kAutoScope,kAutoScope,W,H,kY,PixelAreaWinding::PositiveIsCounterClockwise);
    expect(opposite.result==QueryResult::ConservativelyBehindCoverage,"opposite explicit area convention must be honored");
    auto frontCull=base;
    for (auto& t : frontCull) { t.frontWinding=FrontWinding::Clockwise; t.cullMode=CullMode::Front; }
    auto rejectedFront=runCase("front-cull-rejects-frontface",frontCull,{50,50,74,74},0.8f,0.9f,false,
        kAutoScope,kAutoScope,W,H,kY,PixelAreaWinding::PositiveIsClockwise);
    expect(rejectedFront.result==QueryResult::VisibleOrUnknown,"front-cull front-facing geometry is not an occluder");
    auto reversed=base;
    for (auto& t : reversed)
    {
        std::swap(t.vertices[1],t.vertices[2]);
        std::swap(t.sourceVertexId[1],t.sourceVertexId[2]);
        t.frontWinding=FrontWinding::CounterClockwise;
        t.cullMode=CullMode::Back;
    }
    auto negativeAccepted=runCase("negative-area-ccw-front",reversed,{50,50,74,74},0.8f,0.9f,false,
        kAutoScope,kAutoScope,W,H,kY,PixelAreaWinding::PositiveIsClockwise);
    expect(negativeAccepted.result==QueryResult::ConservativelyBehindCoverage,"negative area CCW front face should pass back culling");
    runCase("unknown-area-winding-fails-open",base,{50,50,74,74},0.8f,0.9f,false,
        kScope,kScope,W,H,kY,PixelAreaWinding::Unknown);
    runCase("scope-mismatch",base,{50,50,74,74},0.8f,0.9f,false,kScope,kScope+1);
    runCase("viewport-mismatch",base,{50,50,74,74},0.8f,0.9f,false,kScope,kScope,W/2,H/2);
    runCase("query-outside-viewport",base,{-20,40,-4,56});
    std::vector<Triangle> translated; addRect(translated,{14,14,126,126},0.2f,80);
    runCase("translated-view-rebuilt",translated,{54,54,74,74});
    constexpr uint32_t RW=192, RH=96;
    std::vector<Triangle> resized; addRect(resized,{10,8,182,88},0.2f,90,RW,RH);
    runCase("resized-view-rebuilt",resized,{70,34,98,62},0.8f,0.9f,false,
        kAutoScope,kAutoScope,RW,RH,kY,PixelAreaWinding::PositiveIsClockwise,RW,RH);
    runCase("viewport-y-mismatch",base,{50,50,74,74},0.8f,0.9f,false,kScope,kScope,W,H,
        ViewportYDirection::ClipPositiveYMapsToLowerPixelY);
    runCase("zero-build-scope",base,{50,50,74,74},0.8f,0.9f,false,0,0);

    auto bad=base;
    bad[0].vertices[0].clip[3]=0.0f;
    runCase("zero-w-vertex",bad,{50,50,74,74});
    bad=base;
    bad[0].vertices[0].clip[2]=-0.1f;
    runCase("near-plane-invalid-vertex",bad,{50,50,74,74});
    auto corners=queryCorners({50,50,74,74},0.8f,0.9f);
    corners[0].clip[3]=0.0f;
    CoverageGrid grid;
    BuildInput bi{}; bi.observationScopeId=kScope; bi.triangles=base.data(); bi.triangleCount=base.size();
    bi.viewportWidth=W; bi.viewportHeight=H; bi.viewportY=kY;
    bi.pixelAreaWinding=PixelAreaWinding::PositiveIsClockwise; bi.rasterEdgeGuardPixels=kEdgeGuard;
    BuildStats bs{}; grid.Build(bi,&bs); QueryStats qs{};
    expect(grid.QueryPaddedAabb(corners.data(),W,H,kY,kEdgeGuard,kScope,&qs)==QueryResult::VisibleOrUnknown && qs.invalidInput,
        "invalid query corner must fail open");
}

void testUpstreamErrorEnvelope()
{
    std::vector<Triangle> nominal; addRect(nominal,{8,8,120,120},0.15f,600);
    for (auto& t : nominal)
        for (auto& v : t.vertices)
        {
            for (int c=0;c<4;++c) { v.clip[c]*=2.0f; v.clipAbsDotTerms[c]*=2.0f; }
            v.upstreamAbsError[0]=0.0004f;
            v.upstreamAbsError[1]=0.0004f;
            v.upstreamAbsError[2]=0.00002f;
            v.upstreamAbsError[3]=0.00001f;
        }
    CoverageGrid grid;
    BuildInput input{}; input.observationScopeId=0x60000001ull;
    input.triangles=nominal.data(); input.triangleCount=nominal.size();
    input.viewportWidth=W; input.viewportHeight=H; input.viewportY=kY;
    input.pixelAreaWinding=PixelAreaWinding::PositiveIsClockwise;
    input.rasterEdgeGuardPixels=kEdgeGuard;
    BuildStats bs{};
    expect(grid.Build(input,&bs),"bounded-error/nonunit-W fixture must build");
    const Rect q{50,50,74,74};
    auto corners=queryCorners(q,0.8f,0.9f);
    QueryStats qs{};
    const auto result=grid.QueryPaddedAabb(corners.data(),W,H,kY,kEdgeGuard,input.observationScopeId,&qs);
    expect(result==QueryResult::ConservativelyBehindCoverage,"bounded-error interior should remain useful");
    if (result==QueryResult::ConservativelyBehindCoverage)
    {
        // The nominal clip output is only a center. Independently test eight
        // signed, admissible per-vertex arithmetic perturbation patterns.
        for (uint32_t pattern=0;pattern<8;++pattern)
        {
            auto actual=nominal;
            for (size_t ti=0;ti<actual.size();++ti)
                for (size_t vi=0;vi<3;++vi)
                {
                    auto& v=actual[ti].vertices[vi];
                    const uint32_t bit=uint32_t(actual[ti].sourceVertexId[vi]%3u);
                    const float sign=((pattern>>bit)&1u)?1.0f:-1.0f;
                    v.clip[0]+=sign*v.upstreamAbsError[0];
                    v.clip[1]+=(((pattern>>((bit+1u)%3u))&1u)?1.0f:-1.0f)*v.upstreamAbsError[1];
                    v.clip[2]+=(((pattern>>((bit+2u)%3u))&1u)?1.0f:-1.0f)*v.upstreamAbsError[2];
                    v.clip[3]-=sign*v.upstreamAbsError[3];
                }
            const RefStats ref=reference(actual,q,0.8f,true,PixelAreaWinding::PositiveIsClockwise,W,H);
            if (ref.samples==0 || ref.covered!=ref.samples || ref.subSamples==0 ||
                ref.subCovered!=ref.subSamples || !(ref.maxFrontDepth<0.8) || !(ref.maxSubFrontDepth<0.8))
            {
                ++failures;
                std::fprintf(stderr,"FALSE-HIDDEN upstream-envelope pattern=%u pixels=%u/%u sub=%u/%u front=%.9g subfront=%.9g\\n",
                    pattern,ref.covered,ref.samples,ref.subCovered,ref.subSamples,ref.maxFrontDepth,ref.maxSubFrontDepth);
            }
        }
    }
    std::printf("case=nonunit-W-upstream-error-envelope result=%s patterns=8 invalid=%d\\n",
        result==QueryResult::ConservativelyBehindCoverage?"hidden":"visible",int(qs.invalidInput));
}

void testCameraViewAndPairedMismatch()
{
    std::vector<Triangle> first; addRect(first,{8,8,120,120},0.2f,1);
    auto built=runCase("first-view",first,{50,50,74,74});
    expect(built.result==QueryResult::ConservativelyBehindCoverage,"first view positive");
    // A different scope denotes a moved/jittered view even when clip fixture is reused.
    runCase("moved-view-stale-query",first,{50,50,74,74},0.8f,0.9f,false,kScope,kScope+99);
    auto mismatched=first;
    mismatched[0].optionalMateTriangle=1; mismatched[1].optionalMateTriangle=0;
    // Same source vertex IDs but changed state/clip interval invalidates the proposed pair.
    for (int i : {0,1})
    {
        mismatched[1].vertices[i].clip[0] -= 0.01f;
        mismatched[1].vertices[i].clipAbsDotTerms[0] = std::abs(mismatched[1].vertices[i].clip[0]);
    }
    auto rejected=runCase("pair-interval-mismatch-crack",mismatched,{60,60,68,68},0.8f,0.9f,true);
    expect(rejected.build.pairedUnionsRejected>0,"mismatched shared clip interval should reject pair");
    expect(rejected.build.pairedUnionsAccepted==0,"mismatched pair must never take projected-union path");
    expect(rejected.result==QueryResult::VisibleOrUnknown,"query intersecting cracked pair seam must remain visible");
}

void testDeterministicMixedSuite()
{
    uint32_t state=0x5a17c3u;
    auto next=[&state]() { state=1664525u*state+1013904223u; return state; };
    for (uint32_t i=0;i<64;++i)
    {
        const uint32_t kind=i%4;
        const double cx=48.0+double(next()%33u);
        const double cy=48.0+double(next()%33u);
        const double half=6.0+double(next()%9u);
        const Rect q{cx-half,cy-half,cx+half,cy+half};
        std::vector<Triangle> ts;
        float nearZ=0.8f;
        if (kind==0)
            addRect(ts,{8,8,120,120},0.15f+float(next()%20u)*0.001f,1000+i*4);
        else if (kind==1)
        {
            addRect(ts,{8,8,120,120},0.72f+float(next()%20u)*0.001f,1000+i*4);
            nearZ=0.55f;
        }
        else if (kind==2)
        {
            addRect(ts,{8,8,120,40},0.2f,1000+i*4);
            addRect(ts,{8,88,120,120},0.2f,1001+i*4);
            addRect(ts,{8,40,40,88},0.2f,1002+i*4);
            addRect(ts,{88,40,120,88},0.2f,1003+i*4);
        }
        else
            ts.push_back(tri({8,8},{120,8},{8,120},0.2f,1000+i*4));
        runCase("deterministic-mixed",ts,q,nearZ,0.95f,(i%8u)==0u);
    }
}

void testCaps()
{
    std::vector<Triangle> capped;
    addRect(capped,{8,8,58,120},0.2f,1); // proven safe subset first
    std::vector<Triangle> filler;
    filler.push_back(tri({80,20},{81,20},{80,21},0.2f,10)); // tiny, never proves the lower-right query
    while (capped.size() < size_t(kMaxTriangles)+1) capped.push_back(filler[0]);
    auto left=runCase("triangle-cap-retains-safe-prefix",capped,{24,50,28,54});
    expect(left.build.triangleCapReached,"triangle cap reported");
    expect(!left.build.cellTestCapReached,"triangle cap case must reach triangle cap before cell-work cap");
    expect(left.result==QueryResult::ConservativelyBehindCoverage,"proven prefix cells may remain useful after triangle cap");
    auto right=runCase("triangle-cap-skipped-region-visible",capped,{100,100,104,104});
    expect(right.build.triangleCapReached,"triangle cap reported for skipped region");
    expect(!right.build.cellTestCapReached,"triangle cap case must not be masked by cell-work cap");
    expect(right.result==QueryResult::VisibleOrUnknown,"triangle-cap skipped region must remain visible");

    std::vector<Triangle> cellLimited;
    addRect(cellLimited,{8,8,58,120},0.2f,1);
    Triangle costly=tri({60,8},{120,8},{60,120},0.2f,30);
    for (int i=0;i<256;++i) cellLimited.push_back(costly);
    auto prefix=runCase("cell-cap-retains-proven-region",cellLimited,{24,50,28,54});
    expect(prefix.build.cellTestCapReached,"primitive cell-test cap reported");
    expect(prefix.result==QueryResult::ConservativelyBehindCoverage,"cell cap may retain proven prefix coverage");
    auto omitted=runCase("cell-cap-uncertain-region-visible",cellLimited,{100,100,104,104});
    expect(omitted.build.cellTestCapReached,"primitive cell-test cap reported");
    expect(omitted.result==QueryResult::VisibleOrUnknown,"unproven region remains visible after cell cap");
}
} // namespace

int main()
{
    testBasicAndDepth();
    testSeamsAndNegativeSpace();
    testEligibilityAndScope();
    testUpstreamErrorEnvelope();
    testCameraViewAndPairedMismatch();
    testDeterministicMixedSuite();
    testCaps();
    std::printf("summary= %s cases_hidden=%d cases_visible=%d failures=%d\n",
        failures==0?"PASS":"FAIL",hiddenCount,visibleCount,failures);
    return failures==0?0:1;
}
