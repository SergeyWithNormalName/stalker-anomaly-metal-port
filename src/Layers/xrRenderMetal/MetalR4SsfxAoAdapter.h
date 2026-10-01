#pragma once

#include "IR4Backend.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>

// Owns the independent production SSFX AO and IL branches before combine_1.
class MetalR4SsfxAoAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
    };
    struct FrameInputs
    {
        std::shared_ptr<IR4Texture> position;
        std::shared_ptr<IR4Texture> motionVectors;
        std::shared_ptr<IR4Texture> accumulator;
        std::shared_ptr<IR4Buffer> staticGlobals;
        std::array<float, 16> view{};
        std::array<float, 16> projection{};
        std::array<float, 16> previousView{};
        std::array<float, 16> previousProjection{};
        R4EnvironmentSnapshot environment{};
        bool cameraCut = false;
        bool previousValid = false;
        std::uint32_t previousFrame = 0;
    };
    struct Telemetry
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t frame = 0;
        std::uint32_t passes = 0;
        std::uint32_t indirectLightPasses = 0;
        bool temporalHistory = false;
        bool aoEnabled = false, ilEnabled = false;
        bool aoHistory = false, ilHistory = false;
    };

    MetalR4SsfxAoAdapter();
    ~MetalR4SsfxAoAdapter();
    MetalR4SsfxAoAdapter(const MetalR4SsfxAoAdapter&) = delete;
    MetalR4SsfxAoAdapter& operator=(const MetalR4SsfxAoAdapter&) = delete;

    bool Prepare(IR4Backend& backend, const Options& options, std::string& status);
    bool PrepareRenderTargets(IR4Backend& backend, std::uint32_t width, std::uint32_t height,
        std::string& error);
    bool Draw(IR4Backend& backend, const FrameInputs& inputs, std::string& error);
    void DiscardUnsubmittedFrame();
    void CommitSubmittedFrame();
    void InvalidateHistory();

    const std::shared_ptr<IR4Texture>& Output() const;
    const std::shared_ptr<IR4Texture>& IndirectLightOutput() const;
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
