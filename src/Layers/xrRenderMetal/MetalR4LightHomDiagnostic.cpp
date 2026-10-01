#if defined(STATIC_RENDERER_R4)
#include "../xrRenderPC_R4/stdafx.h"
#endif
#include "MetalR4LightHomDiagnostic.h"
#include "MetalR4LocalLightAdapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace
{
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{ return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
struct Group
{
    std::vector<std::size_t> indices;
    std::array<float,3> minimum{}, maximum{};
    bool invalid=false, hud=false, volumetric=false, omni=false;
    unsigned faceMask=0;
};
}

bool MetalR4LightHomDiagnosticEnabled()
{
    static const bool enabled=[] {
        const char* value=std::getenv("XRAY_METAL_LIGHT_HOM_DIAGNOSTIC");
        return value && !std::strcmp(value,"1");
    }();
    return enabled;
}

std::size_t R4LightHomDiagnosticFrame::CountHidden(const std::vector<std::size_t>& indices,
    bool eligibleOnly) const
{
    std::size_t count=0;
    for (const auto i:indices)
        if (i<hidden.size() && hidden[i] && (!eligibleOnly || eligible[i])) ++count;
    return count;
}

R4LightHomDiagnosticFrame BeginMetalR4LightHomDiagnostic(
    const std::array<float,16>& view, const std::array<float,16>& projection,
    const std::array<float,3>& eye, bool cameraValid, std::uint32_t frame,
    const R4EngineLocalLightSnapshot* snapshot, const R4EnvironmentSnapshot& environment,
    const std::vector<std::vector<std::size_t>>& batches, const std::vector<std::size_t>& unshadowed)
{
    R4LightHomDiagnosticFrame result;
    result.enabled=MetalR4LightHomDiagnosticEnabled(); result.frame=frame;
    if (!result.enabled) return result;
    static std::uint64_t sample=0; result.sample=++sample;
    const auto start=Clock::now();
    std::size_t plannedRecords=0; for (const auto& batch:batches) plannedRecords+=batch.size();
    bool valid=cameraValid && environment.valid && environment.engineFrame==frame &&
        snapshot && snapshot->engineFrame==frame && frame!=UINT32_MAX;
    for (const auto v:view) valid &= std::isfinite(v);
    for (const auto v:projection) valid &= std::isfinite(v);
    for (const auto v:eye) valid &= std::isfinite(v);
    double buildMs=0, testMs=0;
#if defined(STATIC_RENDERER_R4)
    valid &= frame==Device.dwFrame;
    if (valid)
    {
        Fmatrix v{}, p{}, full;
        std::memcpy(&v,view.data(),sizeof(v)); std::memcpy(&p,projection.data(),sizeof(p));
        full.mul(p,v);
        for (const auto& row:full.m) for (const auto component:row) valid &= std::isfinite(component);
        Fvector camera; camera.set(eye[0],eye[1],eye[2]);
        const auto buildStart=Clock::now();
        // This owner was loaded with scheduleParallel=false by Metal level_Load.
        valid=valid && RImplementation.HOM.RenderDiagnostic(full,camera,frame);
        buildMs=elapsed(buildStart);
    }
#else
    valid=false; // Standalone bootstrap has no live xrFS/HOM level lifecycle.
#endif
    if (!valid)
    {
        std::fprintf(stderr,"METAL_LIGHT_HOM_FRAME frame=%u sample=%llu status=unavailable geometry_verified=0 suppressed=0 groups=0 tested=0 hidden=0 eligible_hidden=0 excluded_hud=0 excluded_volumetric=0 invalid=0 build_ms=%.6f test_ms=0 overhead_ms=%.6f planned_batches=%zu planned_shadow_records=%zu unshadowed_records=%zu\n",
            frame,(unsigned long long)result.sample,buildMs,elapsed(start),batches.size(),plannedRecords,unshadowed.size());
        return result;
    }
    result.ready=true;
    result.hidden.resize(snapshot->lights.size(),0); result.eligible.resize(snapshot->lights.size(),0);
    std::map<std::uint64_t,Group> groups;
    for (std::size_t i=0;i<snapshot->lights.size();++i)
    {
        const auto& light=snapshot->lights[i];
        auto& group=groups[light.parentLightId ? light.parentLightId : light.lightId];
        std::array<float,3> lo{},hi{};
        const bool bounds=BuildMetalR4LocalLightVolumeBounds(light,lo,hi);
        group.invalid |= !bounds || !light.active;
        group.hud |= light.hudMode;
        // SSFX can enable volume even when the authored bVolumetric is false.
        group.volumetric |= environment.localVolumetricEnabled && environment.volumetricMode!=1 &&
            (light.volumetricEnabled || environment.ssfxVolumetric[0]>0) && light.shadowed &&
            (light.type==R4EngineLocalLightType::Spot || light.type==R4EngineLocalLightType::OmniFace) &&
            !light.hudMode;
        if (light.type==R4EngineLocalLightType::OmniFace)
        {
            group.omni=true;
            if (light.omniFace>=6 || (group.faceMask&(1u<<light.omniFace))) group.invalid=true;
            else group.faceMask|=1u<<light.omniFace;
        }
        if (bounds)
            for (unsigned axis=0;axis<3;++axis)
            {
                group.minimum[axis]=group.indices.empty()?lo[axis]:std::min(group.minimum[axis],lo[axis]);
                group.maximum[axis]=group.indices.empty()?hi[axis]:std::max(group.maximum[axis],hi[axis]);
            }
        group.indices.push_back(i);
    }
    std::size_t tested=0, hidden=0, eligibleHidden=0, hud=0, volume=0, invalid=0;
    for (const auto& entry:groups)
    {
        const auto& group=entry.second;
        const bool bad=group.invalid || (group.omni && (group.faceMask!=63 || group.indices.size()!=6));
        const char* reason=bad?"invalid_volume":group.hud?"hud":group.volumetric?"volumetric":"none";
        const bool eligible=!bad && !group.hud && !group.volumetric;
        bool occluded=false;
        if (bad) ++invalid;
        else if (group.hud) ++hud;
        else if (group.volumetric) ++volume;
        else
        {
            const auto testStart=Clock::now();
#if defined(STATIC_RENDERER_R4)
            Fbox box; box.min.set(group.minimum[0],group.minimum[1],group.minimum[2]);
            box.max.set(group.maximum[0],group.maximum[1],group.maximum[2]);
            occluded=!RImplementation.HOM.visibleDiagnostic(box,frame);
#endif
            testMs+=elapsed(testStart); ++tested;
            hidden+=occluded; eligibleHidden+=occluded && eligible;
        }
        for (const auto i:group.indices)
        { result.hidden[i]=occluded; result.eligible[i]=eligible; }
        const char* state=bad?"invalid":!eligible?"exempt":occluded?"hidden":"visible";
        std::fprintf(stderr,"METAL_LIGHT_HOM_GROUP frame=%u sample=%llu parent=%llu state=%s records=%zu eligible=%u reason=%s bounds_min=%.9g,%.9g,%.9g bounds_max=%.9g,%.9g,%.9g\n",
            frame,(unsigned long long)result.sample,(unsigned long long)entry.first,state,group.indices.size(),unsigned(eligible),reason,
            group.minimum[0],group.minimum[1],group.minimum[2],group.maximum[0],group.maximum[1],group.maximum[2]);
    }
    std::uint64_t cameraHash=1469598103934665603ull;
    for (const auto* matrix:{&view,&projection})
        for (const auto value:*matrix)
        {
            std::uint32_t bits; std::memcpy(&bits,&value,sizeof(bits));
            cameraHash^=bits; cameraHash*=1099511628211ull;
        }
    std::fprintf(stderr,"METAL_LIGHT_HOM_FRAME frame=%u sample=%llu status=ready geometry_verified=0 suppressed=0 groups=%zu tested=%zu hidden=%zu eligible_hidden=%zu excluded_hud=%zu excluded_volumetric=%zu invalid=%zu build_ms=%.6f test_ms=%.6f overhead_ms=%.6f eye=%.9g,%.9g,%.9g camera_hash=%llx planned_batches=%zu planned_shadow_records=%zu unshadowed_records=%zu\n",
        frame,(unsigned long long)result.sample,groups.size(),tested,hidden,eligibleHidden,hud,volume,invalid,buildMs,testMs,elapsed(start),
        eye[0],eye[1],eye[2],(unsigned long long)cameraHash,batches.size(),plannedRecords,unshadowed.size());
    return result;
}
