#include "../xrRenderPC_R4/stdafx.h"

#include "MetalRenderDeviceRender.h"
#include "../xrRender/Blender_BmmD.h"
#include "../xrRender/FBasicVisual.h"
#include "../xrRender/MetalParticleCapture.h"
#include "../xrRender/MetalRainCapture.h"
#include "../xrRender/MetalLensFlareCapture.h"
#include "../xrRender/light.h"

#include <SDL.h>

extern "C" void MacOS_ActivateAppWindow(void* sdlWindow);

#include <cstdlib>
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

    backend->ResizeDrawable();
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

    std::string error;
    switch (backend->BeginFrame(error))
    {
    case R4FrameAcquireResult::Ready:
        frameActive = true;
        if (scene.IsPrepared() && g_pGameLevel != nullptr)
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
                frameActive = false;
                passActive = false;
                deviceState = dsLost;
                return;
            }
            passActive = true;
        }
        deviceState = dsOK;
        return;
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

	std::string error;
	std::size_t submittedLightingCount = 0u;
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
		static const Fvector omniNorm[6] = {
			{0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f},
			{0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}};
		static const Fvector omniDir[6] = {
			{1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
			{0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};
		for (const light* source : RImplementation.v_all_lights)
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
			const u64 parentIdentity = static_cast<u64>(reinterpret_cast<uintptr_t>(source));
			const auto append = [&](CDSGraphManager::MetalLocalLight::Type type,
				const Fvector& direction, const Fvector& right, const u8 omniFace)
			{
				CDSGraphManager::MetalLocalLight target;
				target.identity = (parentIdentity << 3u) |
					(omniFace == 0xff ? 0u : u64(omniFace + 1u));
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
		for (const CDSGraphManager::MetalLocalLight& source : capturedLights)
		{
			R4EngineLocalLight target;
			target.lightId = source.identity;
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
			target.hudMode = source.hudMode;
			target.omniFace = source.omniFace;
			// shared_str represents an absent authored cookie as nullptr.  The
			// value-only backend contract uses std::string, whose const char*
			// assignment calls strlen; normalize the optional engine value before
			// copying it so point lights and cookie-less spots cannot fault during
			// level startup.
			target.projectorTexture = shared_str_or_empty(source.projectorTexture);
			if (target.staticLight) ++staticPointSpot;
			lightingFrame.lights.emplace_back(std::move(target));
		}
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
	{
		xray::metal::rain::FrameBatch rain = xray::metal::rain::DrainFrame();
		if (scene.IsPrepared())
			scene.SubmitRainFrame(std::move(rain));
	}
	{
		xray::metal::lensflare::Frame flare = xray::metal::lensflare::Drain();
		if (scene.IsPrepared())
			scene.SubmitLensFlareFrame(std::move(flare));
	}
    if (viewState.valid)
    {
        const float dx = viewState.eye[0] - lastLoggedSceneEye[0];
        const float dy = viewState.eye[1] - lastLoggedSceneEye[1];
        const float dz = viewState.eye[2] - lastLoggedSceneEye[2];
        if (!sceneCameraLogged || dx * dx + dy * dy + dz * dz >= 25.0f)
        {
            Msg("* METAL_ENGINE_CAMERA frame=%u eye=%.3f,%.3f,%.3f forward=%.3f,%.3f,%.3f",
                Device.dwFrame, viewState.eye[0], viewState.eye[1], viewState.eye[2],
                Device.vCameraDirection.x, Device.vCameraDirection.y,
                Device.vCameraDirection.z);
            lastLoggedSceneEye = viewState.eye;
            sceneCameraLogged = true;
        }
    }
    if (scene.IsPrepared() && g_pGameLevel != nullptr)
    {
        if (passActive)
        {
            ReportFailure("Render production R4 scene",
                "unexpected render pass active before scene bridge");
            ShutdownBackend();
            deviceState = dsLost;
            return;
        }
        R4GameplayVisionSnapshot vision;
        CopyGameplayVisionSnapshotForFrame(Device.dwFrame, vision);
        if (!scene.Render(*backend, viewState, environmentSnapshot, vision, error))
        {
            ReportFailure("Render production R4 scene", error);
            if (backend->HasCommandBufferError())
            {
                ShutdownBackend();
                deviceState = dsLost;
                return;
            }
        }
		if (!sceneFrameLogged)
		{
			Msg("* METAL_ENGINE_FIRST_FRAME camera=%s", std::getenv("XRAY_METAL_FIXED_SCENE_CAMERA") ?
                "fixed_scene" : (viewState.valid ? "engine" : "engine_pending"));
			sceneFrameLogged = true;
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

    if (!backend->SubmitFrame(error))
    {
        ReportFailure("End", error);
        deviceState = dsLost;
    }
    else
    {
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
    viewState.valid = true;
}
void MetalRenderDeviceRender::SetCacheXform_prev(Fmatrix& view, Fmatrix& projection)
{
    static_assert(sizeof(Fmatrix) == sizeof(float) * 16,
        "engine Metal scene requires a packed previous float4x4 Fmatrix");
    std::memcpy(viewState.previousView.data(), &view, sizeof(Fmatrix));
    std::memcpy(viewState.previousProjection.data(), &projection, sizeof(Fmatrix));
    viewState.previousValid = true;
}

void MetalRenderDeviceRender::OnAssetsChanged()
{
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
