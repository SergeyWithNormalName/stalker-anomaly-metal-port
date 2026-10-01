#include "../../xrRenderPC_R4/stdafx.h"
#include "../MetalR4SunCascadeCapture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
constexpr std::uint32_t ShadowResolution = 2048u;
constexpr float Pi = 3.14159265358979323846f;

struct Camera
{
    Fmatrix view;
    Fmatrix projection;
    Fvector eye;
    Fvector forward;
};

Camera makeCamera(const Fvector& eye)
{
    Camera result;
    result.eye = eye;
    result.forward.set(0.f, 0.f, 1.f);
    Fvector up;
    up.set(0.f, 1.f, 0.f);
    result.view.build_camera_dir(result.eye, result.forward, up);

    const float aspect = 16.f / 9.f;
    const float yScale = 1.f / std::tan(Pi / 6.f);
    const float nearPlane = 0.1f;
    const float farPlane = 600.f;
    result.projection.identity();
    result.projection._11 = yScale / aspect;
    result.projection._22 = yScale;
    result.projection._33 = farPlane / (farPlane - nearPlane);
    result.projection._34 = 1.f;
    result.projection._43 = -nearPlane * farPlane / (farPlane - nearPlane);
    result.projection._44 = 0.f;
    return result;
}

R4SunCascadeSet build(const Camera& camera, const Fvector& sunDirection,
    const std::array<float, 3>& sizes, const bool resetLast = false)
{
    Fvector sunPosition;
    sunPosition.mad(camera.eye, sunDirection, -500.f);
    return BuildMetalR4SunCascades(camera.view, camera.projection, camera.eye,
        camera.forward, sunPosition, sunDirection, sizes, ShadowResolution,
        resetLast, 777u);
}

bool require(const bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "R4 sun-cascade capture probe failed: %s\n", message);
    return false;
}

bool finiteCascadeSet(const R4SunCascadeSet& set, const bool requireCasterPlanes = true)
{
    if (!set.valid || set.resolution != ShadowResolution || set.engineFrame != 777u)
        return false;
    for (const R4SunCascade& cascade : set.cascades)
    {
        if (!(cascade.size > 0.f) || (requireCasterPlanes && cascade.casterPlanes.empty()))
            return false;
        for (const float value : cascade.lightViewProjection)
            if (!std::isfinite(value)) return false;
        for (const auto& point : cascade.receiverVolume)
            for (const float value : point)
                if (!std::isfinite(value)) return false;
        for (const auto& plane : cascade.casterPlanes)
            for (const float value : plane)
                if (!std::isfinite(value)) return false;
        for (const auto& ray : cascade.nextRayOrigins)
            for (const float value : ray)
                if (!std::isfinite(value)) return false;
        for (const auto& ray : cascade.initialRayOrigins)
            for (const float value : ray)
                if (!std::isfinite(value)) return false;
    }
    return true;
}

Fmatrix matrixFrom(const std::array<float, 16>& values)
{
    Fmatrix result;
    std::memcpy(&result, values.data(), sizeof(result));
    return result;
}

std::array<float, 4> transformPoint(const std::array<float, 16>& matrix,
    const std::array<float, 4>& point)
{
    std::array<float, 4> result{};
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            result[column] += point[row] * matrix[row * 4u + column];
    return result;
}

float maxPointDelta(const std::array<std::array<float, 4>, 8>& left,
    const std::array<std::array<float, 4>, 8>& right)
{
    float maximum = 0.f;
    for (std::size_t point = 0; point < left.size(); ++point)
        for (std::size_t axis = 0; axis < 3u; ++axis)
            maximum = std::max(maximum, std::abs(left[point][axis] - right[point][axis]));
    return maximum;
}

bool receiverCubeMapsToUnitFootprint(const R4SunCascade& cascade)
{
    for (const auto& receiver : cascade.receiverVolume)
    {
        const auto clip = transformPoint(cascade.lightViewProjection, receiver);
        if (!std::isfinite(clip[3]) || std::abs(clip[3]) < 1.0e-6f)
            return false;
        const float x = clip[0] / clip[3];
        const float y = clip[1] / clip[3];
        if (std::abs(std::abs(x) - 1.f) > 2.0e-3f ||
            std::abs(std::abs(y) - 1.f) > 2.0e-3f)
            return false;
    }
    return true;
}

