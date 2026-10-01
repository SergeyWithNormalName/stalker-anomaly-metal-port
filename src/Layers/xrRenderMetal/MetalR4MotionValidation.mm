#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "R4StaticMotionValidation.h"
#include "R4TemporalTextureValidation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr NSUInteger ReadbackRowAlignment = 256u;
constexpr std::size_t SampleLimit = 16u;
constexpr double MovingThresholdPixels = 0.01;

struct Matrix4
{
    // Column-major, matching the engine's projection * view convention.
    double values[16]{};
};

struct Vec4
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
};

struct Sample
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    double expectedX = 0.0;
    double expectedY = 0.0;
    double actualX = 0.0;
    double actualY = 0.0;
    double errorPixels = 0.0;
};

struct Metrics
{
    std::uint64_t validPixels = 0;
    std::uint64_t movingPixels = 0;
    std::uint64_t actualNonzeroPixels = 0;
    std::uint64_t invalidPreviousW = 0;
    std::uint64_t nonFiniteMotion = 0;
    double expectedMin = std::numeric_limits<double>::infinity();
    double expectedMax = 0.0;
    double expectedSum = 0.0;
    double actualMin = std::numeric_limits<double>::infinity();
    double actualMax = 0.0;
    double actualSum = 0.0;
    double maxErrorPixels = 0.0;
    std::array<Sample, SampleLimit> samples{};
    std::array<bool, SampleLimit> sampleBinValid{};
    std::array<double, SampleLimit> sampleBinDistances{};
    std::size_t sampleCount = 0u;

    Metrics()
    {
        sampleBinDistances.fill(std::numeric_limits<double>::infinity());
    }
};

struct ReportContext
{
    std::string family;
    std::string outputDirectory;
    std::uint32_t engineFrame = 0;
    std::uint32_t previousFrame = 0;
    bool reset = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    NSUInteger depthBytesPerRow = 0;
    NSUInteger motionBytesPerRow = 0;
    std::array<float, 16> currentViewProjection{};
    std::array<float, 16> previousViewProjection{};
};

std::mutex& outputMutex()
{
    static std::mutex mutex;
    return mutex;
}

NSUInteger alignedRowBytes(NSUInteger tightBytes)
{
    return (tightBytes + ReadbackRowAlignment - 1u) & ~(ReadbackRowAlignment - 1u);
}

Matrix4 asMatrix(const std::array<float, 16>& values)
{
    Matrix4 result;
    for (std::size_t i = 0; i < 16u; ++i)
        result.values[i] = static_cast<double>(values[i]);
    return result;
}

Vec4 multiply(const Matrix4& matrix, const Vec4& vector)
{
    return {
        matrix.values[0] * vector.x + matrix.values[4] * vector.y + matrix.values[8] * vector.z + matrix.values[12] * vector.w,
        matrix.values[1] * vector.x + matrix.values[5] * vector.y + matrix.values[9] * vector.z + matrix.values[13] * vector.w,
        matrix.values[2] * vector.x + matrix.values[6] * vector.y + matrix.values[10] * vector.z + matrix.values[14] * vector.w,
        matrix.values[3] * vector.x + matrix.values[7] * vector.y + matrix.values[11] * vector.z + matrix.values[15] * vector.w,
    };
}

bool inverse(const Matrix4& matrix, Matrix4& result)
{
    // Gauss-Jordan with partial pivoting. Work in row-major augmented form,
    // while retaining the column-major representation at the interface.
    double augmented[4][8]{};
    for (std::size_t row = 0; row < 4u; ++row)
    {
        for (std::size_t column = 0; column < 4u; ++column)
            augmented[row][column] = matrix.values[column * 4u + row];
        augmented[row][4u + row] = 1.0;
    }

    for (std::size_t column = 0; column < 4u; ++column)
    {
        std::size_t pivot = column;
        for (std::size_t row = column + 1u; row < 4u; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
                pivot = row;
        const double pivotValue = augmented[pivot][column];
        if (!std::isfinite(pivotValue) || std::abs(pivotValue) < 1.0e-14)
            return false;
        if (pivot != column)
            for (std::size_t index = 0; index < 8u; ++index)
                std::swap(augmented[pivot][index], augmented[column][index]);

        const double scale = augmented[column][column];
        for (double& value : augmented[column])
            value /= scale;
        for (std::size_t row = 0; row < 4u; ++row)
        {
            if (row == column)
                continue;
            const double factor = augmented[row][column];
            for (std::size_t index = 0; index < 8u; ++index)
                augmented[row][index] -= factor * augmented[column][index];
        }
    }

    for (std::size_t row = 0; row < 4u; ++row)
        for (std::size_t column = 0; column < 4u; ++column)
            result.values[column * 4u + row] = augmented[row][4u + column];
    return true;
}

double halfToDouble(const std::uint16_t bits)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16u;
    const std::uint32_t exponent = (bits >> 10u) & 0x1fu;
    const std::uint32_t mantissa = bits & 0x03ffu;
    std::uint32_t floatBits = 0u;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
            floatBits = sign;
        else
        {
            std::uint32_t normalized = mantissa;
            int shift = 0;
            while ((normalized & 0x0400u) == 0u)
            {
                normalized <<= 1u;
                ++shift;
            }
            normalized &= 0x03ffu;
            const std::uint32_t floatExponent = static_cast<std::uint32_t>(127 - 15 - shift + 1);
            floatBits = sign | (floatExponent << 23u) | (normalized << 13u);
        }
    }
    else if (exponent == 0x1fu)
        floatBits = sign | 0x7f800000u | (mantissa << 13u);
    else
        floatBits = sign | ((exponent + (127u - 15u)) << 23u) | (mantissa << 13u);
    float value = 0.0f;
    std::memcpy(&value, &floatBits, sizeof(value));
    return static_cast<double>(value);
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream escaped;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20u)
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
            else
                escaped << static_cast<char>(character);
        }
    }
    return escaped.str();
}

