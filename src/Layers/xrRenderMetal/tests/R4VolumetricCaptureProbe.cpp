#include "../../xrRenderPC_R4/stdafx.h"
#include "../MetalR4VolumetricCapture.h"
#include "../R4GameplayFrameAdmission.h"
#include "../../xrRenderPC_R4/r2_types.h"
#include "../../../xrCDB/Frustum.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
constexpr float Epsilon = 2.0e-4f;

bool require(const bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "R4 volumetric capture probe failed: %s\n", message);
    return false;
}

bool near(const float left, const float right, const float tolerance = Epsilon)
{
    return std::fabs(left - right) <= tolerance;
}

bool testGameplayFrameAdmission()
{
    const auto ready = EvaluateR4GameplayFrameAdmission(false, true, true,
        77u, true, 77u, true, 77u, true, 77u, true, 77u);
    if (!require(ready == R4GameplayFrameAdmission::Ready,
            "gameplay frame should admit matching current snapshots"))
        return false;
    const auto loading = EvaluateR4GameplayFrameAdmission(true, true, true,
        77u, true, 77u, true, 77u, true, 77u, true, 77u);
    if (!require(loading == R4GameplayFrameAdmission::Loading,
            "pending level events must prevent gameplay rendering even with matching stamps"))
        return false;
    const auto beginReadyBeforeLightingCapture = EvaluateR4GameplayFrameAdmission(false, true, true,
        77u, true, 77u, true, 77u, true, 76u, true, 76u, false);
    if (!require(beginReadyBeforeLightingCapture == R4GameplayFrameAdmission::Ready,
            "Begin admission must not wait for world/light snapshots captured later in End"))
        return false;
    const auto staleWorld = EvaluateR4GameplayFrameAdmission(false, true, true,
        77u, true, 77u, true, 77u, true, 76u, true, 77u);
    if (!require(staleWorld == R4GameplayFrameAdmission::StaleWorldSnapshot,
            "End admission must reject a world snapshot from a different frame"))
        return false;
    const auto staleEnvironment = EvaluateR4GameplayFrameAdmission(false, true, true,
        78u, true, 78u, true, 77u, true, 78u, true, 78u);
    return require(staleEnvironment == R4GameplayFrameAdmission::StaleEnvironment,
        "a first LoadDraw frame with stale environment must defer gameplay scene rendering");
}

Fmatrix perspective(const float nearPlane, const float farPlane, const float aspect)
{
    Fmatrix result;
    result.identity();
    const float yScale = 1.0f / std::tan(0.5f);
    result._11 = yScale / aspect;
    result._22 = yScale;
    result._33 = farPlane / (farPlane - nearPlane);
    result._34 = 1.0f;
    result._43 = -nearPlane * farPlane / (farPlane - nearPlane);
    result._44 = 0.0f;
    return result;
}

