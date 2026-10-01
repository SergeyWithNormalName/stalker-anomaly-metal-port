#include "../xrRenderPC_R4/stdafx.h"

#include "MetalRenderDeviceRender.h"
#include "MetalR4SunCascadeCapture.h"
#include "MetalR4VolumetricCapture.h"
#include "R4GameplayFrameAdmission.h"
#include "MetalR4FloraMaterialCapture.h"
#include "../xrRender/Blender_BmmD.h"
#include "../xrRender/FBasicVisual.h"
#include "../xrRender/MetalParticleCapture.h"
#include "../xrRender/MetalRainCapture.h"
#include "../xrRender/MetalLensFlareCapture.h"
#include "../xrRender/light.h"
#include "../xrRender/xrRender_console.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/xr_efflensflare.h"

#include <SDL.h>

extern "C" void MacOS_ActivateAppWindow(void* sdlWindow);

#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#include <vector>
#include <utility>

extern Fvector4 ps_ssfx_lightsetup_1;
extern Fvector4 ps_ssfx_pom;
extern Fvector4 ps_ssfx_hud_drops_1;
extern Fvector4 ps_ssfx_hud_drops_2;
extern int scope_3D_fake_enabled;
extern u32 psCurrentVidMode[];
extern XRAPI_API xr_token* vid_mode_token;

// Source LOD controls captured before the value-only OmniFace setup.
extern float r_ssaGLOD_start, r_ssaGLOD_end;
extern ENGINE_API BOOL g_appLoaded;

namespace
{
// shared_str uses nullptr, rather than an empty character array, for an absent
// value.  The Direct Metal frame ABI owns std::string values, so every bridge
// conversion must normalize the engine's optional representation before a
// standard-library string operation can call strlen(nullptr).
const char* shared_str_or_empty(const shared_str& value)
{
	const char* text = value.c_str();
	return text ? text : "";
}

struct _uniq_mode
{
	_uniq_mode(LPCSTR v) : _val(v) {}
	LPCSTR _val;
	bool operator()(LPCSTR _m) const { return !xr_strcmp(_val, _m); }
};

void free_metal_vid_mode_list()
{
	if (!vid_mode_token) return;
	for (int i = 0; vid_mode_token[i].name; i++)
	{
		xr_free(vid_mode_token[i].name);
	}
	xr_free(vid_mode_token);
	vid_mode_token = NULL;
}

bool apply_metal_benchmark_window_override(SDL_Window* window)
{
    const char* benchmarkSize = std::getenv("XRAY_BENCHMARK_LOGICAL_SIZE");
    if (!benchmarkSize)
        return true;

    unsigned logicalWidth = 0;
    unsigned logicalHeight = 0;
    if (std::sscanf(benchmarkSize, "%ux%u", &logicalWidth, &logicalHeight) != 2 ||
        logicalWidth == 0u || logicalHeight == 0u)
    {
        Msg("! [Metal] invalid XRAY_BENCHMARK_LOGICAL_SIZE: %s", benchmarkSize);
        return false;
    }

    SDL_SetWindowSize(window, static_cast<int>(logicalWidth), static_cast<int>(logicalHeight));
    psCurrentVidMode[0] = logicalWidth;
    psCurrentVidMode[1] = logicalHeight;
    Msg("* [Metal] benchmark logical window override: %ux%u", logicalWidth, logicalHeight);
    return true;
}

bool temporalLifecycleProbeEnabled()
{
    const char* value = std::getenv("XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE");
    return value && std::strcmp(value, "1") == 0;
}

R4GameplayFrameAdmission gameplayFrameAdmission(const MetalR4EngineScene& scene,
    const MetalR4EngineScene::ViewState& view, const R4EnvironmentSnapshot& environment,
    const IR4Backend* backend, const bool levelReady,
    const bool requireWorldLightingSnapshots = true)
{
    const auto world = backend ? backend->EngineWorldSnapshot() : nullptr;
    const auto lighting = backend ? backend->EngineLightingFrameSnapshot() : nullptr;
    return EvaluateR4GameplayFrameAdmission(!g_appLoaded || !g_loading_events.empty(), scene.IsPrepared(),
        levelReady, Device.dwFrame, view.valid, view.engineFrame, environment.valid,
        environment.engineFrame, static_cast<bool>(world), world ? world->engineFrame : 0u,
        static_cast<bool>(lighting), lighting ? lighting->engineFrame : 0u,
        requireWorldLightingSnapshots);
}

struct TemporalLifecycleProbeState
{
    std::uint32_t preparedBeginTicks = 0u;
    bool acquireSkipped = false;
    bool encodeDiscarded = false;
    bool reloadAttempted = false;
    bool multiViewChecked = false;
    bool resizeStarted = false;
    bool resizeRequested = false;
    bool resizeChanged = false;
    bool resizeRestored = false;
    bool nativeRetinaValueSaved = false;
    bool originalNativeRetinaPresent = false;
    bool nativeRetinaForcedOff = false;
    std::string originalNativeRetinaValue;
    std::uint32_t originalWidth = 0u;
    std::uint32_t originalHeight = 0u;
    std::uint32_t probeWidth = 0u;
    std::uint32_t probeHeight = 0u;
};

TemporalLifecycleProbeState& temporalLifecycleProbeState()
{
    static TemporalLifecycleProbeState state;
    return state;
}

bool disableTemporalProbeNativeRetina(TemporalLifecycleProbeState& state)
{
    if (!state.nativeRetinaValueSaved)
    {
        const char* value = std::getenv("XRAY_METAL_NATIVE_RETINA");
        state.originalNativeRetinaPresent = value != nullptr;
        if (value)
            state.originalNativeRetinaValue = value;
        state.nativeRetinaValueSaved = true;
    }
    if (::setenv("XRAY_METAL_NATIVE_RETINA", "0", 1) != 0)
        return false;
    state.nativeRetinaForcedOff = true;
    return true;
}

bool restoreTemporalProbeNativeRetina(TemporalLifecycleProbeState& state,
    const std::uint32_t engineFrame)
{
    if (!state.nativeRetinaValueSaved || !state.nativeRetinaForcedOff)
        return true;
    const int result = state.originalNativeRetinaPresent ?
        ::setenv("XRAY_METAL_NATIVE_RETINA", state.originalNativeRetinaValue.c_str(), 1) :
        ::unsetenv("XRAY_METAL_NATIVE_RETINA");
    std::fprintf(stderr,
        "METAL_TEMPORAL_LIFECYCLE_PROBE event=restore_native_retina_env engine_frame=%u original_present=%u result=%s\n",
        engineFrame, state.originalNativeRetinaPresent ? 1u : 0u,
        result == 0 ? "restored" : "failed");
    if (result == 0)
    {
        state.nativeRetinaForcedOff = false;
    }
    return result == 0;
}

void fill_metal_vid_mode_list(SDL_Window* window)
{
	if (vid_mode_token != NULL) return;
	xr_vector<LPCSTR> _tmp;

	int display_index = 0;
	if (window)
	{
		const int window_display = SDL_GetWindowDisplayIndex(window);
		if (window_display >= 0)
			display_index = window_display;
	}

	const int mode_count = SDL_GetNumDisplayModes(display_index);
	for (int i = 0; i < mode_count; ++i)
	{
		SDL_DisplayMode mode{};
		if (SDL_GetDisplayMode(display_index, i, &mode) != 0 || mode.w < 800 || mode.h < 600)
			continue;

		string32 str;
		xr_sprintf(str, sizeof(str), "%dx%d", mode.w, mode.h);
		if (_tmp.end() != std::find_if(_tmp.begin(), _tmp.end(), _uniq_mode(str)))
			continue;

		_tmp.push_back(xr_strdup(str));
	}

	if (_tmp.empty())
	{
		SDL_DisplayMode desktop_mode{};
		if (SDL_GetDesktopDisplayMode(display_index, &desktop_mode) == 0 &&
			desktop_mode.w > 0 && desktop_mode.h > 0)
		{
			string32 str;
			xr_sprintf(str, sizeof(str), "%dx%d", desktop_mode.w, desktop_mode.h);
			_tmp.push_back(xr_strdup(str));
		}
		else if (psCurrentVidMode[0] > 0 && psCurrentVidMode[1] > 0)
		{
			string32 str;
			xr_sprintf(str, sizeof(str), "%dx%d", psCurrentVidMode[0], psCurrentVidMode[1]);
			_tmp.push_back(xr_strdup(str));
		}
	}

	const u32 _cnt = static_cast<u32>(_tmp.size()) + 1u;
	vid_mode_token = xr_alloc<xr_token>(_cnt);
	vid_mode_token[_cnt - 1].id = -1;
	vid_mode_token[_cnt - 1].name = NULL;

	for (u32 i = 0; i < _tmp.size(); ++i)
	{
		vid_mode_token[i].id = static_cast<int>(i);
		vid_mode_token[i].name = _tmp[i];
	}
	Msg("* [Metal] Populated %zu video modes for display %d", _tmp.size(), display_index);
}

struct MetalHudTransformHistoryEntry
{
	std::string assetName;
	std::array<float, 16> transform{};
	std::size_t boneCount = 0u;
	std::uint64_t contentHash = 0u;
	std::vector<std::uint32_t> eligibleGeometry;
	std::vector<std::uint32_t> scopeGeometryRoles;
};

struct MetalHudTransformHistory
{
	u32 frame = u32(-1);
	std::vector<MetalHudTransformHistoryEntry> renderables;
};

// This lives at the value-only engine-to-backend boundary. It intentionally
// carries neither visual nor skeleton pointers: the HUD capture can rebuild or
// reorder leaves when an item changes, in which case the bridge rejects the
// old history and sends current -> previous for every new ordinal.
MetalHudTransformHistory& hudTransformHistory()
{
	static MetalHudTransformHistory history;
	return history;
}

void resetHudTransformHistory()
{
	hudTransformHistory() = {};
}

bool matchesHudTransformHistory(const MetalHudTransformHistory& history,
	const xr_vector<CDSGraphManager::MetalHudRenderable>& renderables, const u32 frame)
{
	if (frame == u32(-1) || history.frame == u32(-1) || frame <= history.frame ||
		history.renderables.size() != renderables.size())
		return false;
	for (std::size_t ordinal = 0u; ordinal < renderables.size(); ++ordinal)
	{
		const CDSGraphManager::MetalHudRenderable& current = renderables[ordinal];
		const MetalHudTransformHistoryEntry& previous = history.renderables[ordinal];
		const std::size_t currentBoneCount = current.boneTransforms ? current.boneTransforms->size() : 0u;
		const std::uint64_t currentContentHash = current.asset ? current.asset->contentHash : 0u;
		std::vector<std::uint32_t> scopeGeometryRoles;
		scopeGeometryRoles.reserve(current.scopeLeaves.size());
		for (const CDSGraphManager::MetalHudRenderable::ScopeLeaf& leaf : current.scopeLeaves)
			scopeGeometryRoles.push_back((leaf.geometryIndex << 2u) | std::uint32_t(leaf.role));
		if (previous.assetName != shared_str_or_empty(current.assetName) ||
			previous.boneCount != currentBoneCount ||
			previous.contentHash != currentContentHash ||
			previous.eligibleGeometry != std::vector<std::uint32_t>(
				current.eligibleGeometryIndices.begin(), current.eligibleGeometryIndices.end()) ||
			previous.scopeGeometryRoles != scopeGeometryRoles)
			return false;
	}
	return true;
}

void updateHudTransformHistory(MetalHudTransformHistory& history,
	const std::vector<R4EngineHudRenderable>& renderables, const u32 frame)
{
	history.frame = frame;
	history.renderables.clear();
	history.renderables.reserve(renderables.size());
	for (const R4EngineHudRenderable& renderable : renderables)
	{
		MetalHudTransformHistoryEntry entry;
		entry.assetName = renderable.assetName;
		entry.transform = renderable.transform;
		entry.boneCount = renderable.boneTransforms ? renderable.boneTransforms->size() : 0u;
		entry.contentHash = renderable.asset ? renderable.asset->contentHash : 0u;
		entry.eligibleGeometry = renderable.eligibleGeometryIndices;
		entry.scopeGeometryRoles.reserve(renderable.scopeLeaves.size());
		for (const R4EngineHudRenderable::ScopeLeaf& leaf : renderable.scopeLeaves)
			entry.scopeGeometryRoles.push_back(
				(leaf.geometryIndex << 2u) | std::uint32_t(leaf.role));
		history.renderables.emplace_back(std::move(entry));
	}
}

bool LoadTerrainShaderMaterials(LPCSTR shaderLibrary,
    std::vector<MetalR4EngineScene::TerrainShaderMaterial>& materials,
    std::string& error)
{
    materials.clear();
    IReader* library = FS.r_open(shaderLibrary);
    if (!library)
    {
        error = "cannot open the archive-winning shaders.xr library";
        return false;
    }
    IReader* blenderChunks = library->open_chunk(2u);
    if (!blenderChunks)
    {
        FS.r_close(library);
        error = "shaders.xr has no blender chunk";
        return false;
    }
    std::unordered_set<std::string> unique;
    for (u32 chunkId = 0u;; ++chunkId)
    {
        IReader* chunk = blenderChunks->open_chunk(chunkId);
        if (!chunk)
            break;
        CBlender_DESC description;
        if (chunk->elapsed() < sizeof(description))
        {
            chunk->close();
            blenderChunks->close();
            FS.r_close(library);
            error = "shaders.xr contains a truncated blender record";
            return false;
        }
        chunk->r(&description, sizeof(description));
        const std::string shaderName(description.cName);
        if (description.CLS == B_BmmD && shaderName.rfind("levels\\", 0u) == 0u)
        {
            if (description.version > 3u)
            {
                chunk->close();
                blenderChunks->close();
                FS.r_close(library);
                error = "shaders.xr terrain blender version is newer than the native reader";
                return false;
            }
            CBlender_BmmD blender;
            chunk->seek(0u);
            blender.Load(*chunk, description.version);
            if (!unique.emplace(shaderName).second)
            {
                chunk->close();
                blenderChunks->close();
                FS.r_close(library);
                error = "shaders.xr contains a duplicate level terrain blender";
                return false;
            }
            MetalR4EngineScene::TerrainShaderMaterial material;
            material.shaderName = shaderName;
            material.detailTextures = {
                blender.oR_Name, blender.oG_Name, blender.oB_Name, blender.oA_Name};
            if (std::any_of(material.detailTextures.begin(), material.detailTextures.end(),
                    [](const std::string& identity) {
                        return identity.empty() || identity == "$null";
                    }))
            {
                chunk->close();
                blenderChunks->close();
                FS.r_close(library);
                error = "shaders.xr terrain blender has an empty detail channel";
                return false;
            }
            materials.emplace_back(std::move(material));
        }
        chunk->close();
    }
    blenderChunks->close();
    FS.r_close(library);
    if (materials.empty())
    {
        error = "shaders.xr contains no level terrain blenders";
        return false;
    }
    std::fprintf(stderr, "METAL_TERRAIN_BLENDER_LIBRARY materials=%zu source=shaders.xr\n",
        materials.size());
    return true;
}
} // namespace

