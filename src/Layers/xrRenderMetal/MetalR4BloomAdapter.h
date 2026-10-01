#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include <filesystem>

// The SSFX producer consumed by combine_2. Legacy bloom remains a distinct
// luminance path; this adapter follows the active profile's original HLSL.
class MetalR4BloomAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };
    struct Inputs
    {
        std::shared_ptr<IR4Texture> position, scene, emissive, hudMask;
        std::shared_ptr<IR4Buffer> staticGlobals;
        R4EnvironmentSnapshot environment;
        R4GameplayVisionSnapshot vision;
    };
    MetalR4BloomAdapter();
    ~MetalR4BloomAdapter();
    bool Prepare(IR4Backend& backend, const Options& options, std::string& error);
    bool Draw(IR4Backend& backend, const Inputs& inputs, std::string& error);
    const std::shared_ptr<IR4Texture>& Output() const;
    void CommitSubmittedFrame();
    void DiscardUnsubmittedFrame();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
