#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

struct SDL_Window;

#if defined(__OBJC__)
@class CAMetalLayer;
#else
typedef struct objc_object CAMetalLayer;
#endif

// Isolated display capabilities and presentation state snapshot.
// Captures screen timing (ProMotion), scaling, dimensions, EDR/HDR headroom,
// and CAMetalLayer presentation state without altering any engine state.
struct DisplayCapabilities
{
    // Screen identity and refresh timing (ProMotion readiness)
    std::string screenName = "Unknown";
    std::uint32_t displayId = 0;
    std::int64_t maxFramesPerSecond = 0;
    double minRefreshInterval = 0.0;
    double maxRefreshInterval = 0.0;
    double displayUpdateGranularity = 0.0;

    // Scaling and surface dimensions
    double backingScaleFactor = 1.0;
    double windowWidthPoints = 0.0;
    double windowHeightPoints = 0.0;
    std::uint32_t backingWidthPixels = 0;
    std::uint32_t backingHeightPixels = 0;
    double layerDrawableWidth = 0.0;
    double layerDrawableHeight = 0.0;
    std::uint32_t engineRenderWidth = 0;
    std::uint32_t engineRenderHeight = 0;

    // Extended Dynamic Range (HDR / EDR readiness)
    double maxPotentialEdr = 1.0;
    double maxEdr = 1.0;
    double maxReferenceEdr = 1.0;
    bool isEdrSupported = false;

    // CAMetalLayer presentation state
    std::uint32_t layerPixelFormat = 0;
    std::string layerPixelFormatName = "Unknown";
    std::string layerColorSpaceName = "Default";
    bool layerWantsEdr = false;
    bool layerDisplaySyncEnabled = false;
    std::uint32_t layerMaximumDrawableCount = 0;
    bool layerPresentsWithTransaction = false;
    bool layerFramebufferOnly = false;
    bool layerAllowsNextDrawableTimeout = false;

    // Current presentation / vsync state available to engine
    bool engineVsyncEnabled = false;

    bool operator==(const DisplayCapabilities& o) const
    {
        auto doubleNear = [](double a, double b, double eps) {
            return std::fabs(a - b) <= eps;
        };

        return screenName == o.screenName &&
               displayId == o.displayId &&
               maxFramesPerSecond == o.maxFramesPerSecond &&
               doubleNear(minRefreshInterval, o.minRefreshInterval, 1e-4) &&
               doubleNear(maxRefreshInterval, o.maxRefreshInterval, 1e-4) &&
               doubleNear(displayUpdateGranularity, o.displayUpdateGranularity, 1e-4) &&
               doubleNear(backingScaleFactor, o.backingScaleFactor, 1e-3) &&
               doubleNear(windowWidthPoints, o.windowWidthPoints, 0.5) &&
               doubleNear(windowHeightPoints, o.windowHeightPoints, 0.5) &&
               backingWidthPixels == o.backingWidthPixels &&
               backingHeightPixels == o.backingHeightPixels &&
               doubleNear(layerDrawableWidth, o.layerDrawableWidth, 0.5) &&
               doubleNear(layerDrawableHeight, o.layerDrawableHeight, 0.5) &&
               engineRenderWidth == o.engineRenderWidth &&
               engineRenderHeight == o.engineRenderHeight &&
               doubleNear(maxPotentialEdr, o.maxPotentialEdr, 1e-3) &&
               doubleNear(maxEdr, o.maxEdr, 1e-3) &&
               doubleNear(maxReferenceEdr, o.maxReferenceEdr, 1e-3) &&
               isEdrSupported == o.isEdrSupported &&
               layerPixelFormat == o.layerPixelFormat &&
               layerPixelFormatName == o.layerPixelFormatName &&
               layerColorSpaceName == o.layerColorSpaceName &&
               layerWantsEdr == o.layerWantsEdr &&
               layerDisplaySyncEnabled == o.layerDisplaySyncEnabled &&
               layerMaximumDrawableCount == o.layerMaximumDrawableCount &&
               layerPresentsWithTransaction == o.layerPresentsWithTransaction &&
               layerFramebufferOnly == o.layerFramebufferOnly &&
               layerAllowsNextDrawableTimeout == o.layerAllowsNextDrawableTimeout &&
               engineVsyncEnabled == o.engineVsyncEnabled;
    }

    bool operator!=(const DisplayCapabilities& o) const
    {
        return !(*this == o);
    }

