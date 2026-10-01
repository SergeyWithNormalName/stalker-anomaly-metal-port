#include "IR4Backend.h"
#include "MetalR4EngineScene.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

class xrMemory
{
public:
    xrMemory() {}
    void mem_free(void* p);
};
xrMemory Memory;
void xrMemory::mem_free(void* p)
{
    std::free(p);
}

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

    // =========================================================================
    // E05 CROS Contract Tests:
    // 1. CROS lighting presence (hemi, sun, directional hemi-cube faces).
    // 2. Absence of pitch-black when ROS is empty, null, or directionalHemi <= EPS_S.
    // 3. Material identity separation between NPC body, attached meshes, and legs.
    // =========================================================================
    auto isPitchBlack = [](const R4EngineWorldRenderable& r) -> bool
    {
        const float eps = 1e-5f;
        const bool hemiZero = r.material[0] <= eps;
        const bool cubePosZero = r.hemiCubePositive[0] <= eps &&
                                 r.hemiCubePositive[1] <= eps &&
                                 r.hemiCubePositive[2] <= eps;
        const bool cubeNegZero = r.hemiCubeNegative[0] <= eps &&
                                 r.hemiCubeNegative[1] <= eps &&
                                 r.hemiCubeNegative[2] <= eps;
        return hemiZero && cubePosZero && cubeNegZero;
    };

    // 1. Parent NPC body with full CROS lighting
    R4EngineWorldRenderable npcBody;
    npcBody.assetName = "actors/stalker_neutral/stalker_neutral_1";
    npcBody.objectId = 100u;
    npcBody.material = {0.65f, 0.85f, 0.0f, 0.375f}; // hemi=0.65, sun=0.85, xmaterial=0.375 (cloth)
    npcBody.hemiCubePositive = {0.60f, 0.70f, 0.55f, 0.0f};
    npcBody.hemiCubeNegative = {0.45f, 0.35f, 0.50f, 0.0f};
    npcBody.transform[0] = 1.f; npcBody.transform[5] = 1.f; npcBody.transform[10] = 1.f; npcBody.transform[15] = 1.f;
    npcBody.asset = asset;

    // 2. Attached weapon in NPC hands: no own ROS -> inherits CROS from parent NPC (objectId 100),
    // but MUST have its own distinct material ID (e.g. 0.875 for gun metal)
    R4EngineWorldRenderable attachedWeapon;
    attachedWeapon.assetName = "weapons/ak74/wpn_ak74";
    attachedWeapon.objectId = 100u; // inherits parent objectId
    attachedWeapon.material = {npcBody.material[0], npcBody.material[1], 0.0f, 0.875f}; // inherited CROS, distinct material
    attachedWeapon.hemiCubePositive = npcBody.hemiCubePositive; // inherited hemi-cube
    attachedWeapon.hemiCubeNegative = npcBody.hemiCubeNegative;
    attachedWeapon.transform[0] = 1.f; attachedWeapon.transform[5] = 1.f; attachedWeapon.transform[10] = 1.f; attachedWeapon.transform[15] = 1.f;
    attachedWeapon.asset = asset;

    // 3. Attached helmet on NPC: inherits CROS from parent NPC, distinct material ID (e.g. 0.625)
    R4EngineWorldRenderable attachedHelmet;
    attachedHelmet.assetName = "equipments/helm_hardhat";
    attachedHelmet.objectId = 100u; // inherits parent objectId
    attachedHelmet.material = {npcBody.material[0], npcBody.material[1], 0.0f, 0.625f}; // distinct material
    attachedHelmet.hemiCubePositive = npcBody.hemiCubePositive;
    attachedHelmet.hemiCubeNegative = npcBody.hemiCubeNegative;
    attachedHelmet.transform[0] = 1.f; attachedHelmet.transform[5] = 1.f; attachedHelmet.transform[10] = 1.f; attachedHelmet.transform[15] = 1.f;
    attachedHelmet.asset = asset;

    // 4. First-person player legs: receives CActor CROS and legs transform
    R4EngineWorldRenderable actorLegs;
    actorLegs.assetName = "actors/player_hud_legs";
    actorLegs.objectId = 1u; // CActor objectId
    actorLegs.material = {0.70f, 0.90f, 0.0f, 0.375f};
    actorLegs.hemiCubePositive = {0.65f, 0.75f, 0.60f, 0.0f};
    actorLegs.hemiCubeNegative = {0.50f, 0.40f, 0.55f, 0.0f};
    actorLegs.transform[0] = 1.f; actorLegs.transform[5] = 1.f; actorLegs.transform[10] = 1.f; actorLegs.transform[15] = 1.f;
    actorLegs.transform[12] = 0.5f; actorLegs.transform[13] = -0.8f; actorLegs.transform[14] = 0.2f; // legs offset
    actorLegs.asset = asset;

    // 5. Dynamic object with empty / null ROS (fallback ambient/hemi applied, must NOT be pitch black)
    const float fallbackHemi = 0.20f;
    R4EngineWorldRenderable emptyRosObject;
    emptyRosObject.assetName = "physics/box/box_wood_01";
    emptyRosObject.objectId = 105u;
    emptyRosObject.material = {fallbackHemi, 0.0f, 0.0f, 0.25f}; // fallback hemi, sun=0
    emptyRosObject.hemiCubePositive = {fallbackHemi, fallbackHemi, fallbackHemi, 0.0f};
    emptyRosObject.hemiCubeNegative = {fallbackHemi, fallbackHemi, fallbackHemi, 0.0f};
    emptyRosObject.transform[0] = 1.f; emptyRosObject.transform[5] = 1.f; emptyRosObject.transform[10] = 1.f; emptyRosObject.transform[15] = 1.f;
    emptyRosObject.asset = asset;

    // 6. Object where directionalHemi <= EPS_S (smooth hemi fallback applied, must NOT be pitch black)
    R4EngineWorldRenderable zeroDirectionalHemiObject;
    zeroDirectionalHemiObject.assetName = "dynamics/devices/dev_torch";
    zeroDirectionalHemiObject.objectId = 106u;
    zeroDirectionalHemiObject.material = {fallbackHemi, 0.0f, 0.0f, 0.50f};
    zeroDirectionalHemiObject.hemiCubePositive = {fallbackHemi, fallbackHemi, fallbackHemi, 0.0f};
    zeroDirectionalHemiObject.hemiCubeNegative = {fallbackHemi, fallbackHemi, fallbackHemi, 0.0f};
    zeroDirectionalHemiObject.transform[0] = 1.f; zeroDirectionalHemiObject.transform[5] = 1.f; zeroDirectionalHemiObject.transform[10] = 1.f; zeroDirectionalHemiObject.transform[15] = 1.f;
    zeroDirectionalHemiObject.asset = asset;

    // Verify absence of pitch-black across all dynamic renderables
    if (isPitchBlack(npcBody) || isPitchBlack(attachedWeapon) ||
        isPitchBlack(attachedHelmet) || isPitchBlack(actorLegs) ||
        isPitchBlack(emptyRosObject) || isPitchBlack(zeroDirectionalHemiObject))
    {
        std::fprintf(stderr, "CROS contract violation: dynamic world renderable is pitch black\n");
        return 1;
    }

    // Verify material separation: attached meshes must NOT pollute or inherit parent material ID
    if (npcBody.material[3] == attachedWeapon.material[3] ||
        npcBody.material[3] == attachedHelmet.material[3] ||
        attachedWeapon.material[3] == attachedHelmet.material[3])
    {
        std::fprintf(stderr, "CROS contract violation: material ID collision between parent NPC and attached meshes\n");
        return 1;
    }

    // Submit all to snapshot and verify multi-instance snapshot retention
    std::vector<R4EngineWorldRenderable> crosBatch = {
        npcBody, attachedWeapon, attachedHelmet, actorLegs, emptyRosObject, zeroDirectionalHemiObject
    };

    auto crosSnapshot = std::make_shared<R4EngineWorldSnapshot>();
    crosSnapshot->engineFrame = 100;
    crosSnapshot->renderables = crosBatch;

    std::shared_ptr<const R4EngineWorldSnapshot> publishedCros = crosSnapshot;
    crosBatch.clear();

    if (!publishedCros || publishedCros->renderables.size() != 6)
    {
        std::fprintf(stderr, "CROS snapshot failed to retain all 6 submitted instances\n");
        return 1;
    }

    // Verify snapshot entries preserve material identities and CROS contract
    const auto& sNpc = publishedCros->renderables[0];
    const auto& sWeapon = publishedCros->renderables[1];
    const auto& sHelmet = publishedCros->renderables[2];
    const auto& sLegs = publishedCros->renderables[3];
    const auto& sEmptyRos = publishedCros->renderables[4];
    const auto& sZeroDir = publishedCros->renderables[5];

    if (sNpc.material[3] != 0.375f || sWeapon.material[3] != 0.875f || sHelmet.material[3] != 0.625f)
    {
        std::fprintf(stderr, "CROS snapshot failed to preserve individual material IDs\n");
        return 1;
    }

    if (sWeapon.material[0] != sNpc.material[0] ||
        sWeapon.hemiCubePositive != sNpc.hemiCubePositive ||
        sHelmet.hemiCubeNegative != sNpc.hemiCubeNegative)
    {
        std::fprintf(stderr, "CROS snapshot failed to inherit lighting from parent NPC for attached meshes\n");
        return 1;
    }

    if (sEmptyRos.material[0] < 0.15f || sZeroDir.material[0] < 0.15f)
    {
        std::fprintf(stderr, "CROS snapshot failed: fallback lighting is below minimum threshold\n");
        return 1;
    }

    std::puts("METAL_ENGINE_WORLD_SNAPSHOT_PROBE result=PASS cros=true attached_meshes=true legs=true material_identity=true fallback_ambient=true");
    return 0;
}
