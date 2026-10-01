#!/usr/bin/env python3
"""Write independent SCRATCH-ONLY observer/order drafts from the live prestate."""
from pathlib import Path
import difflib
import hashlib
import json

ROOT = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parent
CPP = "src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp"
H = "src/Layers/xrRenderMetal/MetalR4FloraAdapter.h"
SCENE = "src/Layers/xrRenderMetal/MetalR4EngineScene.mm"
ORIGINAL = {p: (ROOT / p).read_text() for p in (CPP, H, SCENE)}


def replace_once(text, old, new):
    assert text.count(old) == 1, f"source contract count={text.count(old)}: {old[:100]!r}"
    return text.replace(old, new, 1)


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def emit(name, draft):
    patch = "".join(
        "".join(difflib.unified_diff(ORIGINAL[p].splitlines(keepends=True),
                draft[p].splitlines(keepends=True), fromfile="a/" + p, tofile="b/" + p))
        for p in draft if ORIGINAL[p] != draft[p]
    )
    (OUT / (name + ".patch")).write_text(patch)
    for p, content in draft.items():
        dest = OUT / name / p
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(content)
    return {"patch_sha256": sha(patch), "files": {
        p: {"prestate_sha256": sha(ORIGINAL[p]), "draft_sha256": sha(draft[p])}
        for p in draft if ORIGINAL[p] != draft[p]
    }}


