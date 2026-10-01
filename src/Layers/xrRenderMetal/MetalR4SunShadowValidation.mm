#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "R4SunShadowValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr NSUInteger RowAlignment = 256u;
constexpr double ClipEdgeTolerance = 1.0e-5;
constexpr double DepthCompareTolerance = 2.0e-6;

struct Matrix4 { double v[16]{}; };
struct Vec4 { double x=0.0, y=0.0, z=0.0, w=0.0; };

struct Readback
{
    __strong id<MTLBuffer> buffer = nil;
    NSUInteger rowBytes = 0;
};

struct PhaseCapture
{
    bool registered = false;
    R4SunShadowValidationDesc descriptor;
    std::uint32_t width = 0, height = 0;
    std::uint32_t mapWidth = 0, mapHeight = 0;
    Readback depth;
    Readback stencil;
    Readback map;
    Readback position;
    Readback accumulation;
};

struct Session
{
    std::mutex mutex;
    std::string key;
    std::array<PhaseCapture, 4> phases;
    bool reportWritten = false;
    bool commandFailed = false;
    std::string commandError;
};

struct PhaseMetrics
{
    std::string name;
    bool pass = false;
    double sourceCascadeSize = 0.0;
    double sourceCascadeBias = 0.0;
    std::uint64_t baselineMarkedPixels = 0;
    std::uint64_t validPositionPixels = 0;
    std::uint64_t positionInsideVolumePixels = 0;
    std::uint64_t expectedStencilPassPixels = 0;
    std::uint64_t stencilChangedPixels = 0;
    std::uint64_t stencilRelevantChangedPixels = 0;
    std::uint64_t rasterPassReceiverOutsideVolumePixels = 0;
    std::uint64_t stencilMismatchPixels = 0;
    std::uint64_t expectedStencilWriteMisses = 0;
    std::uint64_t edgeAmbiguousPixels = 0;
    std::uint64_t depthBoundaryAmbiguousPixels = 0;
    std::uint64_t mapCoveredPixels = 0;
    std::uint64_t mapNonFinitePixels = 0;
    double mapMinimum = std::numeric_limits<double>::infinity();
    double mapMaximum = -std::numeric_limits<double>::infinity();
    std::uint64_t accumulationNonzeroRgbPixels = 0;
    std::uint64_t accumulationNonFiniteChannels = 0;
    std::uint64_t accumulationFiniteChannels = 0;
    std::uint64_t hudPositionPixelsExcluded = 0;
    std::uint64_t clearPositionPixelsExcluded = 0;
    std::uint64_t cameraProjectablePixels = 0;
    double maxPositionScreenErrorPixels = 0.0;
    struct NonFiniteAccumulationSample
    {
        std::uint32_t x=0,y=0;
        float sceneDepth=0.0f;
        double packedPosition[4]{};
        double packedNormalXYRadiusSquared=0.0;
        double decodedNormalZArgument=0.0;
        double viewPosition[3]{};
        double accumulation[4]{};
    };
    std::vector<NonFiniteAccumulationSample> nonFiniteAccumulationSamples;
    struct StencilComparisonSample
    {
        std::uint32_t x=0,y=0;
        std::uint8_t before=0,after=0;
        float sceneDepth=0.0f;
        double faceDepth=std::numeric_limits<double>::quiet_NaN();
        double depthDelta=std::numeric_limits<double>::quiet_NaN();
        bool facePass=false,receiverInsideVolume=false,edgeAmbiguous=false;
        bool depthBoundaryAmbiguous=false;
        double packedPosition[4]{};
        double viewPosition[3]{};
    };
    std::vector<StencilComparisonSample> stencilComparisonSamples;
    struct DepthBoundarySample
    {
        std::uint32_t x=0,y=0;
        float sceneDepth=0.0f;
        double faceDepth=std::numeric_limits<double>::quiet_NaN();
        double depthDelta=std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<DepthBoundarySample> depthBoundarySamples;
};

std::mutex& sessionsMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::shared_ptr<Session>>& sessions()
{
    static std::map<std::string, std::shared_ptr<Session>> value;
    return value;
}

std::mutex& outputMutex()
{
    static std::mutex mutex;
    return mutex;
}

NSUInteger aligned(NSUInteger bytes)
{
    return (bytes + RowAlignment - 1u) & ~(RowAlignment - 1u);
}

std::size_t phaseIndex(R4SunShadowValidationPhase phase)
{
    return static_cast<std::size_t>(phase);
}

const char* phaseName(R4SunShadowValidationPhase phase)
{
    switch (phase)
    {
    case R4SunShadowValidationPhase::StencilBaseline: return "stencil-baseline";
    case R4SunShadowValidationPhase::Near: return "near";
    case R4SunShadowValidationPhase::Middle: return "middle";
    case R4SunShadowValidationPhase::Far: return "far";
    case R4SunShadowValidationPhase::OffCameraCasters: return "off-camera-casters";
    }
    return "invalid";
}

std::string safePart(const std::string& value)
{
    std::string output;
    output.reserve(value.size());
    for (const unsigned char ch : value)
        output.push_back(((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') ? static_cast<char>(ch) : '_');
    return output.empty() ? "unnamed" : output;
}

std::string escapeJson(const std::string& value)
{
    std::ostringstream out;
    for (const unsigned char ch : value)
    {
        if (ch == '"') out << "\\\"";
        else if (ch == '\\') out << "\\\\";
        else if (ch == '\n') out << "\\n";
        else if (ch == '\r') out << "\\r";
        else if (ch >= 0x20u) out << static_cast<char>(ch);
    }
    return out.str();
}

Matrix4 asMatrix(const std::array<float, 16>& input)
{
    Matrix4 output;
    for (std::size_t i = 0; i < 16u; ++i) output.v[i] = input[i];
    return output;
}

Vec4 multiply(const Matrix4& m, const Vec4& p)
{
    return {
        m.v[0]*p.x + m.v[4]*p.y + m.v[8]*p.z + m.v[12]*p.w,
        m.v[1]*p.x + m.v[5]*p.y + m.v[9]*p.z + m.v[13]*p.w,
        m.v[2]*p.x + m.v[6]*p.y + m.v[10]*p.z + m.v[14]*p.w,
        m.v[3]*p.x + m.v[7]*p.y + m.v[11]*p.z + m.v[15]*p.w,
    };
}

bool inverse(const Matrix4& m, Matrix4& out)
{
    double a[4][8]{};
    for (std::size_t r = 0; r < 4u; ++r)
    {
        for (std::size_t c = 0; c < 4u; ++c) a[r][c] = m.v[c*4u+r];
        a[r][4u+r] = 1.0;
    }
    for (std::size_t c = 0; c < 4u; ++c)
    {
        std::size_t pivot = c;
        for (std::size_t r = c + 1u; r < 4u; ++r)
            if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c]) < 1.0e-14) return false;
        if (pivot != c) for (std::size_t k = 0; k < 8u; ++k) std::swap(a[pivot][k], a[c][k]);
        const double d = a[c][c];
        for (double& value : a[c]) value /= d;
        for (std::size_t r = 0; r < 4u; ++r)
        {
            if (r == c) continue;
            const double factor = a[r][c];
            for (std::size_t k = 0; k < 8u; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (std::size_t r = 0; r < 4u; ++r)
        for (std::size_t c = 0; c < 4u; ++c) out.v[c*4u+r] = a[r][4u+c];
    return true;
}

double halfToDouble(std::uint16_t bits)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16u;
    const std::uint32_t exponent = (bits >> 10u) & 31u;
    const std::uint32_t mantissa = bits & 1023u;
    std::uint32_t raw = 0u;
    if (exponent == 0u)
    {
        if (mantissa == 0u) raw = sign;
        else
        {
            std::uint32_t normalized = mantissa;
            int shift = 0;
            while ((normalized & 0x400u) == 0u) { normalized <<= 1u; ++shift; }
            normalized &= 0x3ffu;
            raw = sign | (static_cast<std::uint32_t>(127 - 15 - shift + 1) << 23u) | (normalized << 13u);
        }
    }
    else if (exponent == 31u) raw = sign | 0x7f800000u | (mantissa << 13u);
    else raw = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    float result = 0.0f;
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

void writeJsonNumber(std::ostream& out, double value)
{
    if (std::isfinite(value)) out<<value;
    else out<<"null";
}

bool insideVolume(const Vec4& clip, bool& ambiguous)
{
    if (!std::isfinite(clip.w) || std::abs(clip.w) < 1.0e-12) return false;
    const double x = clip.x / clip.w, y = clip.y / clip.w, z = clip.z / clip.w;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    ambiguous = std::abs(std::abs(x)-1.0) < ClipEdgeTolerance ||
        std::abs(std::abs(y)-1.0) < ClipEdgeTolerance ||
        std::abs(z-0.7) < ClipEdgeTolerance || std::abs(z-1.0) < ClipEdgeTolerance;
    return x >= -1.0 && x <= 1.0 && y >= -1.0 && y <= 1.0 && z >= 0.7 && z <= 1.0;
}

struct SunVolumeTriangle
{
    double a[3]{};
    double b[3]{};
    double c[3]{};
    double clipOrientation = 0.0;
};

struct SunVolumeRayOracle
{
    std::array<SunVolumeTriangle,12> triangles{};
    bool valid = false;
};

SunVolumeRayOracle makeSunVolumeRayOracle(const Matrix4& volume, const Matrix4& current,
    const R4SunShadowValidationDesc& descriptor)
{
    static constexpr double localVertices[8][4] = {
        {-1.0,-1.0,0.70,1.0}, {-1.0,-1.0,1.00,1.0},
        {-1.0, 1.0,1.00,1.0}, {-1.0, 1.0,0.70,1.0},
        { 1.0, 1.0,1.00,1.0}, { 1.0, 1.0,0.70,1.0},
        { 1.0,-1.0,1.00,1.0}, { 1.0,-1.0,0.70,1.0},
    };
    static constexpr unsigned indices[36] = {
        3,2,1, 3,1,0, 7,6,5, 5,6,4,
        3,5,2, 4,2,5, 1,6,7, 7,0,1,
        5,3,0, 7,5,0, 1,4,6, 2,4,1,
    };
    SunVolumeRayOracle oracle;
    Matrix4 inverseVolume;
    if (!descriptor.hasReceiverVolume && !inverse(volume,inverseVolume)) return oracle;
    Vec4 world[8];
    Vec4 camera[8];
    for (unsigned i=0;i<8u;++i)
    {
        if (descriptor.hasReceiverVolume)
        {
            const auto& vertex=descriptor.receiverVolume[i];
            if (!std::isfinite(vertex[0])||!std::isfinite(vertex[1])||
                !std::isfinite(vertex[2])||!std::isfinite(vertex[3])||std::abs(vertex[3])<1.0e-12)
                return oracle;
            // These are the exact float4 POSITION values uploaded to the
            // production cube vertex buffer; normalize only homogeneous W.
            world[i]={vertex[0]/vertex[3],vertex[1]/vertex[3],vertex[2]/vertex[3],1.0};
        }
        else
        {
            world[i]=multiply(inverseVolume,{localVertices[i][0],localVertices[i][1],localVertices[i][2],1.0});
            if (!std::isfinite(world[i].w)||std::abs(world[i].w)<1.0e-12) return oracle;
            world[i]={world[i].x/world[i].w,world[i].y/world[i].w,world[i].z/world[i].w,1.0};
        }
        camera[i]=multiply(current,world[i]);
        if (!std::isfinite(camera[i].w)||std::abs(camera[i].w)<1.0e-12) return oracle;
    }
    for (unsigned i=0;i<oracle.triangles.size();++i)
    {
        const Vec4& a=world[indices[i*3u]];
        const Vec4& b=world[indices[i*3u+1u]];
        const Vec4& c=world[indices[i*3u+2u]];
        auto& triangle=oracle.triangles[i];
        triangle.a[0]=a.x; triangle.a[1]=a.y; triangle.a[2]=a.z;
        triangle.b[0]=b.x; triangle.b[1]=b.y; triangle.b[2]=b.z;
        triangle.c[0]=c.x; triangle.c[1]=c.y; triangle.c[2]=c.z;
        const Vec4& ca=camera[indices[i*3u]];
        const Vec4& cb=camera[indices[i*3u+1u]];
        const Vec4& cc=camera[indices[i*3u+2u]];
        // NDC area alone reverses when a source vertex has camera-space W < 0.
        // The homogeneous determinant preserves the winding after clipping the
        // triangle to positive W, including the normal camera-inside-cuboid case.
        const double ndcAreaNumerator=ca.x*(cb.y*cc.w-cb.w*cc.y)-
            ca.y*(cb.x*cc.w-cb.w*cc.x)+ca.w*(cb.x*cc.y-cb.y*cc.x);
        // Metal's viewport flips NDC Y. Clockwise front faces therefore have
        // negative signed NDC area for positive-W clipped vertices.
        triangle.clipOrientation=ndcAreaNumerator;
    }
    oracle.valid=true;
    return oracle;
}

bool rayVolumeDepthPass(const Matrix4& inverseCurrent, const Matrix4& current,
    const SunVolumeRayOracle& oracle, double ndcX, double ndcY, double sceneDepth,
    bool far, bool zCull, bool& edgeAmbiguous, double& hitFaceDepth,
    double& hitDepthDelta, bool& depthBoundaryAmbiguous)
{
    hitFaceDepth=std::numeric_limits<double>::quiet_NaN();
    hitDepthDelta=std::numeric_limits<double>::quiet_NaN();
    depthBoundaryAmbiguous=false;
    if (!oracle.valid) return false;
    Vec4 a = multiply(inverseCurrent, {ndcX, ndcY, 0.0, 1.0});
    Vec4 b = multiply(inverseCurrent, {ndcX, ndcY, 1.0, 1.0});
    if (!std::isfinite(a.w) || !std::isfinite(b.w) || std::abs(a.w) < 1.0e-12 || std::abs(b.w) < 1.0e-12)
        return false;
    a = {a.x/a.w, a.y/a.w, a.z/a.w, 1.0};
    b = {b.x/b.w, b.y/b.w, b.z/b.w, 1.0};
    const double direction[3]={b.x-a.x,b.y-a.y,b.z-a.z};
    for (const SunVolumeTriangle& triangle:oracle.triangles)
    {
        if (std::abs(triangle.clipOrientation)<1.0e-12) continue;
        // The PSO culls back faces and declares clockwise winding as front.
        if (triangle.clipOrientation>0.0) continue;
        const double e1[3]={triangle.b[0]-triangle.a[0],triangle.b[1]-triangle.a[1],triangle.b[2]-triangle.a[2]};
        const double e2[3]={triangle.c[0]-triangle.a[0],triangle.c[1]-triangle.a[1],triangle.c[2]-triangle.a[2]};
        const double pvec[3]={direction[1]*e2[2]-direction[2]*e2[1],
            direction[2]*e2[0]-direction[0]*e2[2],direction[0]*e2[1]-direction[1]*e2[0]};
        const double determinant=e1[0]*pvec[0]+e1[1]*pvec[1]+e1[2]*pvec[2];
        if (std::abs(determinant)<1.0e-14) continue;
        const double inverseDeterminant=1.0/determinant;
        const double fromA[3]={a.x-triangle.a[0],a.y-triangle.a[1],a.z-triangle.a[2]};
        const double u=(fromA[0]*pvec[0]+fromA[1]*pvec[1]+fromA[2]*pvec[2])*inverseDeterminant;
        const double qvec[3]={fromA[1]*e1[2]-fromA[2]*e1[1],
            fromA[2]*e1[0]-fromA[0]*e1[2],fromA[0]*e1[1]-fromA[1]*e1[0]};
        const double v=(direction[0]*qvec[0]+direction[1]*qvec[1]+direction[2]*qvec[2])*inverseDeterminant;
        if (u < -ClipEdgeTolerance || v < -ClipEdgeTolerance || u+v > 1.0+ClipEdgeTolerance) continue;
        const double t=(e2[0]*qvec[0]+e2[1]*qvec[1]+e2[2]*qvec[2])*inverseDeterminant;
        if (t < -ClipEdgeTolerance || t > 1.0+ClipEdgeTolerance) continue;
        if (u<=ClipEdgeTolerance || v<=ClipEdgeTolerance || 1.0-u-v<=ClipEdgeTolerance)
        { edgeAmbiguous=true; continue; }
        const Vec4 world{a.x+direction[0]*t,a.y+direction[1]*t,a.z+direction[2]*t,1.0};
        const Vec4 cameraClip=multiply(current,world);
        if (!std::isfinite(cameraClip.w) || cameraClip.w <= 1.0e-12 ||
            !std::isfinite(cameraClip.z) || cameraClip.z < 0.0 || cameraClip.z > cameraClip.w)
            continue;
        const double faceDepth = cameraClip.z / cameraClip.w;
        if (!std::isfinite(faceDepth)||faceDepth<0.0||faceDepth>1.0) continue;
        const double depthDelta=faceDepth-static_cast<double>(sceneDepth);
        hitFaceDepth=faceDepth;
        hitDepthDelta=depthDelta;
        if (far && !zCull) return true;
        if (std::abs(depthDelta)<=DepthCompareTolerance)
        {
            edgeAmbiguous=true;
            depthBoundaryAmbiguous=true;
            continue;
        }
        if (far ? faceDepth < sceneDepth : faceDepth > sceneDepth)
            return true;
    }
    return false;
}

bool readFloat(const Readback& readback, std::uint32_t x, std::uint32_t y, float& value)
{
    if (!readback.buffer || x * sizeof(float) >= readback.rowBytes) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(readback.buffer.contents);
    std::memcpy(&value, bytes + static_cast<std::size_t>(y) * readback.rowBytes + x * sizeof(float), sizeof(value));
    return true;
}

std::uint8_t stencilAt(const PhaseCapture& phase, std::uint32_t x, std::uint32_t y)
{
    const auto* bytes = static_cast<const std::uint8_t*>(phase.stencil.buffer.contents);
    return bytes[static_cast<std::size_t>(y) * phase.stencil.rowBytes + x];
}

bool reconstructViewPosition(const std::uint16_t* packed, std::uint32_t x, std::uint32_t y,
    const R4SunShadowValidationDesc& descriptor,
    double viewPosition[3])
{
    const double viewZ=halfToDouble(packed[2]);
    if (!std::isfinite(viewZ)) return false;
    for (const float value:descriptor.positionDecompressionParams)
        if (!std::isfinite(value)) return false;
    for (const float value:descriptor.positionJitter)
        if (!std::isfinite(value)) return false;
    const double pixelX=static_cast<double>(x)+0.5+descriptor.positionJitter[0];
    const double pixelY=static_cast<double>(y)+0.5+descriptor.positionJitter[1];
    viewPosition[0]=viewZ*(pixelX*descriptor.positionDecompressionParams[2]-
        descriptor.positionDecompressionParams[0]);
    viewPosition[1]=viewZ*(pixelY*descriptor.positionDecompressionParams[3]-
        descriptor.positionDecompressionParams[1]);
    viewPosition[2]=viewZ;
    return std::isfinite(viewPosition[0])&&std::isfinite(viewPosition[1])&&std::isfinite(viewPosition[2]);
}

bool eligibleWorldReceiver(float depth, const double packedPositionX,
    const double packedPositionY, const double viewPosition[3])
{
    constexpr double ClearEpsilon=1.0e-8;
    if (!std::isfinite(depth)||depth<=0.02f||depth>=1.0f||viewPosition[2]<=0.02) return false;
    if (std::abs(packedPositionX)<=ClearEpsilon && std::abs(packedPositionY)<=ClearEpsilon) return false;
    return true;
}

PhaseMetrics analyzePhase(const Session& session, std::size_t phaseIdx)
{
    const PhaseCapture& phase = session.phases[phaseIdx];
    PhaseMetrics metrics;
    metrics.name = phaseName(phase.descriptor.phase);
    metrics.sourceCascadeSize=phase.descriptor.sourceCascadeSize;
    metrics.sourceCascadeBias=phase.descriptor.sourceCascadeBias;
    if (phaseIdx == 0u)
    {
        for (std::uint32_t y = 0; y < phase.height; ++y)
            for (std::uint32_t x = 0; x < phase.width; ++x)
                if (stencilAt(phase,x,y) == phase.descriptor.stencilMarker) ++metrics.baselineMarkedPixels;
        metrics.pass = metrics.baselineMarkedPixels > 0u;
        return metrics;
    }

    const PhaseCapture& baseline = session.phases[0];
    const PhaseCapture& previous = session.phases[phaseIdx - 1u];
    const auto& d = phase.descriptor;
    Matrix4 inverseView = asMatrix(d.inverseView);
    Matrix4 current = asMatrix(d.currentViewProjection);
    Matrix4 inverseCurrent;
    Matrix4 volume = asMatrix(d.stageVolumeViewProjection);
    const bool invertible = inverse(current, inverseCurrent);
    const SunVolumeRayOracle volumeOracle=makeSunVolumeRayOracle(volume,current,phase.descriptor);
    std::uint64_t stencilMismatch = 0u;
    std::uint64_t checkedStencil = 0u;

    const auto* positionBytes = static_cast<const std::uint8_t*>(phase.position.buffer.contents);
    const auto* accumulationBytes = static_cast<const std::uint8_t*>(phase.accumulation.buffer.contents);
    const auto* mapBytes = static_cast<const std::uint8_t*>(phase.map.buffer.contents);
    for (std::uint32_t y = 0; y < phase.height; ++y)
    {
        const auto* positionRow = positionBytes + static_cast<std::size_t>(y)*phase.position.rowBytes;
        const auto* accumulationRow = accumulationBytes + static_cast<std::size_t>(y)*phase.accumulation.rowBytes;
        for (std::uint32_t x = 0; x < phase.width; ++x)
        {
            const auto* halves = reinterpret_cast<const std::uint16_t*>(positionRow + static_cast<std::size_t>(x)*8u);
            const double packedX=halfToDouble(halves[0]);
            const double packedY=halfToDouble(halves[1]);
            const double packedZ=halfToDouble(halves[2]);
            const double packedW=halfToDouble(halves[3]);
            double viewPosition[3]{};
            const bool viewPositionFinite=reconstructViewPosition(halves,x,y,d,viewPosition);
            float sceneDepth=1.0f;
            readFloat(phase.depth,x,y,sceneDepth);
            const bool worldReceiver=viewPositionFinite&&eligibleWorldReceiver(sceneDepth,
                packedX,packedY,viewPosition);
            if (sceneDepth<=0.02f) ++metrics.hudPositionPixelsExcluded;
            else if (std::isfinite(packedX)&&std::isfinite(packedY)&&
                std::abs(packedX)<=1.0e-8&&std::abs(packedY)<=1.0e-8&&packedZ==0.0)
                ++metrics.clearPositionPixelsExcluded;
            if (worldReceiver)
            {
                ++metrics.validPositionPixels;
                const Vec4 world4 = multiply(inverseView, {viewPosition[0],viewPosition[1],viewPosition[2],1.0});
                if (std::isfinite(world4.w) && std::abs(world4.w)>1e-12)
                {
                    const Vec4 world{world4.x/world4.w,world4.y/world4.w,world4.z/world4.w,1.0};
                    bool ambiguous=false;
                    if (insideVolume(multiply(volume,world),ambiguous)) ++metrics.positionInsideVolumePixels;
                    const Vec4 screen=multiply(current,world);
                    if (std::isfinite(screen.w) && std::abs(screen.w)>1e-12)
                    {
                        const double sx=(screen.x/screen.w+1.0)*0.5*phase.width;
                        const double sy=(1.0-screen.y/screen.w)*0.5*phase.height;
                        if (sx>=0.0 && sx<phase.width && sy>=0.0 && sy<phase.height)
                        {
                            ++metrics.cameraProjectablePixels;
                            metrics.maxPositionScreenErrorPixels=std::max(metrics.maxPositionScreenErrorPixels,
                                std::max(std::abs(sx-(x+0.5)),std::abs(sy-(y+0.5))));
                        }
                    }
                }
            }

            const auto* accumHalves = reinterpret_cast<const std::uint16_t*>(accumulationRow + static_cast<std::size_t>(x)*8u);
            bool anyRgbNonzero=false;
            for (int c=0;c<4;++c)
            {
                const double value=halfToDouble(accumHalves[c]);
                if (std::isfinite(value)) ++metrics.accumulationFiniteChannels;
                else ++metrics.accumulationNonFiniteChannels;
                if (c<3 && std::isfinite(value) && std::abs(value)>1.0e-7) anyRgbNonzero=true;
            }
            if (anyRgbNonzero) ++metrics.accumulationNonzeroRgbPixels;
            if (metrics.nonFiniteAccumulationSamples.size()<16u &&
                (!std::isfinite(halfToDouble(accumHalves[0]))||!std::isfinite(halfToDouble(accumHalves[1]))||
                 !std::isfinite(halfToDouble(accumHalves[2]))||!std::isfinite(halfToDouble(accumHalves[3]))))
            {
                PhaseMetrics::NonFiniteAccumulationSample sample;
                sample.x=x; sample.y=y; sample.sceneDepth=sceneDepth;
                sample.packedPosition[0]=packedX; sample.packedPosition[1]=packedY;
                sample.packedPosition[2]=packedZ; sample.packedPosition[3]=packedW;
                const double normalX=packedX*2.0-1.0,normalY=packedY*2.0-1.0;
                sample.packedNormalXYRadiusSquared=normalX*normalX+normalY*normalY;
                sample.decodedNormalZArgument=1.0-sample.packedNormalXYRadiusSquared;
                for (unsigned c=0;c<3u;++c) sample.viewPosition[c]=viewPosition[c];
                for (unsigned c=0;c<4u;++c) sample.accumulation[c]=halfToDouble(accumHalves[c]);
                metrics.nonFiniteAccumulationSamples.push_back(sample);
            }
        }
    }

    for (std::uint32_t y = 0; y < phase.mapHeight; ++y)
    {
        const auto* row = mapBytes + static_cast<std::size_t>(y)*phase.map.rowBytes;
        for (std::uint32_t x = 0; x < phase.mapWidth; ++x)
        {
            float depth=1.0f;
            std::memcpy(&depth,row+static_cast<std::size_t>(x)*sizeof(float),sizeof(depth));
            if (!std::isfinite(depth)) { ++metrics.mapNonFinitePixels; continue; }
            metrics.mapMinimum=std::min(metrics.mapMinimum,static_cast<double>(depth));
            metrics.mapMaximum=std::max(metrics.mapMaximum,static_cast<double>(depth));
            if (depth>=0.0f && depth<1.0f-1.0e-7f) ++metrics.mapCoveredPixels;
        }
    }

    for (std::uint32_t y = 0; y < phase.height; ++y)
    {
        for (std::uint32_t x = 0; x < phase.width; ++x)
        {
            const bool wasMasked=stencilAt(baseline,x,y)==d.stencilMarker;
            if (wasMasked) ++metrics.baselineMarkedPixels;
            const std::uint8_t before=stencilAt(previous,x,y);
            const std::uint8_t actual=stencilAt(phase,x,y);
            if (actual!=before) ++metrics.stencilChangedPixels;
            if (!wasMasked || before<d.stencilMarker || !invertible) continue;
            float originalDepth=1.0f;
            readFloat(phase.depth,x,y,originalDepth);
            const auto* positionRow=static_cast<const std::uint8_t*>(phase.position.buffer.contents)+
                static_cast<std::size_t>(y)*phase.position.rowBytes;
            const auto* half=reinterpret_cast<const std::uint16_t*>(positionRow+static_cast<std::size_t>(x)*8u);
            const double packedX=halfToDouble(half[0]);
            const double packedY=halfToDouble(half[1]);
            const double packedZ=halfToDouble(half[2]);
            const double packedW=halfToDouble(half[3]);
            double viewPosition[3]{};
            if (!reconstructViewPosition(half,x,y,d,viewPosition) ||
                !eligibleWorldReceiver(originalDepth,packedX,packedY,viewPosition))
                continue;
            if (actual!=before) ++metrics.stencilRelevantChangedPixels;
            bool receiverInsideVolume=false;
            {
                const Vec4 worldView=multiply(inverseView,{viewPosition[0],viewPosition[1],viewPosition[2],1.0});
                if (std::isfinite(worldView.w)&&std::abs(worldView.w)>1.0e-12)
                {
                    bool ignoredPositionEdge=false;
                    receiverInsideVolume=insideVolume(multiply(volume,{worldView.x/worldView.w,
                        worldView.y/worldView.w,worldView.z/worldView.w,1.0}),ignoredPositionEdge);
                }
            }
            const double ndcX=2.0*(static_cast<double>(x)+0.5)/phase.width-1.0;
            const double ndcY=1.0-2.0*(static_cast<double>(y)+0.5)/phase.height;
            bool edge=false;
            bool depthBoundaryAmbiguous=false;
            double hitFaceDepth=std::numeric_limits<double>::quiet_NaN();
            double hitDepthDelta=std::numeric_limits<double>::quiet_NaN();
            const bool facePass=rayVolumeDepthPass(inverseCurrent,current,volumeOracle,ndcX,ndcY,
                originalDepth,phaseIdx==3u,d.farZCullEnabled,edge,hitFaceDepth,
                hitDepthDelta,depthBoundaryAmbiguous);
            if (edge)
            {
                ++metrics.edgeAmbiguousPixels;
                if (depthBoundaryAmbiguous)
                {
                    ++metrics.depthBoundaryAmbiguousPixels;
                    if (metrics.depthBoundarySamples.size()<16u)
                        metrics.depthBoundarySamples.push_back({x,y,originalDepth,hitFaceDepth,hitDepthDelta});
                }
                continue;
            }
            if (facePass && !receiverInsideVolume)
                ++metrics.rasterPassReceiverOutsideVolumePixels;
            // The source PS has no discard and stencil updates are controlled
            // by raster coverage plus depth/stencil tests. G-buffer position
            // volume membership is diagnostic only, not another GPU test.
            const bool shouldClear=facePass;
            if (shouldClear) ++metrics.expectedStencilPassPixels;

            bool comparisonMismatch=false;
            if (phaseIdx==3u)
            {
                // The far pass uses the previous cascade cuboid as its
                // screen-space coverage boundary but must preserve stencil.
                if (actual!=before) { ++stencilMismatch; comparisonMismatch=true; }
            }
            else if (actual!=before)
            {
                if (!shouldClear || actual!=static_cast<std::uint8_t>(before & 1u))
                { ++stencilMismatch; comparisonMismatch=true; }
            }
            else if (shouldClear && before!=static_cast<std::uint8_t>(before & 1u))
                ++metrics.expectedStencilWriteMisses;
            if ((comparisonMismatch || (actual==before && shouldClear &&
                before!=static_cast<std::uint8_t>(before&1u))) &&
                metrics.stencilComparisonSamples.size()<16u)
            {
                PhaseMetrics::StencilComparisonSample sample;
                sample.x=x; sample.y=y; sample.before=before; sample.after=actual;
                sample.sceneDepth=originalDepth; sample.facePass=facePass;
                sample.faceDepth=hitFaceDepth; sample.depthDelta=hitDepthDelta;
                sample.receiverInsideVolume=receiverInsideVolume; sample.edgeAmbiguous=edge;
                sample.depthBoundaryAmbiguous=depthBoundaryAmbiguous;
                sample.packedPosition[0]=packedX; sample.packedPosition[1]=packedY;
                sample.packedPosition[2]=packedZ; sample.packedPosition[3]=packedW;
                for (unsigned c=0;c<3u;++c) sample.viewPosition[c]=viewPosition[c];
                metrics.stencilComparisonSamples.push_back(sample);
            }
            ++checkedStencil;
        }
    }
    metrics.stencilMismatchPixels=stencilMismatch;
    const bool hasStencilCoverage=phaseIdx==3u ? metrics.expectedStencilPassPixels>0u :
        metrics.expectedStencilPassPixels>0u && metrics.stencilRelevantChangedPixels>0u;
    metrics.pass=invertible && metrics.baselineMarkedPixels>0u &&
        hasStencilCoverage && checkedStencil>0u && stencilMismatch==0u &&
        metrics.expectedStencilWriteMisses==0u &&
        metrics.mapCoveredPixels>0u && metrics.mapNonFinitePixels==0u &&
        metrics.accumulationNonFiniteChannels==0u && metrics.accumulationFiniteChannels>0u &&
        (d.sunColorIntensity<=1.0e-6f || metrics.accumulationNonzeroRgbPixels>0u);
    return metrics;
}

struct CasterMetrics
{
    std::uint64_t offCameraCasterCount=0;
    std::uint64_t validReceiverPixels=0;
    std::uint64_t receiversInsideMap=0;
    std::uint64_t shadowedReceiverPixels=0;
    std::uint64_t mapCoveredPixels=0;
    std::uint64_t mapNonFinitePixels=0;
    std::uint64_t nonFiniteReceiverPixels=0;
    double mapMinimum=std::numeric_limits<double>::infinity();
    double mapMaximum=-std::numeric_limits<double>::infinity();
    bool pass=false;
};

CasterMetrics analyzeCasterOnly(const PhaseCapture& phase)
{
    const auto& d=phase.descriptor;
    CasterMetrics metrics;
    metrics.offCameraCasterCount=d.offCameraCasterCount;
    const Matrix4 inverseView=asMatrix(d.inverseView);
    const Matrix4 current=asMatrix(d.currentViewProjection);
    const Matrix4 shadow=asMatrix(d.stageVolumeViewProjection);
    const auto* positionBytes=static_cast<const std::uint8_t*>(phase.position.buffer.contents);
    const auto* mapBytes=static_cast<const std::uint8_t*>(phase.map.buffer.contents);

    for (std::uint32_t y=0;y<phase.mapHeight;++y)
    {
        const auto* row=mapBytes+static_cast<std::size_t>(y)*phase.map.rowBytes;
        for (std::uint32_t x=0;x<phase.mapWidth;++x)
        {
            float value=1.0f;
            std::memcpy(&value,row+static_cast<std::size_t>(x)*sizeof(float),sizeof(value));
            if (!std::isfinite(value)) { ++metrics.mapNonFinitePixels; continue; }
            metrics.mapMinimum=std::min(metrics.mapMinimum,static_cast<double>(value));
            metrics.mapMaximum=std::max(metrics.mapMaximum,static_cast<double>(value));
            if (value>=0.0f && value<1.0f-1.0e-7f) ++metrics.mapCoveredPixels;
        }
    }

    for (std::uint32_t y=0;y<phase.height;++y)
    {
        const auto* positionRow=positionBytes+static_cast<std::size_t>(y)*phase.position.rowBytes;
        for (std::uint32_t x=0;x<phase.width;++x)
        {
            float sceneDepth=1.0f;
            if (!readFloat(phase.depth,x,y,sceneDepth) || !std::isfinite(sceneDepth) ||
                sceneDepth<=0.02f || sceneDepth>=1.0f)
                continue;
            const auto* half=reinterpret_cast<const std::uint16_t*>(positionRow+static_cast<std::size_t>(x)*8u);
            const double packedX=halfToDouble(half[0]), packedY=halfToDouble(half[1]);
            double viewPosition[3]{};
            if (!reconstructViewPosition(half,x,y,d,viewPosition) ||
                !eligibleWorldReceiver(sceneDepth,packedX,packedY,viewPosition))
            { ++metrics.nonFiniteReceiverPixels; continue; }
            const Vec4 world4=multiply(inverseView,{viewPosition[0],viewPosition[1],viewPosition[2],1.0});
            if (!std::isfinite(world4.w)||std::abs(world4.w)<1.0e-12)
            { ++metrics.nonFiniteReceiverPixels; continue; }
            const Vec4 world{world4.x/world4.w,world4.y/world4.w,world4.z/world4.w,1.0};
            const Vec4 cameraClip=multiply(current,world);
            const Vec4 shadowClip=multiply(shadow,world);
            if (!std::isfinite(cameraClip.w)||std::abs(cameraClip.w)<1.0e-12||
                !std::isfinite(shadowClip.w)||std::abs(shadowClip.w)<1.0e-12)
            { ++metrics.nonFiniteReceiverPixels; continue; }
            const double cx=cameraClip.x/cameraClip.w, cy=cameraClip.y/cameraClip.w;
            const double cz=cameraClip.z/cameraClip.w;
            if (cx < -1.0 || cx > 1.0 || cy < -1.0 || cy > 1.0 || cz < 0.0 || cz > 1.0)
                continue;
            ++metrics.validReceiverPixels;

            const double mapU=shadowClip.x/shadowClip.w*0.5+0.5;
            const double mapV=-shadowClip.y/shadowClip.w*0.5+0.5;
            if (!std::isfinite(mapU)||!std::isfinite(mapV)||mapU<0.0||mapU>=1.0||mapV<0.0||mapV>=1.0)
                continue;
            ++metrics.receiversInsideMap;
            const std::uint32_t sx=std::min<std::uint32_t>(phase.mapWidth-1u,
                static_cast<std::uint32_t>(mapU*phase.mapWidth));
            const std::uint32_t sy=std::min<std::uint32_t>(phase.mapHeight-1u,
                static_cast<std::uint32_t>(mapV*phase.mapHeight));
            float casterDepth=1.0f;
            readFloat(phase.map,sx,sy,casterDepth);
            const double receiverShadowDepth=shadowClip.z/shadowClip.w;
            if (std::isfinite(casterDepth) && std::isfinite(receiverShadowDepth) &&
                receiverShadowDepth >= 0.0 && receiverShadowDepth <= 1.0 &&
                casterDepth < receiverShadowDepth-d.receiverDepthBiasTolerance)
                ++metrics.shadowedReceiverPixels;
        }
    }
    metrics.pass=metrics.offCameraCasterCount>0u && metrics.mapCoveredPixels>0u &&
        metrics.mapNonFinitePixels==0u && metrics.validReceiverPixels>0u && metrics.receiversInsideMap>0u &&
        (!d.requireShadowedReceivers || metrics.shadowedReceiverPixels>0u);
    return metrics;
}

void writeCasterReport(const PhaseCapture& phase, bool commandCompleted, const std::string& commandError)
{
    const auto& d=phase.descriptor;
    const CasterMetrics metrics=analyzeCasterOnly(phase);
    const std::string path=(std::filesystem::path(d.outputDirectory)/
        ("r4_sun_cascade_"+safePart(d.family)+"_frame_"+std::to_string(d.engineFrame)+"_caster.json")).string();
    std::lock_guard<std::mutex> lock(outputMutex());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(),ec);
    std::string reportError=commandError;
    std::string rawMapFilename;
    if (d.exportShadowMap && commandCompleted && phase.map.buffer)
    {
        std::filesystem::path rawMapPath(path);
        rawMapPath.replace_extension(".r32");
        const auto* mapBytes=static_cast<const std::uint8_t*>(phase.map.buffer.contents);
        std::ofstream raw(rawMapPath,std::ios::binary|std::ios::out|std::ios::trunc);
        if (!mapBytes || !raw)
            reportError="could not open optional caster shadow-map export";
        else
        {
            const std::size_t packedRow=static_cast<std::size_t>(phase.mapWidth)*sizeof(float);
            for (std::uint32_t y=0;y<phase.mapHeight && raw;++y)
                raw.write(reinterpret_cast<const char*>(mapBytes+static_cast<std::size_t>(y)*phase.map.rowBytes),
                    static_cast<std::streamsize>(packedRow));
            raw.close();
            if (!raw) reportError="failed while writing optional caster shadow-map export";
            else rawMapFilename=rawMapPath.filename().string();
        }
    }
    const bool passed=commandCompleted && reportError.empty() && metrics.pass;
    if (!reportError.empty())
    {
        // Preserve the required report even when the optional sidecar cannot
        // be written, and make the requested evidence export an explicit FAIL.
        std::error_code removeError;
        if (d.exportShadowMap) std::filesystem::remove(
            std::filesystem::path(path).replace_extension(".r32"),removeError);
        rawMapFilename.clear();
    }
    std::ofstream out(path,std::ios::out|std::ios::trunc);
    if (!out) { std::cerr<<"R4 sun caster validation FAIL: cannot write "<<path<<std::endl; return; }
    const bool noCoverage=metrics.offCameraCasterCount==0u || metrics.mapCoveredPixels==0u ||
        metrics.validReceiverPixels==0u || metrics.receiversInsideMap==0u ||
        (d.requireShadowedReceivers && metrics.shadowedReceiverPixels==0u);
    const char* result=passed?"PASS":((commandCompleted&&reportError.empty()&&noCoverage)?"NO_COVERAGE":"FAIL");
    out<<std::setprecision(10)<<"{\n  \"schema\": \"r4_sun_cascade_caster_validation_v1\",\n"
       <<"  \"result\": \""<<result<<"\",\n"
       <<"  \"family\": \""<<escapeJson(d.family)<<"\",\n"
       <<"  \"engine_frame\": "<<d.engineFrame<<",\n"
       <<"  \"command_completed\": "<<(commandCompleted?"true":"false")<<",\n"
       <<"  \"command_error\": \""<<escapeJson(reportError)<<"\",\n"
       <<"  \"off_camera_caster_count\": "<<metrics.offCameraCasterCount<<",\n"
       <<"  \"require_shadowed_receivers\": "<<(d.requireShadowedReceivers?"true":"false")<<",\n"
       <<"  \"receiver_depth_bias_tolerance\": "<<d.receiverDepthBiasTolerance<<",\n"
       <<"  \"source_cascade_size\": "<<d.sourceCascadeSize<<",\n"
       <<"  \"source_cascade_bias\": "<<d.sourceCascadeBias<<",\n"
       <<"  \"valid_receiver_pixels\": "<<metrics.validReceiverPixels<<",\n"
       <<"  \"receivers_inside_shadow_map\": "<<metrics.receiversInsideMap<<",\n"
       <<"  \"shadowed_receiver_pixels\": "<<metrics.shadowedReceiverPixels<<",\n"
       <<"  \"shadow_map_covered_pixels\": "<<metrics.mapCoveredPixels<<",\n"
       <<"  \"shadow_map_nonfinite_pixels\": "<<metrics.mapNonFinitePixels<<",\n"
       <<"  \"nonfinite_receiver_pixels\": "<<metrics.nonFiniteReceiverPixels<<",\n"
       <<"  \"shadow_map_width\": "<<phase.mapWidth<<",\n"
       <<"  \"shadow_map_height\": "<<phase.mapHeight<<",\n"
       <<"  \"shadow_map_export_requested\": "<<(d.exportShadowMap?"true":"false")<<",\n"
       <<"  \"shadow_map_raw_file\": \""<<escapeJson(rawMapFilename)<<"\"";
    const auto writeMatrix=[&out](const char* name,const std::array<float,16>& matrix) {
        out<<",\n  \""<<name<<"\": [";
        for (std::size_t i=0;i<matrix.size();++i)
        {
            out<<(i?", ":"");
            if (std::isfinite(matrix[i])) out<<matrix[i];
            else out<<"null";
        }
        out<<"]";
    };
    writeMatrix("stage_volume_view_projection",d.stageVolumeViewProjection);
    writeMatrix("current_view_projection",d.currentViewProjection);
    writeMatrix("inverse_view",d.inverseView);
    out<<",\n  \"shadow_map_depth_min\": "
       <<(std::isfinite(metrics.mapMinimum)?metrics.mapMinimum:0.0)<<",\n"
       <<"  \"shadow_map_depth_max\": "
       <<(std::isfinite(metrics.mapMaximum)?metrics.mapMaximum:0.0)<<"\n}\n";
    out.close();
    std::cerr<<"R4 sun off-camera caster validation "<<d.family<<" frame "<<d.engineFrame<<" "
        <<result<<" casters="<<metrics.offCameraCasterCount
        <<" receivers_shadowed="<<metrics.shadowedReceiverPixels<<" report="<<path;
    if (!reportError.empty()) std::cerr<<" error="<<reportError;
    std::cerr<<std::endl;
}

std::string localizedError(id<MTLCommandBuffer> commandBuffer)
{
    NSError* e=commandBuffer.error;
    if (!e) return "command buffer did not complete successfully";
    const char* text=e.localizedDescription.UTF8String;
    return text ? text : "unknown Metal command-buffer error";
}

void writeReport(const Session& session, bool commandCompleted, const std::string& commandError)
{
    const auto& d=session.phases[0].descriptor;
    bool passed=commandCompleted && !commandError.size();
    std::array<PhaseMetrics,4> metrics;
    for (std::size_t i=0;i<4u;++i)
    {
        metrics[i]=analyzePhase(session,i);
        if (!metrics[i].pass) passed=false;
    }
    const std::string path=(std::filesystem::path(d.outputDirectory)/
        ("r4_sun_cascade_"+safePart(d.family)+"_frame_"+std::to_string(d.engineFrame)+".json")).string();
    std::lock_guard<std::mutex> lock(outputMutex());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(),ec);
    std::ofstream out(path,std::ios::out|std::ios::trunc);
    if (!out)
    {
        std::cerr<<"R4 sun cascade validation FAIL: cannot write "<<path<<std::endl;
        return;
    }
    out<<std::setprecision(10)<<"{\n  \"schema\": \"r4_sun_cascade_validation_v1\",\n"
       <<"  \"result\": \""<<(passed?"PASS":"FAIL")<<"\",\n"
       <<"  \"family\": \""<<escapeJson(d.family)<<"\",\n"
       <<"  \"engine_frame\": "<<d.engineFrame<<",\n"
       <<"  \"command_completed\": "<<(commandCompleted?"true":"false")<<",\n"
       <<"  \"command_error\": \""<<escapeJson(commandError)<<"\",\n"
       <<"  \"stencil_marker\": "<<static_cast<unsigned>(d.stencilMarker)<<",\n"
       <<"  \"near_depth\": {\"scale\": "<<d.nearDepthScale<<", \"bias\": "<<d.nearDepthBias<<"},\n"
       <<"  \"far_depth\": {\"scale\": "<<d.farDepthScale<<", \"bias\": "<<d.farDepthBias
       <<", \"z_cull\": "<<(d.farZCullEnabled?"true":"false")<<", \"tsm_bias\": "<<d.tsmBias<<"},\n"
       <<"  \"position_decompression_params\": ["<<d.positionDecompressionParams[0]<<", "
       <<d.positionDecompressionParams[1]<<", "<<d.positionDecompressionParams[2]<<", "
       <<d.positionDecompressionParams[3]<<"],\n"
       <<"  \"position_jitter\": ["<<d.positionJitter[0]<<", "<<d.positionJitter[1]<<"],\n"
       <<"  \"sun_color_intensity\": "<<d.sunColorIntensity<<",\n"
       <<"  \"grass_shadows_enabled\": "<<(d.grassShadowsEnabled?"true":"false")<<",\n"
       <<"  \"phases\": [\n";
    for (std::size_t i=0;i<4u;++i)
    {
        const PhaseMetrics& m=metrics[i];
        out<<"    {\"phase\": \""<<m.name<<"\", \"result\": \""<<(m.pass?"PASS":"FAIL")
           <<"\", \"source_cascade_size\": "<<m.sourceCascadeSize
           <<", \"source_cascade_bias\": "<<m.sourceCascadeBias
           <<", \"baseline_marked_pixels\": "<<m.baselineMarkedPixels
           <<", \"valid_position_pixels\": "<<m.validPositionPixels
           <<", \"position_inside_volume_pixels\": "<<m.positionInsideVolumePixels
           <<", \"camera_projectable_pixels\": "<<m.cameraProjectablePixels
           <<", \"max_position_screen_error_pixels\": "<<m.maxPositionScreenErrorPixels
           <<", \"expected_stencil_pass_pixels\": "<<m.expectedStencilPassPixels
           <<", \"stencil_changed_pixels\": "<<m.stencilChangedPixels
           <<", \"stencil_relevant_changed_pixels\": "<<m.stencilRelevantChangedPixels
           <<", \"stencil_mismatch_pixels\": "<<m.stencilMismatchPixels
           <<", \"expected_stencil_write_misses\": "<<m.expectedStencilWriteMisses
           <<", \"edge_ambiguous_pixels\": "<<m.edgeAmbiguousPixels
           <<", \"depth_boundary_ambiguous_pixels\": "<<m.depthBoundaryAmbiguousPixels
           <<", \"depth_boundary_samples\": [";
        for (std::size_t sampleIndex=0;sampleIndex<m.depthBoundarySamples.size();++sampleIndex)
        {
            const auto& sample=m.depthBoundarySamples[sampleIndex];
            out<<(sampleIndex?", ":"")<<"{\"x\": "<<sample.x<<", \"y\": "<<sample.y
               <<", \"scene_depth\": ";
            writeJsonNumber(out,sample.sceneDepth);
            out<<", \"face_depth\": ";
            writeJsonNumber(out,sample.faceDepth);
            out<<", \"depth_delta\": ";
            writeJsonNumber(out,sample.depthDelta);
            out<<"}";
        }
        out<<"], \"shadow_map_covered_pixels\": "<<m.mapCoveredPixels
           <<", \"shadow_map_nonfinite_pixels\": "<<m.mapNonFinitePixels
           <<", \"shadow_map_depth_min\": "<<(std::isfinite(m.mapMinimum)?m.mapMinimum:0.0)
           <<", \"shadow_map_depth_max\": "<<(std::isfinite(m.mapMaximum)?m.mapMaximum:0.0)
           <<", \"accumulation_nonzero_rgb_pixels\": "<<m.accumulationNonzeroRgbPixels
           <<", \"accumulation_finite_channels\": "<<m.accumulationFiniteChannels
           <<", \"accumulation_nonfinite_channels\": "<<m.accumulationNonFiniteChannels
           <<", \"hud_position_pixels_excluded\": "<<m.hudPositionPixelsExcluded
           <<", \"clear_position_pixels_excluded\": "<<m.clearPositionPixelsExcluded
           <<", \"raster_pass_receiver_outside_volume_pixels\": "<<m.rasterPassReceiverOutsideVolumePixels
           <<", \"nonfinite_accumulation_samples\": [";
        for (std::size_t sampleIndex=0;sampleIndex<m.nonFiniteAccumulationSamples.size();++sampleIndex)
        {
            const auto& sample=m.nonFiniteAccumulationSamples[sampleIndex];
            out<<(sampleIndex?", ":"")<<"{\"x\": "<<sample.x<<", \"y\": "<<sample.y<<", \"scene_depth\": ";
            writeJsonNumber(out,sample.sceneDepth);
            out<<", \"packed_position\": [";
            for (unsigned c=0;c<4u;++c)
            { if (c) out<<", "; writeJsonNumber(out,sample.packedPosition[c]); }
            out<<"], \"decoded_view_position\": [";
            for (unsigned c=0;c<3u;++c)
            { if (c) out<<", "; writeJsonNumber(out,sample.viewPosition[c]); }
            out<<"], \"packed_normal_xy_radius_squared\": ";
            writeJsonNumber(out,sample.packedNormalXYRadiusSquared);
            out<<", \"decoded_normal_z_argument\": ";
            writeJsonNumber(out,sample.decodedNormalZArgument);
            out<<", \"accumulation_rgba\": [";
            for (unsigned c=0;c<4u;++c)
            { if (c) out<<", "; writeJsonNumber(out,sample.accumulation[c]); }
            out<<"]}";
        }
        out<<"], \"stencil_comparison_samples\": [";
        for (std::size_t sampleIndex=0;sampleIndex<m.stencilComparisonSamples.size();++sampleIndex)
        {
            const auto& sample=m.stencilComparisonSamples[sampleIndex];
            out<<(sampleIndex?", ":"")<<"{\"x\": "<<sample.x<<", \"y\": "<<sample.y
               <<", \"before_stencil\": "<<static_cast<unsigned>(sample.before)
               <<", \"after_stencil\": "<<static_cast<unsigned>(sample.after)
               <<", \"scene_depth\": ";
            writeJsonNumber(out,sample.sceneDepth);
            out<<", \"face_pass\": "<<(sample.facePass?"true":"false")
               <<", \"receiver_inside_volume\": "<<(sample.receiverInsideVolume?"true":"false")
               <<", \"edge_ambiguous\": "<<(sample.edgeAmbiguous?"true":"false")
               <<", \"face_depth\": ";
            writeJsonNumber(out,sample.faceDepth);
            out<<", \"depth_delta\": ";
            writeJsonNumber(out,sample.depthDelta);
            out<<", \"depth_boundary_ambiguous\": "<<(sample.depthBoundaryAmbiguous?"true":"false")
               <<", \"packed_position\": [";
            for (unsigned c=0;c<4u;++c)
            { if (c) out<<", "; writeJsonNumber(out,sample.packedPosition[c]); }
            out<<"], \"decoded_view_position\": [";
            for (unsigned c=0;c<3u;++c)
            { if (c) out<<", "; writeJsonNumber(out,sample.viewPosition[c]); }
            out<<"]}";
        }
        out<<"]}"<<(i==3u?"\n":",\n");
    }
    out<<"  ]\n}\n";
    out.close();
    std::cerr<<"R4 sun cascade validation "<<d.family<<" frame "<<d.engineFrame<<" "
        <<(passed?"PASS":"FAIL")<<" report="<<path;
    if (!commandError.empty()) std::cerr<<" error="<<commandError;
    std::cerr<<std::endl;
}

std::shared_ptr<Session> getSession(const R4SunShadowValidationDesc& d)
{
    const std::string key=d.outputDirectory+"|"+d.family+"|"+std::to_string(d.engineFrame);
    std::lock_guard<std::mutex> lock(sessionsMutex());
    auto& item=sessions()[key];
    if (!item) { item=std::make_shared<Session>(); item->key=key; }
    return item;
}

void completed(const std::shared_ptr<Session>& session, id<MTLCommandBuffer> buffer)
{
    bool shouldWrite=false, success=buffer.status==MTLCommandBufferStatusCompleted;
    std::string error=success?std::string():localizedError(buffer);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!success) { session->commandFailed=true; session->commandError=error; }
        const bool all=std::all_of(session->phases.begin(),session->phases.end(),
            [](const PhaseCapture& p){return p.registered;});
        if (!session->reportWritten && (all || session->commandFailed))
        {
            session->reportWritten=true;
            shouldWrite=true;
        }
        success=success&&!session->commandFailed;
        if (!session->commandError.empty()) error=session->commandError;
    }
    if (shouldWrite)
    {
        writeReport(*session,success,error);
        std::lock_guard<std::mutex> lock(sessionsMutex());
        sessions().erase(session->key);
    }
}
} // namespace

