#include "MetalR4UiVideo.h"

#if defined(STATIC_RENDERER_R4)
#include "../xrRenderPC_R4/stdafx.h"
#include "../../xrEngine/xrTheora_Surface.h"

#include <algorithm>
#include <vector>

namespace
{
class TheoraUiVideoPlayback : public IR4UiVideoPlayback
{
public:
    explicit TheoraUiVideoPlayback(std::unique_ptr<CTheoraSurface> theora)
        : m_theora(std::move(theora))
    {
        m_width = m_theora->Width(true);
        m_height = m_theora->Height(true);
        m_pixels.resize(static_cast<std::size_t>(m_width) * m_height, 0xFF000000);

        m_theora->Play(TRUE, Device.dwTimeContinual);
        // Pre-roll theora decoder so first frame is ready immediately
        m_theora->Update(Device.dwTimeContinual);
        m_theora->Update(Device.dwTimeContinual);
        m_theora->Update(Device.dwTimeContinual);
        int dummyPos = 0;
        m_theora->DecompressFrame(m_pixels.data(), 0, dummyPos);
    }

    ~TheoraUiVideoPlayback() override
    {
        if (m_theora)
        {
            m_theora->Stop();
        }
    }

    std::uint32_t Width() const override { return m_width; }
    std::uint32_t Height() const override { return m_height; }

    bool Update(R4UiVideoFrame& outFrame, std::string& error) override
    {
        if (!m_theora)
        {
            error = "no theora stream";
            return false;
        }
        outFrame.width = m_width;
        outFrame.height = m_height;
        outFrame.pixels = m_pixels.data();

        const u32 currentFrame = Device.dwFrame;
        if (m_lastUpdateFrame != currentFrame)
        {
            m_lastUpdateFrame = currentFrame;
            if (m_theora->Update(Device.dwTimeContinual))
            {
                int dummyPos = 0;
                m_theora->DecompressFrame(m_pixels.data(), 0, dummyPos);
                outFrame.newFrameAvailable = true;
            }
            else
            {
                outFrame.newFrameAvailable = false;
            }
        }
        else
        {
            outFrame.newFrameAvailable = false;
        }
        return true;
    }

private:
    std::unique_ptr<CTheoraSurface> m_theora;
    std::vector<std::uint32_t> m_pixels;
    std::uint32_t m_width = 0u;
    std::uint32_t m_height = 0u;
    u32 m_lastUpdateFrame = u32(-1);
};

static bool locateOgmFile(const std::string& textureIdentity, string_path& outPath)
{
    std::string baseIdentity = textureIdentity;
    if (baseIdentity.size() >= 4 &&
        _stricmp(baseIdentity.c_str() + baseIdentity.size() - 4, ".dds") == 0)
    {
        baseIdentity.erase(baseIdentity.size() - 4);
    }
    if (baseIdentity.size() >= 4 &&
        _stricmp(baseIdentity.c_str() + baseIdentity.size() - 4, ".ogm") == 0)
    {
        baseIdentity.erase(baseIdentity.size() - 4);
    }

    if (FS.exist(outPath, "$game_textures$", baseIdentity.c_str(), ".ogm"))
        return true;

    std::string backslashName = baseIdentity;
    std::replace(backslashName.begin(), backslashName.end(), '/', '\\');
    if (FS.exist(outPath, "$game_textures$", backslashName.c_str(), ".ogm"))
        return true;

    std::string forwardSlashName = baseIdentity;
    std::replace(forwardSlashName.begin(), forwardSlashName.end(), '\\', '/');
    if (FS.exist(outPath, "$game_textures$", forwardSlashName.c_str(), ".ogm"))
        return true;

    return false;
}
} // namespace

bool OpenMetalR4UiVideo(
    const std::string& textureIdentity,
    std::unique_ptr<IR4UiVideoPlayback>& outPlayback,
    std::string& error)
{
    outPlayback.reset();
    string_path fn;
    if (!locateOgmFile(textureIdentity, fn))
    {
        error = "video file not found";
        return false;
    }

    auto theora = std::make_unique<CTheoraSurface>();
    if (!theora->Load(fn))
    {
        error = std::string("failed to load OGM stream: ") + fn;
        return false;
    }

    outPlayback = std::make_unique<TheoraUiVideoPlayback>(std::move(theora));
    error.clear();
    return true;
}

bool GetMetalR4UiVideoDimensions(
    const std::string& textureIdentity,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error)
{
    width = 0u;
    height = 0u;
    string_path fn;
    if (!locateOgmFile(textureIdentity, fn))
    {
        error = "video file not found";
        return false;
    }

    CTheoraSurface probe;
    if (!probe.Load(fn))
    {
        error = std::string("failed to probe OGM stream: ") + fn;
        return false;
    }

    width = probe.Width(true);
    height = probe.Height(true);
    error.clear();
    return true;
}

#else

bool OpenMetalR4UiVideo(
    const std::string&,
    std::unique_ptr<IR4UiVideoPlayback>& outPlayback,
    std::string& error)
{
    outPlayback.reset();
    error = "video playback requires engine target";
    return false;
}

bool GetMetalR4UiVideoDimensions(
    const std::string&,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error)
{
    width = 0u;
    height = 0u;
    error = "video playback requires engine target";
    return false;
}

#endif