# The census is one captured CPU encode frame. It does not reorder or intercept
# backend calls, query clocks, retain slices, or change the selected casters.
census = ORIGINAL.copy()
census[H] = replace_once(census[H], "struct R4FloraViewState\n", """// Observer provenance only. Family 1=production local, 2=production sun.
// A default context excludes standalone and validation views from the census.
struct R4FloraShadowCensusView
{
    std::uint32_t engineFrame = 0;
    std::uint8_t family = 0;
    std::uint64_t identity = 0;
    std::uint32_t face = 0;
};

struct R4FloraViewState
""")
census[H] = replace_once(census[H], """    bool DrawShadowCasters(IR4Backend& backend,
        const std::array<float, 16>& lightViewProjection,
        const R4FloraViewState& view, std::string& error);
""", """    bool DrawShadowCasters(IR4Backend& backend,
        const std::array<float, 16>& lightViewProjection,
        const R4FloraViewState& view, std::string& error,
        const R4FloraShadowCensusView& census = {});
""")
census[CPP] = replace_once(census[CPP], """bool MetalR4FloraAdapter::DrawShadowCasters(IR4Backend& backend,
    const std::array<float, 16>& lightViewProjection,
    const R4FloraViewState& view, std::string& error)
""", """bool MetalR4FloraAdapter::DrawShadowCasters(IR4Backend& backend,
    const std::array<float, 16>& lightViewProjection,
    const R4FloraViewState& view, std::string& error,
    const R4FloraShadowCensusView& census)
""")
census[SCENE] = replace_once(census[SCENE],
    "        if (flora && flora->IsReady() && !flora->DrawShadowCasters(backend,tileMatrix,floraView,error))\n            return false;\n",
    """        if (flora && flora->IsReady() && !flora->DrawShadowCasters(backend,tileMatrix,floraView,error,
                {view.engineFrame, 1u, record.lightIdentity, record.face}))
            return false;
""")
census[SCENE] = replace_once(census[SCENE],
    "            if (flora && flora->IsReady() && !flora->DrawShadowCasters(backend,lightVP,floraView,error))\n                return false;\n",
    """            if (flora && flora->IsReady() && !flora->DrawShadowCasters(backend,lightVP,floraView,error,
                    {view.engineFrame, 2u, cascade, 0u}))
                return false;
""")
census[CPP] = replace_once(census[CPP], "    bool shadowInstancing = false;\n", """    bool shadowInstancing = false;
    bool shadowStateCensus = false;
    bool censusTargetSet = false;
    std::uint32_t censusTargetFrame = 0;
    bool censusStarted = false;
    bool censusFinished = false;
    std::uint32_t censusFrame = 0;
    struct CensusView
    {
        std::uint8_t family;
        std::uint64_t identity;
        std::uint32_t face;
        std::array<float, 16> lightViewProjection;
    };
    std::vector<CensusView> censusedViews;
""")
census[CPP] = replace_once(census[CPP], "    m_impl = std::make_unique<Impl>();\n", """    m_impl = std::make_unique<Impl>();
    const char* censusFlag = std::getenv("XRAY_METAL_FLORA_SHADOW_STATE_CENSUS");
    m_impl->shadowStateCensus = censusFlag && std::strcmp(censusFlag, "1") == 0;
    if (m_impl->shadowStateCensus)
    {
        if (const char* target = std::getenv("XRAY_METAL_FLORA_SHADOW_STATE_CENSUS_FRAME"))
        {
            char* end = nullptr;
            const unsigned long long frame = std::strtoull(target, &end, 10);
            if (!*target || !end || *end || frame > std::numeric_limits<std::uint32_t>::max())
            {
                std::fprintf(stderr, "METAL_FLORA_SHADOW_STATE_CENSUS_DISABLED reason=invalid_frame\\n");
                m_impl->shadowStateCensus = false;
            }
            else
            {
                m_impl->censusTargetSet = true;
                m_impl->censusTargetFrame = static_cast<std::uint32_t>(frame);
            }
        }
    }
""")
END_LOOP = """    if (m_impl->shadowInstancing)
        for (const auto& group : m_impl->shadowGroups)
            if (!group.models.empty() &&
                !encodeCaster(m_impl->draws[group.representative], &group.models))
                return false;
    return true;
}

bool MetalR4FloraAdapter::DrawShadowCastersForValidation"""
CENSUS_BODY = """    if (m_impl->shadowStateCensus && m_impl->shadowInstancing &&
        (census.family == 1u || census.family == 2u) &&
        !view.shadowIgnoreAlpha && !view.shadowOnlyOffCamera && !m_impl->censusFinished)
    {
        if (!m_impl->censusStarted &&
            (m_impl->censusTargetSet ? census.engineFrame == m_impl->censusTargetFrame :
                census.engineFrame % 120u == 0u))
        {
            m_impl->censusStarted = true;
            m_impl->censusFrame = census.engineFrame;
        }
        if (m_impl->censusStarted && census.engineFrame != m_impl->censusFrame)
        {
            std::fprintf(stderr, "METAL_FLORA_SHADOW_STATE_CENSUS_DONE frame=%u unique_views=%zu\\n",
                m_impl->censusFrame, m_impl->censusedViews.size());
            m_impl->censusFinished = true;
        }
        if (m_impl->censusStarted && !m_impl->censusFinished)
        {
            const Impl::CensusView key{census.family, census.identity,
                census.face, lightViewProjection};
            const bool alreadySeen = std::any_of(m_impl->censusedViews.begin(),
                m_impl->censusedViews.end(), [&key](const auto& previous) {
                    return previous.family == key.family && previous.identity == key.identity &&
                        previous.face == key.face && std::memcmp(previous.lightViewProjection.data(),
                            key.lightViewProjection.data(), sizeof(Matrix4)) == 0;
                });
            // A hard cap bounds diagnostic work and log output in pathological
            // scenes. It is explicit in the log and cannot silently imply a full census.
            if (!alreadySeen && m_impl->censusedViews.size() < 1024u)
            {
                m_impl->censusedViews.push_back(key);
                std::vector<std::size_t> groups;
                std::vector<const IR4GraphicsPipeline*> pipelineKeys;
                const auto pipelineFor = [&](const auto& draw) {
                    return (draw.material == Impl::Material::LeafBump ?
                        m_impl->leafInstancedShadowPipeline : m_impl->trunkInstancedShadowPipeline).get();
                };
                // Key order follows all immutable groups, matching the runtime
                // draft's Prepare-time partition even when its first group is culled.
                for (std::size_t index = 0; index < m_impl->shadowGroups.size(); ++index)
                {
                    const auto* pipeline = pipelineFor(m_impl->draws[
                        m_impl->shadowGroups[index].representative]);
                    if (std::find(pipelineKeys.begin(), pipelineKeys.end(), pipeline) == pipelineKeys.end())
                        pipelineKeys.push_back(pipeline);
                    if (!m_impl->shadowGroups[index].models.empty())
                        groups.push_back(index);
                }
                std::vector<std::size_t> partitioned;
                partitioned.reserve(groups.size());
                for (const auto* pipeline : pipelineKeys)
                    for (const auto index : groups)
                        if (pipelineFor(m_impl->draws[m_impl->shadowGroups[index].representative]) == pipeline)
                            partitioned.push_back(index);
                const auto counts = [&](const auto& order) {
                    std::array<std::size_t, 2> result{};
                    const IR4GraphicsPipeline* lastPso = nullptr;
                    const IR4Buffer* lastVb = nullptr;
                    for (const auto index : order)
                    {
                        const auto& draw = m_impl->draws[m_impl->shadowGroups[index].representative];
                        const auto* pso = pipelineFor(draw);
                        if (pso != lastPso) { ++result[0]; lastPso = pso; }
                        if (draw.vertices.get() != lastVb) { ++result[1]; lastVb = draw.vertices.get(); }
                    }
                    return result;
                };
                const auto current = counts(groups);
                const auto grouped = counts(partitioned);
                std::fprintf(stderr,
                    "METAL_FLORA_SHADOW_STATE_CENSUS frame=%u family=%s identity=%llu face=%u "
                    "view=%zu total_groups=%zu selected_groups=%zu broad_candidates=%zu instances=%zu "
                    "current_pso_binds=%zu grouped_pso_binds=%zu current_vb_binds=%zu grouped_vb_binds=%zu\\n",
                    census.engineFrame, census.family == 1u ? "local" : "sun",
                    static_cast<unsigned long long>(census.identity), census.face,
                    m_impl->censusedViews.size(), m_impl->shadowGroups.size(), groups.size(), selected.size(),
                    m_telemetry.shadowSubmittedVisuals, current[0], grouped[0], current[1], grouped[1]);
            }
            else if (!alreadySeen)
            {
                std::fprintf(stderr, "METAL_FLORA_SHADOW_STATE_CENSUS_TRUNCATED frame=%u cap=1024\\n",
                    census.engineFrame);
                m_impl->censusFinished = true;
            }
        }
    }
"""
census[CPP] = replace_once(census[CPP], END_LOOP,
    END_LOOP.replace("    return true;\n}", CENSUS_BODY + "    return true;\n}", 1))


