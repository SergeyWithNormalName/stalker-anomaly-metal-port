#include "../R4LocalLightFaceVisibility.h"

#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace
{
using Clip=std::array<long double,4>;
long double planeDistance(const Clip& p,unsigned plane)
{
    switch (plane)
    {
    case 0: return p[0]+p[3]; case 1: return p[3]-p[0];
    case 2: return p[1]+p[3]; case 3: return p[3]-p[1];
    case 4: return p[2]; default: return p[3]-p[2];
    }
}

// Independent Sutherland-Hodgman clipping of actual triangles, rather than
// repeating the production corner-rejection algorithm. Keep even degenerate
// clipped polygons: their presence makes this oracle conservatively stricter.
bool clippedBoxHasPolygon(const std::array<float,3>& lo,const std::array<float,3>& hi,
    const std::array<float,16>& matrix)
{
    std::array<Clip,8> corners{};
    for (unsigned corner=0;corner<8;++corner)
    {
        const Clip p{corner&1u?hi[0]:lo[0],corner&2u?hi[1]:lo[1],corner&4u?hi[2]:lo[2],1};
        for (unsigned row=0;row<4;++row)
            for (unsigned column=0;column<4;++column)
                corners[corner][row]+=p[column]*matrix[column*4+row];
    }
    const unsigned triangles[12][3]={{0,1,3},{0,3,2},{4,6,7},{4,7,5},
        {0,4,5},{0,5,1},{2,3,7},{2,7,6},{0,2,6},{0,6,4},{1,5,7},{1,7,3}};
    for (const auto& triangle:triangles)
    {
        std::vector<Clip> polygon{corners[triangle[0]],corners[triangle[1]],corners[triangle[2]]};
        for (unsigned plane=0;plane<6 && !polygon.empty();++plane)
        {
            std::vector<Clip> clipped;
            Clip previous=polygon.back();
            long double previousDistance=planeDistance(previous,plane);
            for (const auto& current:polygon)
            {
                const long double currentDistance=planeDistance(current,plane);
                if ((previousDistance>=0)!=(currentDistance>=0))
                {
                    const long double t=previousDistance/(previousDistance-currentDistance);
                    Clip intersection{};
                    for (unsigned axis=0;axis<4;++axis)
                        intersection[axis]=previous[axis]+t*(current[axis]-previous[axis]);
                    clipped.push_back(intersection);
                }
                if (currentDistance>=0) clipped.push_back(current);
                previous=current; previousDistance=currentDistance;
            }
            polygon=std::move(clipped);
        }
        if (!polygon.empty()) return true;
    }
    return false;
}
} // namespace

