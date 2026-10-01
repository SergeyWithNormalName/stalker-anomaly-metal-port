from pathlib import Path
import hashlib, json, shutil

d = Path(__file__).resolve().parent
src = d / 'runtime-root-stage-07/stage'
out = d / 'runtime-root-stage-08'
assert not out.exists()
shutil.copytree(src, out / 'stage')
s = out / 'stage'

def replace(file, old, new):
    p = s / file
    text = p.read_text()
    assert text.count(old) == 1, (file, text.count(old), old[:100])
    p.write_text(text.replace(old, new))

metal = 'src/Layers/xrRenderMetal/'
replace('src/Layers/xrRender/R_calculate.cpp',
    'if (Device.dwFrame == 700u && topologyCensus && std::strcmp(topologyCensus, "1") == 0)',
    '''const char* portalPerFrame = std::getenv("XRAY_METAL_FLORA_PORTAL_PERFRAME");
            const bool perFrame = portalPerFrame &&
                (std::strcmp(portalPerFrame, "observe") == 0 || std::strcmp(portalPerFrame, "on") == 0);
            if (perFrame || (Device.dwFrame == 700u && topologyCensus && std::strcmp(topologyCensus, "1") == 0))''')
replace('src/Layers/xrRenderPC_R4/r4_loader.cpp',
    'if (portalObserve && std::strcmp(portalObserve, "1") == 0 &&',
    '''const char* portalPerFrame = std::getenv("XRAY_METAL_FLORA_PORTAL_PERFRAME");
        const bool perFrame = portalPerFrame &&
            (std::strcmp(portalPerFrame, "observe") == 0 || std::strcmp(portalPerFrame, "on") == 0);
        if (((portalObserve && std::strcmp(portalObserve, "1") == 0) || perFrame) &&''')

scene = metal + 'MetalR4EngineScene.mm'
replace(scene, 'if (!cache.qualified || !portal.valid || input.frameID != 700u ||',
    '''if (!cache.qualified || !portal.valid || portal.frameID != input.frameID ||
        portal.generation != input.generation ||''')
replace(scene, '    bool portalObserverEnabled = false;', '''    bool portalObserverEnabled = false;
    bool portalPerFrameEnabled = false;
    bool portalPerFrameSuppress = false;
    bool portalPerFrameReady = false;
    xray::metal::R4PortalContextFrameInput portalPerFrameInput;
    xray::metal::R4PortalContextFrameOutput portalPerFrameOutput;
    std::ofstream portalPerFrameLog;''')
replace(scene,
    '''    impl->portalObserverEnabled = portalEnabled && std::strcmp(portalEnabled, "1") == 0;
    if (impl->portalObserverEnabled)''',
    '''    impl->portalObserverEnabled = portalEnabled && std::strcmp(portalEnabled, "1") == 0;
    const char* portalPerFrame = std::getenv("XRAY_METAL_FLORA_PORTAL_PERFRAME");
    impl->portalPerFrameEnabled = portalPerFrame &&
        (std::strcmp(portalPerFrame, "observe") == 0 || std::strcmp(portalPerFrame, "on") == 0);
    impl->portalPerFrameSuppress = portalPerFrame && std::strcmp(portalPerFrame, "on") == 0;
    if (impl->portalPerFrameEnabled)
    {
        const char* logPath = std::getenv("XRAY_METAL_FLORA_PORTAL_PERFRAME_FILE");
        if (logPath && logPath[0] == '/')
        {
            impl->portalPerFrameLog.open(logPath, std::ios::trunc);
            impl->portalPerFrameLog << "frame,generation,mode,qualified,start_sector,graph_cpu_ms,mask_cpu_ms,excluded,actual_skips,draws,camera_culled,wind_unknown,owner_unknown,reason\\n";
        }
    }
    if (impl->portalObserverEnabled || impl->portalPerFrameEnabled)''')
