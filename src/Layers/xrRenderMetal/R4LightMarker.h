#pragma once

#include "IR4Pipeline.h"

namespace xray::metal
{
// Non-MSAA R4: sun has already consumed 5 in the first local-light batch.
// After clearing marker bits, Windows restarts at 5. 255 is a valid marker.
struct R4LightMarker
{
    std::uint32_t next = 7u;
    bool NeedsReset() const { return next > 255u; }
    void Reset() { next = 5u; }
    void Increment() { next += 2u; }
};

inline R4GraphicsPipelineDesc R4LightMarkerResetPipelineDesc()
{
    R4GraphicsPipelineDesc desc;
    desc.label = "R4 local light stencil marker rollover";
    desc.shaderKey = "r4-local-marker-reset-nomsaa-v1";
    // Fixed-function equivalent of Windows s_occq E[2]. No light shader,
    // texture or constants are used; only stencil bits 1..7 are written.
    desc.mslSource = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 r4_marker_reset_vertex(uint index [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(positions[index], 0, 1);
}
fragment float4 r4_marker_reset_fragment() { return float4(0); }
)";
    desc.vertexFunction = "r4_marker_reset_vertex";
    desc.fragmentFunction = "r4_marker_reset_fragment";
    desc.colorFormats = {R4PixelFormat::RGBA16Float};
    desc.colorWriteMasks = {0u};
    desc.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    desc.depthCompare = R4CompareFunction::Always;
    desc.depthWrite = false;
    desc.cullMode = R4CullMode::None;
    desc.stencil.enabled = true;
    desc.stencil.frontFace.compare = R4CompareFunction::Always;
    desc.stencil.frontFace.readMask = 0u;
    desc.stencil.frontFace.writeMask = 0xfeu;
    desc.stencil.frontFace.stencilFailure = R4StencilOperation::Zero;
    desc.stencil.frontFace.depthFailure = R4StencilOperation::Zero;
    desc.stencil.frontFace.depthStencilPass = R4StencilOperation::Zero;
    desc.stencil.backFace = desc.stencil.frontFace;
    return desc;
}
} // namespace xray::metal
