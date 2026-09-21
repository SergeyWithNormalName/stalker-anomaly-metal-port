#pragma once

#include "../xrRender/MetalUICapture.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

namespace xray::metal::ui::overlay
{
struct ScissorRect
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline bool BuildXrfsTexturePath(const std::string& texture, std::filesystem::path& path, std::string& error)
{
    constexpr std::size_t MaximumNameBytes = 1024u;
    if (texture.empty() || texture.size() > MaximumNameBytes)
    {
        error = "UI texture identity is empty or exceeds the bounded path length";
        return false;
    }
    std::string normalized = texture;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    if (normalized.front() == '\\' || normalized.find(':') != std::string::npos)
    {
        error = "UI texture identity must be relative to $game_textures$";
        return false;
    }
    std::size_t start = 0;
    while (start <= normalized.size())
    {
        const std::size_t end = normalized.find('\\', start);
        const std::string segment = normalized.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..")
        {
            error = "UI texture identity contains an empty or traversal path component";
            return false;
        }
        if (end == std::string::npos)
            break;
        start = end + 1u;
    }
    const std::size_t extension = normalized.rfind('.');
    if (extension == std::string::npos)
        normalized += ".dds";
    else if (normalized.substr(extension) != ".dds" && normalized.substr(extension) != ".DDS")
    {
        error = "UI texture identity must name a DDS texture";
        return false;
    }
    path = std::filesystem::path("xrfs:$game_textures$@" + normalized);
    error.clear();
    return true;
}

inline bool ResolveScissor(const Scissor& source, std::uint32_t drawableWidth, std::uint32_t drawableHeight,
    ScissorRect& result)
{
    if (drawableWidth == 0 || drawableHeight == 0)
        return false;
    if (!source.enabled)
    {
        result = {0, 0, drawableWidth, drawableHeight};
        return true;
    }
    const std::int64_t x1 = std::clamp<std::int64_t>(source.x1, 0, drawableWidth);
    const std::int64_t y1 = std::clamp<std::int64_t>(source.y1, 0, drawableHeight);
    const std::int64_t x2 = std::clamp<std::int64_t>(source.x2, 0, drawableWidth);
    const std::int64_t y2 = std::clamp<std::int64_t>(source.y2, 0, drawableHeight);
    if (x2 <= x1 || y2 <= y1)
        return false;
    result = {static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(y1),
        static_cast<std::uint32_t>(x2 - x1), static_cast<std::uint32_t>(y2 - y1)};
    return true;
}
} // namespace xray::metal::ui::overlay
