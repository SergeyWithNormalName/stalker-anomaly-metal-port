#pragma once
#include "R4SunCascade.h"
#include "R4SunShadowValidation.h"
#include "R4LocalShadowBatchValidation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DisplayCapabilities.h"
#include "IR4Pipeline.h"
#include "R4StaticMotionValidation.h"
#include "R4TemporalTextureValidation.h"
#include "../xrRender/MetalUICapture.h"

struct SDL_Window;
class IR4Resources;

enum class R4FrameAcquireResult
{
    Ready,
    Retry,
    Error,
};

struct R4ClearColor
{
    double red;
    double green;
    double blue;
    double alpha;
};

// CPU-owned snapshot of the most recently completed presentation frame,
// reduced to the fixed save-thumbnail extent. Pixels are tightly packed BGRA8
// in top-left origin order; no drawable or Metal resource escapes the backend.
struct R4SaveThumbnailSnapshot
{
    static constexpr std::uint32_t Width = 128;
    static constexpr std::uint32_t Height = 128;
    std::uint64_t frameSerial = 0;
    std::vector<std::uint8_t> bgraPixels;
};

// A unique, immutable range in the acquired frame context. The backend owns
// the native storage until that context's command buffer completes.
struct R4FrameBufferSlice
{
    std::shared_ptr<IR4Buffer> buffer;
    std::size_t offset = 0;
    std::size_t size = 0;
    // Unique backend acquisition that created this immutable upload range.
    // Zero denotes a slice with no native acquisition proof.
    std::uint64_t acquireEpoch = 0;
};

// Narrow native skin dispatch description. The canonical input is immutable
// 88-byte scalar-array records decoded and validated by the caller. Its bone
// indices must be below boneCount; maxReferencedBoneIndex is trusted metadata
// only after validating every canonical record. Output is exactly the
// existing 36-byte SKIN_0 stream. The canonical buffer must outlive the
// acquired frame and must not be an arena upload slice. The backend validates every resource span
// and the full job batch before it encodes any writes.
struct R4ComputeSkinOracleRequest
{
    std::string stableKey;
    std::vector<std::uint8_t> expectedBytes;
    std::uint64_t poseHash = 0u;
    std::uint64_t outputWriteSerial = 0u;
    std::uint64_t prewarmEpoch = 0u;
    std::uint32_t engineFrame = 0u;
};

struct R4ComputeSkinJob
{
    std::shared_ptr<IR4Buffer> canonicalInput;
    std::size_t inputOffset = 0;
    std::size_t inputSize = 0;
    R4FrameBufferSlice bones;
    std::shared_ptr<IR4Buffer> output;
    std::size_t outputOffset = 0;
    std::size_t outputSize = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t influenceCount = 0;
    std::uint32_t boneCount = 0;
    std::uint32_t maxReferencedBoneIndex = 0;
    // Optional, default-off diagnostic request. The backend owns the request
    // through command-buffer completion and may skip it when its per-frame
    // readback cap is exhausted. This never changes compute dispatch semantics.
    std::shared_ptr<const R4ComputeSkinOracleRequest> oracle;
};

// Value-only gameplay state captured after the actor update and before the
// Direct Metal frame is submitted.  The legacy R4 phases use integer shader
// element selectors rather than a continuous night-vision intensity, so the
// selector is preserved verbatim and intensity is the effective 0/1 phase
// gate.  Heat vision additionally keeps its authored palette mode verbatim.
// No game object, post-process effector, D3D resource, or renderer object is
// allowed to cross this boundary.
struct R4GameplayVisionSnapshot
{
    std::uint32_t engineFrame = 0;
    std::int32_t nightVisionPhase = 0;
    std::int32_t heatVisionPhase = 0;
    float heatVisionMode = 0.0f;
    float nightVisionIntensity = 0.0f;
    float heatVisionIntensity = 0.0f;
    std::array<float, 4> shaderParam7{};
    std::array<float, 4> shaderParam8{};
    bool valid = false;
    bool gameplayStateValid = false;
    bool actorNightVisionRequested = false;
    bool actorNightVisionEffectorActive = false;
    bool nightVisionActive = false;
    bool heatVisionActive = false;
};