bool footprintMatchesSize(const R4SunCascade& cascade)
{
    // The first two projection rows map the shadow-map world footprint to
    // [-1,1]. Their spatial norms recover the orthographic map width without
    // relying on the authored size field itself.
    const auto& m = cascade.lightViewProjection;
    const float xScale = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    const float yScale = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    if (!(xScale > 0.f) || !(yScale > 0.f))
        return false;
    const float xWidth = 2.f / xScale;
    const float yWidth = 2.f / yScale;
    return std::abs(xWidth - cascade.size) < cascade.size * 0.01f &&
        std::abs(yWidth - cascade.size) < cascade.size * 0.01f;
}

bool buildCasterFrustum(const R4SunCascade& cascade, CFrustum& frustum)
{
    frustum._clear();
    for (const auto& values : cascade.casterPlanes)
    {
        Fplane plane;
        plane.n.set(values[0], values[1], values[2]);
        plane.d = values[3];
        frustum._add(plane);
    }
    return !cascade.casterPlanes.empty();
}

bool findCasterOutsideCamera(const Camera& camera, const R4SunCascadeSet& set)
{
    Fmatrix viewProjection;
    viewProjection.mul(camera.projection, camera.view);
    CFrustum cameraFrustum;
    cameraFrustum.CreateFromMatrix(viewProjection, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

    for (const R4SunCascade& cascade : set.cascades)
    {
        CFrustum casterFrustum;
        if (!buildCasterFrustum(cascade, casterFrustum))
            continue;
        // Search the actual camera/light-volume boundary region. A found
        // sphere proves the submitted caster volume is wider than the primary
        // view, as required for a cold off-camera shadow caster.
        for (int z = 2; z <= 180; z += 2)
        {
            for (int y = -160; y <= 160; y += 2)
            {
                for (int x = -160; x <= 160; x += 8)
                {
                    Fvector center{float(x), float(y), float(z)};
                    constexpr float radius = 0.5f;
                    const bool visibleToCamera = cameraFrustum.testSphere_dirty(center, radius) != fcvNone;
                    const bool insideCaster = casterFrustum.testSphere_dirty(center, radius) != fcvNone;
                    if (!visibleToCamera && insideCaster)
                        return true;
                }
            }
        }
    }
    return false;
}

float maxRayDelta(const std::array<std::array<float, 3>, 4>& left,
    const std::array<std::array<float, 3>, 4>& right)
{
    float maximum = 0.f;
    for (std::size_t ray = 0; ray < left.size(); ++ray)
        for (std::size_t axis = 0; axis < 3u; ++axis)
            maximum = std::max(maximum, std::abs(left[ray][axis] - right[ray][axis]));
    return maximum;
}

bool cascadeContract(const Fvector& sunDirection, const bool testCasterCoverage,
    const bool compareMiddleRange)
{
    const Camera camera = makeCamera(Fvector{0.f, 0.f, 0.f});
    const std::array<float, 3> configuredSizes{20.f, 60.f, 160.f};
    const std::array<float, 3> legacyMiddleSizes{20.f, 40.f, 160.f};
    const R4SunCascadeSet configured = build(camera, sunDirection, configuredSizes);
    if (!require(finiteCascadeSet(configured, testCasterCoverage),
            "source cascade builder must produce finite geometry for real sun directions"))
        return false;
    for (std::size_t cascade = 0; cascade < configured.cascades.size(); ++cascade)
    {
        if (!require(footprintMatchesSize(configured.cascades[cascade]),
                "projected shadow footprint must retain its configured world width"))
            return false;
        if (cascade < 2u && !require(receiverCubeMapsToUnitFootprint(configured.cascades[cascade]),
                "near and middle receiver cubes must map to their own shadow-map footprint"))
            return false;
    }

    // The far receiver cube is authored from the middle cascade footprint.
    if (!require(maxPointDelta(configured.cascades[2].receiverVolume,
                    configured.cascades[1].receiverVolume) < 1.0e-4f,
            "far receiver cube must follow the middle cascade inverse footprint"))
        return false;

    const R4SunCascadeSet legacyMiddle = build(camera, sunDirection, legacyMiddleSizes);
    if (!require(finiteCascadeSet(legacyMiddle, testCasterCoverage),
            "the Windows 20/40/160 comparison must remain geometrically valid"))
        return false;
    if (compareMiddleRange)
    {
        if (!require(maxRayDelta(configured.cascades[1].nextRayOrigins,
                        legacyMiddle.cascades[1].nextRayOrigins) > 1.0e-3f &&
                maxPointDelta(configured.cascades[1].receiverVolume,
                        legacyMiddle.cascades[1].receiverVolume) > 1.0e-3f,
                "changing only the middle cascade range must change chained rays and its boundary"))
            return false;
        if (!require(maxPointDelta(configured.cascades[0].receiverVolume,
                        legacyMiddle.cascades[0].receiverVolume) < 1.0e-4f,
                "changing the middle cascade must not perturb the near cascade boundary"))
            return false;
    }

    if (testCasterCoverage &&
        !require(findCasterOutsideCamera(camera, configured),
            "a source caster volume must admit an object outside the primary camera frustum"))
        return false;
    return true;
}

bool cameraSnapBounded()
{
    const Fvector sunDirection{0.f, -1.f, 0.f};
    const std::array<float, 3> sizes{20.f, 60.f, 160.f};
    const Camera stationary = makeCamera(Fvector{0.f, 0.f, 0.f});
    const Camera nudged = makeCamera(Fvector{0.00001f, 0.f, 0.f});
    const R4SunCascadeSet first = build(stationary, sunDirection, sizes);
    const R4SunCascadeSet second = build(nudged, sunDirection, sizes);
    if (!require(finiteCascadeSet(first) && finiteCascadeSet(second),
            "texel-snap inputs must keep every cascade finite"))
        return false;

    const std::array<float, 4> fixedWorldPoint{0.f, 0.f, 40.f, 1.f};
    for (std::size_t cascade = 0; cascade < first.cascades.size(); ++cascade)
    {
        const auto a = transformPoint(first.cascades[cascade].lightViewProjection, fixedWorldPoint);
        const auto b = transformPoint(second.cascades[cascade].lightViewProjection, fixedWorldPoint);
        if (std::abs(a[3]) < 1.0e-6f || std::abs(b[3]) < 1.0e-6f)
            return require(false, "texel-snap probe must retain finite clip w");
        const float dx = (b[0] / b[3] - a[0] / a[3]) * (ShadowResolution * 0.5f);
        const float dy = (b[1] / b[3] - a[1] / a[3]) * (ShadowResolution * 0.5f);
        if (!require(std::hypot(dx, dy) <= 4.05f,
                "sub-texel camera motion must remain within the source four-pixel snap step"))
            return false;
    }
    return true;
}

bool resetLastCascadeChangesRayChain()
{
    const Camera camera = makeCamera(Fvector{0.f, 0.f, 0.f});
    const Fvector sunDirection{0.f, -1.f, 0.f};
    const std::array<float, 3> sizes{20.f, 60.f, 160.f};
    const R4SunCascadeSet chained = build(camera, sunDirection, sizes, false);
    const R4SunCascadeSet reset = build(camera, sunDirection, sizes, true);
    if (!require(finiteCascadeSet(chained) && finiteCascadeSet(reset),
            "reset-last-cascade variants must remain finite"))
        return false;
    const bool earlierInputsUnchanged =
        maxRayDelta(chained.cascades[0].initialRayOrigins,
            reset.cascades[0].initialRayOrigins) <= 1.0e-5f &&
        maxRayDelta(chained.cascades[1].initialRayOrigins,
            reset.cascades[1].initialRayOrigins) <= 1.0e-5f;
    return require(chained.cascades[2].resetChain == false && reset.cascades[2].resetChain &&
            earlierInputsUnchanged &&
            maxRayDelta(chained.cascades[2].initialRayOrigins,
                reset.cascades[2].initialRayOrigins) > 1.0e-3f,
        "reset-last-cascade must reseed only the final cascade's input rays");
}

bool runProbe()
{
    if (!cascadeContract(Fvector{0.f, -1.f, 0.f}, true, true))
        return false;

    Fvector nearParallelSun{0.015f, -0.02f, 0.9997f};
    nearParallelSun.normalize();
    if (!cascadeContract(nearParallelSun, false, false))
        return false;
    if (!cameraSnapBounded())
        return false;
    return resetLastCascadeChangesRayChain();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4SunCascadeCaptureProbe --self-test\n");
        return 2;
    }
    return runProbe() ? 0 : 1;
}
