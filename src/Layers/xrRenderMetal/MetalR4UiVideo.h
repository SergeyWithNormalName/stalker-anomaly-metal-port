#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct R4UiVideoFrame
{
    const std::uint32_t* pixels = nullptr;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    bool newFrameAvailable = false;
};

class IR4UiVideoPlayback
{
public:
    virtual ~IR4UiVideoPlayback() = default;
    virtual bool Update(R4UiVideoFrame& outFrame, std::string& error) = 0;
    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
};

// Checks if a .ogm video exists for the given UI texture identity (e.g. "ui\\ui_background_new")
// and returns a playback instance if found and loaded.
bool OpenMetalR4UiVideo(
    const std::string& textureIdentity,
    std::unique_ptr<IR4UiVideoPlayback>& outPlayback,
    std::string& error);

// Checks if a .ogm video exists for the given UI texture identity and queries its dimensions.
bool GetMetalR4UiVideoDimensions(
    const std::string& textureIdentity,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error);