std::string safeFilenamePart(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value)
        result.push_back(((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_') ?
            static_cast<char>(character) : '_');
    if (result.empty())
        result = "unnamed";
    return result;
}

double magnitude(const double x, const double y, const std::uint32_t width, const std::uint32_t height)
{
    const double pixelX = x * static_cast<double>(width);
    const double pixelY = y * static_cast<double>(height);
    return std::sqrt(pixelX * pixelX + pixelY * pixelY);
}

Metrics analyze(const ReportContext& context, const std::uint8_t* depthBytes,
    const std::uint8_t* motionBytes, bool& inverseValid)
{
    Metrics metrics;
    const Matrix4 current = asMatrix(context.currentViewProjection);
    const Matrix4 previous = asMatrix(context.previousViewProjection);
    Matrix4 inverseCurrent;
    inverseValid = inverse(current, inverseCurrent);
    if (!inverseValid)
        return metrics;

    for (std::uint32_t y = 0; y < context.height; ++y)
    {
        const std::uint8_t* depthRow = depthBytes + static_cast<std::size_t>(y) * context.depthBytesPerRow;
        const std::uint8_t* motionRow = motionBytes + static_cast<std::size_t>(y) * context.motionBytesPerRow;
        for (std::uint32_t x = 0; x < context.width; ++x)
        {
            float depth = 1.0f;
            std::memcpy(&depth, depthRow + static_cast<std::size_t>(x) * sizeof(float), sizeof(depth));
            if (!std::isfinite(depth) || depth >= 1.0f)
                continue;
            if (depth < 0.0f)
                continue;

            const std::uint16_t* motionHalf = reinterpret_cast<const std::uint16_t*>(
                motionRow + static_cast<std::size_t>(x) * 8u);
            const double actualX = halfToDouble(motionHalf[0]);
            const double actualY = halfToDouble(motionHalf[1]);
            if (!std::isfinite(actualX) || !std::isfinite(actualY))
            {
                ++metrics.nonFiniteMotion;
                continue;
            }

            const double currentU = (static_cast<double>(x) + 0.5) / context.width;
            const double currentV = (static_cast<double>(y) + 0.5) / context.height;
            const Vec4 clip{currentU * 2.0 - 1.0, 1.0 - currentV * 2.0, depth, 1.0};
            Vec4 world = multiply(inverseCurrent, clip);
            if (!std::isfinite(world.w) || std::abs(world.w) < 1.0e-14)
                continue;
            world.x /= world.w;
            world.y /= world.w;
            world.z /= world.w;
            world.w = 1.0;

            const Vec4 previousClip = multiply(previous, world);
            if (!std::isfinite(previousClip.w) || previousClip.w <= 1.0e-9)
            {
                ++metrics.invalidPreviousW;
                continue;
            }
            const double previousU = (previousClip.x / previousClip.w + 1.0) * 0.5;
            const double previousV = (1.0 - previousClip.y / previousClip.w) * 0.5;
            const double expectedX = currentU - previousU;
            const double expectedY = currentV - previousV;
            if (!std::isfinite(expectedX) || !std::isfinite(expectedY))
                continue;

            const double expectedMagnitude = magnitude(expectedX, expectedY, context.width, context.height);
            const double actualMagnitude = magnitude(actualX, actualY, context.width, context.height);
            const double errorPixels = magnitude(actualX - expectedX, actualY - expectedY,
                context.width, context.height);
            ++metrics.validPixels;
            if (expectedMagnitude > MovingThresholdPixels)
                ++metrics.movingPixels;
            if (actualMagnitude > MovingThresholdPixels)
                ++metrics.actualNonzeroPixels;
            metrics.expectedMin = std::min(metrics.expectedMin, expectedMagnitude);
            metrics.expectedMax = std::max(metrics.expectedMax, expectedMagnitude);
            metrics.expectedSum += expectedMagnitude;
            metrics.actualMin = std::min(metrics.actualMin, actualMagnitude);
            metrics.actualMax = std::max(metrics.actualMax, actualMagnitude);
            metrics.actualSum += actualMagnitude;
            metrics.maxErrorPixels = std::max(metrics.maxErrorPixels, errorPixels);

            const std::uint32_t binX = std::min<std::uint32_t>(3u,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * 4u) / context.width));
            const std::uint32_t binY = std::min<std::uint32_t>(3u,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * 4u) / context.height));
            const std::size_t bin = static_cast<std::size_t>(binY * 4u + binX);
            const double centerX = (static_cast<double>(binX) + 0.5) * context.width / 4.0;
            const double centerY = (static_cast<double>(binY) + 0.5) * context.height / 4.0;
            const double distance = (static_cast<double>(x) + 0.5 - centerX) *
                (static_cast<double>(x) + 0.5 - centerX) +
                (static_cast<double>(y) + 0.5 - centerY) *
                (static_cast<double>(y) + 0.5 - centerY);
            if (distance < metrics.sampleBinDistances[bin])
            {
                metrics.sampleBinDistances[bin] = distance;
                if (!metrics.sampleBinValid[bin])
                {
                    metrics.sampleBinValid[bin] = true;
                    ++metrics.sampleCount;
                }
                metrics.samples[bin] = {x, y, expectedX, expectedY, actualX, actualY, errorPixels};
            }
        }
    }
    return metrics;
}