// The game calls this only after R4 selected the Direct Metal backend.  The
// implementation lives with the Metal device, keeping the game independent of
// dxRenderDeviceRender and every Metal/D3D implementation type.
void SubmitR4GameplayVisionSnapshot(const R4GameplayVisionSnapshot& snapshot);

enum class R4LoadAction
{
    DontCare,
    Load,
    Clear,
};

enum class R4StoreAction
{
    DontCare,
    Store,
};

struct R4ColorAttachmentDesc
{
    std::shared_ptr<IR4Texture> texture;
    R4LoadAction loadAction = R4LoadAction::Clear;
    R4StoreAction storeAction = R4StoreAction::Store;
    R4ClearColor clearColor{0.0, 0.0, 0.0, 1.0};
};

struct R4DepthStencilAttachmentDesc
{
    std::shared_ptr<IR4Texture> texture;
    R4LoadAction depthLoadAction = R4LoadAction::Clear;
    R4StoreAction depthStoreAction = R4StoreAction::Store;
    R4LoadAction stencilLoadAction = R4LoadAction::Clear;
    R4StoreAction stencilStoreAction = R4StoreAction::Store;
    double clearDepth = 1.0;
    std::uint32_t clearStencil = 0;
};

struct R4Viewport
{
    double originX = 0.0;
    double originY = 0.0;
    double width = 0.0;
    double height = 0.0;
    double nearDepth = 0.0;
    double farDepth = 1.0;
};

struct R4ScissorRect
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class R4WholeLightVisibilityGroupKind : std::uint8_t
{
    SingleRecord,
    OmniParent,
};

// Metadata for one actual local-light accumulation draw. The native backend
// may attach an asynchronous depth/stencil visibility count to this draw.
// Results describe deferred receiver samples only and never grant culling.
struct R4WholeLightVisibilitySample
{
    std::uint32_t engineFrame = 0u;
    // Optional CPU-prediction provenance. Zero preserves GPU-only observations.
    // Nonzero values identify one CPU observation scope within the acquired frame.
    std::uint64_t cpuObservationScopeId = 0u;
    std::int32_t batchId = -1;
    std::uint64_t lightId = 0u;
    std::uint64_t parentLightId = 0u;
    std::uint32_t face = 0u;
    R4WholeLightVisibilityGroupKind groupKind =
        R4WholeLightVisibilityGroupKind::SingleRecord;
    std::uint8_t expectedFaceCount = 1u;
};

struct R4RenderPassDesc
{
    const char* label = nullptr;
    bool useDrawable = true;
    R4ColorAttachmentDesc drawableColor;
    std::vector<R4ColorAttachmentDesc> colorAttachments;
    R4DepthStencilAttachmentDesc depthStencilAttachment;
    bool hasViewport = false;
    R4Viewport viewport;
    bool hasScissor = false;
    R4ScissorRect scissor;
    // Enables a bounded, backend-owned visibility-result buffer for this pass.
    // Default false keeps every existing pass on its current path.
    bool enableVisibilityResultCounts = false;
};

enum class R4ShaderStage
{
    Vertex,
    Fragment,
};

enum class R4MscResourceType
{
    ConstantBuffer,
    Texture,
    Sampler,
};

struct R4MscResourceBinding
{
    R4MscResourceType type = R4MscResourceType::ConstantBuffer;
    std::size_t tlabOffset = 0;
    std::shared_ptr<IR4Buffer> buffer;
    std::size_t bufferOffset = 0;
    std::size_t bufferSize = 0;
    std::shared_ptr<IR4Texture> texture;
    std::shared_ptr<IR4Sampler> sampler;
};

struct R4MscStageBindings
{
    R4ShaderStage stage = R4ShaderStage::Vertex;
    std::size_t tlabSize = 0;
    std::vector<R4MscResourceBinding> resources;
};

