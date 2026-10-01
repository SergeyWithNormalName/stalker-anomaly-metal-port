#!/usr/bin/env python3
"""Independent original-prestate cache and observer drafts; scratch only."""
from pathlib import Path
import difflib
import hashlib
import json

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[3]
REL = "src/Layers/xrRenderMetal/MetalR4EngineScene.mm"
# Root's Flora census may be temporarily applied. Base these independent diffs
# on the recorded original source, not on that disposable observer application.
BASE = (OUT.parent / "flora-shadow-order" / "prestate" / REL).read_text()
assert hashlib.sha256(BASE.encode()).hexdigest() == "c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53"


def once(text, old, new):
    assert text.count(old) == 1, (text.count(old), old[:100])
    return text.replace(old, new, 1)


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


cache = once(BASE,
    '    bool mergeStatic = std::getenv("XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF") == nullptr;\n',
    '''    bool mergeStatic = std::getenv("XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF") == nullptr;
    bool staticPsoReuse = [] {
        const char* flag = std::getenv("XRAY_METAL_LOCAL_STATIC_PSO_REUSE");
        return flag && std::strcmp(flag, "1") == 0;
    }();
''')
cache = once(cache,
    '''    const auto drawStaticTile = [&](std::size_t tile, bool useMerged, bool account) {
        std::size_t lastVertexBuffer = std::numeric_limits<std::size_t>::max();
''', '''    const auto drawStaticTile = [&](std::size_t tile, bool useMerged, bool account) {
        // Other caster families may replace state between tiles or oracle calls.
        std::shared_ptr<IR4GraphicsPipeline> lastPipeline;
        std::size_t lastVertexBuffer = std::numeric_limits<std::size_t>::max();
''')
cache = once(cache,
    '''            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vertexBindings, error))
                return false;
''', '''            if (!shadows.staticPsoReuse || pipeline != lastPipeline)
            {
                if (!backend.BindGraphicsPipeline(pipeline, error))
                    return false;
                if (shadows.staticPsoReuse) lastPipeline = pipeline;
            }
            if (!backend.BindMscResources(vertexBindings, error))
                return false;
''')

observer = once(BASE,
    '    bool mergeStatic = std::getenv("XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF") == nullptr;\n',
    '''    bool mergeStatic = std::getenv("XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF") == nullptr;
    bool staticPsoCensus = [] {
        const char* flag = std::getenv("XRAY_METAL_LOCAL_STATIC_PSO_CENSUS");
        return flag && std::strcmp(flag, "1") == 0;
    }();
''')
observer = once(observer,
    '''            if (account)
            { shadows.submittedCasters += run ? run->rowCount : 1u; ++shadows.mergedEncodedDraws; }
        }
        return true;
    };
''', '''            if (account)
            { shadows.submittedCasters += run ? run->rowCount : 1u; ++shadows.mergedEncodedDraws; }
        }
        // One exact encode frame, after successful real production draws.
        // Inspect the same immutable entries/runs; never wrap backend calls.
        if (shadows.staticPsoCensus && account && view.engineFrame == 360u)
        {
            std::size_t cachedBinds = 0;
            const IR4GraphicsPipeline* previous = nullptr;
            for (std::size_t call = 0; call < drawCount; ++call)
            {
                const auto* run = merge ? &merge->plan.runs[call] : nullptr;
                const auto& entry = materialEntries[tile][run ? run->firstRow : call];
                const bool alphaTested = scene.textureMaterials[entry.first].shaderName == "def_shaders\\\\def_aref";
                const auto* pipeline = (alphaTested ? scene.sunShadowArefPipeline : scene.sunShadowPipeline).get();
                if (pipeline != previous) { ++cachedBinds; previous = pipeline; }
            }
            const auto& record = shadows.records[tile];
            std::fprintf(stderr,
                "METAL_LOCAL_STATIC_PSO_CENSUS frame=%u batch=%u tile=%zu light=%llu face=%u "
                "merged=%d draws=%zu old_pso_binds=%zu cached_pso_binds=%zu\\n",
                view.engineFrame, batchId, tile, static_cast<unsigned long long>(record.lightIdentity),
                record.face, int(bool(merge)), drawCount, drawCount, cachedBinds);
        }
        return true;
    };
''')

manifest = {"schema": "local-static-adjacent-pso-drafts/v1", "base_sha256": sha(BASE),
            "scope": "scratch-only-no-production-application", "independent_patches": True}
for name, content in (("local-static-pso-reuse", cache), ("local-static-pso-census", observer)):
    patch = "".join(difflib.unified_diff(BASE.splitlines(keepends=True), content.splitlines(keepends=True),
        fromfile="a/" + REL, tofile="b/" + REL))
    (OUT / (name + ".patch")).write_text(patch)
    copied = OUT / name / REL
    copied.parent.mkdir(parents=True, exist_ok=True)
    copied.write_text(content)
    manifest[name] = {"patch_sha256": sha(patch), "draft_sha256": sha(content), "files": [REL]}
(OUT / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
prestate = OUT / "prestate" / REL
prestate.parent.mkdir(parents=True, exist_ok=True)
prestate.write_text(BASE)
print(json.dumps(manifest, indent=2))