void writeReport(const ReportContext& context, const Metrics& metrics, bool inverseValid,
    bool commandSucceeded, const std::string& commandError)
{
    const bool passed = commandSucceeded && inverseValid && metrics.validPixels > 0u &&
        metrics.nonFiniteMotion == 0u && metrics.maxErrorPixels <= (context.reset ? 0.01 : 0.5);
    const char* result = commandSucceeded && inverseValid && metrics.validPixels == 0u &&
        metrics.nonFiniteMotion == 0u ? "NO_COVERAGE" : (passed ? "PASS" : "FAIL");
    const auto average = [](double sum, std::uint64_t count) {
        return count == 0u ? 0.0 : sum / static_cast<double>(count);
    };
    const std::string filename = "r4_motion_" + safeFilenamePart(context.family) + "_frame_" +
        std::to_string(context.engineFrame) + ".json";
    const std::filesystem::path outputPath = std::filesystem::path(context.outputDirectory) / filename;

    std::lock_guard<std::mutex> lock(outputMutex());
    std::error_code filesystemError;
    std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
    std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
    if (!output)
    {
        std::cerr << "R4 motion validation " << context.family << " frame " << context.engineFrame
                  << " FAIL: cannot write " << outputPath.string() << std::endl;
        return;
    }
    output << std::setprecision(9)
        << "{\n"
        << "  \"schema\": \"r4_static_motion_validation_v1\",\n"
        << "  \"result\": \"" << result << "\",\n"
        << "  \"family\": \"" << jsonEscape(context.family) << "\",\n"
        << "  \"engine_frame\": " << context.engineFrame << ",\n"
        << "  \"previous_frame\": " << context.previousFrame << ",\n"
        << "  \"reset\": " << (context.reset ? "true" : "false") << ",\n"
        << "  \"extent\": {\"width\": " << context.width << ", \"height\": " << context.height << "},\n"
        << "  \"threshold_pixels\": " << (context.reset ? 0.01 : 0.5) << ",\n"
        << "  \"inverse_current_vp_valid\": " << (inverseValid ? "true" : "false") << ",\n"
        << "  \"command_completed\": " << (commandSucceeded ? "true" : "false") << ",\n"
        << "  \"command_error\": \"" << jsonEscape(commandError) << "\",\n"
        << "  \"valid_pixel_count\": " << metrics.validPixels << ",\n"
        << "  \"expected_moving_pixel_count\": " << metrics.movingPixels << ",\n"
        << "  \"actual_nonzero_pixel_count\": " << metrics.actualNonzeroPixels << ",\n"
        << "  \"invalid_previous_w_pixel_count\": " << metrics.invalidPreviousW << ",\n"
        << "  \"non_finite_motion_pixel_count\": " << metrics.nonFiniteMotion << ",\n"
        << "  \"max_error_pixels\": " << metrics.maxErrorPixels << ",\n"
        << "  \"expected_magnitude_pixels\": {\"min\": " << (metrics.validPixels ? metrics.expectedMin : 0.0)
        << ", \"max\": " << metrics.expectedMax << ", \"average\": "
        << average(metrics.expectedSum, metrics.validPixels) << "},\n"
        << "  \"actual_magnitude_pixels\": {\"min\": " << (metrics.validPixels ? metrics.actualMin : 0.0)
        << ", \"max\": " << metrics.actualMax << ", \"average\": "
        << average(metrics.actualSum, metrics.validPixels) << "},\n"
        << "  \"samples\": [\n";
    bool firstSample = true;
    for (std::size_t i = 0; i < SampleLimit; ++i)
    {
        if (!metrics.sampleBinValid[i])
            continue;
        const Sample& sample = metrics.samples[i];
        output << (firstSample ? "    {\"x\": " : ",\n    {\"x\": ") << sample.x << ", \"y\": " << sample.y
               << ", \"expected_uv\": [" << sample.expectedX << ", " << sample.expectedY
               << "], \"actual_uv\": [" << sample.actualX << ", " << sample.actualY
               << "], \"error_pixels\": " << sample.errorPixels << "}";
        firstSample = false;
    }
    output << "\n  ],\n  \"sample_count\": " << metrics.sampleCount << "\n}\n";
    output.close();
    std::cerr << "R4 motion validation " << context.family << " frame " << context.engineFrame
              << " " << result << ": valid=" << metrics.validPixels
              << " moving=" << metrics.movingPixels << " max_error=" << metrics.maxErrorPixels
              << "px report=" << outputPath.string();
    if (!commandError.empty())
        std::cerr << " error=" << commandError;
    std::cerr << std::endl;
}

std::string localizedError(id<MTLCommandBuffer> commandBuffer)
{
    NSError* error = commandBuffer.error;
    if (!error)
        return "command buffer did not complete successfully";
    const char* description = error.localizedDescription.UTF8String;
    return description ? description : "unknown Metal command-buffer error";
}
} // namespace