MetalRenderDeviceRender::MetalRenderDeviceRender() = default;

MetalRenderDeviceRender::~MetalRenderDeviceRender()
{
    ShutdownBackend();
}

void MetalRenderDeviceRender::SubmitEnvironmentSnapshot(const R4EnvironmentSnapshot& snapshot)
{
    if (snapshot.valid && !environmentSnapshot.valid)
    {
        Msg("* METAL_ENGINE_ENVIRONMENT_READY frame=%u sky=%s,%s env=%s,%s",
            snapshot.engineFrame, snapshot.sky0TextureName.c_str(), snapshot.sky1TextureName.c_str(),
            snapshot.environment0TextureName.c_str(), snapshot.environment1TextureName.c_str());
    }
    environmentSnapshot = snapshot;
}

void MetalRenderDeviceRender::SubmitGameplayVisionSnapshot(
    const R4GameplayVisionSnapshot& submitted)
{
    // The game owns all authority here.  Normalize only the effective gates so
    // a missing control actor, stale caller, or malformed phase cannot enable a
    // Metal effect by accident; retain the raw legacy selectors for telemetry.
    R4GameplayVisionSnapshot snapshot = submitted;
    const bool gameplayReady = snapshot.valid && snapshot.gameplayStateValid;
    snapshot.nightVisionActive = gameplayReady && snapshot.nightVisionActive &&
        snapshot.nightVisionPhase > 0;
    snapshot.heatVisionActive = gameplayReady && snapshot.heatVisionActive &&
        snapshot.heatVisionPhase > 0;
    snapshot.nightVisionIntensity = snapshot.nightVisionActive ? 1.0f : 0.0f;
    snapshot.heatVisionIntensity = snapshot.heatVisionActive
        ? static_cast<float>(snapshot.heatVisionPhase) : 0.0f;

    const bool changed = !gameplayVisionSnapshotLogged ||
        gameplayVisionSnapshot.valid != snapshot.valid ||
        gameplayVisionSnapshot.gameplayStateValid != snapshot.gameplayStateValid ||
        gameplayVisionSnapshot.nightVisionPhase != snapshot.nightVisionPhase ||
        gameplayVisionSnapshot.heatVisionPhase != snapshot.heatVisionPhase ||
        gameplayVisionSnapshot.heatVisionMode != snapshot.heatVisionMode ||
        gameplayVisionSnapshot.shaderParam7 != snapshot.shaderParam7 ||
        gameplayVisionSnapshot.shaderParam8 != snapshot.shaderParam8 ||
        gameplayVisionSnapshot.nightVisionActive != snapshot.nightVisionActive ||
        gameplayVisionSnapshot.heatVisionActive != snapshot.heatVisionActive ||
        gameplayVisionSnapshot.actorNightVisionRequested != snapshot.actorNightVisionRequested ||
        gameplayVisionSnapshot.actorNightVisionEffectorActive != snapshot.actorNightVisionEffectorActive;
    if (changed || snapshot.engineFrame % 300u == 0u)
    {
        Msg("* METAL_GAMEPLAY_VISION frame=%u gameplay=%u night={phase=%d requested=%u effector=%u active=%u intensity=%.1f param7=%.3f,%.3f param8=%.3f,%.3f,%.3f,%.3f} heat={phase=%d mode=%.3f active=%u intensity=%.1f}",
            snapshot.engineFrame, snapshot.gameplayStateValid ? 1u : 0u,
            snapshot.nightVisionPhase, snapshot.actorNightVisionRequested ? 1u : 0u,
            snapshot.actorNightVisionEffectorActive ? 1u : 0u,
            snapshot.nightVisionActive ? 1u : 0u, snapshot.nightVisionIntensity,
            snapshot.shaderParam7[0], snapshot.shaderParam7[1],
            snapshot.shaderParam8[0], snapshot.shaderParam8[1],
            snapshot.shaderParam8[2], snapshot.shaderParam8[3],
            snapshot.heatVisionPhase, snapshot.heatVisionMode,
            snapshot.heatVisionActive ? 1u : 0u, snapshot.heatVisionIntensity);
        gameplayVisionSnapshotLogged = true;
    }
    gameplayVisionSnapshot = snapshot;
}

bool MetalRenderDeviceRender::CopyGameplayVisionSnapshotForFrame(
    const u32 engineFrame, R4GameplayVisionSnapshot& snapshot) const
{
    // A caller must opt in to a same-frame snapshot.  This makes skipped game
    // updates fail closed instead of replaying the previous actor's effect.
    if (!gameplayVisionSnapshot.valid ||
        gameplayVisionSnapshot.engineFrame != engineFrame)
    {
        snapshot = {};
        return false;
    }
    snapshot = gameplayVisionSnapshot;
    return true;
}

void SubmitR4GameplayVisionSnapshot(const R4GameplayVisionSnapshot& snapshot)
{
    if (!Device.m_pRender)
        return;
    static_cast<MetalRenderDeviceRender*>(Device.m_pRender)
        ->SubmitGameplayVisionSnapshot(snapshot);
}

void MetalRenderDeviceRender::Copy(IRenderDeviceRender&)
{
    // Device renderers are process-owned.  Copying an active Metal command
    // lifecycle would duplicate ownership of the drawable and is unsupported.
}

