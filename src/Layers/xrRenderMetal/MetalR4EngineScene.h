#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "R4MaterialSet.h"
#include "R4DdsTexture.h"
#include "IR4Backend.h"
#include "MetalR4DynamicWorldAdapter.h"
#include "MetalR4DefObjectsLodAdapter.h"
#include "MetalR4DefVertexAdapter.h"
#include "MetalR4DetailGrassAdapter.h"
#include "MetalR4FloraAdapter.h"
#include "MetalR4HudAdapter.h"
#include "MetalR4ScopeAdapter.h"
#include "MetalR4LocalLightAdapter.h"
#include "MetalR4ParticleAdapter.h"
#include "MetalR4RainAdapter.h"
#include "MetalR4StaticForwardAdapter.h"
#include "MetalR4SsfxAoAdapter.h"
#include "MetalR4WaterAdapter.h"
#include "../xrRender/MetalLensFlareCapture.h"

// A deliberately narrow bridge between the engine device lifecycle and the
// validated production-level path.  It owns no SDL/CAMetalLayer state: callers
// must call Render() after BeginFrame() and before SubmitFrame(), with no
// render pass currently open.
class MetalR4EngineScene final
{
public:
    struct TerrainShaderMaterial
    {
        std::string shaderName;
        std::array<std::string, 4> detailTextures;
    };

    struct Options
    {
        std::filesystem::path levelDirectory;
        // Admission remains controlled, but the production batch ceiling is 64.
        std::vector<xray::metal::R4ControlledMaterial> materials;
        // Explicitly opt in to per-visual structural admission of the loaded
        // configured-level default+LMH set. This is deliberately distinct from the
        // 64-entry explicit material list above.
        bool autoLmhMaterials = false;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
        std::filesystem::path mscVertexMetallibPath;
        std::filesystem::path mscFragmentMetallibPath;
        std::filesystem::path mscArefFragmentMetallibPath;
        std::filesystem::path lightingVertexMetallibPath;
        std::filesystem::path lightingFragmentMetallibPath;
        std::filesystem::path sunShaderDirectory;
        // Exact converted bloom_luminance_1/2/3 bundle. This is required by
        // the production scene so combine_1 consumes the prior frame's real
        // R4 exposure instead of the former constant 1.0 diagnostic texture.
        std::filesystem::path luminanceShaderDirectory;
        // Optional, but when set must be the exact converted sky2 bundle with
        // its reflection JSON and sky2-msc-receipt.txt.
        std::filesystem::path skyShaderDirectory;
        // Optional exact R4 clouds bundle. It is separate from sky2 because
        // clouds use the live CEnvironment hemisphere and two 2D DDS inputs.
        std::filesystem::path cloudsShaderDirectory;
        // Optional exact converted global night-vision bundle. When present,
        // gameplay-selected Gen1/2/3 runs after the scene/scope composite and
        // before the UI overlay.
        std::filesystem::path nightVisionShaderDirectory;
        // Optional controlled terrain batch. Explicit visual ids remain
        // available for diagnostics; an empty set leaves the
        // established LMH batch unchanged.
        std::vector<std::uint32_t> terrainVisualIds;
        bool terrainAutoSelect = false;
        std::filesystem::path terrainShaderDirectory;
        // Archive-authored CBlender_BmmD channel bindings keyed by the
        // level shader name. The engine runtime fills these directly from
        // shaders.xr; standalone l05_bar proof keeps its bounded fallback.
        std::vector<TerrainShaderMaterial> terrainShaderMaterials;
        // Converted static flora path. Environment setup
        // fills this from XRAY_METAL_MSC_FLORA_DIR or the primary MSC bundle
        // directory; the adapter remains independently fail-closed.
        MetalR4FloraAdapter::Options flora;
        // Independently opt-in exact B_VERT / def_shaders\\def_vertex static
        // color family. It owns only its MSC pipeline and base textures and
        // reuses the ABI-proven level buffers.
        MetalR4DefVertexAdapter::Options defVertex;
        // Independently opt-in exact B_VERT / def_shaders\\def_objects_lod
        // static family. Its flat/bump choice comes only from archive-winning
        // THM metadata and it reuses the raw ABI-proven level buffers.
        MetalR4DefObjectsLodAdapter::Options defObjectsLod;
        MetalR4DynamicWorldAdapter::Options dynamicWorld;
        MetalR4HudAdapter::Options hud;
        MetalR4ParticleAdapter::Options particles;
        // Exact effects_rain sprite family, independently opt-in through its
        // receipted bundle.  Wet-surface and thunder families stay separate.
        MetalR4RainAdapter::Options rain;
        // Exact effects_sun lens-flare bundle. Capture remains value-only and
        // owns the authored flare texture identities separately.
        std::filesystem::path lensFlareShaderDirectory;
        // Exact blended static level families. The adapter writes only the
        // already-combined low color target and depth-tests against the owned
        // scene depth without modifying it.
        MetalR4StaticForwardAdapter::Options staticForward;
        // Exact SSFX AO temporal/bilateral family. It is intentionally
        // separate from water and IL, with an explicit receipted bundle.
        MetalR4SsfxAoAdapter::Options ssfxAo;
        // Full SSFX water graph. It is deliberately separate from
        // staticForward: its SSR/blur/wave dependencies own intermediate
        // targets and must not be encoded as an ordinary alpha draw.
        MetalR4WaterAdapter::Options water;
        // Strict opt-in: only XRAY_METAL_MSC_DETAIL_DIR enables this path.
        MetalR4DetailGrassAdapter::Options detailGrass;
        // Non-MSAA point/spot volume accumulation. Empty disables it.
        MetalR4LocalLightAdapter::Options localLights;
    };