bool EncodeMetalR4StaticMotionValidation(void* commandBufferHandle, void* depthHandle,
    void* motionHandle, const R4StaticMotionValidationDesc& descriptor, std::string& error)
{
    id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)commandBufferHandle;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)depthHandle;
    id<MTLTexture> motion = (__bridge id<MTLTexture>)motionHandle;
    if (!commandBuffer || !depth || !motion)
    {
        error = "R4 motion validation requires a command buffer, depth texture, and motion texture";
        return false;
    }
    if (depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
        motion.pixelFormat != MTLPixelFormatRGBA16Float)
    {
        error = "R4 motion validation requires Depth32Float_Stencil8 and RGBA16Float textures";
        return false;
    }
    if (depth.width == 0u || depth.height == 0u || depth.width != motion.width || depth.height != motion.height)
    {
        error = "R4 motion validation textures must have the same non-zero extent";
        return false;
    }
    if (depth.sampleCount != 1u || motion.sampleCount != 1u)
    {
        error = "R4 motion validation requires single-sample depth and motion textures";
        return false;
    }
    if (descriptor.outputDirectory.empty() || descriptor.family.empty())
    {
        error = "R4 motion validation requires a family label and explicit output directory";
        return false;
    }

    const NSUInteger width = depth.width;
    const NSUInteger height = depth.height;
    const NSUInteger depthRowBytes = alignedRowBytes(width * sizeof(float));
    const NSUInteger motionRowBytes = alignedRowBytes(width * 8u);
    if (height > std::numeric_limits<NSUInteger>::max() / depthRowBytes ||
        height > std::numeric_limits<NSUInteger>::max() / motionRowBytes)
    {
        error = "R4 motion validation readback buffer size overflow";
        return false;
    }
    id<MTLDevice> device = commandBuffer.device;
    id<MTLBuffer> depthReadback = [device newBufferWithLength:depthRowBytes * height options:MTLResourceStorageModeShared];
    id<MTLBuffer> motionReadback = [device newBufferWithLength:motionRowBytes * height options:MTLResourceStorageModeShared];
    if (!depthReadback || !motionReadback)
    {
        error = "Metal failed to allocate R4 motion validation readback buffers";
        return false;
    }
    depthReadback.label = [NSString stringWithFormat:@"R4 motion depth readback %s", descriptor.family.c_str()];
    motionReadback.label = [NSString stringWithFormat:@"R4 motion vector readback %s", descriptor.family.c_str()];

    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (!blit)
    {
        error = "Metal failed to create R4 motion validation blit encoder";
        return false;
    }
    const MTLSize size = MTLSizeMake(width, height, 1u);
    [blit copyFromTexture:depth sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u, 0u, 0u)
        sourceSize:size toBuffer:depthReadback destinationOffset:0u destinationBytesPerRow:depthRowBytes
        destinationBytesPerImage:depthRowBytes * height options:MTLBlitOptionDepthFromDepthStencil];
    [blit copyFromTexture:motion sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u, 0u, 0u)
        sourceSize:size toBuffer:motionReadback destinationOffset:0u destinationBytesPerRow:motionRowBytes
        destinationBytesPerImage:motionRowBytes * height options:MTLBlitOptionNone];
    [blit endEncoding];

    auto context = std::make_shared<ReportContext>();
    context->family = descriptor.family;
    context->outputDirectory = descriptor.outputDirectory;
    context->engineFrame = descriptor.engineFrame;
    context->previousFrame = descriptor.previousFrame;
    context->reset = descriptor.reset;
    context->width = static_cast<std::uint32_t>(width);
    context->height = static_cast<std::uint32_t>(height);
    context->depthBytesPerRow = depthRowBytes;
    context->motionBytesPerRow = motionRowBytes;
    context->currentViewProjection = descriptor.currentViewProjection;
    context->previousViewProjection = descriptor.previousViewProjection;

    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
        const bool succeeded = completedBuffer.status == MTLCommandBufferStatusCompleted;
        const std::string commandError = succeeded ? std::string() : localizedError(completedBuffer);
        Metrics metrics;
        bool inverseValid = false;
        if (succeeded)
        {
            metrics = analyze(*context,
                static_cast<const std::uint8_t*>(depthReadback.contents),
                static_cast<const std::uint8_t*>(motionReadback.contents), inverseValid);
        }
        writeReport(*context, metrics, inverseValid, succeeded, commandError);
    }];
    return true;
}