void MetalRenderDeviceRender::setGamma(float) {}
void MetalRenderDeviceRender::setBrightness(float) {}
void MetalRenderDeviceRender::setContrast(float) {}
void MetalRenderDeviceRender::updateGamma() {}

void MetalRenderDeviceRender::OnDeviceDestroy(BOOL)
{
    ShutdownBackend();
}

void MetalRenderDeviceRender::ValidateHW()
{
    if (!backend)
        deviceState = dsLost;
}

void MetalRenderDeviceRender::DestroyHW()
{
    ShutdownBackend();
}

void MetalRenderDeviceRender::Reset(HWND hWnd, u32& width, u32& height, float& halfWidth, float& halfHeight)
{
    if (!backend)
    {
        deviceState = dsLost;
        return;
    }

    if (frameActive)
    {
        lastError = "Metal reset requested with an active frame";
        deviceState = dsLost;
        return;
    }

    backend->ResizeDrawable(psCurrentVidMode[0], psCurrentVidMode[1]);
    scene.InvalidateHistory();
    UpdateDimensions(width, height, halfWidth, halfHeight);
    fill_metal_vid_mode_list(reinterpret_cast<SDL_Window*>(hWnd));
    deviceState = backend->HasCommandBufferError() ? dsLost : dsOK;
}

void MetalRenderDeviceRender::SetupStates() {}

void MetalRenderDeviceRender::OnDeviceCreate(LPCSTR)
{
    // CRender owns the R4 level lifecycle.  Its Metal branch intentionally
    // avoids D3D target allocation, but it must still be created before the
    // first engine frame reaches Render().
    if (::Render)
        ::Render->create();
    if (Device.Statistic)
        Device.Statistic->OnDeviceCreate();
}

bool MetalRenderDeviceRender::LoadLevelScene(
    const std::filesystem::path& levelDirectory)
{
    if (!backend || !sceneOptionsReady || levelDirectory.empty())
    {
        lastError = "Metal level scene load requires a ready backend, options, and level directory";
        ReportFailure("Load gameplay level scene", lastError);
        return false;
    }
    if (frameActive)
    {
        lastError = "Metal level scene load requested with an active frame";
        ReportFailure("Load gameplay level scene", lastError);
        return false;
    }
    std::string nativeLevelPath = levelDirectory.generic_string();
    std::replace(nativeLevelPath.begin(), nativeLevelPath.end(), '\\', '/');
    std::filesystem::path normalized =
        std::filesystem::path(nativeLevelPath).lexically_normal();
    while (!normalized.empty() && normalized.filename().empty())
        normalized = normalized.parent_path();
    if (normalized.empty())
    {
        lastError = "Metal level scene directory normalizes to an empty path";
        ReportFailure("Load gameplay level scene", lastError);
        return false;
    }
    if (!preparedLevelDirectory.empty() && normalized == preparedLevelDirectory)
        return true;

    MetalR4EngineScene::Options options = sceneOptions;
    options.levelDirectory = normalized;
    scene.Reset();
    preparedLevelDirectory.clear();
    std::string error;
    if (!scene.Prepare(*backend, options, error))
    {
        lastError = error;
        ReportFailure("Load gameplay level scene", lastError);
        return false;
    }
    preparedLevelDirectory = normalized;
    deviceState = dsOK;
    Msg("* METAL_ENGINE_LEVEL_SCENE_READY level=%s visuals=%llu indices=%llu",
        preparedLevelDirectory.filename().string().c_str(),
        static_cast<unsigned long long>(scene.DrawableVisualCount()),
        static_cast<unsigned long long>(scene.DrawableIndexCount()));
    return true;
}

void MetalRenderDeviceRender::UnloadLevelScene()
{
    if (frameActive)
    {
        lastError = "Metal level scene unload requested with an active frame";
        ReportFailure("Unload gameplay level scene", lastError);
        deviceState = dsLost;
        return;
    }
    scene.Reset();
    preparedLevelDirectory.clear();
    sceneFrameLogged = false;
    sceneCameraLogged = false;
}

bool MetalRenderDeviceRender::CopyLatestCompletedSaveThumbnail(
    R4SaveThumbnailSnapshot& snapshot) const
{
    return backend && backend->CopyLatestCompletedSaveThumbnail(snapshot);
}

void MetalRenderDeviceRender::Create(
    HWND hWnd,
    u32& width,
    u32& height,
    float& halfWidth,
    float& halfHeight,
    bool)
{
	Msg("* METAL_ENGINE_DEVICE_CREATE scene=%s", std::getenv("XRAY_METAL_LEVEL_DIR") ? "configured" : "not_configured");
    ShutdownBackend();
    SDL_Window* const window = reinterpret_cast<SDL_Window*>(hWnd);
    if (!apply_metal_benchmark_window_override(window))
    {
        lastError = "invalid Metal benchmark logical window override";
        deviceState = dsLost;
        ReportFailure("Create", lastError);
        return;
    }
    backend = CreateMetalR4Backend();
    if (!backend)
    {
        lastError = "Metal backend factory returned no backend";
        deviceState = dsLost;
        ReportFailure("Create", lastError);
        return;
    }

    std::string error;
    if (!backend->Initialize(window,
            psCurrentVidMode[0], psCurrentVidMode[1], error))
    {
        ReportFailure("Create", error);
        backend->Shutdown();
        backend.reset();
        deviceState = dsLost;
        return;
    }

    UpdateDimensions(width, height, halfWidth, halfHeight);
    fill_metal_vid_mode_list(window);

    MetalR4EngineScene::Options options;
    if (MetalR4EngineScene::OptionsFromEnvironment(options, error))
    {
        if (options.terrainAutoSelect || !options.terrainVisualIds.empty())
        {
            string_path shaderLibrary;
            FS.update_path(shaderLibrary, "$game_data$", "shaders.xr");
            if (!LoadTerrainShaderMaterials(
                    shaderLibrary, options.terrainShaderMaterials, error))
            {
                Msg("! [Metal] Load production R4 terrain materials skipped: %s", error.c_str());
            }
        }
        if (!options.flora.leafVertexMetallibPath.empty() || !options.flora.trunkVertexMetallibPath.empty())
        {
            string_path shaderLibrary;
            FS.update_path(shaderLibrary,"$game_data$","shaders.xr");
            if (!LoadR4FloraBlenderMaterials(shaderLibrary,options.flora.shaderMaterials,error))
            {
                ReportFailure("LoadFloraBlenderMaterials",error);
                return;
            }
            for (const auto& material:options.flora.shaderMaterials)
                if (material.shaderName=="flora\\leaf_wave" || material.shaderName=="flora\\trunk_wave")
                    std::fprintf(stderr,"METAL_FLORA_BLENDER material=%s alpha=%u object_lod=%u source=shaders.xr\n",
                        material.shaderName.c_str(),material.alphaBlend,material.objectLod);
        }
        sceneOptions = std::move(options);
        sceneOptionsReady = true;
        if (!sceneOptions.levelDirectory.empty())
        {
            Msg("* [Metal] Pre-warming level scene: %s", sceneOptions.levelDirectory.string().c_str());
            if (!LoadLevelScene(sceneOptions.levelDirectory))
            {
                Msg("! [Metal] Optional level scene pre-warm skipped: %s", lastError.c_str());
                scene.Reset();
                preparedLevelDirectory.clear();
            }
        }
        else
        {
            Msg("* METAL_ENGINE_DEVICE_READY scene=deferred");
        }
    }
    else
    {
        Msg("! [Metal] Scene options from environment skipped: %s", error.c_str());
    }
    deviceState = dsOK;
    MacOS_ActivateAppWindow(reinterpret_cast<void*>(hWnd));
}

void MetalRenderDeviceRender::SetupGPU(BOOL, BOOL, BOOL reference)
{
    forceReference = reference;
}

void MetalRenderDeviceRender::overdrawBegin() {}
void MetalRenderDeviceRender::overdrawEnd() {}

void MetalRenderDeviceRender::DeferredLoad(BOOL) {}
void MetalRenderDeviceRender::ResourcesDeferredUpload() {}
void MetalRenderDeviceRender::ResourcesDeferredUnload() {}

void MetalRenderDeviceRender::ResourcesGetMemoryUsage(u32& base, u32& baseCount, u32& lightmaps, u32& lightmapCount)
{
    base = 0;
    baseCount = 0;
    lightmaps = 0;
    lightmapCount = 0;
}

void MetalRenderDeviceRender::ResourcesDestroyNecessaryTextures() {}
void MetalRenderDeviceRender::ResourcesStoreNecessaryTextures() {}
void MetalRenderDeviceRender::ResourcesDumpMemoryUsage() {}
void MetalRenderDeviceRender::ResourcesPrefetchCreateTexture(LPCSTR) {}

bool MetalRenderDeviceRender::HWSupportsShaderYUV2RGB()
{
    return false;
}

IRenderDeviceRender::DeviceState MetalRenderDeviceRender::GetDeviceState()
{
    if (!backend)
    {
        static bool s_loggedNull = false;
        if (!s_loggedNull) { Msg("! [Metal] GetDeviceState: backend is null"); s_loggedNull = true; }
        return dsLost;
    }
    if (backend->HasCommandBufferError())
    {
        static bool s_loggedErr = false;
        if (!s_loggedErr) { Msg("! [Metal] GetDeviceState: HasCommandBufferError"); s_loggedErr = true; }
        return dsLost;
    }
    if (deviceState != dsOK)
    {
        static bool s_loggedState = false;
        if (!s_loggedState) { Msg("! [Metal] GetDeviceState: deviceState=%d", (int)deviceState); s_loggedState = true; }
    }
    return deviceState;
}

BOOL MetalRenderDeviceRender::GetForceGPU_REF()
{
    return forceReference;
}

u32 MetalRenderDeviceRender::GetCacheStatPolys()
{
    return 0;
}

