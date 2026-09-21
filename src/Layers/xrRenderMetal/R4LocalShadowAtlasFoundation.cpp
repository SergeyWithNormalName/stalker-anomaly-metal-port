#include "R4LocalShadowAtlasFoundation.h"

#include <algorithm>
#include <cmath>

namespace xray::metal
{
namespace
{
bool finite(const std::array<float, 16>& m)
{
    return std::all_of(m.begin(), m.end(), [](float x) { return std::isfinite(x); });
}
std::array<float, 16> multiply(const std::array<float, 16>& a, const std::array<float, 16>& b)
{
    std::array<float, 16> r{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}
bool meaningful(const std::array<float, 16>& m)
{
    const float d = m[0]*(m[5]*m[10]-m[6]*m[9])-m[4]*(m[1]*m[10]-m[2]*m[9])+m[8]*(m[1]*m[6]-m[2]*m[5]);
    return std::fabs(d) > 1.0e-10f && (std::fabs(m[3])+std::fabs(m[7])+std::fabs(m[11])+std::fabs(m[15])) > 1.0e-6f;
}
}

bool DeriveR4LocalShadowMatrix(const R4LocalShadowAtlasRecord& r, float scale, float bias,
    std::array<float, 16>& out, std::string& error)
{
    if (!std::isfinite(scale) || !std::isfinite(bias) || r.atlasExtent == 0 || r.atlasSize < 3 ||
        r.atlasX > r.atlasExtent - r.atlasSize || r.atlasY > r.atlasExtent - r.atlasSize)
    { error = "invalid shadow atlas parameters"; return false; }
    const float e = float(r.atlasExtent), d = float(r.atlasSize - 2) / e;
    const float sx = float(r.atlasX + 1) / e, sy = float(r.atlasY + 1) / e, halfTexel = .5f / e;
    // Exact r4_rendertarget_accum_spot.cpp m_TexelAdjust (column-major).
    const std::array<float, 16> adjust = {d/2,0,0,0, 0,-d/2,0,0, 0,0,scale,0,
        d/2+sx+halfTexel,d/2+sy+halfTexel,bias,1};
    // R4's mulA_44(A) computes A * current, so the source contract is
    // (texelAdjust * lightProjection) * (lightView * deviceInverseView).
    out = multiply(multiply(adjust, r.lightProjection), multiply(r.lightView, r.deviceInverseView));
    if (!finite(out) || !meaningful(out)) { error = "derived R4 m_shadow is degenerate"; return false; }
    error.clear(); return true;
}

bool DeriveR4LocalLightMapMatrix(const R4LocalShadowAtlasRecord& r, float scale, float bias,
    std::array<float, 16>& out, std::string& error)
{
    if (!std::isfinite(scale) || !std::isfinite(bias) || r.atlasExtent == 0)
    { error = "invalid local-light projection parameters"; return false; }
    const float halfTexel = .5f / float(r.atlasExtent);
    const std::array<float, 16> adjust = {.5f,0,0,0, 0,-.5f,0,0, 0,0,scale,0,
        .5f+halfTexel,.5f+halfTexel,bias,1};
    out = multiply(multiply(adjust, r.lightProjection),
        multiply(r.lightView, r.deviceInverseView));
    if (!finite(out) || !meaningful(out))
    { error = "derived R4 m_lmap is degenerate"; return false; }
    error.clear(); return true;
}

bool ValidateR4LocalShadowAtlasRecord(const R4LocalShadowAtlasRecord& r, std::uint32_t frame, std::string& error)
{
    if (r.lightIdentity == 0 || r.engineFrame != frame || !r.renderedDepth || r.face > 5)
    { error = "shadow record identity, frame, depth completion, or face is invalid"; return false; }
    if (r.atlasExtent == 0 || r.atlasSize < 3 || r.atlasX > r.atlasExtent-r.atlasSize || r.atlasY > r.atlasExtent-r.atlasSize)
    { error = "shadow atlas rectangle is out of bounds"; return false; }
    if (!finite(r.lightView)||!finite(r.lightProjection)||!finite(r.deviceInverseView)||!finite(r.shadowMatrix)||
        !finite(r.lightMapMatrix)|| !meaningful(r.lightView)||!meaningful(r.lightProjection)||
        !meaningful(r.deviceInverseView)||!meaningful(r.shadowMatrix)||!meaningful(r.lightMapMatrix))
    { error = "shadow matrix is nonfinite or lacks a meaningful homogeneous transform"; return false; }
    error.clear(); return true;
}

bool RunR4LocalShadowAtlasFoundationSelfTest(std::string& error)
{
    R4LocalShadowAtlasRecord r; r.lightIdentity=9; r.engineFrame=8; r.smapId=2; r.atlasX=64; r.atlasY=128; r.atlasSize=256; r.atlasExtent=2048; r.renderedDepth=true;
    for (int i=0;i<16;++i) r.lightView[i]=r.lightProjection[i]=r.deviceInverseView[i]=(i%5)==0?1.f:0.f;
    // Keep this non-commutative: it proves the exact R4 mulA_44 order rather
    // than only accepting the same result from identity matrices.
    r.lightView[12]=3.f;
    r.lightProjection[0]=2.f;
    if (!DeriveR4LocalShadowMatrix(r,1.f,0.f,r.shadowMatrix,error) ||
        !DeriveR4LocalLightMapMatrix(r,1.f,0.f,r.lightMapMatrix,error) ||
        !ValidateR4LocalShadowAtlasRecord(r,8,error)) return false;
    const float atlasScale = (float(254)/2048.f)/2.f;
    const float atlasTranslate = atlasScale + float(65)/2048.f + .5f/2048.f;
    const float expected = atlasScale * 2.f * 3.f + atlasTranslate;
    if (std::fabs(r.shadowMatrix[12]-expected)>1.e-6f) { error="atlas +1/border/half-texel golden transform mismatch"; return false; }
    r.engineFrame=7; if (ValidateR4LocalShadowAtlasRecord(r,8,error)) { error="stale shadow record accepted"; return false; }
    return true;
}
} // namespace xray::metal
