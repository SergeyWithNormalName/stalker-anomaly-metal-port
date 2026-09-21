#include "../../xrRender/MetalUICapture.h"

#include <iostream>

int main()
{
    using namespace xray::metal::ui;
    ResetFrameBatch();

    DrawRecord uiDraw;
    uiDraw.shader = {"hud\\default", "ui\\ui_icon_equipment"};
    uiDraw.primitive = PrimitiveType::TriangleList;
    uiDraw.pointType = PointType::TL;
    uiDraw.scissor = {true, 10, 20, 300, 400};
    uiDraw.alphaRef = 127;
    uiDraw.cullMode = 2;
    const std::vector<Vertex> uiVertices = {
        {1.f, 2.f, 0.f, 0xff112233u, 0.f, 0.f},
        {3.f, 4.f, 0.f, 0xff445566u, 1.f, 0.f},
        {5.f, 6.f, 0.f, 0xff778899u, 1.f, 1.f},
    };
    if (!AppendSequentialDraw(uiDraw, uiVertices))
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=ui-append\n";
        return 1;
    }

    DrawRecord fontDraw;
    fontDraw.shader = {"hud\\font", "ui\\font_hud_01"};
    fontDraw.primitive = PrimitiveType::TriangleList;
    fontDraw.pointType = PointType::TL;
    fontDraw.textureCoordinateSpace = TextureCoordinateSpace::AtlasPixels;
    fontDraw.font = true;
    const std::vector<Vertex> fontVertices = {
        {0.f, 16.f, .0001f, 0xffffffffu, 0.f, 16.f},
        {0.f, 0.f, .0001f, 0xffffffffu, 0.f, 0.f},
        {8.f, 16.f, .0001f, 0xffffffffu, 8.f, 16.f},
        {8.f, 0.f, .0001f, 0xffffffffu, 8.f, 0.f},
    };
    if (!AppendDraw(fontDraw, fontVertices, {0u, 1u, 2u, 3u, 2u, 1u}))
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=font-append\n";
        return 1;
    }

    const FrameBatch batch = DrainFrameBatch();
    if (batch.draws.size() != 2u || batch.vertices.size() != 7u || batch.indices.size() != 9u ||
        batch.draws[0].shader.texture != "ui\\ui_icon_equipment" || !batch.draws[0].scissor.enabled ||
        batch.draws[0].firstVertex != 0u || batch.draws[0].firstIndex != 0u ||
        batch.draws[1].firstVertex != 3u || batch.draws[1].firstIndex != 3u || !batch.draws[1].font ||
        batch.draws[1].textureCoordinateSpace != TextureCoordinateSpace::AtlasPixels ||
        batch.indices[3] != 3u || batch.indices[4] != 4u || batch.indices[5] != 5u ||
        batch.indices[6] != 6u || batch.indices[7] != 5u || batch.indices[8] != 4u)
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=drain\n";
        return 1;
    }

    if (!DrainFrameBatch().draws.empty())
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=drain-reset\n";
        return 1;
    }

    RecordDroppedDraw(5u);
    const FrameBatch rejectedBatch = DrainFrameBatch();
    if (rejectedBatch.droppedDraws != 1u || rejectedBatch.droppedVertices != 5u)
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=explicit-reject\n";
        return 1;
    }

    FrameCapture limitedCapture(2u, 6u, 1u);
    if (limitedCapture.append(uiDraw, uiVertices, {0u, 1u, 2u}))
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=limit-admission\n";
        return 1;
    }
    const FrameBatch limitedBatch = limitedCapture.drain();
    if (!limitedBatch.draws.empty() || !limitedBatch.vertices.empty() || !limitedBatch.indices.empty() ||
        limitedBatch.droppedDraws != 1u || limitedBatch.droppedVertices != 3u)
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=limit-accounting\n";
        return 1;
    }

    FrameCapture invalidIndexCapture;
    if (invalidIndexCapture.append(uiDraw, uiVertices, {3u}))
    {
        std::cerr << "METAL_UI_CAPTURE_SELF_TEST result=FAIL stage=index-validation\n";
        return 1;
    }

    std::cout << "METAL_UI_CAPTURE_SELF_TEST result=PASS draws=2 vertices=7 indices=9 bounded=true\n";
    return 0;
}