bool run()
{
    if (!testGameplayFrameAdmission())
        return false;
    // The native point children never run the D3D exporter. Seed only parent
    // values and verify all six source faces without any live light/query state.
    const std::array<std::array<float, 3>, 6> faceDirections = {{{1,0,0}, {-1,0,0},
        {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}}};
    const std::array<float, 6> sourceLods = {0.863155403f,1.000000000f,0.986269820f,1.000000000f,0.760721968f,1.000000000f};
    for (std::size_t f=0; f<faceDirections.size(); ++f)
    {
        R4EngineLocalLight face;
        face.position={3,4,6}; face.range=25; face.direction=faceDirections[f];
        face.volumetricQuality=.72f; face.rawColor={.4f,.5f,.6f};
        CaptureMetalR4OmniFaceVolumetricValues(face, {-4,2,-7}, {1,.8f,3,0}, 1,.03f,.001f);
        if (!require(face.volumetricEnabled && near(face.volumetricQuality,.72f) &&
                near(face.volumetricIntensity,.8f) && near(face.volumetricDistance,1) &&
                near(face.volumetricSphereRadius,17.6776695f) &&
                near(face.volumetricLod,sourceLods[f]) && !face.volumetricPlayerLight &&
                near(face.rawColor[0],.4f), "source parent controls/LOD must reach every point face"))
            return false;
        for (std::size_t axis=0; axis<3; ++axis)
            if (!require(near(face.volumetricSphereCenter[axis], face.position[axis] +
                    faceDirections[f][axis]*17.6776695f), "source six-face sphere orientation"))
                return false;
    }

    Fvector eye, forward, up;
    eye.set(-4.0f, 2.0f, -7.0f);
    forward.set(0.25f, -0.08f, 1.0f);
    forward.normalize();
    up.set(0.0f, 1.0f, 0.0f);
    Fmatrix view;
    view.build_camera_dir(eye, forward, up);
    const Fmatrix projection = perspective(0.1f, 180.0f, 1.37f);

    Fvector lightEye, lightForward;
    lightEye.set(8.0f, 5.0f, 2.0f);
    lightForward.set(-1.0f, -0.3f, 0.15f);
    lightForward.normalize();
    Fmatrix lightView;
    lightView.build_camera_dir(lightEye, lightForward, up);
    const Fmatrix lightProjection = perspective(0.4f, 55.0f, 1.0f);

    R4EngineLocalLight light;
    light.lightId = 0x90001003u;
    light.parentLightId = 0x90001000u;
    light.type = R4EngineLocalLightType::OmniFace;
    light.omniFace = 3u;
    light.position = {3.0f, 4.0f, 6.0f};
    light.range = 25.0f;
    light.shadowed = true;
    light.volumetricEnabled = true;
    light.volumetricVisibilityDistance = 35.0f;
    light.volumetricQuality = 0.7f;
    light.volumetricIntensity = 1.25f;
    light.volumetricDistance = 0.5f;
    light.volumetricLod = 0.5f;
    light.rawColor = {2.0f, 1.0f, 0.5f};
    light.volumetricSphereCenter = {6.0f, 10.0f, 3.0f};
    light.volumetricSphereRadius = 4.0f;

    R4EnvironmentSnapshot environment;
    environment.engineFrame = 77u;
    environment.localVolumetricEnabled = true;
    environment.volumetricMode = 0;
    environment.ssfxVolumetricAvailable = true;
    environment.ssfxVolumetric = {1.0f, 1.0f, 3.0f, 1.0f};
    environment.volumetricGlossMethod = 0;
    environment.volumetricGlossMinimum = 0.1f;
    environment.volumetricGlossFactor = 0.8f;

    xray::metal::R4LocalShadowAtlasRecord record;
    record.lightIdentity = light.lightId;
    record.engineFrame = environment.engineFrame;
    record.atlasSize = 256u;
    record.atlasExtent = 4096u;
    record.face = light.omniFace;
    record.renderedDepth = true;
    std::memcpy(record.lightView.data(), &lightView, sizeof(lightView));
    std::memcpy(record.lightProjection.data(), &lightProjection, sizeof(lightProjection));
    for (std::size_t index = 0u; index < 16u; ++index)
    {
        record.shadowMatrix[index] = index % 5u == 0u ? 1.0f : 0.0f;
        record.lightMapMatrix[index] = record.shadowMatrix[index];
    }

    std::array<float, 16> viewBytes{}, projectionBytes{};
    std::memcpy(viewBytes.data(), &view, sizeof(view));
    std::memcpy(projectionBytes.data(), &projection, sizeof(projection));
    MetalR4VolumetricAdapter::SpotPass output;
    std::string error;
    if (!require(BuildMetalR4VolumetricSpotPass(light, environment, record, viewBytes,
            projectionBytes, output, error), error.c_str()))
        return false;
    if (!require(output.sliceCount == 72u, "SSFX 3-quality source slice count must be 72"))
        return false;

    // Source formula: raw color * distance * visibility falloff * OMNIPART
    // modulation / quality * source LOD.
    const float sourceIntensity = 0.85f * 0.2f / 3.0f * 0.5f;
    if (!require(near(output.dynamicColor[0], 2.0f * 0.5f * sourceIntensity) &&
            near(output.dynamicColor[1], 1.0f * 0.5f * sourceIntensity) &&
            near(output.dynamicColor[2], 0.5f * 0.5f * sourceIntensity),
            "OmniFace source color, SSS falloff, distance and LOD must be applied once"))
        return false;
    const float colorAverage = (output.dynamicColor[0] + output.dynamicColor[1] +
        output.dynamicColor[2]) / 3.0f;
    const float expectedSpecular = 0.1f + 0.8f * std::pow(colorAverage, 2.0f / 3.0f);
    if (!require(near(output.dynamicColor[3], expectedSpecular),
            "source u_diffuse2s specular controls must come from the immutable environment snapshot"))
        return false;
    const float expectedAttenuation = 1.0f / (0.5f * 25.0f * 0.95f * 0.5f * 25.0f * 0.95f);
    if (!require(near(output.dynamicPositionAttenuation[3], expectedAttenuation),
            "source volumetric attenuation range must use distance * range * 0.95"))
        return false;

    Fvector expectedCenter;
    expectedCenter.set(6.0f, 10.0f, 3.0f);
    Fvector lightPosition;
    lightPosition.set(3.0f, 4.0f, 6.0f);
    expectedCenter.sub(lightPosition);
    expectedCenter.mul(0.5f);
    expectedCenter.add(lightPosition);
    view.transform(expectedCenter);
    if (!require(near(output.boundsMin[0], expectedCenter.x - 2.0f) &&
            near(output.boundsMin[1], expectedCenter.y - 2.0f) &&
            near(output.boundsMin[2], expectedCenter.z - 2.0f) &&
            near(output.boundsMax[0], expectedCenter.x + 2.0f) &&
            near(output.boundsMax[1], expectedCenter.y + 2.0f) &&
            near(output.boundsMax[2], expectedCenter.z - 2.0f + 4.0f / 0.6f),
            "OmniFace sphere center/radius and source reduced-slice AABB must match"))
        return false;

    // Independent source-contract reconstruction of the six planes. The
    // accepted camera matrices do not commute, so reversing projection/view
    // changes the expected transformed coefficients materially.
    Fmatrix sourceCameraFull;
    sourceCameraFull.mul(projection, view);
    Fmatrix sourceInverse;
    sourceInverse.invert(sourceCameraFull);
    Fmatrix sourcePlaneTransform;
    sourcePlaneTransform.transpose(sourceInverse);
    Fmatrix sourceProjector;
    sourceProjector.mul(lightProjection, lightView);
    CFrustum sourceFrustum;
    sourceFrustum.CreateFromMatrix(sourceProjector, FRUSTUM_P_ALL);
    sourceFrustum.planes[4].d -= (sourceFrustum.planes[4].d + sourceFrustum.planes[5].d) * 0.5f;
    float planeDelta = 0.0f;
    for (std::size_t index = 0u; index < 6u; ++index)
    {
        const Fplane& plane = sourceFrustum.planes[index];
        Fvector4 raw, transformed;
        raw.set(plane.n.x, plane.n.y, plane.n.z, plane.d);
        sourcePlaneTransform.transform(transformed, raw);
        transformed.mul(-1.0f);
        const std::array<float, 4> values{transformed.x, transformed.y, transformed.z, transformed.w};
        for (std::size_t axis = 0u; axis < 4u; ++axis)
        {
            if (!require(near(output.frustumClipPlanes[index][axis], values[axis]),
                    "captured clip planes must use source projection*view inverse-transpose order"))
                return false;
        }
    }
    Fmatrix reversedCameraFull;
    reversedCameraFull.mul(view, projection);
    Fmatrix reversedInverse;
    reversedInverse.invert(reversedCameraFull);
    Fmatrix reversedPlaneTransform;
    reversedPlaneTransform.transpose(reversedInverse);
    const Fplane& testPlane = sourceFrustum.planes[0];
    Fvector4 raw, reversed;
    raw.set(testPlane.n.x, testPlane.n.y, testPlane.n.z, testPlane.d);
    reversedPlaneTransform.transform(reversed, raw);
    reversed.mul(-1.0f);
    planeDelta = std::fabs(output.frustumClipPlanes[0][0] - reversed.x) +
        std::fabs(output.frustumClipPlanes[0][1] - reversed.y) +
        std::fabs(output.frustumClipPlanes[0][2] - reversed.z) +
        std::fabs(output.frustumClipPlanes[0][3] - reversed.w);
    if (!require(planeDelta > 1.0e-2f, "probe matrices must distinguish source and reversed multiplication"))
        return false;

    auto staleRecord = record;
    --staleRecord.engineFrame;
    if (!require(!BuildMetalR4VolumetricSpotPass(light, environment, staleRecord, viewBytes,
            projectionBytes, output, error) && error.find("frame=76/77") != std::string::npos,
            "stale shadow frame should fail with both record and expected frame values"))
        return false;

    environment.volumetricMode = 1;
    if (!require(BuildMetalR4VolumetricSpotPass(light, environment, record, viewBytes,
            projectionBytes, output, error) && output.sliceCount == 0u,
            "unsupported accum_volumetric_lv mode must remain inactive"))
        return false;

    std::puts("R4 volumetric capture source-contract probe passed");
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4VolumetricCaptureProbe --self-test\n");
        return 2;
    }
    return run() ? 0 : 1;
}