int main()
{
    using namespace xray::metal;
    std::array<float, 16> vp{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const auto outside = [&](std::array<float, 3> lo, std::array<float, 3> hi) {
        return R4LocalLightFaceBoundsOutsideView(lo, hi, vp);
    };
    bool ok = outside({2,.2f,.2f}, {3,.3f,.3f});
    ok &= outside({-3,.2f,.2f}, {-2,.3f,.3f});
    ok &= outside({.2f,2,.2f}, {.3f,3,.3f});
    ok &= outside({.2f,-3,.2f}, {.3f,-2,.3f});
    ok &= outside({.2f,.2f,2}, {.3f,.3f,3});
    ok &= !outside({.2f,.2f,.2f}, {.3f,.3f,.3f});
    ok &= !outside({1,.2f,.2f}, {2,.3f,.3f}); // Boundary touches.
    ok &= outside({2,.2f,-.1f}, {3,.3f,.3f}); // Side separates despite near crossing.
    ok &= !outside({.2f,.2f,-.1f}, {.3f,.3f,.3f}); // Visible near crossing.
    ok &= outside({.2f,.2f,-3}, {.3f,.3f,-2}); // Fully behind near plane.
    ok &= !outside({-.5f,-.5f,-.5f}, {.5f,.5f,.5f}); // Eye-inside box.
    ok &= !outside({-5,-5,-5}, {5,5,5}); // Box contains the frustum.
    ok &= !outside({3,.2f,.2f}, {2,.3f,.3f}); // Invalid box.
    vp[15] = 0;
    ok &= outside({2,.2f,.2f}, {3,.3f,.3f});
    vp[15] = -1;
    ok &= outside({2,.2f,.2f}, {3,.3f,.3f});
    vp[15] = 1;
    // Compare translated/cancelling clip evaluation to the actual float dot
    // products. A double result strictly outside is insufficient near an edge.
    const auto floatOutsideRight = [&](std::array<float,3> lo, std::array<float,3> hi) {
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            const std::array<float,4> p{corner & 1u ? hi[0] : lo[0],
                corner & 2u ? hi[1] : lo[1], corner & 4u ? hi[2] : lo[2], 1};
            float x=0, w=0;
            for (unsigned column=0; column<4; ++column)
            { x+=p[column]*vp[column*4]; w+=p[column]*vp[column*4+3]; }
            if (!(x>w)) return false;
        }
        return true;
    };
    vp[12] = -100000000.0f;
    const std::array<float,3> largeLo{100000016.0f,.2f,.2f}, largeHi{100000032.0f,.3f,.3f};
    ok &= floatOutsideRight(largeLo,largeHi);
    ok &= !outside(largeLo,largeHi); // Float roundoff budget exceeds separation.
    const std::array<float,3> distantLo{100016384.0f,.2f,.2f}, distantHi{100032768.0f,.3f,.3f};
    ok &= floatOutsideRight(distantLo,distantHi) && outside(distantLo,distantHi);
    vp[12] = 0;
    vp[14] = -100000000.0f;
    ok &= !outside({.2f,.2f,100000000.0f},{.3f,.3f,100000016.0f}); // Float near-plane touch.
    vp[14] = 0;
    std::uint32_t randomState=0x589341d7u;
    const auto random=[&]() {
        randomState=randomState*1664525u+1013904223u;
        return float(randomState>>8u)/float(0x01000000u)*2-1;
    };
    std::size_t rejected=0, tested=0, omissions=0;
    for (unsigned sample=0;sample<4000;++sample)
    {
        std::array<float,3> lo{},hi{};
        for (unsigned axis=0;axis<3;++axis)
        {
            const float center=random()*50;
            const float radius=std::abs(random())*15+.05f;
            lo[axis]=center-radius; hi[axis]=center+radius;
        }
        // Dense projections include mixed-sign W, shear, translation and
        // near-plane crossings; the certificate applies to any finite matrix.
        for (auto& component:vp) component=random()*3;
        const bool reject=outside(lo,hi);
        ++tested;
        rejected+=reject;
        if (reject && clippedBoxHasPolygon(lo,hi,vp))
        { std::fprintf(stderr,"face visibility omitted an independently clipped triangle at sample %u\n",sample); ++omissions; ok=false; break; }
    }
    ok &= rejected>100; // Ensure the oracle exercised real certificates.
    std::printf("FACE_CLIP_ORACLE random_views=%zu certified_rejects=%zu clipped_omissions=%zu\n",tested,rejected,omissions);
    vp[0] = std::numeric_limits<float>::quiet_NaN();
    ok &= !outside({2,.2f,.2f}, {3,.3f,.3f});
    R4EngineLocalLight light;
    light.type = R4EngineLocalLightType::OmniFace;
    light.active = light.shadowed = true;
    R4EnvironmentSnapshot environment;
    environment.valid = true;
    ok &= R4LocalLightFaceMayBeCulled(light, environment);
    light.hudMode = true;
    ok &= !R4LocalLightFaceMayBeCulled(light, environment);
    light.hudMode = false;
    environment.localVolumetricEnabled = true;
    light.volumetricEnabled = true;
    ok &= !R4LocalLightFaceMayBeCulled(light, environment);
    light.volumetricEnabled = false;
    environment.ssfxVolumetric[0] = 1;
    ok &= !R4LocalLightFaceMayBeCulled(light, environment);
    environment.volumetricMode = 1;
    ok &= R4LocalLightFaceMayBeCulled(light, environment);
    light.type = R4EngineLocalLightType::Spot;
    ok &= !R4LocalLightFaceMayBeCulled(light, environment);
    light.type = R4EngineLocalLightType::Point;
    ok &= !R4LocalLightFaceMayBeCulled(light, environment);
    light.type = R4EngineLocalLightType::OmniFace;
    environment.valid = false;
    ok &= !R4LocalLightFaceMayBeCulled(light, environment);
    if (!ok) std::fprintf(stderr, "local-light face visibility safety checks failed\n");
    return ok ? 0 : 1;
}
