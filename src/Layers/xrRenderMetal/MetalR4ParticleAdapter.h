#pragma once

#include "IR4Pipeline.h"
#include "IR4Resources.h"
#include "R4DdsTexture.h"
#include "../xrRender/MetalParticleCapture.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class IR4Backend;
struct R4FrameBufferSlice;

// Owns the Direct Metal side of the CPU-expanded FVF::LIT particle stream.
// The scene owns the load/store forward pass and attachments; this adapter
// binds only the independently converted hard-particle shader and resources.
class MetalR4ParticleAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    struct Telemetry
    {
        std::size_t capturedDraws = 0;
        std::size_t capturedVertices = 0;
        std::size_t preparedDraws = 0;
        std::size_t preparedVertices = 0;
        std::size_t preparedIndices = 0;
        std::size_t rejectedDraws = 0;
        std::size_t unsupportedMaterialDraws = 0;
        std::size_t missingMaterialMetadataDraws = 0;
        std::size_t droppedCaptureDraws = 0;
        std::size_t droppedCaptureVertices = 0;
        std::size_t softDraws = 0;
        std::size_t hardDraws = 0;
        std::size_t additiveDraws = 0;
        std::size_t alphaAddDraws = 0;
        std::size_t distortionDraws = 0;
        std::size_t hudDraws = 0;
        std::size_t clampedDraws = 0;
        std::size_t strictDraws = 0;
        std::size_t submittedDraws = 0;
        std::size_t submittedDistortionDraws = 0;
        std::size_t softRejectedDraws = 0;
        std::size_t hudRejectedDraws = 0;
        std::size_t textureRejectedDraws = 0;
    };

    struct PreparedDraw
    {
        xray::metal::particles::DrawRecord state;
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> indices;
        std::shared_ptr<IR4Texture> texture;
        std::shared_ptr<IR4Sampler> sampler;
        std::shared_ptr<IR4GraphicsPipeline> pipeline;
        bool colorPass = false;
        bool distortionPass = false;
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        std::size_t vertexBufferOffset = 0;
        std::size_t indexBufferOffset = 0;
        std::size_t vertexByteSize = 0;
        std::size_t indexByteSize = 0;
    };

    MetalR4ParticleAdapter();
    ~MetalR4ParticleAdapter();
    MetalR4ParticleAdapter(const MetalR4ParticleAdapter&) = delete;
    MetalR4ParticleAdapter& operator=(const MetalR4ParticleAdapter&) = delete;

    // Validates the exact independently-converted soft and hard MSC bundle.
    bool Prepare(const Options& options, std::string& status);

    // Creates the exact converted soft/hard particle forward pipelines and
    // sampler state, including the soft position-G-buffer dependency.
    bool Configure(IR4Backend& backend, const Options& options, std::string& status);

    // Repackages each captured draw into <=16383-sprite uint16-local batches.
    // Supplying the active frame backend stores immutable arena slices;
    // omitting it keeps the standalone Shared-buffer staging path. Bad captures
    // are isolated per source draw; upload failure aborts the frame fail-closed.
    bool StageFrame(IR4Resources& resources, const xray::metal::particles::FrameBatch& frame,
        std::string& error, IR4Backend* frameBackend = nullptr);

    // Drops per-frame prepared draws after an acquired frame is discarded.
    // Pipeline, sampler, texture-cache, and readiness state remain intact.
    void DiscardFrame();

    bool DrawIntoActiveForward(IR4Backend& backend,
        const std::array<float, 16>& viewProjection,
        const std::shared_ptr<IR4Texture>& positionGbuffer,
        const std::shared_ptr<IR4Buffer>& staticGlobals,
        std::string& error);

    // Keeps the neutral distortion mask sized to the current drawable. This
    // is called before any render pass begins so resize-driven replacement is
    // never attempted while an encoder owns the prior texture.
    bool PrepareRenderTargets(IR4Backend& backend, std::string& error);
    bool HasDistortionDraws() const;
    const std::shared_ptr<IR4Texture>& DistortionTarget() const;

    bool DrawIntoActiveDistortion(IR4Backend& backend,
        const std::array<float, 16>& viewProjection,
        const std::shared_ptr<IR4Buffer>& staticGlobals,
        std::string& error);

    // Runs the exact converted R4 combine_2_naa USE_DISTORT shader into the
    // active drawable pass. The caller supplies the actual combine_1 image,
    // G-buffer depth source, high/bloom source, neutral or authored LUT atlas,
    // and ssfx_lut parameters.
    bool DrawPostprocess(IR4Backend& backend,
        const std::shared_ptr<IR4Texture>& position,
        const std::shared_ptr<IR4Texture>& image,
        const std::shared_ptr<IR4Texture>& bloom,
        const std::shared_ptr<IR4Texture>& lut,
        const std::shared_ptr<IR4Buffer>& staticGlobals,
        std::string& error);

    bool DrawPostprocess(IR4Backend& backend,
        const std::shared_ptr<IR4Texture>& position,
        const std::shared_ptr<IR4Texture>& image,
        const std::shared_ptr<IR4Texture>& bloom,
        const std::shared_ptr<IR4Texture>& lut,
        const std::shared_ptr<IR4Buffer>& staticGlobals,
        const std::array<float, 4>& ssfxLut,
        std::string& error);

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }
    const std::vector<PreparedDraw>& PreparedDraws() const;

private:
    using FrameUploadFunction = std::function<bool(const void*, std::size_t, std::size_t,
        R4FrameBufferSlice&, std::string&)>;
    bool StageFrameImpl(IR4Resources& resources,
        const xray::metal::particles::FrameBatch& frame, std::string& error,
        const FrameUploadFunction* frameUpload);

    friend bool RunMetalR4ParticleAdapterSelfTest(std::string& error);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunMetalR4ParticleAdapterSelfTest(std::string& error);