    std::string ToString() const
    {
        std::ostringstream ss;
        ss << std::fixed;
        ss << "screen=\"" << screenName << "\" (id=" << displayId << ")"
           << " max_fps=" << maxFramesPerSecond
           << " refresh_intervals=[" << std::setprecision(6) << minRefreshInterval << "s, "
           << std::setprecision(6) << maxRefreshInterval << "s]"
           << " granularity=" << std::setprecision(6) << displayUpdateGranularity << "s"
           << " scale=" << std::setprecision(2) << backingScaleFactor
           << " window_points=" << std::setprecision(1) << windowWidthPoints << "x" << windowHeightPoints
           << " backing_surface=" << backingWidthPixels << "x" << backingHeightPixels
           << " layer_drawable=" << std::setprecision(1) << layerDrawableWidth << "x" << layerDrawableHeight
           << " engine_render=" << engineRenderWidth << "x" << engineRenderHeight
           << " edr_support=" << (isEdrSupported ? "YES" : "NO")
           << " potential_edr=" << std::setprecision(3) << maxPotentialEdr
           << " current_edr=" << std::setprecision(3) << maxEdr
           << " ref_edr=" << std::setprecision(3) << maxReferenceEdr
           << " pixel_format=" << layerPixelFormatName << "(" << layerPixelFormat << ")"
           << " colorspace=\"" << layerColorSpaceName << "\""
           << " wants_edr=" << (layerWantsEdr ? "YES" : "NO")
           << " vsync=" << (layerDisplaySyncEnabled ? "YES" : "NO")
           << " max_drawables=" << layerMaximumDrawableCount
           << " presents_with_tx=" << (layerPresentsWithTransaction ? "YES" : "NO")
           << " fb_only=" << (layerFramebufferOnly ? "YES" : "NO")
           << " allows_timeout=" << (layerAllowsNextDrawableTimeout ? "YES" : "NO")
           << " engine_vsync=" << (engineVsyncEnabled ? "YES" : "NO");
        return ss.str();
    }

    std::string ToJson() const
    {
        std::ostringstream ss;
        ss << std::fixed;
        ss << "{"
           << "\"screen\":{"
               << "\"name\":\"" << screenName << "\","
               << "\"id\":" << displayId
           << "},"
           << "\"promotion\":{"
               << "\"max_fps\":" << maxFramesPerSecond << ","
               << "\"min_refresh_interval\":" << std::setprecision(6) << minRefreshInterval << ","
               << "\"max_refresh_interval\":" << std::setprecision(6) << maxRefreshInterval << ","
               << "\"display_update_granularity\":" << std::setprecision(6) << displayUpdateGranularity
           << "},"
           << "\"dimensions\":{"
               << "\"backing_scale_factor\":" << std::setprecision(2) << backingScaleFactor << ","
               << "\"window_points\":{\"width\":" << std::setprecision(1) << windowWidthPoints << ",\"height\":" << windowHeightPoints << "},"
               << "\"backing_surface_pixels\":{\"width\":" << backingWidthPixels << ",\"height\":" << backingHeightPixels << "},"
               << "\"layer_drawable_size\":{\"width\":" << std::setprecision(1) << layerDrawableWidth << ",\"height\":" << layerDrawableHeight << "},"
               << "\"engine_render_resolution\":{\"width\":" << engineRenderWidth << ",\"height\":" << engineRenderHeight << "}"
           << "},"
           << "\"edr\":{"
               << "\"supported\":" << (isEdrSupported ? "true" : "false") << ","
               << "\"max_potential_edr\":" << std::setprecision(3) << maxPotentialEdr << ","
               << "\"max_edr\":" << std::setprecision(3) << maxEdr << ","
               << "\"max_reference_edr\":" << std::setprecision(3) << maxReferenceEdr
           << "},"
           << "\"layer\":{"
               << "\"pixel_format_name\":\"" << layerPixelFormatName << "\","
               << "\"pixel_format\":" << layerPixelFormat << ","
               << "\"colorspace\":\"" << layerColorSpaceName << "\","
               << "\"wants_extended_dynamic_range_content\":" << (layerWantsEdr ? "true" : "false")
           << "},"
           << "\"presentation\":{"
               << "\"display_sync_enabled\":" << (layerDisplaySyncEnabled ? "true" : "false") << ","
               << "\"maximum_drawable_count\":" << layerMaximumDrawableCount << ","
               << "\"presents_with_transaction\":" << (layerPresentsWithTransaction ? "true" : "false") << ","
               << "\"framebuffer_only\":" << (layerFramebufferOnly ? "true" : "false") << ","
               << "\"allows_next_drawable_timeout\":" << (layerAllowsNextDrawableTimeout ? "true" : "false") << ","
               << "\"engine_vsync_enabled\":" << (engineVsyncEnabled ? "true" : "false")
           << "}"
           << "}";
        return ss.str();
    }
};

DisplayCapabilities QueryDisplayCapabilities(
    SDL_Window* sdlWindow,
    void* metalView,
    CAMetalLayer* layer,
    std::uint32_t engineRenderWidth,
    std::uint32_t engineRenderHeight);
