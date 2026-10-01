#include "../xrRenderPC_R4/stdafx.h"
#include "../../xrCDB/Frustum.h"

#include "MetalR4VolumetricCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace
{
constexpr std::uint32_t SourceSliceCount = 100u;

bool finiteMatrix(const Fmatrix& matrix)
{
    const float* values = &matrix._11;
    for (std::size_t index = 0u; index < 16u; ++index)
        if (!std::isfinite(values[index]))
            return false;
    return true;
}

template <std::size_t Count>
bool finiteArray(const std::array<float, Count>& values)
{
    return std::all_of(values.begin(), values.end(), [](const float value) {
        return std::isfinite(value);
    });
}

Fvector asVector(const std::array<float, 3>& value)
{
    Fvector result;
    result.set(value[0], value[1], value[2]);
    return result;
}

void copyMatrix(const Fmatrix& source, std::array<float, 16>& target)
{
    std::memcpy(target.data(), &source, sizeof(source));
}
} // namespace

void CaptureMetalR4OmniFaceVolumetricValues(R4EngineLocalLight& face,
    const std::array<float, 3>& cameraPosition,
    const std::array<float, 4>& ssfxVolumetric,
    const float slightFade, const float lodStart, const float lodEnd)
{
    // This is the value-side equivalent of light::export_()'s OMNIPART
    // synchronization. The source child is constructed before the parent is
    // exported; Direct Metal bypasses that D3D list builder, so reading the
    // child would observe constructor values such as bVolumetric=false and
    // the child sphere at its initial position.
    const float radius = face.range * std::sqrt(0.5f);
    Fvector position, direction, center, camera;
    position.set(face.position[0], face.position[1], face.position[2]);
    direction.set(face.direction[0], face.direction[1], face.direction[2]);
    center.mad(position, direction, radius);
    camera.set(cameraPosition[0], cameraPosition[1], cameraPosition[2]);
    const float distanceSquared = camera.distance_to_sqr(center);
    const float lodDistanceSquared = distanceSquared + EPS;
    const float ssa = slightFade * radius / lodDistanceSquared;
    const float lodSpan = lodStart - lodEnd;
    const float lodValue = lodSpan != 0.0f ?
        std::clamp((ssa - lodEnd) / lodSpan, 0.0f, 1.0f) : 0.0f;

    face.volumetricEnabled = face.volumetricEnabled || ssfxVolumetric[0] > 0.0f;
    face.volumetricVisibilityDistance = camera.distance_to(center);
    // The legacy child setter ignores its argument and stores this console
    // control; preserve that source behavior without exporting the child.
    face.volumetricIntensity = ssfxVolumetric[1];
    face.volumetricDistance = 1.0f;
    face.volumetricLod = _sqrt(lodValue);
    face.volumetricPlayerLight = false; // source exporter does not copy this field
    face.volumetricSphereCenter = {center.x, center.y, center.z};
    face.volumetricSphereRadius = radius;
}