// Value-only capture of the live R4 environment mixer. The engine publishes
// it before the legacy D3D environment path early-outs for Direct Metal; no
// D3D texture or environment object crosses into the Metal renderer.
struct R4EnvironmentSnapshot
{
    std::uint32_t engineFrame = 0;
    std::string sky0TextureName;
    std::string sky1TextureName;
    std::string environment0TextureName;
    std::string environment1TextureName;
    // The cloud hemisphere is built by CEnvironment and normally consumed by
    // the D3D environment renderer.  Keep an immutable value-only copy so
    // Direct Metal can render the same R4 mesh after that renderer early-outs.
    std::string clouds0TextureName;
    std::string clouds1TextureName;
    std::array<float, 4> cloudsColor{};
    std::vector<std::array<float, 3>> cloudsVertices;
    std::vector<std::uint16_t> cloudsIndices;
    bool cloudsValid = false;
    std::array<float, 3> skyColor{};
    float skyBlend = 0.0f;
    float skyRotation = 0.0f;
    std::array<float, 3> ambient{};
    std::array<float, 4> hemiColor{};
    std::array<float, 3> sunColor{};
    std::array<float, 3> sunDirection{};
    // Exact r4_rendertarget_phase_combine values. Keeping the already-scaled
    // values here avoids recreating R4 console/NV policy inside Metal.
    std::array<float, 4> combineAmbient{};
    std::array<float, 4> combineEnvironment{};
    std::array<float, 4> hmodelStuff{};
    // R4 tonemap controls captured from the authoritative renderer console
    // state. xyz are the already-mixed MiddleGray numerator/amount/low-lum
    // terms; w is the adaptation speed used with frameDeltaSeconds.
    std::array<float, 4> tonemapParameters{1.0f, 0.0f, 1.0f, 1.0f};
    // Authoritative combine_2 image controls from the R4 console binder.
    std::array<float, 4> imageCorrections{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> colorGrading{0.0f, 0.0f, 0.0f, 1.0f};
    float frameDeltaSeconds = 0.0f;
    // The direct-sun passes consume Lights.sun_adapted rather than the raw
    // environment sun. Its direction is world-space; the scene derives the
    // view-space dynamic-light direction from its current view matrix.
    std::array<float, 3> adaptedSunColor{};
    float adaptedSunSpecular=0.5f;
    std::array<float,4> ssfxFloraFixes1{};
    std::array<float,4> ssfxFloraFixes2{};
    std::array<float,4> ssfxLightSetup1{};
    std::array<float, 3> adaptedSunDirection{};
    std::array<float, 3> adaptedSunPosition{};
    // Source R4 volumetric controls. These are copied on the engine update
    // thread so the Metal scene does not infer active modes from shader names.
    bool ssfxVolumetricAvailable = false;
    std::array<float, 4> ssfxVolumetric{1.0f, 1.0f, 3.0f, 1.0f};
    bool localVolumetricEnabled = false;
    bool sunShaftsEnabled = false;
    std::uint32_t sunShaftsMode = 0;
    float sunShaftsIntensity = 0.0f;
    float sunShaftsFarRange = 100.0f;
    std::uint32_t sunShaftsQuality = 1;
    std::uint32_t volumetricResolutionDivisor = 8;
    std::int32_t volumetricMode = 0;
    std::int32_t volumetricGlossMethod = 0;
    float volumetricGlossMinimum = 0.0f;
    float volumetricGlossFactor = 0.0f;
    std::array<float, 3> volumetricGlossMinMax{};
    std::array<float, 4> ssfxShadowBias{0.0f,0.03f,0.0f,0.0f};
    std::array<float, 3> ssfxShadowCascades{20.0f, 60.0f, 160.0f};
    std::array<float, 4> ssfxGrassShadows{};
    std::array<float, 2> sunDepthScale{1.0f, 1.0f};
    std::array<float, 2> sunDepthBias{-0.00001f, -0.00002f};
    float sunTsmBias = -0.01f;
    bool grassShadows = false;
    float detailFadeDistance = 47.5f;
    bool sunZCulling = false;
    bool sunResetLastCascade = false;
    R4SunCascadeSet sunCascades;
    std::array<float, 3> fogColor{};
    float fogDensity = 0.0f;
    float fogNear = 0.0f;
    float fogFar = 0.0f;
    // Active-profile SSFX fog-scattering presence and live console controls.
    // The Metal leaf consumes these only when the source R4 feature gate is on.
    bool ssfxFogScatteringAvailable = false;
    std::array<float, 4> ssfxFog{};
    float ssfxFogScattering = 0.0f;
    float globalTime = 0.0f;
    // Exact live SSFX water controls captured beside the environment mixer.
    // These are values only: the Metal water subsystem never borrows legacy
    // R4 textures, shader objects, or render targets.
    std::array<float, 4> ssfxWater{};
    std::array<float, 4> ssfxWaterSetup1{};
    std::array<float, 4> ssfxWaterSetup2{};
    std::array<float, 3> ssfxWaterQuality{};
    // Exact live general SSFX SSR controls and the R4 source-presence gate.
    // The admitted converted bundle currently supports quality zero only.
    bool ssfxSsrAvailable = false;
    std::uint32_t ssfxSsrQuality = 0u;
    std::array<float, 4> ssfxSsr{};
    std::array<float, 4> ssfxSsr2{};
    std::array<float, 4> ssfxIsUnderground{};
    // Exact R4 SSFX AO controls. The production Metal pass consumes them
    // before combine_1; it does not recreate console policy in the backend.
    bool ssfxAoAvailable = false;
    bool ssfxIlAvailable = false;
    std::array<float, 4> ssfxAo{};
    std::array<float, 4> ssfxAoSetup1{};
    std::array<float, 4> ssfxIl{};
    std::array<float, 4> ssfxIlSetup1{};
    // Values from the exact R4 bloom binder, including optional weather presets.
    std::array<float, 4> ssfxBloom1{};
    std::array<float, 4> ssfxBloom2{};
    std::array<float, 4> maskControl{};
    float windAnimation = 0.0f;
    float windVelocity = 0.0f;
    float rainDensity = 0.0f;
    // Dynamic accumulation owned by CEnvironment. This is distinct from the
    // authored m_fWetSurfaces value in hmodelStuff and feeds rain_params.y.
    float wetnessFactor = 0.0f;
    float treeScaleBiasCorrection = 1.0f;
    bool valid = false;
};

// Engine world instances are submitted separately from static level assets.
// Their geometry remains owned by the Metal asset loader; this value-only ABI
// lets the engine drive instance visibility without exposing D3D resources.
struct R4EngineWorldRenderable
{
    std::string assetName;
    std::array<float, 16> transform{};
    std::array<float, 4> bounds{};
    std::uint32_t visualType = 0;
    std::uint16_t objectId = 0xffffu;
    bool castsShadow = true;
	// Resolved legacy L_hotness: x is the HeatVision scalar, y retains legacy
	// transparency, z/w are zero. Capture already applies the texture hot gate.
	std::array<float, 4> hotness{};
	std::array<float, 4> material{};
	std::array<float, 4> hemiCubePositive{};
	std::array<float, 4> hemiCubeNegative{};
	std::shared_ptr<const struct R4EngineWorldAsset> asset;
	// Current local-space bone transforms captured with the visible instance.
	// Dynamic Metal CPU skinning consumes these before entering the G-buffer
	// callback; no engine-owned skeleton data crosses a command encoder.
	std::shared_ptr<const std::vector<std::array<float, 16>>> boneTransforms;
};

struct R4EngineWorldGeometry
{
    std::string shaderName;
    std::string textureName;
    std::uint32_t visualType = 0;
    std::uint32_t vertexFormat = 0;
    std::uint32_t vertexStride = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    bool skinned = false;
    std::shared_ptr<const std::vector<std::uint8_t>> vertexBytes;
    std::shared_ptr<const std::vector<std::uint16_t>> indexData;
};

struct R4EngineWorldAsset
{
    std::string sourceName;
    std::uint64_t contentHash = 0;
    bool hasUnresolvedGeometryLinks = false;
    std::vector<R4EngineWorldGeometry> geometry;
};

// A frame-local, immutable ownership boundary between the R4 engine scene
// graph and Metal scene callbacks.  The backend publishes a new shared
// snapshot on each submission, so a callback can retain this value while the
// engine advances to the next frame without borrowing CDSGraph storage.
struct R4EngineWorldSnapshot
{
    std::uint32_t engineFrame = 0;
    std::vector<R4EngineWorldRenderable> renderables;
};

// HUD is deliberately a distinct value-only stream.  HUD models are submitted
// under CHudInitializer/rmNear and are not members of the spatial world set.
struct R4EngineHudRenderable
{
	enum class ScopeRole : std::uint8_t
	{
		Back,
		Zwrite,
		ReticlePrecise,
	};
	struct SelectedShaderFlags
	{
		std::uint8_t priority = 0;
		bool strictB2F = false;
		bool emissive = false;
		bool distort = false;
		bool wmark = false;
		std::uint8_t scopeLense = 0;
	};
	struct ScopeLeaf
	{
		std::uint32_t geometryIndex = 0;
		ScopeRole role = ScopeRole::Back;
		SelectedShaderFlags selectedShader;
	};
    std::string assetName;
    std::array<float, 16> transform{};
	// Value-only prior HUD instance transform. The bridge supplies current when
	// the previous snapshot cannot be matched by ordinal/count identity.
	std::array<float, 16> previousTransform{};
	std::array<float, 4> material{};
	std::array<float, 4> hotness{};
	std::array<float, 4> hemiCubePositive{};
	std::array<float, 4> hemiCubeNegative{};
    std::shared_ptr<const R4EngineWorldAsset> asset;
    std::shared_ptr<const std::vector<std::array<float, 16>>> boneTransforms;
	// Preserves the exact bone ordinal/count of boneTransforms. It is safe for a
	// future SKIN_4 motion-vector consumer without retaining engine skeletons.
	std::shared_ptr<const std::vector<std::array<float, 16>>> previousBoneTransforms;
	// Explicit opaque geometry subset. Empty is deliberately not a whole-asset
	// fallback: unsupported CPU-only aggregate leaves must stay unsubmitted.
	std::vector<std::uint32_t> eligibleGeometryIndices;
	std::vector<std::uint32_t> emissiveGeometryIndices;
	std::vector<SelectedShaderFlags> geometryShaderFlags;
	// Immutable 3DSS roles, deliberately separate from opaque geometry.
	std::vector<ScopeLeaf> scopeLeaves;
};

struct R4EngineHudSnapshot
{
    std::uint32_t engineFrame = 0;
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> previousView{};
    std::array<float, 16> previousProjection{};
	// Exact frame-global b0 values read by the converted HUD fragment
	// permutations. Per-renderable L_hotness is retained on R4EngineHudRenderable.
	std::array<float, 4> ssfxPom{};
	std::array<float, 4> ssfxHudDrops1{};
	std::array<float, 4> ssfxHudDrops2{};
	// Exact value-only precise-scope globals for the future dedicated 320-byte
	// b0. markswitchCurrent mirrors the legacy integer binder in x, zeroing yzw.
	std::array<float, 4> hudParams{};
	std::array<float, 4> hudFovParams{};
	std::array<float, 4> ogseCScreen{};
	std::array<float, 4> s3dsParam1{};
	std::array<float, 4> s3dsParam2{};
	std::array<float, 4> s3dsParam3{};
	std::array<float, 4> s3dsParam4{};
	std::array<float, 4> markswitchCurrent{};
	std::array<float, 4> markswitchColor{};
	std::array<float, 4> shaderParam8{};
	std::uint32_t captureRequested = 0;
	std::uint32_t captureAdmitted = 0;
	std::uint32_t captureAggregateFallback = 0;
	std::uint32_t captureStrict = 0;
	std::uint32_t captureScopeForward = 0;
	std::uint32_t captureScopeBack = 0;
	std::uint32_t captureScopeZwrite = 0;
	std::uint32_t captureScopeReticlePrecise = 0;
	std::uint32_t captureScopeTriplets = 0;
	std::uint32_t captureScopePartialRejected = 0;
	std::uint32_t captureScopeDuplicateRejected = 0;
	std::uint32_t captureNonHq = 0;
	std::uint32_t captureDistortFollowup = 0;
	std::uint32_t captureEmissiveFollowup = 0;
	std::uint32_t captureWmarkFollowup = 0;
	bool scope3DFakeEnabled = false;
    bool valid = false;
    std::vector<R4EngineHudRenderable> renderables;
};

// Immutable value-only copy of the live R4 local-light registry.  It is
// deliberately separate from world renderables: no light, shader, texture or
// D3D object lifetime crosses the engine/Metal boundary.
enum class R4EngineLocalLightType : std::uint8_t
{
    Point,
    Spot,
    OmniFace,
};

struct R4EngineLocalLight
{
	// Stable only for the lifetime of the engine process.  It is assigned at
	// capture time from the source light plus OmniFace ordinal, never from
	// vector order, so shadow records remain coherent after culling/reordering.
	std::uint64_t lightId = 0;
    // Explicit source parent for atomic six-face atlas batching.
    std::uint64_t parentLightId = 0;
    R4EngineLocalLightType type = R4EngineLocalLightType::Point;
    std::array<float, 3> position{};
    std::array<float, 3> direction{};
    std::array<float, 3> right{};
    std::array<float, 4> color{};
    float range = 0.0f;
    float cone = 0.0f;
    float virtualSize = 0.1f;
    // Captured in the R4 owner through u_diffuse2s(L_clr); source alpha is
    // not the deferred local-light specular scalar.
    float specular = 0.0f;
    bool active = false;
    bool staticLight = false;
    bool shadowed = false;
    bool decorative = false;
    float cameraDistance = 0.0f;
	bool hudMode = false;
	std::uint8_t omniFace = 0xff;
	std::string projectorTexture;
    // Raw values consumed by the source R4 spot-volumetric formulas. `color`
    // above remains the existing local-light value (already multiplied by
    // LOD); rawColor is separate to avoid applying LOD twice in that pass.
    bool volumetricEnabled = false;
    float volumetricVisibilityDistance = 0.0f;
    float volumetricQuality = 1.0f;
    float volumetricIntensity = 1.0f;
    float volumetricDistance = 1.0f;
    float volumetricLod = 1.0f;
    bool volumetricPlayerLight = false;
    std::array<float, 3> rawColor{};
    std::array<float, 3> volumetricSphereCenter{};
    float volumetricSphereRadius = 0.0f;
};

// Canonical one-frame value snapshot shared by all Direct Metal lighting
// passes.  It replaces the raw light registry bridge; frame mismatch is a
// fail-closed error in the backend rather than stale lights on screen.
struct R4LightingFrameSnapshot
{
    std::uint32_t engineFrame = 0;
    std::array<float, 4> ssfxLightSetup1{};
    float shadowDepthScale = 1.0f;
    float shadowDepthBias = 0.0f;
    std::vector<R4EngineLocalLight> lights;
};

using R4EngineLocalLightSnapshot = R4LightingFrameSnapshot;

class IR4Backend
{
public:
    virtual ~IR4Backend() = default;