bool EncodeMetalR4SunShadowValidation(void* commandBufferHandle,
    void* shadowHandle, void* positionHandle, void* depthStencilHandle,
    void* accumulationHandle, const R4SunShadowValidationDesc& descriptor,
    std::string& error)
{
    id<MTLCommandBuffer> commandBuffer=(__bridge id<MTLCommandBuffer>)commandBufferHandle;
    id<MTLTexture> shadow=(__bridge id<MTLTexture>)shadowHandle;
    id<MTLTexture> position=(__bridge id<MTLTexture>)positionHandle;
    id<MTLTexture> depthStencil=(__bridge id<MTLTexture>)depthStencilHandle;
    id<MTLTexture> accumulation=(__bridge id<MTLTexture>)accumulationHandle;
    const bool baseline=descriptor.phase==R4SunShadowValidationPhase::StencilBaseline;
    const bool casterOnly=descriptor.phase==R4SunShadowValidationPhase::OffCameraCasters;
    if (!commandBuffer || !depthStencil || descriptor.family.empty() || descriptor.outputDirectory.empty())
    {
        error="R4 sun shadow validation needs command buffer, depth/stencil, family, and output directory";
        return false;
    }
    if (depthStencil.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8 || depthStencil.sampleCount!=1u ||
        depthStencil.width==0u || depthStencil.height==0u)
    {
        error="R4 sun shadow validation requires a non-empty single-sample Depth32Float_Stencil8 texture";
        return false;
    }
    if (casterOnly && (!shadow || !position || shadow.pixelFormat!=MTLPixelFormatR32Float ||
        position.pixelFormat!=MTLPixelFormatRGBA16Float || shadow.sampleCount!=1u || position.sampleCount!=1u ||
        position.width!=depthStencil.width || position.height!=depthStencil.height))
    {
        error="R4 off-camera caster validation requires R32Float shadow, matching RGBA16Float position, and depth";
        return false;
    }
    if (!baseline && !casterOnly && (!shadow || !position || !accumulation ||
        shadow.pixelFormat!=MTLPixelFormatR32Float || position.pixelFormat!=MTLPixelFormatRGBA16Float ||
        accumulation.pixelFormat!=MTLPixelFormatRGBA16Float || shadow.sampleCount!=1u ||
        position.sampleCount!=1u || accumulation.sampleCount!=1u ||
        position.width!=depthStencil.width || position.height!=depthStencil.height ||
        accumulation.width!=depthStencil.width || accumulation.height!=depthStencil.height))
    {
        error="R4 sun cascade phase needs R32Float shadow and matching RGBA16Float position/accumulation textures";
        return false;
    }

    if (casterOnly)
    {
        PhaseCapture capture;
        capture.registered=true;
        capture.descriptor=descriptor;
        capture.width=static_cast<std::uint32_t>(depthStencil.width);
        capture.height=static_cast<std::uint32_t>(depthStencil.height);
        capture.mapWidth=static_cast<std::uint32_t>(shadow.width);
        capture.mapHeight=static_cast<std::uint32_t>(shadow.height);
        capture.depth.rowBytes=aligned(depthStencil.width*sizeof(float));
        capture.map.rowBytes=aligned(shadow.width*sizeof(float));
        capture.position.rowBytes=aligned(position.width*8u);
        if (capture.height>std::numeric_limits<NSUInteger>::max()/capture.depth.rowBytes ||
            capture.mapHeight>std::numeric_limits<NSUInteger>::max()/capture.map.rowBytes ||
            capture.height>std::numeric_limits<NSUInteger>::max()/capture.position.rowBytes)
        {
            error="R4 off-camera caster readback size overflow";
            return false;
        }
        id<MTLDevice> device=commandBuffer.device;
        capture.depth.buffer=[device newBufferWithLength:capture.depth.rowBytes*capture.height options:MTLResourceStorageModeShared];
        capture.map.buffer=[device newBufferWithLength:capture.map.rowBytes*capture.mapHeight options:MTLResourceStorageModeShared];
        capture.position.buffer=[device newBufferWithLength:capture.position.rowBytes*capture.height options:MTLResourceStorageModeShared];
        if (!capture.depth.buffer||!capture.map.buffer||!capture.position.buffer)
        {
            error="Metal failed to allocate off-camera caster validation readback buffers";
            return false;
        }
        id<MTLBlitCommandEncoder> blit=[commandBuffer blitCommandEncoder];
        if (!blit) { error="Metal failed to create off-camera caster readback blit encoder"; return false; }
        [blit copyFromTexture:depthStencil sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
            sourceSize:MTLSizeMake(depthStencil.width,depthStencil.height,1u) toBuffer:capture.depth.buffer
            destinationOffset:0u destinationBytesPerRow:capture.depth.rowBytes
            destinationBytesPerImage:capture.depth.rowBytes*capture.height options:MTLBlitOptionDepthFromDepthStencil];
        [blit copyFromTexture:position sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
            sourceSize:MTLSizeMake(position.width,position.height,1u) toBuffer:capture.position.buffer
            destinationOffset:0u destinationBytesPerRow:capture.position.rowBytes
            destinationBytesPerImage:capture.position.rowBytes*capture.height options:MTLBlitOptionNone];
        [blit copyFromTexture:shadow sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
            sourceSize:MTLSizeMake(shadow.width,shadow.height,1u) toBuffer:capture.map.buffer
            destinationOffset:0u destinationBytesPerRow:capture.map.rowBytes
            destinationBytesPerImage:capture.map.rowBytes*capture.mapHeight options:MTLBlitOptionNone];
        [blit endEncoding];
        auto keepAlive=std::make_shared<PhaseCapture>(std::move(capture));
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            const bool success=completedBuffer.status==MTLCommandBufferStatusCompleted;
            writeCasterReport(*keepAlive,success,success?std::string():localizedError(completedBuffer));
        }];
        return true;
    }
    const std::size_t index=phaseIndex(descriptor.phase);
    if (index>=4u)
    {
        error="R4 sun shadow validation received an invalid phase";
        return false;
    }
    auto session=getSession(descriptor);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->phases[index].registered)
        {
            error="R4 sun shadow validation phase was registered twice for the same frame/family";
            return false;
        }
    }

    PhaseCapture capture;
    capture.registered=true;
    capture.descriptor=descriptor;
    capture.width=static_cast<std::uint32_t>(depthStencil.width);
    capture.height=static_cast<std::uint32_t>(depthStencil.height);
    capture.depth.rowBytes=aligned(depthStencil.width*sizeof(float));
    capture.stencil.rowBytes=aligned(depthStencil.width);
    if (capture.height>std::numeric_limits<NSUInteger>::max()/capture.depth.rowBytes ||
        capture.height>std::numeric_limits<NSUInteger>::max()/capture.stencil.rowBytes)
    {
        error="R4 sun shadow depth/stencil readback size overflow";
        return false;
    }
    id<MTLDevice> device=commandBuffer.device;
    capture.depth.buffer=[device newBufferWithLength:capture.depth.rowBytes*capture.height options:MTLResourceStorageModeShared];
    capture.stencil.buffer=[device newBufferWithLength:capture.stencil.rowBytes*capture.height options:MTLResourceStorageModeShared];
    if (!capture.depth.buffer || !capture.stencil.buffer)
    {
        error="Metal failed to allocate R4 sun shadow depth/stencil readback buffers";
        return false;
    }

    id<MTLBlitCommandEncoder> blit=[commandBuffer blitCommandEncoder];
    if (!blit)
    {
        error="Metal failed to create R4 sun shadow validation blit encoder";
        return false;
    }
    const MTLSize viewportSize=MTLSizeMake(depthStencil.width,depthStencil.height,1u);
    [blit copyFromTexture:depthStencil sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:viewportSize toBuffer:capture.depth.buffer destinationOffset:0u
        destinationBytesPerRow:capture.depth.rowBytes destinationBytesPerImage:capture.depth.rowBytes*capture.height
        options:MTLBlitOptionDepthFromDepthStencil];
    [blit copyFromTexture:depthStencil sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
        sourceSize:viewportSize toBuffer:capture.stencil.buffer destinationOffset:0u
        destinationBytesPerRow:capture.stencil.rowBytes destinationBytesPerImage:capture.stencil.rowBytes*capture.height
        options:MTLBlitOptionStencilFromDepthStencil];

    if (!baseline)
    {
        capture.mapWidth=static_cast<std::uint32_t>(shadow.width);
        capture.mapHeight=static_cast<std::uint32_t>(shadow.height);
        capture.map.rowBytes=aligned(shadow.width*sizeof(float));
        capture.position.rowBytes=aligned(position.width*8u);
        capture.accumulation.rowBytes=aligned(accumulation.width*8u);
        if (capture.mapHeight>std::numeric_limits<NSUInteger>::max()/capture.map.rowBytes ||
            capture.height>std::numeric_limits<NSUInteger>::max()/capture.position.rowBytes ||
            capture.height>std::numeric_limits<NSUInteger>::max()/capture.accumulation.rowBytes)
        {
            [blit endEncoding];
            error="R4 sun cascade texture readback size overflow";
            return false;
        }
        capture.map.buffer=[device newBufferWithLength:capture.map.rowBytes*capture.mapHeight options:MTLResourceStorageModeShared];
        capture.position.buffer=[device newBufferWithLength:capture.position.rowBytes*capture.height options:MTLResourceStorageModeShared];
        capture.accumulation.buffer=[device newBufferWithLength:capture.accumulation.rowBytes*capture.height options:MTLResourceStorageModeShared];
        if (!capture.map.buffer || !capture.position.buffer || !capture.accumulation.buffer)
        {
            [blit endEncoding];
            error="Metal failed to allocate R4 sun cascade texture readback buffers";
            return false;
        }
        const MTLSize mapSize=MTLSizeMake(shadow.width,shadow.height,1u);
        [blit copyFromTexture:shadow sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
            sourceSize:mapSize toBuffer:capture.map.buffer destinationOffset:0u
            destinationBytesPerRow:capture.map.rowBytes destinationBytesPerImage:capture.map.rowBytes*capture.mapHeight
            options:MTLBlitOptionNone];
        [blit copyFromTexture:position sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
            sourceSize:viewportSize toBuffer:capture.position.buffer destinationOffset:0u
            destinationBytesPerRow:capture.position.rowBytes destinationBytesPerImage:capture.position.rowBytes*capture.height
            options:MTLBlitOptionNone];
        [blit copyFromTexture:accumulation sourceSlice:0u sourceLevel:0u sourceOrigin:MTLOriginMake(0u,0u,0u)
            sourceSize:viewportSize toBuffer:capture.accumulation.buffer destinationOffset:0u
            destinationBytesPerRow:capture.accumulation.rowBytes destinationBytesPerImage:capture.accumulation.rowBytes*capture.height
            options:MTLBlitOptionNone];
    }
    [blit endEncoding];

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->reportWritten)
        {
            error="R4 sun shadow validation session already completed";
            return false;
        }
        session->phases[index]=std::move(capture);
    }
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
        completed(session,completedBuffer);
    }];
    return true;
}