replace(scene,
    '''                if (!impl->flora->DrawIntoActiveGBuffer(callbackBackend, floraView, callbackError))
                    return false;''',
    '''                MainFloraPortalCandidateSet perFrameCandidates;
                double maskCpuMs = 0.0;
                bool qualified = false;
                std::size_t portalSkipped = 0u;
                if (impl->portalPerFrameEnabled)
                {
                    const auto maskStart = std::chrono::steady_clock::now();
                    try
                    {
                        const auto& input = impl->portalPerFrameInput;
                        const bool matchingView = impl->portalPerFrameReady && callbackView.valid &&
                            callbackView.engineFrame == input.frameID &&
                            input.generation == impl->portalCache.generation &&
                            std::memcmp(callbackView.view.data(), input.currentView.data(), sizeof(float) * 16u) == 0 &&
                            std::memcmp(callbackView.projection.data(), input.currentProjection.data(), sizeof(float) * 16u) == 0 &&
                            std::memcmp(callbackView.eye.data(), &input.currentEye, sizeof(float) * 3u) == 0;
                        std::uint32_t width = 0u, height = 0u;
                        callbackBackend.DrawableSize(width, height);
                        const bool extentMatches = width == input.width && height == input.height;
                        const bool motionFresh = impl->floraMotion.valid &&
                            impl->floraMotion.engineFrame == callbackView.engineFrame;
                        const bool environmentFresh = impl->floraEnvironment.valid &&
                            impl->floraEnvironment.engineFrame == callbackView.engineFrame;
                        const auto& records = impl->flora->RestRecordsForValidation();
                        const bool populationComplete = !records.empty() && records.size() ==
                            impl->flora->LastTelemetry().admittedVisuals;
                        if (populationComplete && matchingView && extentMatches)
                            qualified = buildMainFloraPortalCandidateSet(impl->portalCache, input,
                                impl->portalPerFrameOutput, impl->scene.geometry,
                                impl->portalFloraShaderMaterials, records, floraView,
                                matchingView, motionFresh, environmentFresh, perFrameCandidates);
                        else perFrameCandidates.reason = "frame-view-extent-or-population-mismatch";
                    }
                    catch (...)
                    { qualified = false; perFrameCandidates = {}; perFrameCandidates.reason = "candidate-exception-fail-open"; }
                    maskCpuMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - maskStart).count();
                }
                const bool drawn = qualified && impl->portalPerFrameSuppress ?
                    impl->flora->DrawIntoActiveGBufferWithPortalMask(callbackBackend, floraView,
                        perFrameCandidates.excludedVisualIds, portalSkipped, callbackError) :
                    impl->flora->DrawIntoActiveGBuffer(callbackBackend, floraView, callbackError);
                if (!drawn) return false;
                if (impl->portalPerFrameEnabled)
                {
                    const auto& telemetry = impl->flora->LastTelemetry();
                    if (impl->portalPerFrameLog)
                        impl->portalPerFrameLog << callbackView.engineFrame << ',' << impl->portalPerFrameInput.generation << ','
                            << (impl->portalPerFrameSuppress ? "on" : "observe") << ',' << qualified << ','
                            << impl->portalPerFrameOutput.startSector << ',' << impl->portalPerFrameOutput.frameCpuMs << ','
                            << maskCpuMs << ',' << perFrameCandidates.excludedVisualIds.size() << ',' << portalSkipped << ','
                            << telemetry.draws << ',' << telemetry.culledVisuals << ','
                            << perFrameCandidates.windBoundUnknown << ',' << perFrameCandidates.ownerFrustumUnknown << ','
                            << perFrameCandidates.reason << '\\n';
                    if (callbackView.engineFrame % 120u == 0u)
                    {
                        if (impl->portalPerFrameLog) impl->portalPerFrameLog.flush();
                        std::fprintf(stderr, "METAL_FLORA_PORTAL_PERFRAME frame=%u mode=%s qualified=%d graph_ms=%.6f mask_ms=%.6f excluded=%zu skipped=%zu draws=%zu reason=%s\\n",
                            callbackView.engineFrame, impl->portalPerFrameSuppress ? "on" : "observe", int(qualified),
                            impl->portalPerFrameOutput.frameCpuMs, maskCpuMs, perFrameCandidates.excludedVisualIds.size(),
                            portalSkipped, telemetry.draws, perFrameCandidates.reason.c_str());
                    }
                }''')