    // A non-zero preferred size is the engine-selected render resolution. The
    // CAMetalLayer may scale it to a differently sized Retina window without
    // silently multiplying the scene workload by the backing scale.
    virtual bool Initialize(SDL_Window* window, std::uint32_t preferredWidth,
        std::uint32_t preferredHeight, std::string& error) = 0;
    virtual void ResizeDrawable(std::uint32_t width = 0, std::uint32_t height = 0) = 0;

    virtual R4FrameAcquireResult BeginFrame(std::string& error) = 0;
    // E01 diagnostic metadata. No effect unless XRAY_METAL_E01_FRAMES is set.
    virtual void SetE01FrameTiming(std::uint32_t engineFrame,
        std::uint32_t cameraFrame, double sceneMs) = 0;
    virtual void AddE01CpuStage(const char* stage, double milliseconds) = 0;
    virtual bool BeginRenderPass(const R4RenderPassDesc& descriptor, std::string& error) = 0;
    bool BeginRenderPass(const R4ClearColor& clearColor, std::string& error)
    {
        R4RenderPassDesc descriptor;
        descriptor.drawableColor.clearColor = clearColor;
        return BeginRenderPass(descriptor, error);
    }
    virtual bool EndRenderPass(std::string& error) = 0;
    // Change only raster region inside a pass. Attachments, clears and resource
    // ownership remain unchanged; used for disjoint local-shadow atlas tiles.
    virtual bool SetActiveRenderRegion(const R4Viewport&, const R4ScissorRect&, std::string& error)
    { error = "active render region is unsupported by this backend"; return false; }
    // Opt-in draw attribution. Native backends restart the current pass with
    // Load/Store attachments; callers must bind all draw state afterwards.
    virtual bool SplitActivePassForDiagnostics(const std::string&, std::string&) { return true; }
    // Encodes a same-format, full-extent mip-0 copy between scene-owned 2D
    // textures. It is intentionally outside IR4Resources: copies are ordered
    // on the active frame command buffer between render passes.
    virtual bool CopyTexture2D(const std::shared_ptr<IR4Texture>& source,
        const std::shared_ptr<IR4Texture>& destination, std::string& error) = 0;
    // Copies bytes once into a fresh range. Use 256-byte alignment for MSC
    // constants and at least 16 bytes for vertex/instance streams.
    virtual bool UploadFrameData(const void* data, std::size_t size,
        std::size_t alignment, R4FrameBufferSlice& slice, std::string& error) = 0;
    // Unsupported by default so other backends retain their CPU fallback.
    // Contract for every implementation: a false return means that no compute
    // skin output write was encoded. Validate/preallocate before the first
    // dispatch; after dispatch starts, return true and let asynchronous GPU
    // command-buffer failures follow the backend's existing fatal path.
    virtual bool EncodeComputeSkinJobs(const std::vector<R4ComputeSkinJob>&,
        std::string& error)
    { error = "compute skinning is unsupported by this backend"; return false; }
    virtual bool SubmitFrame(std::string& error) = 0;
    // A transient drawable loss at late submit discards the unsubmitted frame.
    // Retry preserves device readiness; no history or serial was committed.
    // Cleared by the next BeginFrame/SubmitFrame. Other failures stay fatal.
    virtual bool LastFrameSubmitWasRetry() const { return false; }
    // Discard an acquired, unsubmitted command buffer, including partial
    // encoders. No GPU writes or temporal producer histories are committed.
    virtual void DiscardFrame() = 0;
    virtual bool ValidateSunShadowCascade(const R4SunShadowValidationDesc&, std::string&) = 0;
    virtual bool ValidateLocalShadowBatch(const R4LocalShadowBatchValidationDesc&,
        const std::shared_ptr<IR4Texture>&, const std::shared_ptr<IR4Texture>&,
        const std::shared_ptr<IR4Texture>&, const std::shared_ptr<IR4Texture>&, std::string& error)
    { error="local shadow batch validation is unsupported by this backend"; return false; }
    virtual bool ValidateTemporalTextures(const R4TemporalTextureValidationDesc&, std::string&) = 0;
    virtual bool ValidateStaticMotionVectors(const R4StaticMotionValidationDesc& descriptor,
        std::string& error) = 0;
    virtual bool CopyLatestCompletedSaveThumbnail(R4SaveThumbnailSnapshot& snapshot) const = 0;