# Independent runtime candidate: no census provenance or observer branch. The
# pipeline-key partitions are immutable Prepare-time permutations of existing
# groups. The default path retains the original group traversal verbatim.
order = {CPP: ORIGINAL[CPP]}
order[CPP] = replace_once(order[CPP], "    bool shadowInstancing = false;\n", """    bool shadowInstancing = false;
    bool shadowPsoOrder = [] {
        const char* flag = std::getenv("XRAY_METAL_FLORA_SHADOW_PSO_ORDER");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    std::array<std::vector<std::size_t>, 2> shadowPipelineGroupOrder;
    const std::shared_ptr<IR4GraphicsPipeline>& ShadowPipelineFor(const Draw& draw,
        const bool instanced, const bool ignoreAlpha) const
    {
        const bool leaf = draw.material == Material::LeafBump;
        return instanced ? (leaf ? (ignoreAlpha ? leafNoAlphaInstancedShadowPipeline :
            leafInstancedShadowPipeline) : trunkInstancedShadowPipeline) :
            (leaf ? (ignoreAlpha ? leafNoAlphaShadowPipeline : leafShadowPipeline) : trunkShadowPipeline);
    }
""")
order[CPP] = replace_once(order[CPP],
    "    for (std::size_t g = 0; g < groupSizes.size(); ++g)\n        m_impl->shadowGroups[g].models.reserve(groupSizes[g]);\n",
    """    for (std::size_t g = 0; g < groupSizes.size(); ++g)
        m_impl->shadowGroups[g].models.reserve(groupSizes[g]);
    if (m_impl->shadowPsoOrder && m_impl->shadowInstancing)
        for (std::size_t ignoreAlpha = 0; ignoreAlpha < 2u; ++ignoreAlpha)
        {
            std::vector<const IR4GraphicsPipeline*> pipelineKeys;
            for (const auto& group : m_impl->shadowGroups)
            {
                const auto* pipeline = m_impl->ShadowPipelineFor(
                    m_impl->draws[group.representative], true, ignoreAlpha != 0u).get();
                if (std::find(pipelineKeys.begin(), pipelineKeys.end(), pipeline) == pipelineKeys.end())
                    pipelineKeys.push_back(pipeline);
            }
            auto& partition = m_impl->shadowPipelineGroupOrder[ignoreAlpha];
            partition.reserve(m_impl->shadowGroups.size());
            for (const auto* pipeline : pipelineKeys)
                for (std::size_t group = 0; group < m_impl->shadowGroups.size(); ++group)
                    if (m_impl->ShadowPipelineFor(m_impl->draws[m_impl->shadowGroups[group].representative],
                            true, ignoreAlpha != 0u).get() == pipeline)
                        partition.push_back(group);
        }
""")
order[CPP] = replace_once(order[CPP], """        const auto& pipeline = models ? (leaf ?
            (view.shadowIgnoreAlpha ? m_impl->leafNoAlphaInstancedShadowPipeline : m_impl->leafInstancedShadowPipeline) :
            m_impl->trunkInstancedShadowPipeline) : (leaf ?
            (view.shadowIgnoreAlpha ? m_impl->leafNoAlphaShadowPipeline : m_impl->leafShadowPipeline) :
            m_impl->trunkShadowPipeline);
""", """        const auto& pipeline = m_impl->ShadowPipelineFor(draw, models != nullptr, view.shadowIgnoreAlpha);
""")
order[CPP] = replace_once(order[CPP], END_LOOP, """    if (m_impl->shadowInstancing)
    {
        if (m_impl->shadowPsoOrder)
        {
            for (const auto index : m_impl->shadowPipelineGroupOrder[view.shadowIgnoreAlpha ? 1u : 0u])
            {
                const auto& group = m_impl->shadowGroups[index];
                if (!group.models.empty() &&
                    !encodeCaster(m_impl->draws[group.representative], &group.models))
                    return false;
            }
        }
        else
            for (const auto& group : m_impl->shadowGroups)
                if (!group.models.empty() &&
                    !encodeCaster(m_impl->draws[group.representative], &group.models))
                    return false;
    }
    return true;
}

bool MetalR4FloraAdapter::DrawShadowCastersForValidation""")


manifest = {"schema": "scratch-flora-shadow-order-v1", "scope": "draft-only-no-production-edits",
    "independent_patches": True,
    "census": emit("flora-shadow-state-census", census),
    "order": emit("flora-shadow-pso-order", order)}
(OUT / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
for p, content in ORIGINAL.items():
    dest = OUT / "prestate" / p
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(content)
print(json.dumps(manifest, indent=2))