void MetalRenderDeviceRender::Begin()
{
    if (!backend || frameActive)
        return;

    static bool s_firstFrameActivated = false;
    if (!s_firstFrameActivated)
    {
        MacOS_ActivateAppWindow(nullptr);
        s_firstFrameActivated = true;
    }

    if (temporalLifecycleProbeEnabled())
    {
        TemporalLifecycleProbeState& probe = temporalLifecycleProbeState();
        if (probe.resizeRestored && probe.nativeRetinaForcedOff)
            restoreTemporalProbeNativeRetina(probe, Device.dwFrame);
    }

    if (temporalLifecycleProbeEnabled() && scene.IsPrepared() && g_pGameLevel != nullptr &&
        viewState.valid && environmentSnapshot.valid)
    {
        TemporalLifecycleProbeState& probe = temporalLifecycleProbeState();
        ++probe.preparedBeginTicks;
        if (!probe.acquireSkipped && probe.preparedBeginTicks >= 5u)
        {
            probe.acquireSkipped = true;
            std::fprintf(stderr,
                "METAL_TEMPORAL_LIFECYCLE_PROBE event=skip_acquire prepared_begin=%u engine_frame=%u submitted_serial=%llu\n",
                probe.preparedBeginTicks, Device.dwFrame,
                static_cast<unsigned long long>(backend->LastSubmittedSerial()));
            ::setenv("XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY", "1", 1);
        }
        if (!probe.reloadAttempted && probe.preparedBeginTicks >= 10u)
        {
            probe.reloadAttempted = true;
            const std::filesystem::path levelDirectory = preparedLevelDirectory;
            const bool canReload = !levelDirectory.empty();
            bool reloaded = false;
            if (canReload)
            {
                UnloadLevelScene();
                reloaded = LoadLevelScene(levelDirectory);
            }
            if (!reloaded)
                scene.InvalidateHistory();
            std::fprintf(stderr,
                "METAL_TEMPORAL_LIFECYCLE_PROBE event=producer_reload prepared_begin=%u engine_frame=%u result=%s fallback=%s\n",
                probe.preparedBeginTicks, Device.dwFrame,
                reloaded ? "reloaded" : "unavailable",
                reloaded ? "none" : "history_invalidate");
        }
        if (!probe.resizeStarted && probe.preparedBeginTicks >= 25u)
        {
            probe.resizeStarted = true;
            backend->DrawableSize(probe.originalWidth, probe.originalHeight);
            if (probe.originalWidth != 0u && probe.originalHeight != 0u)
            {
                probe.probeWidth = std::max(1u, probe.originalWidth / 2u);
                probe.probeHeight = std::max(1u, probe.originalHeight / 2u);
                if (probe.probeWidth == probe.originalWidth && probe.originalWidth > 1u)
                    --probe.probeWidth;
                if (probe.probeHeight == probe.originalHeight && probe.originalHeight > 1u)
                    --probe.probeHeight;
                probe.resizeRequested = probe.probeWidth != probe.originalWidth ||
                    probe.probeHeight != probe.originalHeight;
            }
            if (probe.resizeRequested)
            {
                const bool retinaDisabled = disableTemporalProbeNativeRetina(probe);
                if (retinaDisabled)
                {
                    backend->ResizeDrawable(probe.probeWidth, probe.probeHeight);
                    std::uint32_t resizedWidth = 0u, resizedHeight = 0u;
                    backend->DrawableSize(resizedWidth, resizedHeight);
                    probe.resizeChanged = resizedWidth != probe.originalWidth ||
                        resizedHeight != probe.originalHeight;
                }
                scene.InvalidateHistory();
            }
            std::fprintf(stderr,
                "METAL_TEMPORAL_LIFECYCLE_PROBE event=resize prepared_begin=%u engine_frame=%u original=%ux%u test=%ux%u changed=%u retina_override=%u\n",
                probe.preparedBeginTicks, Device.dwFrame, probe.originalWidth,
                probe.originalHeight, probe.probeWidth, probe.probeHeight,
                probe.resizeChanged ? 1u : 0u, probe.nativeRetinaForcedOff ? 1u : 0u);
        }
        if (!probe.resizeRestored && probe.preparedBeginTicks >= 30u)
        {
            probe.resizeRestored = true;
            bool restored = !probe.resizeRequested;
            if (probe.resizeRequested)
            {
                // Restore the exact prior policy before asking the backend to
                // return to the original extent. With native-retina enabled,
                // ResizeDrawable() must see the original window backing size.
                const bool retinaRestored = restoreTemporalProbeNativeRetina(
                    probe, Device.dwFrame);
                if (retinaRestored)
                    backend->ResizeDrawable(probe.originalWidth, probe.originalHeight);
                scene.InvalidateHistory();
                std::uint32_t restoredWidth = 0u, restoredHeight = 0u;
                backend->DrawableSize(restoredWidth, restoredHeight);
                restored = restoredWidth == probe.originalWidth &&
                    restoredHeight == probe.originalHeight && retinaRestored;
            }
            std::fprintf(stderr,
                "METAL_TEMPORAL_LIFECYCLE_PROBE event=resize_restore prepared_begin=%u engine_frame=%u extent=%ux%u restored=%u\n",
                probe.preparedBeginTicks, Device.dwFrame, probe.originalWidth,
                probe.originalHeight, restored ? 1u : 0u);
        }
    }

    std::string error;
    switch (backend->BeginFrame(error))
    {
    case R4FrameAcquireResult::Ready:
    {
        frameActive = true;
        const bool levelReady = g_pGameLevel != nullptr && g_pGameLevel->bReady;
        if (gameplayFrameAdmission(scene, viewState, environmentSnapshot, backend.get(), levelReady,
                false) ==
            R4GameplayFrameAdmission::Ready)
        {
            deviceState = dsOK;
            return;
        }
        {
            R4RenderPassDesc clearPass;
            clearPass.label = "Engine Metal clear";
            clearPass.useDrawable = true;
            clearPass.drawableColor.loadAction = R4LoadAction::Clear;
            clearPass.drawableColor.storeAction = R4StoreAction::Store;
            clearPass.drawableColor.clearColor = {0.008, 0.012, 0.020, 1.0};
            if (!backend->BeginRenderPass(clearPass, error))
            {
                ReportFailure("Begin clear pass", error);
                scene.DiscardUnsubmittedFrame();
                backend->DiscardFrame();
                frameActive = false;
                passActive = false;
                deviceState = dsLost;
                return;
            }
            passActive = true;
        }
        deviceState = dsOK;
        return;
    }
    case R4FrameAcquireResult::Retry:
        // There is no drawable this tick.  Do not submit an empty command
        // buffer; the following engine frame will retry acquisition.
        deviceState = dsOK;
        return;
    case R4FrameAcquireResult::Error:
        ReportFailure("Begin", error);
        deviceState = dsLost;
        return;
    }
}

void MetalRenderDeviceRender::Clear() {}