    // Scene-owned callbacks may issue bounded draw/bind commands into the
    // already-open G-buffer pass.  Implementations execute the callback while
    // rejecting attempts to begin/end a pass or submit the frame; the callback
    // cannot disable this bracket itself. Attachments remain scene-owned.
    virtual bool RunSceneOwnedGBufferCallback(
        const std::function<bool(IR4Backend&, std::string&)>& callback,
        std::string& error) = 0;

    virtual std::shared_ptr<IR4GraphicsPipeline> CreateGraphicsPipeline(
        const R4GraphicsPipelineDesc& descriptor,
        std::string& error) = 0;
    virtual bool BindGraphicsPipeline(
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
        std::string& error) = 0;
    virtual bool BindVertexBuffer(
        std::uint32_t index,
        const std::shared_ptr<IR4Buffer>& buffer,
        std::size_t offset,
        std::string& error) = 0;
    virtual bool BindFragmentTexture(
        std::uint32_t index,
        const std::shared_ptr<IR4Texture>& texture,
        std::string& error) = 0;
    virtual bool BindFragmentSampler(
        std::uint32_t index,
        const std::shared_ptr<IR4Sampler>& sampler,
        std::string& error) = 0;
    virtual bool BindMscResources(
        const R4MscStageBindings& bindings,
        std::string& error) = 0;
    virtual bool SetStencilReference(std::uint32_t reference, std::string& error) = 0;
    virtual bool DrawIndexed(const R4IndexedDrawDesc& descriptor, std::string& error) = 0;
    // Draws normally and optionally records a depth/stencil visibility count.
    // Unsupported backends preserve output while reporting no receipt.
    virtual bool DrawIndexedWithVisibility(const R4IndexedDrawDesc& descriptor,
        const R4WholeLightVisibilitySample&, bool& resultEncoded, std::string& error)
    {
        resultEncoded = false;
        return DrawIndexed(descriptor, error);
    }
    virtual bool SubmitEngineWorldRenderables(
		const std::vector<R4EngineWorldRenderable>& renderables,
		std::uint32_t engineFrame,
		std::string& error) = 0;
	// Returns an immutable, independently owned copy of the latest engine
	// world submission. A null result means no submission has occurred yet.
    virtual std::shared_ptr<const R4EngineWorldSnapshot> EngineWorldSnapshot() const = 0;
    virtual bool SubmitEngineHudRenderables(const R4EngineHudSnapshot& snapshot,
        std::string& error) = 0;
    virtual std::shared_ptr<const R4EngineHudSnapshot> EngineHudSnapshot() const = 0;
	virtual bool SubmitEngineLightingFrame(const R4LightingFrameSnapshot& snapshot,
		std::string& error) = 0;
	virtual std::shared_ptr<const R4LightingFrameSnapshot> EngineLightingFrameSnapshot() const = 0;
    // A self-contained UI batch is encoded after all scene presentation passes
    // and before frame submission. Implementations must skip unsupported
    // textures per draw rather than letting UI data invalidate the world frame.
    virtual bool RenderUIOverlay(const xray::metal::ui::FrameBatch& batch, std::string& error) = 0;
    virtual std::size_t GraphicsPipelineCacheSize() const = 0;

