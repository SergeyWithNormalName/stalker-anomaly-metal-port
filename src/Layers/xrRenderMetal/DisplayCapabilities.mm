#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include "DisplayCapabilities.h"

namespace
{
const char* GetMetalPixelFormatName(MTLPixelFormat format)
{
    switch (format)
    {
    case MTLPixelFormatBGRA8Unorm: return "BGRA8Unorm";
    case MTLPixelFormatBGRA8Unorm_sRGB: return "BGRA8Unorm_sRGB";
    case MTLPixelFormatRGBA8Unorm: return "RGBA8Unorm";
    case MTLPixelFormatRGBA8Unorm_sRGB: return "RGBA8Unorm_sRGB";
    case MTLPixelFormatRGBA16Float: return "RGBA16Float";
    case MTLPixelFormatRGB10A2Unorm: return "RGB10A2Unorm";
    case MTLPixelFormatBGR10A2Unorm: return "BGR10A2Unorm";
    case MTLPixelFormatRGBA16Unorm: return "RGBA16Unorm";
    default: return "Unknown";
    }
}

std::string GetColorSpaceName(CGColorSpaceRef colorSpace)
{
    if (!colorSpace)
        return "Default";

    CFStringRef name = CGColorSpaceGetName(colorSpace);
    if (name)
    {
        char buffer[256] = {};
        if (CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8))
            return buffer;
    }

    CFStringRef desc = CFCopyDescription(colorSpace);
    if (desc)
    {
        char buffer[256] = {};
        std::string result;
        if (CFStringGetCString(desc, buffer, sizeof(buffer), kCFStringEncodingUTF8))
            result = buffer;
        CFRelease(desc);
        if (!result.empty())
            return result;
    }

    return "Custom";
}
} // namespace