void MetalRenderDeviceRender::End()
{
    if (!backend || !frameActive)
        return;

	double e01SceneMs = 0.0;
	std::string error;
	std::size_t submittedLightingCount = 0u;
	if (!g_appLoaded || !g_loading_events.empty())
	{
		consecutiveSceneRenderFailures = 0u;
		// LoadDraw runs through this renderer while engine-owned level events are
		// still being processed. Present its clear/loading UI frame without
		// recapturing gameplay lighting, HUD, particles, or world renderables.
		if (!passActive)
		{
			R4RenderPassDesc clearPass;
			clearPass.label = "Engine Metal loading clear";
			clearPass.useDrawable = true;
			clearPass.drawableColor.loadAction = R4LoadAction::Clear;
			clearPass.drawableColor.storeAction = R4StoreAction::Store;
			clearPass.drawableColor.clearColor = {0.008, 0.012, 0.020, 1.0};
			if (!backend->BeginRenderPass(clearPass, error))
			{
				ReportFailure("Begin loading clear pass", error);
				backend->DiscardFrame();
				frameActive = false;
				passActive = false;
				deviceState = dsLost;
				return;
			}
			passActive = true;
		}
		if (!backend->EndRenderPass(error))
		{
			ReportFailure("End loading clear pass", error);
			backend->DiscardFrame();
			frameActive = false;
			passActive = false;
			deviceState = dsLost;
			return;
		}
		passActive = false;
		xray::metal::ui::FrameBatch loadingUi = xray::metal::ui::DrainFrameBatch();
		if ((!loadingUi.draws.empty() || loadingUi.droppedDraws != 0u) &&
			!backend->RenderUIOverlay(loadingUi, error))
			Msg("! METAL_LOADING_UI_SKIP error=%s", error.c_str());
		scene.DiscardUnsubmittedFrame();
		if (!backend->SubmitFrame(error))
		{
			ReportFailure("Submit loading frame", error);
			backend->DiscardFrame();
			deviceState = dsLost;
		}
		else
			deviceState = dsOK;
		frameActive = false;
		return;
	}
	{
		xr_vector<CDSGraphManager::MetalLocalLight> capturedLights;
		CDSGraphManager::MetalLocalLightCaptureStats captureStats;
		u32 captureFrame = Device.dwFrame;
		// The Direct Metal level loader intentionally omits the D3D portal graph.
		// Its light octree cannot be queried safely, but this frame-end registry
		// enumeration is the established Direct Metal lifetime boundary. Copy all
		// fields now into value records; nothing below retains a live light pointer.
		captureStats.spatial = u32(RImplementation.v_all_lights.size());
		capturedLights.reserve(RImplementation.v_all_lights.size() * 6u);
		std::unordered_map<u64, light*> lightByIdentity;
		std::unordered_map<u64, light*> lightByParentIdentity;
		lightByIdentity.reserve(RImplementation.v_all_lights.size());
		lightByParentIdentity.reserve(RImplementation.v_all_lights.size());
		static const Fvector omniNorm[6] = {
			{0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f},
			{0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}};
		static const Fvector omniDir[6] = {
			{1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
			{0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};
		for (light* source : RImplementation.v_all_lights)
		{
			if (!source || !source->flags.bActive)
			{
				++captureStats.rejectedInactive;
				continue;
			}
			const auto sourceType = static_cast<IRender_Light::LT>(source->flags.type);
			if (sourceType == IRender_Light::DIRECT || sourceType == IRender_Light::REFLECTED ||
				sourceType == IRender_Light::OMNIPART)
				continue;
			// Match the R2/R4 producer's package-level static-light setting.
			if (source->flags.bStatic && !ps_r2_ls_flags.test(R2FLAG_R1LIGHTS))
				continue;
			const u64 parentIdentity = static_cast<u64>(reinterpret_cast<uintptr_t>(source));
			lightByParentIdentity.emplace(parentIdentity, source);
			const auto append = [&](CDSGraphManager::MetalLocalLight::Type type,
				const Fvector& direction, const Fvector& right, const u8 omniFace)
			{
				CDSGraphManager::MetalLocalLight target;
				target.parentIdentity = parentIdentity;
                target.identity = (parentIdentity << 3u) |
					(omniFace == 0xff ? 0u : u64(omniFace + 1u));
				light* faceSource = source;
				if (omniFace != 0xff && sourceType == IRender_Light::POINT && source->omnipart[omniFace])
					faceSource = source->omnipart[omniFace];
				lightByIdentity.emplace(target.identity, faceSource);
				target.type = type;
				target.position.set(source->position);
				target.direction.set(direction);
				target.right.set(right);
				target.color.set(source->color);
				target.range = source->range;
				target.cone = type == CDSGraphManager::MetalLocalLight::Type::OmniFace ?
					PI_DIV_2 : source->cone;
				target.virtualSize = source->virtual_size;
				target.active = true;
				target.staticLight = source->flags.bStatic;
				target.shadowed = source->flags.bShadow;
				target.hudMode = source->flags.bHudMode;
				target.omniFace = omniFace;
				target.projectorTexture = source->projector_texture;
				if (target.hudMode)
				{
					Device.hud_to_world(target.position);
					Device.hud_to_world_dir(target.direction);
				}
				capturedLights.emplace_back(std::move(target));
			};
			if (sourceType == IRender_Light::POINT && source->flags.bShadow)
			{
				for (u8 face = 0; face != 6; ++face)
				{
					Fvector right;
					right.crossproduct(omniNorm[face], omniDir[face]);
					append(CDSGraphManager::MetalLocalLight::Type::OmniFace, omniDir[face], right, face);
				}
			}
			else if (sourceType == IRender_Light::POINT)
				append(CDSGraphManager::MetalLocalLight::Type::Point, source->direction, source->right, 0xff);
			else if (sourceType == IRender_Light::SPOT)
				append(CDSGraphManager::MetalLocalLight::Type::Spot, source->direction, source->right, 0xff);
		}
		captureStats.captured = u32(capturedLights.size());
		captureStats.admitted = captureStats.captured;
		const bool lightingCaptureCoherent = captureFrame == Device.dwFrame;
		if (!lightingCaptureCoherent)
		{
			// End may run once while the menu is constructing, before Calculate
			// has a gameplay camera to publish. Fail closed for lighting (an empty
			// same-frame snapshot), but never tear down the Metal device. The next
			// Calculate replaces it with the real capture. Shutting down here made
			// a harmless loading frame permanently disable Direct Metal.
			capturedLights.clear();
			captureStats = {};
			captureFrame = Device.dwFrame;
			static u32 lastIncoherentLightingLog = u32(-1);
			if (lastIncoherentLightingLog == u32(-1) ||
				Device.dwFrame - lastIncoherentLightingLog >= 300u)
			{
				lastIncoherentLightingLog = Device.dwFrame;
				Msg("! METAL_LIGHTING_FRAME_FAIL_CLOSED frame=%u reason=capture_missing_or_stale", Device.dwFrame);
			}
		}
		R4LightingFrameSnapshot lightingFrame;
		lightingFrame.engineFrame = captureFrame;
		lightingFrame.ssfxLightSetup1 = {ps_ssfx_lightsetup_1.x, ps_ssfx_lightsetup_1.y,
			ps_ssfx_lightsetup_1.z, ps_ssfx_lightsetup_1.w};
		lightingFrame.shadowDepthScale = ps_r2_ls_depth_scale;
		lightingFrame.shadowDepthBias = ps_r2_ls_depth_bias;
		lightingFrame.lights.reserve(capturedLights.size());
		std::size_t staticPointSpot = 0u;
        // A controlled shadow fixture may isolate real Lua-created engine
        // lamps by their captured world position. No light is manufactured;
        // the normal capture/admission and point-face expansion happen first.
        // This keeps the separately audited atlas overflow out of caster tests.
        Fvector fixtureLightPosition{};
        const char* fixtureLightSelector=std::getenv("XRAY_METAL_SHADOW_FIXTURE_LIGHT_POSITION");
        const bool isolateFixtureLights=fixtureLightSelector &&
            std::sscanf(fixtureLightSelector,"%f,%f,%f",&fixtureLightPosition.x,&fixtureLightPosition.y,&fixtureLightPosition.z)==3;
		for (const CDSGraphManager::MetalLocalLight& source : capturedLights)
		{
            if (isolateFixtureLights && (!source.shadowed || source.staticLight ||
                source.position.distance_to_sqr(fixtureLightPosition)>1.0e-6f)) continue;
			R4EngineLocalLight target;
			target.lightId = source.identity;
            target.parentLightId = source.parentIdentity;
			switch (source.type)
			{
			case CDSGraphManager::MetalLocalLight::Type::Point: target.type = R4EngineLocalLightType::Point; break;
			case CDSGraphManager::MetalLocalLight::Type::Spot: target.type = R4EngineLocalLightType::Spot; break;
			case CDSGraphManager::MetalLocalLight::Type::OmniFace: target.type = R4EngineLocalLightType::OmniFace; break;
			}
			target.position = {source.position.x, source.position.y, source.position.z};
			target.direction = {source.direction.x, source.direction.y, source.direction.z};
			target.right = {source.right.x, source.right.y, source.right.z};
			target.color = {source.color.r, source.color.g, source.color.b, source.color.a};
			target.range = source.range;
			target.cone = source.cone;
			target.virtualSize = source.virtualSize;
			target.specular = u_diffuse2s(source.color.r, source.color.g, source.color.b);
			target.active = source.active;
			target.staticLight = source.staticLight;
			target.shadowed = source.shadowed;
            target.decorative = source.decorative;
            target.cameraDistance = source.cameraDistance;
			target.hudMode = source.hudMode;
			target.omniFace = source.omniFace;
			// shared_str represents an absent authored cookie as nullptr.  The
			// value-only backend contract uses std::string, whose const char*
			// assignment calls strlen; normalize the optional engine value before
			// copying it so point lights and cookie-less spots cannot fault during
			// level startup.
			target.projectorTexture = shared_str_or_empty(source.projectorTexture);
			if (target.type == R4EngineLocalLightType::OmniFace)
			{
				const auto parentLight = lightByParentIdentity.find(source.parentIdentity);
				if (parentLight != lightByParentIdentity.end() && parentLight->second)
				{
					const std::array<float, 3> cameraPosition = {
						Device.vCameraPosition.x, Device.vCameraPosition.y, Device.vCameraPosition.z};
					light& parent = *parentLight->second;
                    target.volumetricEnabled = parent.flags.bVolumetric;
                    target.volumetricQuality = parent.m_volumetric_quality;
                    target.rawColor = {parent.color.r, parent.color.g, parent.color.b};
                    CaptureMetalR4OmniFaceVolumetricValues(target, cameraPosition,
                        {ps_ssfx_volumetric.x, ps_ssfx_volumetric.y,
                            ps_ssfx_volumetric.z, ps_ssfx_volumetric.w},
                        ps_r2_slight_fade, r_ssaGLOD_start, r_ssaGLOD_end);
				}
			}
			else
			{
				const auto sourceLight = lightByIdentity.find(source.identity);
				if (sourceLight != lightByIdentity.end() && sourceLight->second)
				{
					light& engineLight = *sourceLight->second;
					target.volumetricEnabled = engineLight.flags.bVolumetric;
					target.volumetricVisibilityDistance = engineLight.vis.distance;
					target.volumetricQuality = engineLight.m_volumetric_quality;
					target.volumetricIntensity = engineLight.m_volumetric_intensity;
					target.volumetricDistance = engineLight.m_volumetric_distance;
					target.volumetricLod = engineLight.get_LOD();
					target.volumetricPlayerLight = engineLight.sss_is_playerlight;
					target.rawColor = {engineLight.color.r, engineLight.color.g, engineLight.color.b};
					if (engineLight.SpatialComponent)
					{
						target.volumetricSphereCenter = {
							engineLight.SpatialComponent->spatial.sphere.P.x,
							engineLight.SpatialComponent->spatial.sphere.P.y,
							engineLight.SpatialComponent->spatial.sphere.P.z};
						target.volumetricSphereRadius = engineLight.SpatialComponent->spatial.sphere.R;
					}
				}
			}
			if (target.staticLight) ++staticPointSpot;
			lightingFrame.lights.emplace_back(std::move(target));
		}
        if (isolateFixtureLights && Device.dwFrame%5u==0u)
            Msg("* METAL_SHADOW_FIXTURE_LIGHTS frame=%u engine_captured=%zu selected_real_records=%zu position=%.6f,%.6f,%.6f",
                Device.dwFrame,capturedLights.size(),lightingFrame.lights.size(),fixtureLightPosition.x,fixtureLightPosition.y,fixtureLightPosition.z);
		if (!backend->SubmitEngineLightingFrame(lightingFrame, error))
		{
			ReportFailure("Submit engine local lights", error);
			ShutdownBackend();
			deviceState = dsLost;
			return;
		}
		submittedLightingCount = lightingFrame.lights.size();
		if (Device.dwFrame == 0u || Device.dwFrame % 300u == 0u)
			Msg("* METAL_LIGHTING_FRAME frame=%u captured=%u admitted=%u lights=%zu static=%zu sector_fallback=%u rejected(frustum=%u inactive=%u sector=%u hom=%u fog=%u lod=%u)",
				Device.dwFrame, captureStats.captured, captureStats.admitted, lightingFrame.lights.size(), staticPointSpot,
				captureStats.sectorFallback,
				captureStats.rejectedFrustum, captureStats.rejectedInactive, captureStats.rejectedSector, captureStats.rejectedHom,
				captureStats.rejectedFog, captureStats.rejectedLod);
	}
	{
		xr_vector<CDSGraphManager::MetalWorldRenderable> engineWorld;
		u32 engineWorldFrame = u32(-1);
		RImplementation.GMBase.copy_metal_world_renderables(engineWorld, engineWorldFrame);

		std::vector<R4EngineWorldRenderable> submitted;
		submitted.reserve(engineWorld.size());
		for (const CDSGraphManager::MetalWorldRenderable& source : engineWorld)
		{
			R4EngineWorldRenderable target;
			target.assetName = shared_str_or_empty(source.assetName);
			static_assert(sizeof(Fmatrix) == sizeof(float) * 16,
				"Metal engine world submission requires a packed Fmatrix");
			std::memcpy(target.transform.data(), &source.transform, sizeof(Fmatrix));
			target.bounds = { source.bounds.P.x, source.bounds.P.y, source.bounds.P.z, source.bounds.R };
			target.visualType = source.visualType;
			target.objectId = source.objectId;
            target.castsShadow = source.castsShadow;
			target.hotness = {source.hotness.x, source.hotness.y, source.hotness.z, source.hotness.w};
			target.material = {source.material.x, source.material.y,
				source.material.z, source.material.w};
			target.hemiCubePositive = {source.hemiCubePositive.x, source.hemiCubePositive.y,
				source.hemiCubePositive.z, source.hemiCubePositive.w};
			target.hemiCubeNegative = {source.hemiCubeNegative.x, source.hemiCubeNegative.y,
				source.hemiCubeNegative.z, source.hemiCubeNegative.w};
			target.boneTransforms = source.boneTransforms;
			if (source.asset)
			{
				auto asset = std::make_shared<R4EngineWorldAsset>();
				asset->sourceName = shared_str_or_empty(source.asset->sourceName);
				asset->contentHash = source.asset->contentHash;
				asset->hasUnresolvedGeometryLinks = source.asset->hasUnresolvedGeometryLinks;
				asset->geometry.reserve(source.asset->geometry.size());
				for (const MetalCpuGeometry& sourceGeometry : source.asset->geometry)
				{
					R4EngineWorldGeometry geometry;
					geometry.shaderName = shared_str_or_empty(sourceGeometry.shaderName);
					geometry.textureName = shared_str_or_empty(sourceGeometry.textureName);
					geometry.visualType = sourceGeometry.visualType;
					geometry.vertexFormat = sourceGeometry.vertexFormat;
					geometry.vertexStride = sourceGeometry.vertexStride;
					geometry.vertexCount = sourceGeometry.vertexCount;
					geometry.indexCount = sourceGeometry.indexCount;
					geometry.skinned = sourceGeometry.skinned;
					geometry.vertexBytes = sourceGeometry.vertexBytes;
					geometry.indexData = sourceGeometry.indexData;
					asset->geometry.push_back(std::move(geometry));
				}
				target.asset = std::move(asset);
			}
			submitted.push_back(std::move(target));
		}
		if (!backend->SubmitEngineWorldRenderables(submitted, engineWorldFrame, error))
		{
			ReportFailure("Submit engine world renderables", error);
			ShutdownBackend();
			deviceState = dsLost;
			return;
		}
		// This is the capture contract: automation must not treat a menu/loading
		// frame as evidence.  A valid gameplay world, same-frame lighting
		// snapshot and non-empty camera scene are all required before capture.
		if (engineWorldFrame == Device.dwFrame && !submitted.empty() && submittedLightingCount != 0u)
		{
			static u32 lastLightingReadyFrame = u32(-1);
			if (lastLightingReadyFrame == u32(-1) || Device.dwFrame - lastLightingReadyFrame >= 300u)
			{
				lastLightingReadyFrame = Device.dwFrame;
				Msg("* METAL_LIGHTING_CAPTURE_READY frame=%u world=%zu local_lights=%zu", Device.dwFrame,
					submitted.size(), submittedLightingCount);
			}
		}
	}
	{
		xr_vector<CDSGraphManager::MetalHudRenderable> engineHud;
		Fmatrix view, projection, previousView, previousProjection;
		CDSGraphManager::MetalHudCaptureStats hudStats;
		CDSGraphManager::MetalHudScopeGlobals scopeGlobals;
		u32 hudFrame = u32(-1);
		RImplementation.GMBase.copy_metal_hud_renderables(engineHud, view, projection,
			previousView, previousProjection, hudStats, scopeGlobals, hudFrame);
		MetalHudTransformHistory& previousHud = hudTransformHistory();
		const bool hasMatchingHudHistory = matchesHudTransformHistory(previousHud, engineHud, hudFrame);
		R4EngineHudSnapshot submitted;
		submitted.engineFrame = hudFrame;
		std::memcpy(submitted.view.data(), &view, sizeof(view));
		std::memcpy(submitted.projection.data(), &projection, sizeof(projection));
		std::memcpy(submitted.previousView.data(), &previousView, sizeof(previousView));
		std::memcpy(submitted.previousProjection.data(), &previousProjection, sizeof(previousProjection));
		auto copyVector4 = [](const Fvector4& source, std::array<float, 4>& target)
		{
			target = {source.x, source.y, source.z, source.w};
		};
		copyVector4(ps_ssfx_pom, submitted.ssfxPom);
		copyVector4(ps_ssfx_hud_drops_1, submitted.ssfxHudDrops1);
		copyVector4(ps_ssfx_hud_drops_2, submitted.ssfxHudDrops2);
		copyVector4(scopeGlobals.hudParams, submitted.hudParams);
		copyVector4(scopeGlobals.hudFovParams, submitted.hudFovParams);
		copyVector4(scopeGlobals.ogseCScreen, submitted.ogseCScreen);
		copyVector4(scopeGlobals.s3dsParam1, submitted.s3dsParam1);
		copyVector4(scopeGlobals.s3dsParam2, submitted.s3dsParam2);
		copyVector4(scopeGlobals.s3dsParam3, submitted.s3dsParam3);
		copyVector4(scopeGlobals.s3dsParam4, submitted.s3dsParam4);
		copyVector4(scopeGlobals.markswitchCurrent, submitted.markswitchCurrent);
		copyVector4(scopeGlobals.markswitchColor, submitted.markswitchColor);
		copyVector4(scopeGlobals.shaderParam8, submitted.shaderParam8);
		submitted.captureRequested = hudStats.requested;
		submitted.captureAdmitted = hudStats.admitted;
		submitted.captureAggregateFallback = hudStats.aggregateFallback;
		submitted.captureStrict = hudStats.strict;
		submitted.captureScopeForward = hudStats.scopeForward;
		submitted.captureScopeBack = hudStats.scopeBack;
		submitted.captureScopeZwrite = hudStats.scopeZwrite;
		submitted.captureScopeReticlePrecise = hudStats.scopeReticlePrecise;
		submitted.captureScopeTriplets = hudStats.scopeTriplets;
		submitted.captureScopePartialRejected = hudStats.scopePartialRejected;
		submitted.captureScopeDuplicateRejected = hudStats.scopeDuplicateRejected;
		submitted.captureNonHq = hudStats.nonHq;
		submitted.captureDistortFollowup = hudStats.distortFollowup;
		submitted.captureEmissiveFollowup = hudStats.emissiveFollowup;
		submitted.captureWmarkFollowup = hudStats.wmarkFollowup;
		submitted.scope3DFakeEnabled = scope_3D_fake_enabled != 0;
		submitted.valid = hudFrame != u32(-1);
		for (const CDSGraphManager::MetalHudRenderable& source : engineHud)
		{
			const std::size_t ordinal = submitted.renderables.size();
			R4EngineHudRenderable target;
			target.assetName = shared_str_or_empty(source.assetName);
			std::memcpy(target.transform.data(), &source.transform, sizeof(source.transform));
			// The capture-side default is current -> previous. Reuse the prior
			// transform only for the same ordinal in an unchanged HUD layout; this
			// prevents a weapon switch from borrowing another renderable's motion.
			std::memcpy(target.previousTransform.data(), &source.previousTransform,
				sizeof(source.previousTransform));
			if (hasMatchingHudHistory)
				target.previousTransform = previousHud.renderables[ordinal].transform;
			target.material = {source.material.x, source.material.y, source.material.z, source.material.w};
			target.hotness = {source.hotness.x, source.hotness.y, source.hotness.z, source.hotness.w};
			target.hemiCubePositive = {source.hemiCubePositive.x, source.hemiCubePositive.y,
				source.hemiCubePositive.z, source.hemiCubePositive.w};
			target.hemiCubeNegative = {source.hemiCubeNegative.x, source.hemiCubeNegative.y,
				source.hemiCubeNegative.z, source.hemiCubeNegative.w};
			target.boneTransforms = source.boneTransforms;
			target.previousBoneTransforms = source.previousBoneTransforms;
			target.eligibleGeometryIndices.assign(source.eligibleGeometryIndices.begin(),
				source.eligibleGeometryIndices.end());
            target.emissiveGeometryIndices.assign(source.emissiveGeometryIndices.begin(), source.emissiveGeometryIndices.end());
            target.geometryShaderFlags.reserve(source.geometryShaderFlags.size());
            for (const auto& flags : source.geometryShaderFlags)
                target.geometryShaderFlags.push_back({flags.priority, flags.strictB2F, flags.emissive,
                    flags.distort, flags.wmark, flags.scopeLense});
			target.scopeLeaves.reserve(source.scopeLeaves.size());
			for (const CDSGraphManager::MetalHudRenderable::ScopeLeaf& sourceLeaf : source.scopeLeaves)
			{
				R4EngineHudRenderable::ScopeLeaf targetLeaf;
				targetLeaf.geometryIndex = sourceLeaf.geometryIndex;
				targetLeaf.role = static_cast<R4EngineHudRenderable::ScopeRole>(sourceLeaf.role);
				targetLeaf.selectedShader.priority = sourceLeaf.selectedShader.priority;
				targetLeaf.selectedShader.strictB2F = sourceLeaf.selectedShader.strictB2F;
				targetLeaf.selectedShader.emissive = sourceLeaf.selectedShader.emissive;
				targetLeaf.selectedShader.distort = sourceLeaf.selectedShader.distort;
				targetLeaf.selectedShader.wmark = sourceLeaf.selectedShader.wmark;
				targetLeaf.selectedShader.scopeLense = sourceLeaf.selectedShader.scopeLense;
				target.scopeLeaves.push_back(targetLeaf);
			}
			if (source.asset)
			{
				auto asset = std::make_shared<R4EngineWorldAsset>();
				asset->sourceName = shared_str_or_empty(source.asset->sourceName);
				asset->contentHash = source.asset->contentHash;
				asset->hasUnresolvedGeometryLinks = source.asset->hasUnresolvedGeometryLinks;
				for (const MetalCpuGeometry& geometry : source.asset->geometry)
					asset->geometry.push_back({shared_str_or_empty(geometry.shaderName), shared_str_or_empty(geometry.textureName),
						geometry.visualType, geometry.vertexFormat, geometry.vertexStride, geometry.vertexCount,
						geometry.indexCount, geometry.skinned, geometry.vertexBytes, geometry.indexData});
				target.asset = std::move(asset);
			}
			submitted.renderables.push_back(std::move(target));
		}
		if (!backend->SubmitEngineHudRenderables(submitted, error))
		{
			ReportFailure("Submit engine HUD renderables", error);
			ShutdownBackend(); deviceState = dsLost; return;
		}
		if (submitted.valid)
			updateHudTransformHistory(previousHud, submitted.renderables, hudFrame);
		if (Device.dwFrame == 0u || Device.dwFrame % 300u == 0u)
			Msg("* METAL_HUD_CAPTURE frame=%u requested=%u admitted=%u aggregate=%u strict=%u scope_forward=%u scope_back=%u scope_zwrite=%u scope_reticle_precise=%u scope_triplets=%u scope_partial_rejected=%u scope_duplicate_rejected=%u non_hq=%u ancillary_distort=%u ancillary_emissive=%u ancillary_wmark=%u",
				Device.dwFrame, submitted.captureRequested, submitted.captureAdmitted,
				submitted.captureAggregateFallback, submitted.captureStrict,
				submitted.captureScopeForward, submitted.captureScopeBack, submitted.captureScopeZwrite,
				submitted.captureScopeReticlePrecise, submitted.captureScopeTriplets,
				submitted.captureScopePartialRejected, submitted.captureScopeDuplicateRejected,
				submitted.captureNonHq, submitted.captureDistortFollowup,
				submitted.captureEmissiveFollowup, submitted.captureWmarkFollowup);
	}
	{
		xray::metal::particles::FrameBatch particles =
			xray::metal::particles::DrainFrameBatch();
		if (scene.IsPrepared())
			scene.SubmitParticleFrame(std::move(particles));
	}
    // Run the value-only producers before draining their per-frame captures.
    if (scene.IsPrepared() && g_pGameLevel && g_pGamePersistent)
    {
        CEnvironment& environment = g_pGamePersistent->Environment();
        const bool pp = g_pGamePersistent->OnRenderPPUI_query();
        const bool menu = g_pGamePersistent->m_pMainMenu &&
            g_pGamePersistent->m_pMainMenu->CanSkipSceneRendering();
        const bool called = !pp && !menu;
        static u32 lastWeatherFrame = u32(-1);
        static bool lastCalled = false;
        static float lastDensity = -1.0f;
        if (lastWeatherFrame == u32(-1) || Device.dwFrame < lastWeatherFrame ||
            Device.dwFrame - lastWeatherFrame >= 300u || called != lastCalled ||
            std::abs(environment.CurrentEnv->rain_density - lastDensity) >= 0.2f)
        {
            Msg("* METAL_WEATHER_PRECONDITION frame=%u called=%u pp=%u menu_skip=%u rain_density=%.3f paused=%u lens_id=%s",
                Device.dwFrame, called ? 1u : 0u, pp ? 1u : 0u, menu ? 1u : 0u,
                environment.CurrentEnv->rain_density, environment.m_paused ? 1u : 0u,
                shared_str_or_empty(environment.CurrentEnv->lens_flare_id));
            lastWeatherFrame = Device.dwFrame; lastCalled = called;
            lastDensity = environment.CurrentEnv->rain_density;
        }
        if (called)
        {
            // CRender::Render normally refreshes this CPU frustum before the
            // weather producers; its Metal early return skips that step.
            RImplementation.ViewBase.CreateFromMatrix(Device.mFullTransform,
                FRUSTUM_P_LRTB + FRUSTUM_P_FAR);
            environment.eff_LensFlare->Render(TRUE, FALSE, FALSE);
            if (ps_r2_anomaly_flags.test(R2_AN_FLAG_FLARES) && ps_r2_heatvision == 0)
                environment.RenderFlares();
            environment.RenderLast();
        }
    }

	{
		xray::metal::rain::FrameBatch rain = xray::metal::rain::DrainFrame();
		if (scene.IsPrepared())
			scene.SubmitRainFrame(std::move(rain));
	}
	{
		xray::metal::lensflare::Frame flare = xray::metal::lensflare::Drain();
        if (!flare.draws.empty() && (Device.dwFrame < 100u || Device.dwFrame % 300u == 0u))
            Msg("* METAL_LENS_CAPTURE frame=%u draws=%zu", Device.dwFrame, flare.draws.size());
		if (scene.IsPrepared())
			scene.SubmitLensFlareFrame(std::move(flare));
	}
    if (viewState.valid)
    {
        const float dx = viewState.eye[0] - lastLoggedSceneEye[0];
        const float dy = viewState.eye[1] - lastLoggedSceneEye[1];
        const float dz = viewState.eye[2] - lastLoggedSceneEye[2];
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (!sceneCameraLogged || distSq >= 25.0f)
        {
            Msg("* METAL_ENGINE_CAMERA frame=%u eye=%.3f,%.3f,%.3f forward=%.3f,%.3f,%.3f",
                Device.dwFrame, viewState.eye[0], viewState.eye[1], viewState.eye[2],
                Device.vCameraDirection.x, Device.vCameraDirection.y,
                Device.vCameraDirection.z);
            lastLoggedSceneEye = viewState.eye;
            sceneCameraLogged = true;
        }
        if (sceneCameraLogged && distSq > 2500.0f)
        {
            viewState.cameraCut = true;
        }
        if (Device.dwFrame % 300 == 1 && backend)
        {
            const auto worldSnap = backend->EngineWorldSnapshot();
            const auto lightSnap = backend->EngineLightingFrameSnapshot();
            const auto hudSnap = backend->EngineHudSnapshot();
            Msg("* METAL_FRAME_STAMPS engine=%u camera=%u world=%u light=%u env=%u hud=%u cut=%d",
                Device.dwFrame, viewState.engineFrame,
                worldSnap ? worldSnap->engineFrame : 0,
                lightSnap ? lightSnap->engineFrame : 0,
                environmentSnapshot.engineFrame,
                hudSnap ? hudSnap->engineFrame : 0,
                viewState.cameraCut ? 1 : 0);
        }
    }
    bool gameplaySceneRendered = false;
    const bool levelReady = g_pGameLevel != nullptr && g_pGameLevel->bReady;
    const R4GameplayFrameAdmission admission = gameplayFrameAdmission(
        scene, viewState, environmentSnapshot, backend.get(), levelReady);
    if (scene.IsPrepared() && g_pGameLevel != nullptr && admission == R4GameplayFrameAdmission::Ready)
    {
        if (passActive)
        {
            // Begin may have started a clear pass before the final same-frame
            // snapshots were published. Close it before admitting the world.
            if (!backend->EndRenderPass(error))
            {
                ReportFailure("End loading clear pass", error);
                ShutdownBackend();
                const std::string fatalError = "Metal renderer could not close its loading clear pass: " + error;
                Debug.fatal(DEBUG_INFO, "%s", fatalError.c_str());
                return;
            }
            passActive = false;
        }
        R4GameplayVisionSnapshot vision;
        CopyGameplayVisionSnapshotForFrame(Device.dwFrame, vision);
        scene.SubmitLuminanceBloomThreshold(Device.dwFrame, ps_r2_ls_bloom_threshold);
        scene.SubmitEmissiveFactor(Device.dwFrame,
            g_pGamePersistent->pda_shader_data.pda_display_factor);
        const auto e01SceneStart = std::chrono::steady_clock::now();
        if (viewState.valid && environmentSnapshot.valid)
        {
            Fmatrix view, projection;
            std::memcpy(&view, viewState.view.data(), sizeof(view));
            std::memcpy(&projection, viewState.projection.data(), sizeof(projection));
            Fvector eye{viewState.eye[0],viewState.eye[1],viewState.eye[2]};
            Fvector forward{view._13,view._23,view._33};
            Fvector lightDirection{environmentSnapshot.adaptedSunDirection[0],
                environmentSnapshot.adaptedSunDirection[1],environmentSnapshot.adaptedSunDirection[2]};
            // Light_DB positions the adapted sun 500m upstream of this camera.
            Fvector lightPosition; lightPosition.mad(eye, lightDirection, -500.0f);
            environmentSnapshot.sunCascades = BuildMetalR4SunCascades(view, projection, eye, forward,
                lightPosition, lightDirection, environmentSnapshot.ssfxShadowCascades,
                2048u, environmentSnapshot.sunResetLastCascade, viewState.engineFrame);
        }
        if (std::getenv("XRAY_METAL_E08_GPU_PASSES") && Device.dwFrame % 120u == 0u)
            std::fprintf(stderr, "METAL_E08_SUN_BUILD frame=%u cpu_ms=%.6f\n", Device.dwFrame,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e01SceneStart).count());
        if (!scene.Render(*backend, viewState, environmentSnapshot, vision, error))
        {
            ReportFailure("Render production R4 scene", error);
            scene.DiscardUnsubmittedFrame();
            backend->DiscardFrame();
            frameActive = false;
            passActive = false;
            if (backend->HasCommandBufferError())
            {
                ShutdownBackend();
                const std::string fatalError = "Metal renderer stopped after an unrecoverable frame failure: " + error;
                Debug.fatal(DEBUG_INFO, "%s", fatalError.c_str());
                return;
            }
            ++consecutiveSceneRenderFailures;
            if (consecutiveSceneRenderFailures >= 3u)
            {
                const std::string fatalError = "Metal renderer stopped after " +
                    std::to_string(consecutiveSceneRenderFailures) +
                    " consecutive scene failures: " + error;
                ShutdownBackend();
                Debug.fatal(DEBUG_INFO, "%s", fatalError.c_str());
                return;
            }
            deviceState = dsOK;
            return;
        }
        consecutiveSceneRenderFailures = 0u;
        gameplaySceneRendered = true;
        const char* coldUploadProbe = std::getenv("XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE");
        static bool coldEncodeDiscarded = false;
        if (coldUploadProbe && std::strcmp(coldUploadProbe, "1") == 0 && !coldEncodeDiscarded)
        {
            coldEncodeDiscarded = true;
            std::fprintf(stderr, "METAL_TEMPORAL_COLD_UPLOAD_ABORT engine_frame=%u submitted_serial=%llu\n",
                Device.dwFrame, static_cast<unsigned long long>(backend->LastSubmittedSerial()));
            scene.DiscardUnsubmittedFrame();
            backend->DiscardFrame();
            frameActive = false;
            passActive = false;
            deviceState = dsOK;
            return;
        }
		if (temporalLifecycleProbeEnabled())
		{
			TemporalLifecycleProbeState& probe = temporalLifecycleProbeState();
			if (!probe.encodeDiscarded && probe.preparedBeginTicks >= 15u)
			{
				probe.encodeDiscarded = true;
				std::fprintf(stderr,
					"METAL_TEMPORAL_LIFECYCLE_PROBE event=discard_encoded_frame prepared_begin=%u engine_frame=%u submitted_serial=%llu\n",
					probe.preparedBeginTicks, Device.dwFrame,
					static_cast<unsigned long long>(backend->LastSubmittedSerial()));
				scene.DiscardUnsubmittedFrame();
				backend->DiscardFrame();
				frameActive = false;
				passActive = false;
				deviceState = dsOK;
				return;
			}
			if (!probe.multiViewChecked && probe.preparedBeginTicks >= 20u)
			{
				probe.multiViewChecked = true;
				std::string secondViewError;
				const bool secondViewRendered = scene.Render(*backend, viewState,
					environmentSnapshot, vision, secondViewError);
				const bool rejectedBeforeEncoding = !secondViewRendered &&
					secondViewError ==
						"only one primary world view may produce temporal history per Metal submission";
				std::fprintf(stderr,
					"METAL_TEMPORAL_LIFECYCLE_PROBE event=multiple_view prepared_begin=%u engine_frame=%u result=%s error=\"%s\"\n",
					probe.preparedBeginTicks, Device.dwFrame,
					rejectedBeforeEncoding ? "rejected_before_encoding" : "unexpected",
					secondViewError.c_str());
				if (!rejectedBeforeEncoding)
				{
					scene.DiscardUnsubmittedFrame();
					backend->DiscardFrame();
					frameActive = false;
					passActive = false;
					deviceState = dsOK;
					return;
				}
			}
		}
		e01SceneMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - e01SceneStart).count();
		if (!sceneFrameLogged)
		{
			Msg("* METAL_ENGINE_FIRST_FRAME camera=%s", std::getenv("XRAY_METAL_FIXED_SCENE_CAMERA") ?
                "fixed_scene" : (viewState.valid ? "engine" : "engine_pending"));
			sceneFrameLogged = true;
        }
    }
    if (!gameplaySceneRendered)
    {
        consecutiveSceneRenderFailures = 0u;
        if (!passActive)
        {
            R4RenderPassDesc clearPass;
            clearPass.label = "Engine Metal loading/coherence clear";
            clearPass.useDrawable = true;
            clearPass.drawableColor.loadAction = R4LoadAction::Clear;
            clearPass.drawableColor.storeAction = R4StoreAction::Store;
            clearPass.drawableColor.clearColor = {0.008, 0.012, 0.020, 1.0};
            if (!backend->BeginRenderPass(clearPass, error))
            {
                ReportFailure("Begin loading/coherence clear pass", error);
                scene.DiscardUnsubmittedFrame();
                backend->DiscardFrame();
                frameActive = false;
                passActive = false;
                deviceState = dsLost;
                return;
            }
            passActive = true;
        }
        if (scene.IsPrepared() && admission != R4GameplayFrameAdmission::Ready)
        {
            static R4GameplayFrameAdmission lastLoggedAdmission = R4GameplayFrameAdmission::Ready;
            static std::uint32_t lastLoggedAdmissionFrame = UINT32_MAX;
            const bool reasonChanged = admission != lastLoggedAdmission;
            const bool intervalElapsed = lastLoggedAdmissionFrame == UINT32_MAX ||
                Device.dwFrame < lastLoggedAdmissionFrame ||
                Device.dwFrame - lastLoggedAdmissionFrame >= 300u;
            if (reasonChanged || intervalElapsed)
            {
                const auto world = backend->EngineWorldSnapshot();
                const auto lighting = backend->EngineLightingFrameSnapshot();
                Msg("* METAL_GAMEPLAY_SCENE_DEFER frame=%u reason=%s view=%u env=%u world=%u light=%u",
                    Device.dwFrame, R4GameplayFrameAdmissionName(admission), viewState.engineFrame,
                    environmentSnapshot.engineFrame, world ? world->engineFrame : 0u,
                    lighting ? lighting->engineFrame : 0u);
                lastLoggedAdmission = admission;
                lastLoggedAdmissionFrame = Device.dwFrame;
            }
        }
    }
    if (passActive)
    {
        if (!backend->EndRenderPass(error))
        {
            ReportFailure("End clear pass", error);
            ShutdownBackend();
            deviceState = dsLost;
            return;
        }
        passActive = false;
    }

    xray::metal::ui::FrameBatch uiBatch = xray::metal::ui::DrainFrameBatch();
    if (!uiBatch.draws.empty() || uiBatch.droppedDraws != 0u)
    {
        if (!backend->RenderUIOverlay(uiBatch, error))
        {
            // UI is an optional presentation layer.  Its isolated pass must
            // never discard an otherwise valid world frame or affect DXVK.
            Msg("! METAL_UI_OVERLAY_SKIP error=%s", error.c_str());
        }
    }

    backend->SetE01FrameTiming(Device.dwFrame, environmentSnapshot.engineFrame, e01SceneMs);
    if (!backend->SubmitFrame(error))
    {
        const bool retry = backend->LastFrameSubmitWasRetry();
        scene.DiscardUnsubmittedFrame();
        backend->DiscardFrame();
        if (retry)
        {
            Msg("* METAL_FRAME_SUBMIT_RETRY frame=%u reason=%s", Device.dwFrame, error.c_str());
            deviceState = dsOK;
        }
        else
        {
            ReportFailure("End", error);
            deviceState = dsLost;
        }
    }
    else
    {
        scene.OnFrameSubmitted();
        deviceState = dsOK;
    }
    frameActive = false;
}