    // The callback runs on the render thread only after the current frame's
    // command buffer has completed and its frame context is reused or drained.
    virtual bool DeferRelease(std::function<void()> release, std::string& error) = 0;
    virtual IR4Resources& Resources() = 0;

    virtual void Shutdown() = 0;

    virtual const char* DeviceName() const = 0;
    virtual void DrawableSize(std::uint32_t& width, std::uint32_t& height) const = 0;
    virtual std::uint64_t LastSubmittedSerial() const = 0;
    virtual std::uint64_t CompletedSerial() const = 0;
    virtual bool HasCommandBufferError() const = 0;
    virtual bool SupportsBCTextureCompression() const = 0;
    virtual std::uint32_t FrameContextCount() const = 0;
    // Stable index of the frame context whose command buffer is currently
    // being encoded. Per-frame mutable resources must key off this value,
    // never an unrelated engine tick modulo the in-flight count.
    virtual std::uint32_t ActiveFrameContextIndex() const { return 0u; }
    virtual const DisplayCapabilities& GetDisplayCapabilities() const = 0;
};

std::unique_ptr<IR4Backend> CreateMetalR4Backend();

// E02 GPU ownership probe. The old-path mode intentionally returns false when
// the pre-submit Shared-buffer overwrite is reproduced.
bool RunMetalR4FrameUploadGpuProbe(bool oldPath, std::string& error);
bool RunMetalR4ComputeSkinBackendGpuProbe(std::string& error);
bool RunMetalR4WholeLightVisibilityGpuProbe(std::string& error);
// Small offscreen regression for abort/replay ownership and bounded recovery.
bool RunMetalR4PrivateUploadRecoveryProbe(std::string& error);

// E05 production-MSC local-light rollover probe. The shader directory must
// contain the converted R4 local-light bundle; image/report artifacts are
// written below artifactDirectory.
bool RunMetalR4LocalLightRolloverGpuProbe(
    const std::string& shaderDirectory,
    const std::string& artifactDirectory,
    std::size_t lightCount,
    std::string& error);

// E04 CPU/GPU screen-space reprojection and motion vector accuracy probe (gate <= 0.5px).
bool RunMetalR4ReprojectionProbe(std::string& error);
