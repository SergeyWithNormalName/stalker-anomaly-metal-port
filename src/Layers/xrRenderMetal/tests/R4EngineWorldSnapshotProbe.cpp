#include "IR4Backend.h"
#include "MetalR4EngineScene.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4EngineWorldSnapshotProbe --self-test\n");
        return 2;
    }

    auto asset = std::make_shared<R4EngineWorldAsset>();
    asset->sourceName = "dynamic-test";
    R4EngineWorldRenderable renderable;
    renderable.assetName = "dynamic-test";
	renderable.hotness = {0.75f, 0.25f, 0.0f, 0.0f};
    renderable.asset = asset;
    std::vector<R4EngineWorldRenderable> submitted{renderable};

    auto mutableSnapshot = std::make_shared<R4EngineWorldSnapshot>();
    mutableSnapshot->engineFrame = 42;
    mutableSnapshot->renderables = submitted;
    std::shared_ptr<const R4EngineWorldSnapshot> published = mutableSnapshot;
    submitted.clear();
    renderable.asset.reset();
    asset.reset();
    mutableSnapshot.reset();
    if (!published || published->engineFrame != 42 ||
        published->renderables.size() != 1 || !published->renderables.front().asset ||
		published->renderables.front().asset->sourceName != "dynamic-test" ||
		published->renderables.front().hotness != std::array<float, 4>{0.75f, 0.25f, 0.0f, 0.0f})
    {
        std::fprintf(stderr, "engine world snapshot did not retain an immutable owned submission\n");
        return 1;
    }

    MetalR4EngineScene scene;
    if (scene.HasGBufferWorldDrawCallback())
    {
        std::fprintf(stderr, "new engine scene unexpectedly has a world callback\n");
        return 1;
    }
    scene.SetGBufferWorldDrawCallback([](
        IR4Backend&,
        const MetalR4EngineScene::ViewState&,
        const std::shared_ptr<const R4EngineWorldSnapshot>&,
        std::string&) { return true; });
    if (!scene.HasGBufferWorldDrawCallback())
    {
        std::fprintf(stderr, "engine scene did not retain world callback configuration\n");
        return 1;
    }
    scene.Reset();
    if (!scene.HasGBufferWorldDrawCallback())
    {
        std::fprintf(stderr, "engine scene reset discarded world callback configuration\n");
        return 1;
    }
    scene.SetGBufferWorldDrawCallback({});
    if (scene.HasGBufferWorldDrawCallback())
    {
        std::fprintf(stderr, "engine scene did not restore no-op world callback\n");
        return 1;
    }
    return 0;
}