void MetalRenderDeviceRender::ClearTarget() {}
void MetalRenderDeviceRender::SetCacheXform(Fmatrix& view, Fmatrix& projection)
{
	if (std::getenv("XRAY_METAL_FIXED_SCENE_CAMERA"))
	{
		viewState.valid = false;
		return;
	}
    static_assert(sizeof(Fmatrix) == sizeof(float) * 16,
        "engine Metal scene requires a packed float4x4 Fmatrix");
    std::memcpy(viewState.view.data(), &view, sizeof(Fmatrix));
    std::memcpy(viewState.projection.data(), &projection, sizeof(Fmatrix));
    viewState.eye = {
        Device.vCameraPosition.x,
        Device.vCameraPosition.y,
        Device.vCameraPosition.z,
    };
    viewState.engineFrame = Device.dwFrame;
    viewState.valid = true;
}
void MetalRenderDeviceRender::SetCacheXform_prev(Fmatrix& view, Fmatrix& projection)
{
    static_assert(sizeof(Fmatrix) == sizeof(float) * 16,
        "engine Metal scene requires a packed previous float4x4 Fmatrix");
    const auto isZero = [](const Fmatrix& m) {
        const float* f = reinterpret_cast<const float*>(&m);
        for (int i = 0; i < 16; ++i)
            if (f[i] != 0.0f) return false;
        return true;
    };
    if (Device.dwFrame <= 1 || isZero(view) || isZero(projection))
    {
        viewState.previousView = viewState.view;
        viewState.previousProjection = viewState.projection;
        viewState.previousFrame = Device.dwFrame;
        viewState.previousValid = false;
        viewState.cameraCut = true;
        return;
    }
    std::memcpy(viewState.previousView.data(), &view, sizeof(Fmatrix));
    std::memcpy(viewState.previousProjection.data(), &projection, sizeof(Fmatrix));
    viewState.previousFrame = Device.dwFrame > 0 ? Device.dwFrame - 1 : 0;
    viewState.previousValid = true;
    viewState.cameraCut = false;
}

