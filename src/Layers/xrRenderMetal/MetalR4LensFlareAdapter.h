#pragma once

#include "IR4Pipeline.h"
#include "IR4Resources.h"
#include "R4DdsTexture.h"
#include "../xrRender/MetalLensFlareCapture.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

class IR4Backend;

class MetalR4LensFlareAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
        R4PixelFormat colorFormat = R4PixelFormat::RGBA8Unorm;
    };

    struct Telemetry
    {
        std::uint32_t ownerEngineFrame = 0;
        std::uint64_t stageGeneration = 0;
        std::size_t capturedDraws = 0;
        std::size_t stagedDraws = 0;
        std::size_t rejectedDraws = 0;
        std::size_t submittedDraws = 0;
    };

    MetalR4LensFlareAdapter();
    ~MetalR4LensFlareAdapter();
    MetalR4LensFlareAdapter(const MetalR4LensFlareAdapter&) = delete;
    MetalR4LensFlareAdapter& operator=(const MetalR4LensFlareAdapter&) = delete;

    // An empty shader path means intentionally disabled. Once a bundle path
    // is supplied, missing MSC artifacts/provider/pipeline fail closed.
    bool Configure(IR4Backend& backend, const Options& options, std::string& error);

    // Stages a value-owned capture into immutable frame-arena vertex slices.
    // ownerEngineFrame lets the later draw reject stale/cross-frame batches.
    bool StageFrame(IR4Backend& backend, xray::metal::lensflare::Frame frame,
        std::uint32_t ownerEngineFrame, std::string& error);

    bool DrawIntoActiveForward(IR4Backend& backend,
        const std::shared_ptr<IR4Buffer>& staticGlobals,
        std::uint32_t ownerEngineFrame, std::string& error);

    // Call only after SubmitFrame succeeds; the command buffer's private DDS
    // uploads are then durable and may be reused by later frames.
    void CommitSubmittedFrame(std::uint32_t ownerEngineFrame);
    // Call after any failed encode/submit; drops uncommitted uploads and draws.
    void DiscardUnsubmittedFrame();

    bool IsReady() const { return ready; }
    bool HasFlares() const;
    const Telemetry& LastTelemetry() const { return telemetry; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Telemetry telemetry;
    bool ready = false;
};