namespace
{
struct TemporalTextureRecord
{
    std::string label;
    std::string formatName;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    NSUInteger bytesPerRow = 0;
    std::uint32_t bytesPerPixel = 0;
    bool rgba8 = false;
    bool bgra8 = false;
};

struct TemporalReportContext
{
    std::vector<std::shared_ptr<IR4Texture>> textureKeepAlive;
    std::vector<TemporalTextureRecord> records;
    std::string phase;
    std::string outputDirectory;
    std::uint32_t engineFrame = 0;
    std::uint32_t previousFrame = 0;
    bool expectZero = false;
    bool expectRgbZero = false;
    bool expectAlphaOne = false;
    bool expectFinite = false;
    bool writeImages = false;
    bool expectEqualPair = false;
    bool comparePair = false;
    bool expectDifferentPair = false;
};

struct TemporalTextureMetrics
{
    std::uint64_t nonzeroRgbPixelCount = 0;
    std::uint64_t nonzeroRgbChannelCount = 0;
    std::uint64_t finiteRgbChannelCount = 0;
    std::uint64_t nonFiniteRgbChannelCount = 0;
    std::uint64_t nonzeroAlphaPixelCount = 0;
    std::uint64_t nonzeroAlphaChannelCount = 0;
    std::uint64_t finiteAlphaChannelCount = 0;
    std::uint64_t nonFiniteAlphaChannelCount = 0;
    std::uint64_t finiteRgbaChannelCount = 0;
    std::uint64_t nonFiniteRgbaChannelCount = 0;
    double minimumRgb = std::numeric_limits<double>::infinity();
    double maximumRgb = -std::numeric_limits<double>::infinity();
    double minimumAlpha = std::numeric_limits<double>::infinity();
    double maximumAlpha = -std::numeric_limits<double>::infinity();
    bool anyFiniteRgb = false;
    bool anyFiniteAlpha = false;
    bool allRgbZero = true;
    bool allRgbaZero = true;
};

TemporalTextureMetrics analyzeTemporalTexture(const TemporalTextureRecord& record, const std::uint8_t* bytes)
{
    TemporalTextureMetrics metrics;
    for (std::uint32_t y = 0; y < record.height; ++y)
    {
        const std::uint8_t* row = bytes + static_cast<std::size_t>(y) * record.bytesPerRow;
        for (std::uint32_t x = 0; x < record.width; ++x)
        {
            const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * record.bytesPerPixel;
            bool pixelNonzero = false;
            bool alphaNonzero = false;
            for (std::size_t channel = 0; channel < 4u; ++channel)
            {
                double value = 0.0;
                if (record.rgba8)
                    value = static_cast<double>(pixel[channel]) / 255.0;
                else
                {
                    std::uint16_t halfBits = 0u;
                    std::memcpy(&halfBits, pixel + channel * sizeof(halfBits), sizeof(halfBits));
                    value = halfToDouble(halfBits);
                }
                if (!std::isfinite(value))
                {
                    ++metrics.nonFiniteRgbaChannelCount;
                    if (channel == 3u)
                        ++metrics.nonFiniteAlphaChannelCount;
                    else
                        ++metrics.nonFiniteRgbChannelCount;
                    continue;
                }
                ++metrics.finiteRgbaChannelCount;
                if (channel == 3u)
                {
                    ++metrics.finiteAlphaChannelCount;
                    metrics.anyFiniteAlpha = true;
                    metrics.minimumAlpha = std::min(metrics.minimumAlpha, value);
                    metrics.maximumAlpha = std::max(metrics.maximumAlpha, value);
                }
                else
                {
                    ++metrics.finiteRgbChannelCount;
                    metrics.anyFiniteRgb = true;
                    metrics.minimumRgb = std::min(metrics.minimumRgb, value);
                    metrics.maximumRgb = std::max(metrics.maximumRgb, value);
                }
                if (value != 0.0)
                {
                    metrics.allRgbaZero = false;
                    if (channel == 3u)
                    {
                        ++metrics.nonzeroAlphaChannelCount;
                        alphaNonzero = true;
                    }
                    else
                    {
                        ++metrics.nonzeroRgbChannelCount;
                        pixelNonzero = true;
                        metrics.allRgbZero = false;
                    }
                }
            }
            if (pixelNonzero)
                ++metrics.nonzeroRgbPixelCount;
            if (alphaNonzero)
                ++metrics.nonzeroAlphaPixelCount;
        }
    }
    return metrics;
}

bool writeTemporalPpm(const std::filesystem::path& path, const TemporalTextureRecord& record,
    const std::uint8_t* bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output)
        return false;
    std::string normalizedLabel;
    for (const unsigned char character : record.label)
        if (std::isalnum(character))
            normalizedLabel.push_back(static_cast<char>(std::tolower(character)));
    const bool visualizeAlpha = normalizedLabel == "ao";
    output << "P6\n" << record.width << " " << record.height << "\n255\n";
    std::vector<char> rgb(static_cast<std::size_t>(record.width) * 3u);
    for (std::uint32_t y = 0; y < record.height; ++y)
    {
        const std::uint8_t* row = bytes + static_cast<std::size_t>(y) * record.bytesPerRow;
        for (std::uint32_t x = 0; x < record.width; ++x)
        {
            const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * record.bytesPerPixel;
            std::array<std::uint8_t,4> encoded{};
            if (!record.rgba8)
            {
                for (unsigned channel=0; channel<4; ++channel)
                {
                    std::uint16_t bits; std::memcpy(&bits, pixel+channel*2u,2u);
                    double value=halfToDouble(bits);
                    // Diagnostic preview only: linear HDR tone mapping, not parity data.
                    value = std::isfinite(value) ? std::max(0.0,value) : 0.0;
                    value = channel==3u ? std::min(1.0,value) : value/(1.0+value);
                    encoded[channel]=static_cast<std::uint8_t>(std::round(value*255.0));
                }
                pixel=encoded.data();
            }
            const std::uint8_t red = visualizeAlpha ? pixel[3] : pixel[record.bgra8 ? 2u : 0u];
            const std::uint8_t green = visualizeAlpha ? pixel[3] : pixel[1];
            const std::uint8_t blue = visualizeAlpha ? pixel[3] : pixel[record.bgra8 ? 0u : 2u];
            rgb[static_cast<std::size_t>(x) * 3u + 0u] = static_cast<char>(red);
            rgb[static_cast<std::size_t>(x) * 3u + 1u] = static_cast<char>(green);
            rgb[static_cast<std::size_t>(x) * 3u + 2u] = static_cast<char>(blue);
        }
        output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
    output.close();
    return static_cast<bool>(output);
}

void writeTemporalReport(const TemporalReportContext& context,
    const std::vector<TemporalTextureMetrics>& metrics,
    const std::vector<std::uint64_t>& pairMismatchPixels,
    const std::vector<std::uint64_t>& pairMismatchBytes,
    const std::vector<std::string>& ppmPaths, bool commandSucceeded,
    const std::string& commandError)
{
    const bool isOutputPhase = context.phase == "output";
    bool passed = commandSucceeded && metrics.size() == context.records.size();
    for (const TemporalTextureMetrics& textureMetrics : metrics)
    {
        if (context.expectZero && (!textureMetrics.allRgbaZero || textureMetrics.nonFiniteRgbaChannelCount != 0u))
            passed = false;
        if (context.expectRgbZero && !textureMetrics.allRgbZero)
            passed = false;
        if (context.expectAlphaOne && (!textureMetrics.anyFiniteAlpha ||
            textureMetrics.minimumAlpha != 1.0 || textureMetrics.maximumAlpha != 1.0))
            passed = false;
        if ((isOutputPhase || context.expectFinite) && (textureMetrics.finiteRgbaChannelCount == 0u ||
            textureMetrics.nonFiniteRgbaChannelCount != 0u))
            passed = false;
    }
    if (context.expectEqualPair && (pairMismatchPixels.size() != 1u || pairMismatchBytes.size() != 1u ||
        pairMismatchPixels[0] != 0u || pairMismatchBytes[0] != 0u))
        passed = false;

    if (context.expectDifferentPair && (pairMismatchPixels.size() != 1u || pairMismatchPixels[0] == 0u))
        passed = false;

    const std::string filename = "r4_temporal_" + safeFilenamePart(context.phase) + "_frame_" +
        std::to_string(context.engineFrame) + ".json";
    const std::filesystem::path outputPath = std::filesystem::path(context.outputDirectory) / filename;
    std::lock_guard<std::mutex> lock(outputMutex());
    std::error_code filesystemError;
    std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
    std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
    if (!output)
    {
        std::cerr << "R4 temporal texture validation " << context.phase << " frame "
                  << context.engineFrame << " FAIL: cannot write " << outputPath.string() << std::endl;
        return;
    }
    output << std::setprecision(9)
        << "{\n"
        << "  \"schema\": \"r4_temporal_texture_validation_v1\",\n"
        << "  \"result\": \"" << (passed ? "PASS" : "FAIL") << "\",\n"
        << "  \"phase\": \"" << jsonEscape(context.phase) << "\",\n"
        << "  \"engine_frame\": " << context.engineFrame << ",\n"
        << "  \"previous_frame\": " << context.previousFrame << ",\n"
        << "  \"expect_zero\": " << (context.expectZero ? "true" : "false") << ",\n"
        << "  \"expect_equal_pair\": " << (context.expectEqualPair ? "true" : "false") << ",\n"
        << "  \"command_completed\": " << (commandSucceeded ? "true" : "false") << ",\n"
        << "  \"command_error\": \"" << jsonEscape(commandError) << "\",\n"
        << "  \"textures\": [\n";
    for (std::size_t i = 0; i < context.records.size(); ++i)
    {
        const TemporalTextureRecord& record = context.records[i];
        const TemporalTextureMetrics& value = i < metrics.size() ? metrics[i] : TemporalTextureMetrics{};
        const bool hasMetrics = i < metrics.size();
        const std::string ppmPath = i < ppmPaths.size() ? ppmPaths[i] : std::string();
        output << "    {\"label\": \"" << jsonEscape(record.label)
               << "\", \"format\": \"" << record.formatName
               << "\", \"width\": " << record.width << ", \"height\": " << record.height
               << ", \"nonzero_rgb_pixel_count\": " << value.nonzeroRgbPixelCount
               << ", \"nonzero_rgb_channel_count\": " << value.nonzeroRgbChannelCount
               << ", \"finite_rgb_channel_count\": " << value.finiteRgbChannelCount
               << ", \"non_finite_rgb_channel_count\": " << value.nonFiniteRgbChannelCount
               << ", \"nonzero_alpha_pixel_count\": " << value.nonzeroAlphaPixelCount
               << ", \"nonzero_alpha_channel_count\": " << value.nonzeroAlphaChannelCount
               << ", \"finite_alpha_channel_count\": " << value.finiteAlphaChannelCount
               << ", \"non_finite_alpha_channel_count\": " << value.nonFiniteAlphaChannelCount
               << ", \"finite_rgba_channel_count\": " << value.finiteRgbaChannelCount
               << ", \"non_finite_rgba_channel_count\": " << value.nonFiniteRgbaChannelCount
               << ", \"rgb_min\": " << (value.anyFiniteRgb ? value.minimumRgb : 0.0)
               << ", \"rgb_max\": " << (value.anyFiniteRgb ? value.maximumRgb : 0.0)
               << ", \"alpha_min\": " << (value.anyFiniteAlpha ? value.minimumAlpha : 0.0)
               << ", \"alpha_max\": " << (value.anyFiniteAlpha ? value.maximumAlpha : 0.0)
               << ", \"all_rgb_zero\": " << (hasMetrics && value.allRgbZero && value.nonFiniteRgbChannelCount == 0u ? "true" : "false")
               << ", \"all_rgba_zero\": " << (hasMetrics && value.allRgbaZero && value.nonFiniteRgbaChannelCount == 0u ? "true" : "false")
               << ", \"ppm_path\": \"" << jsonEscape(ppmPath) << "\"}"
               << (i + 1u == context.records.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"pair_exact\": ";
    if (!context.comparePair)
        output << "null,\n  \"pair_mismatch_pixel_count\": null,\n  \"pair_mismatch_byte_count\": null\n";
    else if (pairMismatchPixels.empty() || pairMismatchBytes.empty())
        output << "false,\n  \"pair_mismatch_pixel_count\": 0,\n  \"pair_mismatch_byte_count\": 0\n";
    else
        output << (pairMismatchPixels[0] == 0u && pairMismatchBytes[0] == 0u ? "true" : "false")
               << ",\n  \"pair_mismatch_pixel_count\": " << pairMismatchPixels[0]
               << ",\n  \"pair_mismatch_byte_count\": " << pairMismatchBytes[0] << "\n";
    output << "}\n";
    output.close();
    std::cerr << "R4 temporal texture validation " << context.phase << " frame " << context.engineFrame
              << " " << (passed ? "PASS" : "FAIL") << ": textures=" << metrics.size()
              << " report=" << outputPath.string();
    if (!commandError.empty())
        std::cerr << " error=" << commandError;
    std::cerr << std::endl;
}
} // namespace

bool EncodeMetalR4TemporalTextureValidation(void* commandBufferHandle,
    const std::vector<void*>& nativeTextures,
    const R4TemporalTextureValidationDesc& descriptor, std::string& error)
{
    id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)commandBufferHandle;
    if (!commandBuffer)
    {
        error = "R4 temporal texture validation requires a command buffer";
        return false;
    }
    if (descriptor.outputDirectory.empty() || descriptor.phase.empty() || descriptor.textures.empty() ||
        nativeTextures.size() != descriptor.textures.size() || descriptor.labels.size() != descriptor.textures.size())
    {
        error = "R4 temporal texture validation requires matching texture, label, and native-handle arrays plus phase/output directory";
        return false;
    }
    if ((descriptor.expectEqualPair || descriptor.comparePair || descriptor.expectDifferentPair) && descriptor.textures.size() != 2u)
    {
        error = "R4 temporal pair equality validation requires exactly two textures";
        return false;
    }

    auto context = std::make_shared<TemporalReportContext>();
    context->textureKeepAlive = descriptor.textures;
    context->phase = descriptor.phase;
    context->outputDirectory = descriptor.outputDirectory;
    context->engineFrame = descriptor.engineFrame;
    context->previousFrame = descriptor.previousFrame;
    context->expectZero = descriptor.expectZero;
    context->expectRgbZero = descriptor.expectRgbZero;
    context->expectAlphaOne = descriptor.expectAlphaOne;
    context->expectFinite = descriptor.expectFinite;
    context->writeImages = descriptor.writeImages;
    context->expectEqualPair = descriptor.expectEqualPair;
    context->comparePair = descriptor.comparePair || descriptor.expectEqualPair || descriptor.expectDifferentPair;
    context->expectDifferentPair = descriptor.expectDifferentPair;

    id<MTLDevice> device = commandBuffer.device;
    NSMutableArray<id<MTLBuffer>>* readbackBuffers = [NSMutableArray arrayWithCapacity:nativeTextures.size()];
    std::vector<std::uint64_t> pairMismatchPixels;
    std::vector<std::uint64_t> pairMismatchBytes;
    auto ppmPaths = std::make_shared<std::vector<std::string>>(nativeTextures.size());
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (!blit)
    {
        error = "Metal failed to create R4 temporal texture validation blit encoder";
        return false;
    }

    for (std::size_t i = 0; i < nativeTextures.size(); ++i)
    {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)nativeTextures[i];
        if (!descriptor.textures[i])
        {
            [blit endEncoding];
            error = "R4 temporal texture validation received an empty IR4Texture reference";
            return false;
        }
        const R4Texture2DDesc& expected = descriptor.textures[i]->Descriptor();
        if (!texture)
        {
            [blit endEncoding];
            error = "R4 temporal texture validation received a null native texture";
            return false;
        }
        if (texture.textureType != MTLTextureType2D || texture.sampleCount != 1u ||
            texture.width == 0u || texture.height == 0u)
        {
            [blit endEncoding];
            error = "R4 temporal texture validation requires non-empty single-sample 2D textures";
            return false;
        }

        TemporalTextureRecord record;
        record.label = descriptor.labels[i];
        record.width = static_cast<std::uint32_t>(texture.width);
        record.height = static_cast<std::uint32_t>(texture.height);
        if (texture.pixelFormat == MTLPixelFormatRGBA8Unorm && expected.format == R4PixelFormat::RGBA8Unorm)
        {
            record.formatName = "RGBA8Unorm";
            record.bytesPerPixel = 4u;
            record.rgba8 = true;
        }
        else if (texture.pixelFormat == MTLPixelFormatBGRA8Unorm && expected.format == R4PixelFormat::BGRA8Unorm)
        {
            record.formatName = "BGRA8Unorm"; record.bytesPerPixel = 4u;
            record.rgba8 = true; record.bgra8 = true;
        }
        else if (texture.pixelFormat == MTLPixelFormatRGBA16Float && expected.format == R4PixelFormat::RGBA16Float)
        {
            record.formatName = "RGBA16Float";
            record.bytesPerPixel = 8u;
            record.rgba8 = false;
        }
        else
        {
            [blit endEncoding];
            error = "R4 temporal texture validation accepts only matching RGBA8Unorm/RGBA16Float resources";
            return false;
        }
        if (expected.width != record.width || expected.height != record.height)
        {
            [blit endEncoding];
            error = "R4 temporal native texture extent disagrees with its IR4Texture descriptor";
            return false;
        }
        record.bytesPerRow = alignedRowBytes(static_cast<NSUInteger>(record.width) * record.bytesPerPixel);
        if (record.height > std::numeric_limits<NSUInteger>::max() / record.bytesPerRow)
        {
            [blit endEncoding];
            error = "R4 temporal texture readback buffer size overflow";
            return false;
        }
        id<MTLBuffer> readback = [device newBufferWithLength:record.bytesPerRow * record.height
            options:MTLResourceStorageModeShared];
        if (!readback)
        {
            [blit endEncoding];
            error = "Metal failed to allocate R4 temporal texture readback buffer";
            return false;
        }
        readback.label = [NSString stringWithFormat:@"R4 temporal readback %s", record.label.c_str()];
        [readbackBuffers addObject:readback];
        [blit copyFromTexture:texture sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u, 0u, 0u)
            sourceSize:MTLSizeMake(record.width, record.height, 1u) toBuffer:readback destinationOffset:0u
            destinationBytesPerRow:record.bytesPerRow destinationBytesPerImage:record.bytesPerRow * record.height
            options:MTLBlitOptionNone];
        context->records.push_back(std::move(record));
    }

    if (descriptor.expectEqualPair || descriptor.comparePair || descriptor.expectDifferentPair)
    {
        const TemporalTextureRecord& first = context->records[0];
        const TemporalTextureRecord& second = context->records[1];
        if (first.width != second.width || first.height != second.height ||
            first.bytesPerPixel != second.bytesPerPixel || first.formatName != second.formatName)
        {
            [blit endEncoding];
            error = "R4 temporal exact-pair validation requires identical extents and pixel formats";
            return false;
        }
    }
    [blit endEncoding];

    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
        const bool commandSucceeded = completedBuffer.status == MTLCommandBufferStatusCompleted;
        const std::string commandError = commandSucceeded ? std::string() : localizedError(completedBuffer);
        std::vector<TemporalTextureMetrics> metrics;
        if (commandSucceeded)
        {
            metrics.reserve(context->records.size());
            for (std::size_t i = 0; i < context->records.size(); ++i)
            {
                const TemporalTextureRecord& record = context->records[i];
                id<MTLBuffer> readback = readbackBuffers[i];
                const auto* bytes = static_cast<const std::uint8_t*>(readback.contents);
                metrics.push_back(analyzeTemporalTexture(record, bytes));
                if ((context->phase == "output" && record.rgba8) || context->writeImages)
                {
                    const std::string ppmFilename = "r4_temporal_" + safeFilenamePart(context->phase) + "_" +
                        safeFilenamePart(record.label) + "_frame_" + std::to_string(context->engineFrame) + ".ppm";
                    const std::filesystem::path ppmPath = std::filesystem::path(context->outputDirectory) / ppmFilename;
                    std::lock_guard<std::mutex> outputLock(outputMutex());
                    std::error_code ppmFilesystemError;
                    std::filesystem::create_directories(ppmPath.parent_path(), ppmFilesystemError);
                    if (writeTemporalPpm(ppmPath, record, bytes))
                        (*ppmPaths)[i] = ppmPath.string();
                }
            }
        }

        std::vector<std::uint64_t> mismatchPixels;
        std::vector<std::uint64_t> mismatchBytes;
        if (commandSucceeded && context->comparePair && context->records.size() == 2u)
        {
            const TemporalTextureRecord& first = context->records[0];
            const TemporalTextureRecord& second = context->records[1];
            const auto* firstBytes = static_cast<const std::uint8_t*>([readbackBuffers[0] contents]);
            const auto* secondBytes = static_cast<const std::uint8_t*>([readbackBuffers[1] contents]);
            std::uint64_t pixelMismatches = 0u;
            std::uint64_t byteMismatches = 0u;
            for (std::uint32_t y = 0; y < first.height; ++y)
            {
                const std::uint8_t* firstRow = firstBytes + static_cast<std::size_t>(y) * first.bytesPerRow;
                const std::uint8_t* secondRow = secondBytes + static_cast<std::size_t>(y) * second.bytesPerRow;
                for (std::uint32_t x = 0; x < first.width; ++x)
                {
                    const std::uint8_t* firstPixel = firstRow + static_cast<std::size_t>(x) * first.bytesPerPixel;
                    const std::uint8_t* secondPixel = secondRow + static_cast<std::size_t>(x) * second.bytesPerPixel;
                    std::uint64_t localMismatch = 0u;
                    for (std::uint32_t byte = 0; byte < first.bytesPerPixel; ++byte)
                        if (firstPixel[byte] != secondPixel[byte])
                            ++localMismatch;
                    if (localMismatch != 0u)
                        ++pixelMismatches;
                    byteMismatches += localMismatch;
                }
            }
            mismatchPixels.push_back(pixelMismatches);
            mismatchBytes.push_back(byteMismatches);
        }
        writeTemporalReport(*context, metrics, mismatchPixels, mismatchBytes, *ppmPaths,
            commandSucceeded, commandError);
    }];
    return true;
}
