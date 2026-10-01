#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"

#include <filesystem>
#include <memory>
#include <string>

class MetalR4HudAdapter final
{
public:
    MetalR4HudAdapter();
    ~MetalR4HudAdapter();

    struct Options
    {
        std::filesystem::path shaderDirectory;
        // Optional exact models_selflightl/deffer_model_flat SKIN_0 bundle.
        std::filesystem::path emissiveShaderDirectory;
        // Exact SKIN_4 reflection sibling used by hardware-skinned HUD emitters.
        std::filesystem::path emissiveSkin4ShaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    struct Telemetry
    {
        std::uint32_t frame = 0;
        std::size_t renderables = 0;
        std::size_t requested = 0;
        std::size_t admitted = 0;
        std::size_t submitted = 0;
        std::size_t bumpDraws = 0;
        std::size_t flatDraws = 0;
        std::size_t skinned = 0;
        std::size_t skin4Requested = 0;
        std::size_t skin4Admitted = 0;
        std::size_t skin4Submitted = 0;
        std::size_t rejected = 0;
        std::size_t rejectNoAsset = 0;
        std::size_t rejectUnresolvedLinks = 0;
        std::size_t rejectNonSkinned = 0;
        std::size_t rejectSkin = 0;
        std::size_t rejectTexture = 0;
        std::size_t rejectUpload = 0;
        std::size_t rejectGeometrySelection = 0;
        std::size_t rejectSkin4RawAbi = 0;
        std::size_t rejectSkin4PaletteMissing = 0;
        std::size_t rejectSkin4PaletteMismatch = 0;
        std::size_t rejectSkin4PaletteCapacity = 0;
        std::size_t rejectSkin4BoneCapacity = 0;
        std::size_t rejectSkin4History = 0;
        std::size_t emissiveCandidates = 0;
        std::size_t emissiveDraws = 0;
        std::size_t emissiveRejected = 0;
        std::size_t emissiveAccumulatorCandidates = 0;
        std::size_t emissiveAccumulatorDraws = 0;
        std::size_t emissiveAccumulatorRejected = 0;
    };

    bool Configure(IR4Backend& backend, const Options& options, std::string& status);
    bool Prewarm(IR4Backend& backend, const std::shared_ptr<const R4EngineHudSnapshot>& snapshot, std::string& error);
    bool Draw(IR4Backend& backend, const std::shared_ptr<const R4EngineHudSnapshot>& snapshot, std::string& error);
    // Called by the scene-owned emissive-target pass while its BGRA8/depth
    // attachments are active; only captured emissive-eligible HUD geometry draws.
    bool DrawEmissive(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        float pdaDisplayFactor, std::string& error);
    // Writes the source first-accumulator contribution to a caller-owned
    // RGBA16Float/depth-stencil pass with depth read-only and stencil ref 1.
    bool DrawEmissiveAccumulator(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        float pdaDisplayFactor, std::string& error);
    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    bool DrawEmissiveTarget(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        float pdaDisplayFactor, bool accumulator, std::string& error);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunMetalR4HudAdapterSelfTest(std::string& error);