bool BuildMetalR4VolumetricSpotPass(const R4EngineLocalLight& light,
    const R4EnvironmentSnapshot& environment,
    const xray::metal::R4LocalShadowAtlasRecord& shadowRecord,
    const std::array<float, 16>& acceptedView,
    const std::array<float, 16>& acceptedProjection,
    MetalR4VolumetricAdapter::SpotPass& output, std::string& error)
{
    output = {};
    error.clear();

    // Volumetrics are optional. Decide whether the source would submit this
    // light before inspecting its shadow metadata, so a stale/culled light
    // cannot reject the whole world frame for a pass it would never draw.
    const bool ssfxMode = environment.ssfxVolumetric[0] > 0.0f;
    if (!environment.localVolumetricEnabled || environment.volumetricMode == 1u ||
        (!ssfxMode && !light.volumetricEnabled) || !light.shadowed ||
        (light.type != R4EngineLocalLightType::Spot && light.type != R4EngineLocalLightType::OmniFace) ||
        light.hudMode)
        return true;

    if (!finiteArray(acceptedView) || !finiteArray(acceptedProjection) ||
        !finiteArray(light.position) || !finiteArray(light.rawColor) ||
        !finiteArray(light.volumetricSphereCenter) || !std::isfinite(light.volumetricSphereRadius) ||
        !std::isfinite(light.range) || !std::isfinite(light.volumetricDistance) ||
        !std::isfinite(light.volumetricQuality) || !std::isfinite(light.volumetricIntensity) ||
        !std::isfinite(light.volumetricLod) || !std::isfinite(light.volumetricVisibilityDistance) ||
        !finiteArray(environment.ssfxVolumetric) ||
        !finiteArray(shadowRecord.lightView) || !finiteArray(shadowRecord.lightProjection) ||
        !finiteArray(shadowRecord.shadowMatrix) || !finiteArray(shadowRecord.lightMapMatrix))
    {
        error = "volumetric light capture contains non-finite input values";
        return false;
    }
    if (shadowRecord.lightIdentity != light.lightId || shadowRecord.engineFrame != environment.engineFrame ||
        (light.type == R4EngineLocalLightType::OmniFace && shadowRecord.face != light.omniFace) ||
        !shadowRecord.renderedDepth || shadowRecord.atlasSize == 0u || shadowRecord.atlasExtent == 0u)
    {
        error = "volumetric pass has no matching rendered local shadow record (light=" +
            std::to_string(shadowRecord.lightIdentity) + "/" + std::to_string(light.lightId) +
            " frame=" + std::to_string(shadowRecord.engineFrame) + "/" +
            std::to_string(environment.engineFrame) + " face=" +
            std::to_string(shadowRecord.face) + "/" + std::to_string(light.omniFace) +
            " rendered=" + std::to_string(shadowRecord.renderedDepth ? 1 : 0) +
            " atlas=" + std::to_string(shadowRecord.atlasSize) + "/" +
            std::to_string(shadowRecord.atlasExtent) + ")";
        return false;
    }

    Fmatrix view, projection, lightView, lightProjection;
    std::memcpy(&view, acceptedView.data(), sizeof(view));
    std::memcpy(&projection, acceptedProjection.data(), sizeof(projection));
    std::memcpy(&lightView, shadowRecord.lightView.data(), sizeof(lightView));
    std::memcpy(&lightProjection, shadowRecord.lightProjection.data(), sizeof(lightProjection));
    if (!finiteMatrix(view) || !finiteMatrix(projection) || !finiteMatrix(lightView) ||
        !finiteMatrix(lightProjection))
    {
        error = "volumetric view or projector matrix is invalid";
        return false;
    }

    Fmatrix fullTransform;
    // Device.cpp builds mFullTransform = mProject * mView; preserve that
    // order before deriving the inverse-transpose source clip planes.
    fullTransform.mul(projection, view);
    Fmatrix inverseFullTransform;
    inverseFullTransform.invert(fullTransform);
    if (!finiteMatrix(inverseFullTransform))
    {
        error = "volumetric inverse camera transform is invalid";
        return false;
    }

    // Match CRenderTarget::accum_volumetric's projection * light-view frustum,
    // including its shortened far plane and inverse-transpose plane transform.
    Fmatrix lightFrustumMatrix;
    lightFrustumMatrix.mul(lightProjection, lightView);
    CFrustum frustum;
    frustum.CreateFromMatrix(lightFrustumMatrix, FRUSTUM_P_ALL);
    if (frustum.p_count != 6)
    {
        error = "volumetric projector did not produce the six source clip planes";
        return false;
    }
    frustum.planes[4].d -= (frustum.planes[4].d + frustum.planes[5].d) *
        (1.0f - light.volumetricDistance);

    Fmatrix planeTransform;
    planeTransform.transpose(inverseFullTransform);
    for (std::size_t index = 0u; index < output.frustumClipPlanes.size(); ++index)
    {
        const Fplane& source = frustum.planes[index];
        Fvector4 input, transformed;
        input.set(source.n.x, source.n.y, source.n.z, source.d);
        planeTransform.transform(transformed, input);
        transformed.mul(-1.0f);
        output.frustumClipPlanes[index] = {transformed.x, transformed.y,
            transformed.z, transformed.w};
    }

    Fvector lightPosition = asVector(light.position);
    Fvector sphereCenter = asVector(light.volumetricSphereCenter);
    sphereCenter.sub(lightPosition);
    sphereCenter.mul(light.volumetricDistance);
    sphereCenter.add(lightPosition);
    view.transform(sphereCenter);
    const float scaledRadius = light.volumetricSphereRadius * light.volumetricDistance;
    if (!std::isfinite(scaledRadius) || scaledRadius < 0.0f)
    {
        error = "volumetric world sphere has invalid radius or distance";
        return false;
    }
    Fvector radius;
    radius.set(scaledRadius, scaledRadius, scaledRadius);
    Fbox bounds;
    bounds.setb(sphereCenter, radius);

    Fvector color;
    color.set(light.rawColor[0], light.rawColor[1], light.rawColor[2]);
    color.mul(light.volumetricDistance);
    int slices = 0;
    float quality = 0.0f;
    if (ssfxMode)
    {
        const float fade = std::clamp((light.volumetricVisibilityDistance - 20.0f) * 0.01f,
            0.0f, 1.0f);
        const float intensityFalloff = environment.ssfxVolumetric[1] -
            fade * environment.ssfxVolumetric[1];
        if (intensityFalloff <= 0.0f)
            return true;
        const float ssfxQuality = environment.ssfxVolumetric[2];
        if (!std::isfinite(ssfxQuality) || ssfxQuality <= 0.0f)
        {
            error = "SSFX volumetric quality must be positive for the source slice formula";
            return false;
        }
        if (ssfxQuality > static_cast<float>(SourceSliceCount) / 24.0f)
        {
            error = "source SSFX slice count exceeds the 100-slice source mesh";
            return false;
        }
        slices = static_cast<int>(24.0f * ssfxQuality);
        if (slices <= 0)
            return true;
        quality = static_cast<float>(slices) / 120.0f;
        const float intensityMod = (light.type == R4EngineLocalLightType::OmniFace ||
            light.volumetricPlayerLight) ? 0.2f : 1.0f;
        color.mul(intensityFalloff * intensityMod / ssfxQuality);
    }
    else
    {
        if (light.volumetricQuality > 1.0f)
        {
            error = "source volumetric quality exceeds the 100-slice source mesh";
            return false;
        }
        slices = std::max(10, static_cast<int>(SourceSliceCount * light.volumetricQuality));
        quality = static_cast<float>(slices) / static_cast<float>(SourceSliceCount);
        if (quality <= 0.0f)
            return true;
        color.mul(light.volumetricIntensity / quality);
    }
    color.mul(light.volumetricLod);

    // The Windows source mesh has 100 slices. Its SSFX quality 5 setting
    // computes 120 slices and indexes past that mesh; fail closed rather than
    // silently clamping a source-invalid draw.
    if (slices > static_cast<int>(SourceSliceCount))
    {
        error = "source volumetric slice count exceeds the 100-slice source mesh";
        return false;
    }
    const float attenuationRange = light.volumetricDistance * light.range * 0.95f;
    if (!std::isfinite(attenuationRange) || attenuationRange <= 0.0f || quality <= 0.0f)
    {
        error = "volumetric attenuation range or quality is invalid";
        return false;
    }
    const float attenuation = 1.0f / (attenuationRange * attenuationRange);
    float specular = 0.0f;
    if (environment.volumetricGlossMethod == 0)
    {
        const float average = (color.x + color.y + color.z) / 3.0f;
        specular = environment.volumetricGlossMinimum + environment.volumetricGlossFactor *
            ((average < 1.0f) ? std::pow(average, 2.0f / 3.0f) : average);
    }
    else
    {
        const float span = std::clamp(environment.volumetricGlossMinMax[1] -
            environment.volumetricGlossMinMax[0], 0.0f, 1.0f);
        specular = environment.volumetricGlossMinMax[0] + span * environment.volumetricGlossFactor;
    }
    Fvector viewLightPosition;
    view.transform_tiny(viewLightPosition, lightPosition);

    output.shadowMatrix = shadowRecord.shadowMatrix;
    output.lightMapMatrix = shadowRecord.lightMapMatrix;
    output.boundsMin = {bounds.x1, bounds.y1, bounds.z1, 0.0f};
    output.boundsMax = {bounds.x2, bounds.y2,
        bounds.z1 + (bounds.z2 - bounds.z1) / quality, 0.0f};
    output.dynamicColor = {color.x, color.y, color.z, specular};
    output.dynamicPositionAttenuation = {viewLightPosition.x, viewLightPosition.y,
        viewLightPosition.z, attenuation};
    output.sliceCount = static_cast<std::uint32_t>(slices);
    error.clear();
    return true;
}