DisplayCapabilities QueryDisplayCapabilities(
    SDL_Window* sdlWindow,
    void* metalView,
    CAMetalLayer* layer,
    std::uint32_t engineRenderWidth,
    std::uint32_t engineRenderHeight)
{
    DisplayCapabilities caps;

    @autoreleasepool
    {
        NSView* nsView = metalView ? (__bridge NSView*)metalView : nil;
        NSWindow* nsWindow = nsView ? [nsView window] : nil;

        if (!nsWindow && sdlWindow)
        {
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            if (SDL_GetWindowWMInfo(sdlWindow, &wmInfo) && wmInfo.subsystem == SDL_SYSWM_COCOA)
            {
                nsWindow = wmInfo.info.cocoa.window;
            }
        }
        if (!nsWindow)
            nsWindow = [NSApp keyWindow];
        if (!nsWindow)
            nsWindow = [NSApp mainWindow];
        if (!nsWindow && [NSApp windows].count > 0)
            nsWindow = [NSApp windows].firstObject;

        NSScreen* screen = nsWindow ? [nsWindow screen] : nil;
        if (!screen)
            screen = [NSScreen mainScreen];
        if (!screen && [NSScreen screens].count > 0)
            screen = [NSScreen screens].firstObject;

        // Screen identity and refresh timing (ProMotion)
        if (screen)
        {
            if (@available(macOS 10.15, *))
            {
                NSString* localized = [screen localizedName];
                if (localized && [localized length] > 0)
                    caps.screenName = [localized UTF8String];
            }
            if (caps.screenName.empty() || caps.screenName == "Unknown")
            {
                NSDictionary* desc = [screen deviceDescription];
                NSNumber* num = [desc objectForKey:@"NSScreenNumber"];
                if (num)
                    caps.screenName = "Display " + std::to_string([num unsignedIntValue]);
            }

            NSDictionary* devDesc = [screen deviceDescription];
            NSNumber* screenNum = [devDesc objectForKey:@"NSScreenNumber"];
            if (screenNum)
                caps.displayId = [screenNum unsignedIntValue];

            if (@available(macOS 12.0, *))
            {
                caps.maxFramesPerSecond = [screen maximumFramesPerSecond];
                caps.minRefreshInterval = [screen minimumRefreshInterval];
                caps.maxRefreshInterval = [screen maximumRefreshInterval];
                caps.displayUpdateGranularity = [screen displayUpdateGranularity];
            }
            else
            {
                caps.maxFramesPerSecond = 60;
                caps.minRefreshInterval = 1.0 / 60.0;
                caps.maxRefreshInterval = 1.0 / 60.0;
                caps.displayUpdateGranularity = 0.0;
            }

            caps.backingScaleFactor = [screen backingScaleFactor];

            if (@available(macOS 10.15, *))
            {
                caps.maxPotentialEdr = [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
                caps.maxEdr = [screen maximumExtendedDynamicRangeColorComponentValue];
                caps.maxReferenceEdr = [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
            }
            else
            {
                caps.maxPotentialEdr = 1.0;
                caps.maxEdr = 1.0;
                caps.maxReferenceEdr = 1.0;
            }
        }
        else if (nsWindow)
        {
            caps.backingScaleFactor = [nsWindow backingScaleFactor];
            caps.maxFramesPerSecond = 60;
            caps.minRefreshInterval = 1.0 / 60.0;
            caps.maxRefreshInterval = 1.0 / 60.0;
            caps.displayUpdateGranularity = 0.0;
            caps.maxPotentialEdr = 1.0;
            caps.maxEdr = 1.0;
            caps.maxReferenceEdr = 1.0;
        }

        caps.isEdrSupported = (caps.maxPotentialEdr > 1.001);

        // Window points (content area)
        if (nsView)
        {
            NSRect bounds = [nsView bounds];
            caps.windowWidthPoints = bounds.size.width;
            caps.windowHeightPoints = bounds.size.height;
        }
        else if (nsWindow)
        {
            NSRect content = [nsWindow contentRectForFrameRect:[nsWindow frame]];
            caps.windowWidthPoints = content.size.width;
            caps.windowHeightPoints = content.size.height;
        }
        else if (sdlWindow)
        {
            int lw = 0, lh = 0;
            SDL_GetWindowSize(sdlWindow, &lw, &lh);
            caps.windowWidthPoints = lw;
            caps.windowHeightPoints = lh;
        }

        // Backing surface in pixels
        if (sdlWindow)
        {
            int bw = 0, bh = 0;
            SDL_Metal_GetDrawableSize(sdlWindow, &bw, &bh);
            caps.backingWidthPixels = static_cast<std::uint32_t>(bw > 0 ? bw : 0);
            caps.backingHeightPixels = static_cast<std::uint32_t>(bh > 0 ? bh : 0);
        }
        else if (nsView)
        {
            NSSize s = [nsView convertSizeToBacking:[nsView bounds].size];
            caps.backingWidthPixels = static_cast<std::uint32_t>(s.width > 0 ? s.width : 0);
            caps.backingHeightPixels = static_cast<std::uint32_t>(s.height > 0 ? s.height : 0);
        }

        // Layer drawable size and presentation configuration
        if (layer)
        {
            caps.layerDrawableWidth = layer.drawableSize.width;
            caps.layerDrawableHeight = layer.drawableSize.height;
            caps.layerPixelFormat = static_cast<std::uint32_t>(layer.pixelFormat);
            caps.layerPixelFormatName = GetMetalPixelFormatName(layer.pixelFormat);
            caps.layerColorSpaceName = GetColorSpaceName(layer.colorspace);
            caps.layerWantsEdr = layer.wantsExtendedDynamicRangeContent;
            caps.layerDisplaySyncEnabled = layer.displaySyncEnabled;
            caps.layerMaximumDrawableCount = static_cast<std::uint32_t>(layer.maximumDrawableCount);
            caps.layerPresentsWithTransaction = layer.presentsWithTransaction;
            caps.layerFramebufferOnly = layer.framebufferOnly;
            if ([layer respondsToSelector:@selector(allowsNextDrawableTimeout)])
                caps.layerAllowsNextDrawableTimeout = layer.allowsNextDrawableTimeout;
            else
                caps.layerAllowsNextDrawableTimeout = true;
        }

        // Engine render resolution
        caps.engineRenderWidth = engineRenderWidth;
        caps.engineRenderHeight = engineRenderHeight;

        // Presentation/vsync state currently known to engine
        caps.engineVsyncEnabled = layer ? layer.displaySyncEnabled : true;
    }

    return caps;
}
