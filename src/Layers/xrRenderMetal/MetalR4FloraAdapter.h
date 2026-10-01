#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include "R4FloraBlenderMaterial.h"
#include "R4LevelGeometry.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// This is deliberately a standalone admission adapter.  MetalR4EngineScene
// owns the G-buffer pass and may decide later whether to call it; the adapter
// never opens, closes, or submits a pass itself.
struct R4FloraViewState
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> previousView{};
    std::array<float, 16> previousProjection{};
    std::array<float, 3> eye{};
    // Authoritative scene static_globals from combine/G-buffer setup. Tree
    // shaders consume ambient/hemi/sun/fog here; synthesizing a zero-filled
    // copy turns otherwise valid foliage into black silhouettes.
    std::shared_ptr<IR4Buffer> staticGlobals;
    std::array<std::array<float, 4>, 32> previousBenders{};
    std::array<std::array<float, 4>, 32> currentBenders{};
    std::array<float, 4> bendersSetup{};
    std::array<float, 4> windParams{};
    std::array<float, 4> windSetupGrass{};
    std::array<float, 4> windSetupTrees{};
    std::array<float, 4> windAnimation{};
    std::array<float, 4> windAnimationPrevious{};
    float treeScaleBiasCorrection = 1.0f;
    float timeSeconds = 0.0f;
    bool valid = false;
    bool previousValid = false;
    bool windValid = false;
    // Validation-only switch: draw alpha-tested leaf geometry through the
    // same branch-wind vertex path but use the opaque depth-only fragment
    // shader to measure the coverage hidden by alpha discard.
    bool shadowIgnoreAlpha = false;
    // Validation-only selector: shadow draws are restricted to tree visuals
    // outside the production camera frustum when true.
    bool shadowOnlyOffCamera = false;
};

// MSC's tree input is P4/N4/T3/B3/I4.  Level geometry is P3/C4/C4/C4/S4;
// do not bind that packed D3D stream directly at the MSC vertex-fetch slots.
struct R4FloraStaticVertex
{
    float position[4];
    float normal[4];
    float tangent[3];
    float binormal[3];
    std::int32_t texcoord[4];
};
static_assert(sizeof(R4FloraStaticVertex) == 72u,
    "tree MSC P4/N4/T3/B3/I4 vertex ABI must remain tightly packed");

bool RepackR4FloraStaticVertices(
    const xray::metal::R4VertexBufferSummary& sourceBuffer,
    const xray::metal::R4StaticVisual& visual,
    const std::vector<std::uint8_t>& geometryBytes,
    std::vector<R4FloraStaticVertex>& vertices,
    std::string& error);

bool ResolveR4FloraTexturePaths(
    const std::string& textureName,
    bool leafBumpMaterial,
    std::vector<std::filesystem::path>& paths,
    std::string& error);

bool RunR4FloraAdapterSelfTest(std::string& error);

class MetalR4FloraAdapter final
{
public:
    struct Options
    {
        std::filesystem::path leafVertexMetallibPath;
        std::filesystem::path leafFragmentMetallibPath;
        std::filesystem::path trunkVertexMetallibPath;
        std::filesystem::path trunkFragmentMetallibPath;
        // Optional, isolated shadow caster shader bundle. When empty, the
        // visible Flora path stays available and shadow draws are a no-op.
        std::filesystem::path shadowShaderDirectory;
        std::filesystem::path instancedShadowShaderDirectory;
        std::filesystem::path wavesTexturePath;
        // Archive-winning CBlender_Tree settings from shaders.xr, keyed by
        // the exact shader name in R4LevelGeometry. Missing records are
        // rejected instead of inferring alpha or Object LOD state.
        std::vector<R4FloraBlenderMaterial> shaderMaterials;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    struct Telemetry
    {
        std::size_t totalVisuals = 0;
        std::size_t requestedVisuals = 0;
        std::size_t admittedVisuals = 0;
        std::size_t skippedVisuals = 0;
        std::size_t submittedVisuals = 0;
        std::size_t culledVisuals = 0;
        std::size_t draws = 0;
        std::size_t shadowSubmittedVisuals = 0;
        std::size_t shadowCulledVisuals = 0;
        std::size_t shadowDraws = 0;
    };

    MetalR4FloraAdapter();
    ~MetalR4FloraAdapter();
    MetalR4FloraAdapter(const MetalR4FloraAdapter&) = delete;
    MetalR4FloraAdapter& operator=(const MetalR4FloraAdapter&) = delete;

    // Builds/uploads resources before a G-buffer encoder is active. It
    // discovers every static l05_bar leaf whose exact material name is
    // flora\\leaf_wave or flora\\trunk_wave. Visuals 544/549 remain covered
    // by that set; every rejected visual is logged and isolated.
    bool Prepare(IR4Backend& backend,
        const xray::metal::R4LevelGeometry& geometry,
        const std::vector<std::uint8_t>& geometryBytes,
        const Options& options,
        std::string& status);

    // Requires an already-open, scene-owned G-buffer encoder.
    bool DrawIntoActiveGBuffer(IR4Backend& backend,
        const R4FloraViewState& view, std::string& error);

    // Requires the active sun/local shadow pass. lightViewProjection must be
    // the exact matrix used by the corresponding Windows shadow phase.
    bool DrawShadowCasters(IR4Backend& backend,
        const std::array<float, 16>& lightViewProjection,
        const R4FloraViewState& view, std::string& error);

    // Diagnostic-only draw into an isolated scene-owned pass. Selects one
    // implementation for this call and restores configured mode/telemetry.
    bool DrawShadowCastersForValidation(IR4Backend& backend,
        const std::array<float, 16>& lightViewProjection,
        const R4FloraViewState& view, bool instanced,
        Telemetry& sampledTelemetry, std::string& error);

    bool HasShadowCasters() const;

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};
