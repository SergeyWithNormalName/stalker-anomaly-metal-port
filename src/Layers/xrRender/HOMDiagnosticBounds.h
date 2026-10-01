#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Admission guard for the existing HOM box projector. The matrix has HOM's
// 0..1 viewport mapping; it retains the engine's D3D/Metal 0..1 clip depth.
// False means visible/unknown, never permission to suppress work.
inline bool HomDiagnosticBoxCanBeTested(const std::array<float,3>& minimum,
    const std::array<float,3>& maximum, const std::array<float,3>& camera,
    const std::array<float,16>& xform, std::uint32_t frame, std::uint32_t renderedFrame)
{
    if (frame==UINT32_MAX || frame!=renderedFrame) return false;
    bool inside=true;
    for (unsigned axis=0;axis<3;++axis)
    {
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
            !std::isfinite(camera[axis]) || minimum[axis]>maximum[axis]) return false;
        inside &= camera[axis]>=minimum[axis] && camera[axis]<=maximum[axis];
    }
    if (inside) return false;
    for (const auto value:xform) if (!std::isfinite(value)) return false;
    for (unsigned corner=0;corner<8;++corner)
    {
        const float x=(corner&1)?maximum[0]:minimum[0];
        const float y=(corner&2)?maximum[1]:minimum[1];
        const float z=(corner&4)?maximum[2]:minimum[2];
        const float depth=x*xform[2]+y*xform[6]+z*xform[10]+xform[14];
        const float w=x*xform[3]+y*xform[7]+z*xform[11]+xform[15];
        const float sx=x*xform[0]+y*xform[4]+z*xform[8]+xform[12];
        const float sy=x*xform[1]+y*xform[5]+z*xform[9]+xform[13];
        // EPS matches HOM.cpp; the extra finite check covers overflowing x/y.
        if (!std::isfinite(depth) || !std::isfinite(w) || depth<=1.e-5f || w<=1.e-5f || depth>=w ||
            !std::isfinite(sx/w) || !std::isfinite(sy/w) ||
            std::abs(sx/w)>1.e6f || std::abs(sy/w)>1.e6f) return false;
    }
    return true;
}