void MetalRenderDeviceRender::OnAssetsChanged()
{
    scene.InvalidateHistory();
    if (backend && !frameActive)
        backend->ResizeDrawable();
}

bool MetalRenderDeviceRender::SwitchOutputMonitor(HMONITOR, HWND, u32, u32&, u32&)
{
    return false;
}

void MetalRenderDeviceRender::ShutdownBackend()
{
    free_metal_vid_mode_list();
    frameActive = false;
    passActive = false;
    scene.Reset();
    sceneOptions = {};
    preparedLevelDirectory.clear();
    sceneOptionsReady = false;
    viewState = {};
    environmentSnapshot = {};
    gameplayVisionSnapshot = {};
    gameplayVisionSnapshotLogged = false;
    sceneFrameLogged = false;
    sceneCameraLogged = false;
    lastLoggedSceneEye = {};
	resetHudTransformHistory();
    if (backend)
    {
        backend->Shutdown();
        backend.reset();
    }
}

void MetalRenderDeviceRender::UpdateDimensions(u32& width, u32& height, float& halfWidth, float& halfHeight) const
{
    if (!backend)
        return;

    backend->DrawableSize(width, height);
    halfWidth = static_cast<float>(width) * 0.5f;
    halfHeight = static_cast<float>(height) * 0.5f;
}

void MetalRenderDeviceRender::ReportFailure(const char* operation, const std::string& error)
{
    lastError = error.empty() ? "unspecified Metal backend error" : error;
    std::fprintf(stderr, "! [Metal] %s failed: %s\n", operation, lastError.c_str());
    Msg("! [Metal] %s failed: %s", operation, lastError.c_str());
}