replace(scene,
    '''    if (!impl || !impl->portalObserverEnabled || input.frameID != 700u || impl->portalAttempted)
        return false;''',
    '''    if (!impl) return false;
    if (impl->portalPerFrameEnabled)
    {
        impl->portalPerFrameReady = false;
        impl->portalPerFrameInput = input;
        impl->portalPerFrameOutput = {};
        impl->portalPerFrameOutput.generation = input.generation;
        impl->portalPerFrameOutput.frameID = input.frameID;
        const auto start = std::chrono::steady_clock::now();
        try
        {
            if (portalFullTransformMatchesSourceOrder(input))
                impl->portalPerFrameReady = xray::metal::ObserveR4PortalGeneration(
                    impl->portalCache, input, borrowedStaticCform, impl->portalPerFrameOutput);
            else impl->portalPerFrameOutput.unavailableReason = "source-full-transform-mismatch";
        }
        catch (...)
        { impl->portalPerFrameOutput.valid = false; impl->portalPerFrameOutput.unavailableReason = "graph-exception-fail-open"; }
        impl->portalPerFrameOutput.frameCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
    if (!impl->portalObserverEnabled || input.frameID != 700u || impl->portalAttempted)
        return impl->portalPerFrameReady;''')
replace(scene, '''void MetalR4EngineScene::OnFrameSubmitted()
{
    if (!impl) return;''', '''void MetalR4EngineScene::OnFrameSubmitted()
{
    if (!impl) return;
    impl->portalPerFrameReady = false;''')

header = metal + 'MetalR4FloraAdapter.h'
replace(header, '''    bool DrawIntoActiveGBufferForValidation(IR4Backend& backend,''',
    '''    // Experimental default-off source-order mask; all shader/upload contracts remain identical.
    bool DrawIntoActiveGBufferWithPortalMask(IR4Backend& backend, const R4FloraViewState& view,
        const std::vector<std::uint32_t>& excludeVisualIds, std::size_t& actuallySkipped, std::string& error);
    bool DrawIntoActiveGBufferForValidation(IR4Backend& backend,''')
flora = metal + 'MetalR4FloraAdapter.cpp'
replace(flora, '''bool MetalR4FloraAdapter::DrawIntoActiveGBufferForValidation(IR4Backend& backend,''',
    '''bool MetalR4FloraAdapter::DrawIntoActiveGBufferWithPortalMask(IR4Backend& backend,
    const R4FloraViewState& view, const std::vector<std::uint32_t>& excludeVisualIds,
    std::size_t& actuallySkipped, std::string& error)
{
    actuallySkipped = 0u;
    if (!std::is_sorted(excludeVisualIds.begin(), excludeVisualIds.end()) ||
        std::adjacent_find(excludeVisualIds.begin(), excludeVisualIds.end()) != excludeVisualIds.end())
        return DrawIntoActiveGBuffer(backend, view, error);
    return DrawIntoActiveGBufferCommon(backend, view, &excludeVisualIds, false,
        nullptr, &actuallySkipped, error);
}

bool MetalR4FloraAdapter::DrawIntoActiveGBufferForValidation(IR4Backend& backend,''')
replace(flora, '''            if (validationRestore) ++validationRestore->skipped;
            continue;''', '''            if (validationRestore) ++validationRestore->skipped;
            else if (actuallySkipped) ++*actuallySkipped;
            continue;''')

mf = json.loads((d / 'runtime-root-source-hashes-v7.json').read_text())
for row in mf['files']:
    row['draft_v8_sha256'] = hashlib.sha256((s / row['file']).read_bytes()).hexdigest()
(d / 'runtime-root-source-hashes-v8.json').write_text(json.dumps(mf, indent=2) + '\n')
(out / 'SCOPE.md').write_text('Defaultoff observe/on per-frame general portal+wind hypothesis. Full immutable population join intentionally retained each frame initially; total graph+mask cost measured before any caching complexity. Original source order, shaders, uploads and shadows preserved. Unknown/stale inputs fail open. Not shipping admitted.\n')
print(json.dumps({'stage':str(out),'files':len(mf['files'])}))
