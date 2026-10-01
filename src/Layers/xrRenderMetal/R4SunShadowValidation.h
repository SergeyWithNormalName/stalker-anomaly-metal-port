#pragma once

#include "IR4Resources.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

enum class R4SunShadowValidationPhase : std::uint8_t
{
    StencilBaseline,
    Near,
    Middle,
    Far,
    OffCameraCasters,
};

struct R4SunShadowValidationDesc
{
    std::shared_ptr<IR4Texture> shadowR32;
    std::shared_ptr<IR4Texture> positionRGBA16F;
    std::shared_ptr<IR4Texture> sceneDepthStencil;
    std::shared_ptr<IR4Texture> accumulationRGBA16F;
    std::array<float, 16> inverseView{};
    std::array<float, 16> currentViewProjection{};
    // R4 gbuffer_stage packs view-position XY into an NDC-like half-float
    // pair, stores view Z in P.z, and reconstructs view XY with these globals.
    std::array<float, 4> positionDecompressionParams{};
    std::array<float, 2> positionJitter{};
    // The actual receiver-volume transform for this phase. Far uses cascade 1's
    // transform, matching R4's previous-cascade volume construction.
    std::array<float, 16> stageVolumeViewProjection{};
    std::array<std::array<float,4>,8> receiverVolume{};
    bool hasReceiverVolume = false;
    std::string family;
    std::string outputDirectory;
    std::uint32_t engineFrame = 0;
    R4SunShadowValidationPhase phase = R4SunShadowValidationPhase::StencilBaseline;
    std::uint8_t stencilMarker = 5u;
    bool farZCullEnabled = false;
    float nearDepthScale = 1.0f;
    float nearDepthBias = 0.0f;
    float farDepthScale = 1.0f;
    float farDepthBias = 0.0f;
    float tsmBias = -0.01f;
    float sourceCascadeSize = 0.0f;
    float sourceCascadeBias = 0.0f;
    float sunColorIntensity = 1.0f;
    bool grassShadowsEnabled = false;
    std::uint32_t offCameraCasterCount = 0u;
    bool requireShadowedReceivers = true;
    // Optional tightly packed little-endian float32 export of the isolated
    // caster map, written from the completed GPU readback buffer.
    bool exportShadowMap = false;
    float receiverDepthBiasTolerance = 1.0e-4f;
};

// Call once after the sun mask (StencilBaseline), then after the near, middle,
// and far accumulation passes. The independent OffCameraCasters phase reads an
// isolated production R32 map plus the primary position/depth targets.
// Snapshots are copied asynchronously into Shared buffers and analyzed after
// their command buffer completes.
bool EncodeMetalR4SunShadowValidation(void* commandBuffer,
    void* shadowR32, void* positionRGBA16F, void* sceneDepthStencil,
    void* accumulationRGBA16F, const R4SunShadowValidationDesc& descriptor,
    std::string& error);
