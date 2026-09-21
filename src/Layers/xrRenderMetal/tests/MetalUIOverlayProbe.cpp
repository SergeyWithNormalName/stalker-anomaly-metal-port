#include "../MetalUIOverlayCommon.h"

#include <iostream>

int main()
{
    using namespace xray::metal::ui;
    using namespace xray::metal::ui::overlay;
    std::filesystem::path path;
    std::string error;
    if (!BuildXrfsTexturePath("ui/ui_icon_equipment", path, error) ||
        path.generic_string() != "xrfs:$game_textures$@ui\\ui_icon_equipment.dds")
    {
        std::cerr << "METAL_UI_OVERLAY_SELF_TEST result=FAIL stage=xrfs-path error=" << error << '\n';
        return 1;
    }
    if (BuildXrfsTexturePath("../secrets", path, error) || BuildXrfsTexturePath("ui/icon.png", path, error))
    {
        std::cerr << "METAL_UI_OVERLAY_SELF_TEST result=FAIL stage=xrfs-reject\n";
        return 1;
    }

    ScissorRect rect;
    if (!ResolveScissor({true, -4, 20, 640, 999}, 320, 200, rect) ||
        rect.x != 0u || rect.y != 20u || rect.width != 320u || rect.height != 180u ||
        ResolveScissor({true, 100, 100, 99, 110}, 320, 200, rect))
    {
        std::cerr << "METAL_UI_OVERLAY_SELF_TEST result=FAIL stage=scissor\n";
        return 1;
    }
    std::cout << "METAL_UI_OVERLAY_SELF_TEST result=PASS xrfs=true scissor=true traversal_rejected=true\n";
    return 0;
}