    // Matrix values use the existing R4 column-major float4x4 ABI.
    struct ViewState
    {
        std::array<float, 16> view{};
        std::array<float, 16> projection{};
        std::array<float, 16> previousView{};
        std::array<float, 16> previousProjection{};
        std::array<float, 3> eye{};
        bool valid = false;
        bool previousValid = false;
    };

    // Runs after the static controlled batch has been encoded into the active
    // four-target G-buffer pass. It receives no render-pass descriptor or
    // attachment handles; the backend brackets it as scene-owned work and
    // rejects pass termination or frame submission from the callback.
    using GBufferWorldDrawCallback = std::function<bool(
        IR4Backend& backend,
        const ViewState& view,
        const std::shared_ptr<const R4EngineWorldSnapshot>& world,
        std::string& error)>;

    MetalR4EngineScene();
    ~MetalR4EngineScene();
    MetalR4EngineScene(const MetalR4EngineScene&) = delete;
    MetalR4EngineScene& operator=(const MetalR4EngineScene&) = delete;

    // Environment convenience for the first engine-backed milestone. It
    // consumes XRAY_METAL_LEVEL_DIR and XRAY_METAL_MATERIALS as a
    // semicolon-separated `visualId|base.dds|hemi.dds` list (maximum 64), or
    // the explicit opt-in literal `all` for exact loaded default+LMH visuals.
    // The one-visual
    // XRAY_METAL_VISUAL_ID/XRAY_METAL_TEXTURE_DDS/XRAY_METAL_HEMI_DDS form is
    // retained as a compatibility fallback. It also consumes XRAY_METAL_MSC_VS_METALLIB,
    // XRAY_METAL_MSC_PS_METALLIB, XRAY_METAL_MSC_LIGHTING_VS_METALLIB,
    // XRAY_METAL_MSC_LIGHTING_PS_METALLIB and XRAY_METAL_MSC_SUN_DIR. Sky2 is
    // enabled by XRAY_METAL_MSC_SKY_DIR and is otherwise explicitly skipped.
    // All
    // production paths are required. XRAY_METAL_TERRAIN_VISUALS admits a
    // comma-separated controlled terrain-root list, or the explicit `all`
    // mode for archive-authored configured-level BmmD admission; its converted artifacts
    // come from XRAY_METAL_MSC_TERRAIN_DIR or the primary MSC directory.
    // Flora artifacts come from XRAY_METAL_MSC_FLORA_DIR, falling back to the
    // primary MSC directory. Optional dynamic OGF admission uses XRAY_METAL_MSC_DYNAMIC_DIR (or the
    // exact paired XRAY_METAL_DYNAMIC_BUMP_VS_METALLIB/PS_METALLIB paths).
    // The directory form also admits its sibling deffer_model_bump.vs.metallib
    // for CPU-skinned SKIN_0 models; explicit callers may provide that exact
    // path with XRAY_METAL_DYNAMIC_MODEL_BUMP_VS_METALLIB. xrfs: paths use the
    // archive-winning in-memory provider when running through the engine.
    // Detail grass is independently opt-in through XRAY_METAL_MSC_DETAIL_DIR;
    // it requires the exact deffer_grass VS/PS bundle and waits for live
    // $level$/level.details rather than reading a host-side substitute.
    // XRAY_METAL_MSC_DEF_VERTEX_DIR independently enables the exact converted
    // deffer_base_flat static-color family for every admitted configured-level visual.
    // XRAY_METAL_MSC_DEF_OBJECTS_LOD_DIR independently enables exact converted
    // flat/bump B_VERT admission; unclassified or steep-parallax materials
    // reject the whole family before any draw.
    // XRAY_METAL_MSC_STATIC_FORWARD_DIR enables exact def_trans/def_trans_v,
    // wallmarkblend and additive lightplanes after combine and before particles.
    // XRAY_METAL_MSC_WATER_DIR enables the receipted SSFX water bundle; it
    // requires XRAY_METAL_MSC_STATIC_FORWARD_DIR for its authoritative level
    // water geometry records.
    static bool OptionsFromEnvironment(Options& options, std::string& error);

    bool Prepare(IR4Backend& backend, const Options& options, std::string& error);
    void Reset();

    // Replacing or clearing the callback is safe while another frame is
    // rendering: Render captures an immutable callback handle for that frame.
    // An empty callback is the default and is a no-op.
    void SetGBufferWorldDrawCallback(GBufferWorldDrawCallback callback);
    bool HasGBufferWorldDrawCallback() const;

    // Takes ownership of the CPU-expanded particle batch for the current
    // engine frame. It is staged before the scene passes and consumed once by
    // the post-combine forward pass.
    void SubmitParticleFrame(xray::metal::particles::FrameBatch frame);
    void SubmitRainFrame(xray::metal::rain::FrameBatch frame);
    void SubmitLensFlareFrame(xray::metal::lensflare::Frame frame);

    // Encodes the shadow, G-buffer, converted sun, combine and presentation
    // passes into the active backend frame.  It intentionally does not acquire
    // or submit a drawable.
    bool Render(
        IR4Backend& backend,
        const ViewState& view,
        const R4EnvironmentSnapshot& environment,
        const R4GameplayVisionSnapshot& vision,
        std::string& error);

    bool IsPrepared() const;
    std::uint64_t DrawableVisualCount() const;
    std::uint64_t DrawableIndexCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Structural proof for the conservative automatic-LMH/terrain culling helper.
// It deliberately exercises clip-space boundaries and fail-open input cases
// without requiring a Metal device or a live game frame.
bool RunMetalR4EngineSceneFrustumCullingSelfTest(std::string& error);
