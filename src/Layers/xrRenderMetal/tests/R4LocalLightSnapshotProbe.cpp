#include "IR4Backend.h"

#include <cstdio>
#include <memory>

int main()
{
    R4EngineLocalLight source;
	source.lightId = 0x12345678u;
    source.type = R4EngineLocalLightType::Spot;
    source.position = {1.0f, 2.0f, 3.0f};
    source.direction = {0.0f, 0.0f, 1.0f};
    source.right = {1.0f, 0.0f, 0.0f};
    source.color = {0.25f, 0.5f, 0.75f, 1.0f};
    source.range = 8.0f;
    source.cone = 1.0f;
    source.specular = 0.625f;
    source.active = true;
    source.staticLight = true;
    source.shadowed = true;
	source.hudMode = true;
	source.omniFace = 4u;
	source.projectorTexture = "lights/custom_flashlight";
    auto snapshot = std::make_shared<R4LightingFrameSnapshot>();
    snapshot->engineFrame = 42u;
    snapshot->ssfxLightSetup1 = {0.35f, 0.5f, 0.0f, 0.0f};
    snapshot->lights.push_back(source);
    source.position[0] = -100.0f;
    source.range = 0.0f; source.specular = 0.0f;
	const std::shared_ptr<const R4LightingFrameSnapshot> immutable = snapshot;
    if (immutable->engineFrame != 42u || immutable->lights.size() != 1u ||
        immutable->lights[0].type != R4EngineLocalLightType::Spot ||
        immutable->lights[0].position[0] != 1.0f || immutable->lights[0].range != 8.0f ||
        immutable->lights[0].specular != 0.625f || immutable->ssfxLightSetup1[0] != 0.35f ||
		immutable->lights[0].lightId != 0x12345678u || !immutable->lights[0].hudMode ||
		immutable->lights[0].omniFace != 4u || immutable->lights[0].projectorTexture != "lights/custom_flashlight" ||
        !immutable->lights[0].active || !immutable->lights[0].staticLight || !immutable->lights[0].shadowed)
    {
        std::fprintf(stderr, "local-light snapshot did not retain a value-only immutable copy\n");
        return 1;
    }
    return 0;
}
