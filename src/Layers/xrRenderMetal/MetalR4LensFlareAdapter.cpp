#include "MetalR4LensFlareAdapter.h"
#include "IR4Backend.h"
#include <filesystem>
struct MetalR4LensFlareAdapter::Impl { std::shared_ptr<IR4GraphicsPipeline> pipeline; };
bool MetalR4LensFlareAdapter::Configure(IR4Backend& backend, const Options& o, std::string& e) {
 impl=std::make_unique<Impl>(); ready=false;
 if (o.shaderDirectory.empty() || !std::filesystem::is_regular_file(o.shaderDirectory/"effects_sun.vs.metallib") || !std::filesystem::is_regular_file(o.shaderDirectory/"effects_sun.ps.metallib")) { e="effects_sun MSC bundle missing"; return true; }
 R4GraphicsPipelineDesc d; d.label="R4 exact effects_sun lens flare"; d.shaderKey="r4-effects-sun-flare-msc-v1";
 const auto vs=(o.shaderDirectory/"effects_sun.vs.metallib").string(), ps=(o.shaderDirectory/"effects_sun.ps.metallib").string(); d.vertexMetallibPath=vs.c_str(); d.fragmentMetallibPath=ps.c_str();
 d.vertexAttributes={{11,6,0,R4VertexFormat::Float4},{12,6,16,R4VertexFormat::Float2},{13,6,24,R4VertexFormat::UChar4Normalized}}; d.vertexLayouts={{6,28}}; d.colorFormats={R4PixelFormat::RGBA8Unorm}; d.depthFormat=R4PixelFormat::Depth32FloatStencil8; d.depthCompare=R4CompareFunction::LessEqual; d.depthWrite=false; d.cullMode=R4CullMode::None; d.blend.enabled=true; d.blend.sourceRgb=d.blend.sourceAlpha=R4BlendFactor::SourceAlpha; d.blend.destinationRgb=d.blend.destinationAlpha=R4BlendFactor::One;
 impl->pipeline=backend.CreateGraphicsPipeline(d,e); ready=static_cast<bool>(impl->pipeline); return true;
}
bool MetalR4LensFlareAdapter::StageFrame(IR4Resources&, xray::metal::lensflare::Frame, std::string&) { return true; }
bool MetalR4LensFlareAdapter::DrawIntoActiveForward(IR4Backend&, const std::shared_ptr<IR4Buffer>&, std::string&) { return true; }
