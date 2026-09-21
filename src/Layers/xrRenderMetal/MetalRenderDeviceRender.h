#pragma once

// This is intentionally a dxRenderDeviceRender subtype: existing R4 code
// still uses dxRenderDeviceRender::Instance() for its resource-manager view.
// The Metal lifecycle itself never calls the D3D implementation.
#include "../xrRender/dxRenderDeviceRender.h"
#include "IR4Backend.h"
#include "MetalR4EngineScene.h"

#include <memory>
#include <array>
#include <string>

class MetalRenderDeviceRender final : public dxRenderDeviceRender
{
public:
    MetalRenderDeviceRender();
    ~MetalRenderDeviceRender() override;

    void Copy(IRenderDeviceRender& in) override;

    void setGamma(float gamma) override;
    void setBrightness(float brightness) override;
    void setContrast(float contrast) override;
    void updateGamma() override;

    void OnDeviceDestroy(BOOL keepTextures) override;
    void ValidateHW() override;
    void DestroyHW() override;
    void Reset(HWND hWnd, u32& width, u32& height, float& halfWidth, float& halfHeight) override;

    void SetupStates() override;
    void OnDeviceCreate(LPCSTR shaderName) override;
    void Create(HWND hWnd, u32& width, u32& height, float& halfWidth, float& halfHeight, bool moveWindow) override;
    void SetupGPU(BOOL forceSoftware, BOOL forceNonPure, BOOL forceReference) override;

    void overdrawBegin() override;
    void overdrawEnd() override;

    void DeferredLoad(BOOL enabled) override;
    void ResourcesDeferredUpload() override;
    void ResourcesDeferredUnload() override;
    void ResourcesGetMemoryUsage(u32& base, u32& baseCount, u32& lightmaps, u32& lightmapCount) override;
    void ResourcesDestroyNecessaryTextures() override;
    void ResourcesStoreNecessaryTextures() override;
    void ResourcesDumpMemoryUsage() override;
    void ResourcesPrefetchCreateTexture(LPCSTR name) override;

    bool HWSupportsShaderYUV2RGB() override;

    DeviceState GetDeviceState() override;
    BOOL GetForceGPU_REF() override;
    u32 GetCacheStatPolys() override;
    void Begin() override;
    void Clear() override;
    void End() override;
    void ClearTarget() override;
    void SetCacheXform(Fmatrix& view, Fmatrix& projection) override;
    void SetCacheXform_prev(Fmatrix& view, Fmatrix& projection) override;
    void OnAssetsChanged() override;
    bool SwitchOutputMonitor(HMONITOR targetMonitor, HWND hWnd, u32 screenMode, u32& width, u32& height) override;

    IR4Backend* Backend() const { return backend.get(); }
    bool IsFrameActive() const { return frameActive; }
    const std::string& LastError() const { return lastError; }
    bool LoadLevelScene(const std::filesystem::path& levelDirectory);
    void UnloadLevelScene();
    bool CopyLatestCompletedSaveThumbnail(R4SaveThumbnailSnapshot& snapshot) const;
    // Called by dxEnvironmentRender::OnFrame on the render thread before its
    // Metal early return. This is value-only and deliberately cannot expose a
    // D3D resource to the Direct Metal path.
    void SubmitEnvironmentSnapshot(const R4EnvironmentSnapshot& snapshot);
    // Called from CLevel after the controlled actor has updated.  This keeps
    // legacy night/heat phase values in a CPU-only snapshot for the Direct
    // Metal post-process bridge.
    void SubmitGameplayVisionSnapshot(const R4GameplayVisionSnapshot& snapshot);
    bool CopyGameplayVisionSnapshotForFrame(
        u32 engineFrame, R4GameplayVisionSnapshot& snapshot) const;

private:
    void ShutdownBackend();
    void UpdateDimensions(u32& width, u32& height, float& halfWidth, float& halfHeight) const;
    void ReportFailure(const char* operation, const std::string& error);

    std::unique_ptr<IR4Backend> backend;
    MetalR4EngineScene scene;
    MetalR4EngineScene::Options sceneOptions;
    std::filesystem::path preparedLevelDirectory;
    bool sceneOptionsReady = false;
    MetalR4EngineScene::ViewState viewState;
    R4EnvironmentSnapshot environmentSnapshot;
    R4GameplayVisionSnapshot gameplayVisionSnapshot;
    bool gameplayVisionSnapshotLogged = false;
    std::string lastError;
    DeviceState deviceState = dsOK;
    BOOL forceReference = FALSE;
    bool frameActive = false;
    bool passActive = false;
    bool sceneFrameLogged = false;
    bool sceneCameraLogged = false;
    std::array<float, 3> lastLoggedSceneEye{};
};
