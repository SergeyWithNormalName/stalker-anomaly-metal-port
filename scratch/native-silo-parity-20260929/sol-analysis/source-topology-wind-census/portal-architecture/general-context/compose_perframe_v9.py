from pathlib import Path
import hashlib, json, shutil

d=Path(__file__).resolve().parent
out=d/'runtime-root-stage-09'
assert not out.exists()
shutil.copytree(d/'runtime-root-stage-08/stage',out/'stage')
p=out/'stage/src/Layers/xrRenderMetal/MetalR4EngineScene.mm'
s=p.read_text()
old='''    const bool environmentSnapshotFresh,
    MetalR4EngineScene::MainFloraPortalCandidateSet& result)'''
assert s.count(old)==1
s=s.replace(old,'''    const bool environmentSnapshotFresh,
    const bool immutablePopulationQualified,
    MetalR4EngineScene::MainFloraPortalCandidateSet& result)''')
start=s.index('    // The adapter publishes one value record per admitted main Flora draw.')
end=s.index('    for (const R4FloraRestRecord& record : restRecords)\n    {\n        if (!record.known',start)
block=s[start:end]
s=s[:start]+'''    // The Scene Prepare/Reset lifecycle owns immutable geometry, material flags,
    // graph and rest records. A caller may reuse ONLY the completed population
    // join for that same generation and same vector storage/count. Mutable view,
    // generation, motion and every owner-frustum test still run above/below.
    if (!immutablePopulationQualified)
    {
'''+''.join('    '+line+'\n' for line in block.splitlines())+'''    }

'''+s[end:]
old='''    bool portalPerFrameReady = false;''';assert s.count(old)==1
s=s.replace(old,'''    bool portalPerFrameReady = false;
    bool portalCachePopulation = false;
    std::uint64_t portalPopulationGeneration = 0u;
    const R4FloraRestRecord* portalPopulationRecords = nullptr;
    std::size_t portalPopulationCount = 0u;''')
old='''    impl->portalPerFrameSuppress = portalPerFrame && std::strcmp(portalPerFrame, "on") == 0;''';assert s.count(old)==1
s=s.replace(old,old+'''
    const char* cachePopulation = std::getenv("XRAY_METAL_FLORA_PORTAL_CACHE_POPULATION");
    impl->portalCachePopulation = cachePopulation && std::strcmp(cachePopulation, "1") == 0;''')
old='''                    impl->portalSelectedViewMatches, motionSnapshotFresh, environmentSnapshotFresh, candidates))''';assert s.count(old)==1
s=s.replace(old,'''                    impl->portalSelectedViewMatches, motionSnapshotFresh, environmentSnapshotFresh, false, candidates))''')
old='''                        if (populationComplete && matchingView && extentMatches)
                            qualified = buildMainFloraPortalCandidateSet(impl->portalCache, input,
                                impl->portalPerFrameOutput, impl->scene.geometry,
                                impl->portalFloraShaderMaterials, records, floraView,
                                matchingView, motionFresh, environmentFresh, perFrameCandidates);
                        else perFrameCandidates.reason = "frame-view-extent-or-population-mismatch";''';assert s.count(old)==1
s=s.replace(old,'''                        const bool storedPopulation = impl->portalCachePopulation && populationComplete &&
                            input.generation != 0u && impl->portalPopulationGeneration == input.generation &&
                            impl->portalPopulationRecords == records.data() &&
                            impl->portalPopulationCount == records.size();
                        if (populationComplete && matchingView && extentMatches)
                        {
                            qualified = buildMainFloraPortalCandidateSet(impl->portalCache, input,
                                impl->portalPerFrameOutput, impl->scene.geometry,
                                impl->portalFloraShaderMaterials, records, floraView,
                                matchingView, motionFresh, environmentFresh, storedPopulation, perFrameCandidates);
                            if (qualified && impl->portalCachePopulation && !storedPopulation)
                            {
                                impl->portalPopulationGeneration = input.generation;
                                impl->portalPopulationRecords = records.data();
                                impl->portalPopulationCount = records.size();
                            }
                        }
                        else perFrameCandidates.reason = "frame-view-extent-or-population-mismatch";''')
p.write_text(s)
mf=json.loads((d/'runtime-root-source-hashes-v8.json').read_text())
for row in mf['files']:
    row['draft_v9_sha256']=hashlib.sha256((out/'stage'/row['file']).read_bytes()).hexdigest()
(d/'runtime-root-source-hashes-v9.json').write_text(json.dumps(mf,indent=2)+'\n')
(out/'SCOPE.md').write_text('Optional defaultoff immutable population qualification reuse only. First successful full candidate validates every source/material/index ID and records exact generation/rest-vector identity/count. Prepare calls Reset, which destroys cache state. All mutable same-frame gates, wind bounds, portal graph and every owner frustum remain fresh. Oracle always forces complete join. No FPS or shipping admission until measured.\n')
print(out)
