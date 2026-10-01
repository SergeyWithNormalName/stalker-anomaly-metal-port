#include "IR4Backend.h"
#include "MetalR4LocalLightAdapter.h"
#include "R4LightMarker.h"
#include "R4LocalLightFoundation.h"
#include "R4LocalShadowAtlasFoundation.h"
#include "R4LocalShadowBatchPlan.h"
#include "R4TemporalTextureValidation.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

void InitializeR4RolloverProbeRuntime();
void ShutdownR4RolloverProbeRuntime();

namespace
{
namespace fs = std::filesystem;
constexpr std::uint32_t Width = 64u;
constexpr std::uint32_t Height = 32u;
constexpr std::uint32_t AtlasExtent = 512u;
constexpr std::uint32_t AtlasTile = AtlasExtent / 4u;
constexpr std::size_t PixelCount = std::size_t(Width) * Height;

struct Matrix4 { float e[16]{}; };
struct Vec3 { float x, y, z; };
constexpr std::array<Vec3,6> CubeDirections={{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
constexpr std::array<Vec3,6> CubeNormals={{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}}};

Matrix4 multiply(const Matrix4& a, const Matrix4& b)
{
    Matrix4 result;
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row)
            for (std::size_t inner = 0; inner < 4u; ++inner)
                result.e[column * 4u + row] += a.e[inner * 4u + row] * b.e[column * 4u + inner];
    return result;
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 normalize(Vec3 value)
{
    const float length = std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
    if (!(length > 1.0e-8f) || !std::isfinite(length)) return {0,0,1};
    return {value.x/length,value.y/length,value.z/length};
}

std::shared_ptr<IR4Texture> makeTexture(IR4Resources& resources, std::uint32_t width,
    std::uint32_t height, R4PixelFormat format, R4TextureUsage usage,
    const char* label, std::string& error)
{
    R4Texture2DDesc descriptor;
    descriptor.width=width; descriptor.height=height; descriptor.format=format;
    descriptor.storage=usage==R4TextureUsage::ShaderRead?R4ResourceStorage::Shared:R4ResourceStorage::Private; descriptor.usage=usage; descriptor.label=label;
    return resources.CreateTexture2D(descriptor,error);
}

std::shared_ptr<IR4Buffer> makeBuffer(IR4Resources& resources, const void* bytes,
    std::size_t size, R4BufferUsage usage, const char* label, std::string& error)
{
    R4BufferDesc descriptor;
    descriptor.size=size; descriptor.storage=R4ResourceStorage::Shared;
    descriptor.usage=usage; descriptor.label=label;
    return resources.CreateBuffer(descriptor,bytes,size,error);
}

std::shared_ptr<IR4Sampler> makeSampler(IR4Resources& resources, R4SamplerFilter filter,
    const char* label, std::string& error,
    R4SamplerCompareFunction compare=R4SamplerCompareFunction::Never)
{
    R4SamplerDesc descriptor;
    descriptor.minFilter=descriptor.magFilter=filter;
    descriptor.mipFilter=R4SamplerFilter::Nearest;
    descriptor.addressU=descriptor.addressV=descriptor.addressW=R4SamplerAddressMode::ClampToEdge;
    descriptor.compareFunction=compare;
    descriptor.label=label;
    return resources.CreateSampler(descriptor,error);
}

Matrix4 lightView(const R4EngineLocalLight& light)
{
    const Vec3 direction=normalize({light.direction[0],light.direction[1],light.direction[2]});
    Vec3 right=normalize({light.right[0],light.right[1],light.right[2]});
    Vec3 up=normalize(cross(direction,right));
    right=normalize(cross(up,direction));
    const Vec3 position{light.position[0],light.position[1],light.position[2]};
    return {{
        right.x,up.x,direction.x,0, right.y,up.y,direction.y,0,
        right.z,up.z,direction.z,0,
        -(position.x*right.x+position.y*right.y+position.z*right.z),
        -(position.x*up.x+position.y*up.y+position.z*up.z),
        -(position.x*direction.x+position.y*direction.y+position.z*direction.z),1,
    }};
}

Matrix4 lightProjection(const R4EngineLocalLight& light)
{
    const float nearPlane=light.virtualSize;
    const float farPlane=light.range+1.0e-5f;
    const float shift=light.type==R4EngineLocalLightType::OmniFace ? 0.3f : 0.0610865f;
    const float cotangent=1.0f/std::tan((light.cone+shift)*0.5f);
    const float q=farPlane/(farPlane-nearPlane);
    Matrix4 result{};
    result.e[0]=cotangent; result.e[5]=cotangent; result.e[10]=q;
    result.e[11]=1.0f; result.e[14]=-q*nearPlane;
    return result;
}

Matrix4 perspectiveProjection()
{
    constexpr float nearPlane=.05f, farPlane=10.0f;
    const float q=farPlane/(farPlane-nearPlane);
    return {{1,0,0,0, 0,1,0,0, 0,0,q,1, 0,0,-q*nearPlane,0}};
}

Matrix4 cameraView(float eyeX)
{
    Matrix4 result{{1,0,0,0, 0,1,0,0, 0,0,1,0, -eyeX,0,0,1}};
    return result;
}
Matrix4 inverseRigidView(const Matrix4& view)
{
    Matrix4 inverse;
    for (std::size_t column=0;column<3u;++column)
        for (std::size_t row=0;row<3u;++row)
            inverse.e[column*4u+row]=view.e[row*4u+column];
    const float tx=view.e[12],ty=view.e[13],tz=view.e[14];
    inverse.e[12]=-(inverse.e[0]*tx+inverse.e[4]*ty+inverse.e[8]*tz);
    inverse.e[13]=-(inverse.e[1]*tx+inverse.e[5]*ty+inverse.e[9]*tz);
    inverse.e[14]=-(inverse.e[2]*tx+inverse.e[6]*ty+inverse.e[10]*tz);
    inverse.e[15]=1.0f;
    return inverse;
}

std::vector<std::size_t> makeIndices(std::size_t begin, std::size_t count)
{
    std::vector<std::size_t> result(count);
    for (std::size_t i=0;i<count;++i) result[i]=begin+i;
    return result;
}

bool makeBatches(const R4EngineLocalLightSnapshot& snapshot,
    std::vector<std::vector<std::size_t>>& batches, std::string& error)
{
    std::vector<xray::metal::R4LocalShadowBatchCandidate> candidates;
    candidates.reserve(snapshot.lights.size());
    for (std::size_t index=0;index<snapshot.lights.size();++index)
    {
        const auto& light=snapshot.lights[index];
        candidates.push_back({index,light.lightId,light.parentLightId,light.omniFace,
            light.type==R4EngineLocalLightType::OmniFace});
    }
    return xray::metal::PlanR4LocalShadowBatches(candidates,batches,error);
}

std::shared_ptr<R4EngineLocalLightSnapshot> makeSnapshot(std::size_t lightCount,
    std::uint32_t frame, bool shadowed)
{
    auto snapshot=std::make_shared<R4EngineLocalLightSnapshot>();
    snapshot->engineFrame=frame;
    snapshot->shadowDepthScale=1.0f; snapshot->shadowDepthBias=0.0f;
    snapshot->ssfxLightSetup1={1.0f,0.0f,0.0f,0.0f};
    for (std::size_t index=0;index<lightCount;++index)
    {
        R4EngineLocalLight light;
        light.active=true; light.shadowed=shadowed; light.staticLight=true;
        light.position=index<6u?std::array<float,3>{-0.5f,0.0f,0.5f}:
            std::array<float,3>{0.0f,0.0f,0.05f};
        light.range=1.2f; light.virtualSize=0.05f;
        light.color={0.04f,0.03f,0.02f,1.0f}; light.specular=0.01f;
        if (index<6u)
        {
            const Vec3 direction=CubeDirections[index];
            const Vec3 right=cross(CubeNormals[index],direction);
            light.type=R4EngineLocalLightType::OmniFace;
            light.direction={direction.x,direction.y,direction.z};
            light.right={right.x,right.y,right.z};
            light.cone=1.570796327f;
            light.omniFace=static_cast<std::uint8_t>(index);
            light.parentLightId=0x451u;
            light.lightId=(light.parentLightId<<3u)|std::uint64_t(index+1u);
        }
        else
        {
            light.type=R4EngineLocalLightType::Spot;
            light.direction={0.0f,0.0f,1.0f}; light.right={1.0f,0.0f,0.0f};
            light.cone=1.2f; light.omniFace=0xffu;
            light.lightId=0x5000u+index;
            light.parentLightId=light.lightId;
        }
        snapshot->lights.emplace_back(std::move(light));
    }
    return snapshot;
}

std::shared_ptr<R4EngineLocalLightSnapshot> makeCanonicalPointFaceCase(
    std::uint32_t frame, std::size_t activeFace, bool shadowed)
{
    auto snapshot=makeSnapshot(6u,frame,shadowed);
    for (std::size_t face=0;face<6u;++face)
    {
        auto& light=snapshot->lights[face];
        const Vec3 direction=CubeDirections[face];
        const Vec3 right=cross(CubeNormals[face],direction);
        const float receiverX=0.5f*(float(Width/4u)*2.0f/float(Width)-1.0f);
        const float receiverY=0.5f*(float(Height/2u)*2.0f/float(Height)-1.0f);
        const Vec3 sourceOffset=CubeDirections[activeFace];
        light.position={receiverX-sourceOffset.x*0.3f,receiverY-sourceOffset.y*0.3f,
            0.5f-sourceOffset.z*0.3f};
        light.direction={direction.x,direction.y,direction.z};
        light.right={right.x,right.y,right.z};
        light.parentLightId=0x600u+activeFace;
        light.lightId=(light.parentLightId<<3u)|std::uint64_t(face+1u);
    }
    return snapshot;
}

bool makeShadowRecord(const R4EngineLocalLight& light, std::uint32_t frame,
    std::size_t tile, std::uint32_t batchId, const std::array<float,16>& viewArray,
    xray::metal::R4LocalShadowAtlasRecord& record,
    std::string& error)
{
    record={};
    record.lightIdentity=light.lightId; record.engineFrame=frame; record.smapId=batchId;
    record.atlasX=static_cast<std::uint32_t>(tile%4u)*AtlasTile;
    record.atlasY=static_cast<std::uint32_t>(tile/4u)*AtlasTile;
    record.atlasSize=AtlasTile; record.atlasExtent=AtlasExtent;
    record.face=light.type==R4EngineLocalLightType::OmniFace ? light.omniFace : 0u;
    record.renderedDepth=true;
    Matrix4 camera{}; std::memcpy(camera.e,viewArray.data(),sizeof(camera.e));
    const Matrix4 lv=lightView(light), lp=lightProjection(light), iv=inverseRigidView(camera);
    std::memcpy(record.lightView.data(),lv.e,sizeof(lv.e));
    std::memcpy(record.lightProjection.data(),lp.e,sizeof(lp.e));
    std::memcpy(record.deviceInverseView.data(),iv.e,sizeof(iv.e));
    if (!xray::metal::DeriveR4LocalShadowMatrix(record,1.0f,0.0f,record.shadowMatrix,error) ||
        !xray::metal::DeriveR4LocalLightMapMatrix(record,1.0f,0.0f,record.lightMapMatrix,error))
        return false;
    return xray::metal::ValidateR4LocalShadowAtlasRecord(record,frame,error);
}

bool makeGeometryPipelines(IR4Backend& backend, std::shared_ptr<IR4GraphicsPipeline>& geometry,
    std::shared_ptr<IR4GraphicsPipeline>& delta, std::string& error)
{
    static const char* geometryMsl=R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 r4_batch_geometry_vertex(uint i [[vertex_id]]) {
  const float4 p[4]={float4(-1,-1,.904522613,1),float4(0,-1,.904522613,1),
                     float4(0,1,.904522613,1),float4(-1,1,.904522613,1)};
  return p[i];
}
fragment float4 r4_batch_geometry_fragment() { return float4(0); }
)";
    R4GraphicsPipelineDesc geometryDesc;
    geometryDesc.label="E08 atlas batch receiver depth and bit0";
    geometryDesc.shaderKey="e05-local-atlas-batch-geometry-v1";
    geometryDesc.mslSource=geometryMsl;
    geometryDesc.vertexFunction="r4_batch_geometry_vertex";
    geometryDesc.fragmentFunction="r4_batch_geometry_fragment";
    geometryDesc.colorFormats={R4PixelFormat::RGBA16Float};
    geometryDesc.colorWriteMasks={0u};
    geometryDesc.depthFormat=R4PixelFormat::Depth32FloatStencil8;
    geometryDesc.depthCompare=R4CompareFunction::Always;
    geometryDesc.depthWrite=true;
    geometryDesc.cullMode=R4CullMode::None;
    geometryDesc.stencil.enabled=true;
    geometryDesc.stencil.frontFace.compare=R4CompareFunction::Always;
    geometryDesc.stencil.frontFace.readMask=0u;
    geometryDesc.stencil.frontFace.writeMask=1u;
    geometryDesc.stencil.frontFace.depthStencilPass=R4StencilOperation::Replace;
    geometryDesc.stencil.backFace=geometryDesc.stencil.frontFace;
    geometry=backend.CreateGraphicsPipeline(geometryDesc,error);
    if (!geometry) return false;

    static const char* deltaMsl=R"(
#include <metal_stdlib>
using namespace metal;
struct R4DeltaOut { float4 position [[position]]; float2 uv; };
vertex R4DeltaOut r4_batch_delta_vertex(uint i [[vertex_id]]) {
  const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
  const float2 uv[3]={float2(0,1),float2(2,1),float2(0,-1)};
  R4DeltaOut o; o.position=float4(p[i],0,1); o.uv=uv[i]; return o;
}
fragment float4 r4_batch_delta_fragment(R4DeltaOut in [[stage_in]],
  texture2d<float> shadowed [[texture(0)]], texture2d<float> unshadowed [[texture(1)]],
  sampler linearSampler [[sampler(0)]]) {
  float3 d=abs(shadowed.sample(linearSampler,in.uv).rgb-unshadowed.sample(linearSampler,in.uv).rgb);
  return float4(d,1);
}
)";
    R4GraphicsPipelineDesc deltaDesc;
    deltaDesc.label="E08 atlas batch shadow contribution diagnostic";
    deltaDesc.shaderKey="e05-local-atlas-batch-delta-v1";
    deltaDesc.mslSource=deltaMsl;
    deltaDesc.vertexFunction="r4_batch_delta_vertex";
    deltaDesc.fragmentFunction="r4_batch_delta_fragment";
    deltaDesc.colorFormats={R4PixelFormat::RGBA16Float};
    deltaDesc.depthFormat=R4PixelFormat::Invalid;
    deltaDesc.depthCompare=R4CompareFunction::Always;
    deltaDesc.depthWrite=false;
    deltaDesc.cullMode=R4CullMode::None;
    delta=backend.CreateGraphicsPipeline(deltaDesc,error);
    return static_cast<bool>(delta);
}

bool encodeShadowTileDraw(IR4Backend& backend, const std::shared_ptr<IR4Buffer>& indices,
    const R4EngineLocalLight& light,
    const xray::metal::R4LocalShadowAtlasRecord& record, std::string& error)
{
    const Vec3 direction=normalize({light.direction[0],light.direction[1],light.direction[2]});
    Vec3 right=normalize({light.right[0],light.right[1],light.right[2]});
    const Vec3 up=normalize(cross(direction,right));
    right=normalize(cross(up,direction));
    const Vec3 center{light.position[0]+direction.x*0.12f,
        light.position[1]+direction.y*0.12f,light.position[2]+direction.z*0.12f};
    const float half=0.12f*std::tan(light.cone*0.5f)*0.55f;
    const std::array<Vec3,4> corners={{
        {center.x-right.x*half-up.x*half,center.y-right.y*half-up.y*half,center.z-right.z*half-up.z*half},
        {center.x-right.x*half+up.x*half,center.y-right.y*half+up.y*half,center.z-right.z*half+up.z*half},
        {center.x+right.x*half+up.x*half,center.y+right.y*half+up.y*half,center.z+right.z*half+up.z*half},
        {center.x+right.x*half-up.x*half,center.y+right.y*half-up.y*half,center.z+right.z*half-up.z*half},
    }};
    std::array<float,12> vertices{};
    for (std::size_t i=0;i<corners.size();++i)
    {
        vertices[i*3u]=corners[i].x; vertices[i*3u+1u]=corners[i].y; vertices[i*3u+2u]=corners[i].z;
    }
    const Matrix4 vp=multiply(lightProjection(light),lightView(light));
    std::array<std::uint8_t,256> transform{};
    std::memcpy(transform.data(),vp.e,sizeof(vp.e));
    R4FrameBufferSlice vertexSlice, transformSlice;
    if (!backend.UploadFrameData(vertices.data(),sizeof(vertices),16u,vertexSlice,error) ||
        !backend.UploadFrameData(transform.data(),transform.size(),256u,transformSlice,error))
        return false;

    if (!backend.BindVertexBuffer(6u,vertexSlice.buffer,vertexSlice.offset,error)) return false;
    R4MscStageBindings bindings;
    const char* largeTlabFlag=std::getenv("XRAY_METAL_E08_TEST_LARGE_TLAB");
    const bool testLargeTlab=largeTlabFlag&&std::strcmp(largeTlabFlag,"1")==0;
    bindings.stage=R4ShaderStage::Vertex;
    // MSC descriptor entries are 24 bytes; 17 entries exercise the backend heap fallback.
    // Only entry zero is used by this shader, so the additional zeroed tail is unreferenced.
    bindings.tlabSize=testLargeTlab?17u*24u:24u;
    bindings.resources={{R4MscResourceType::ConstantBuffer,0u,transformSlice.buffer,
        transformSlice.offset,transform.size(),nullptr,nullptr}};
    R4IndexedDrawDesc draw;
    draw.indexBuffer=indices; draw.indexCount=6u; draw.indexType=R4IndexType::UInt16;
    return backend.BindMscResources(bindings,error) && backend.DrawIndexed(draw,error);
}

R4Viewport tileViewport(const xray::metal::R4LocalShadowAtlasRecord& record)
{
    return {double(record.atlasX),double(record.atlasY),double(record.atlasSize),
        double(record.atlasSize),0.0,1.0};
}

R4ScissorRect tileScissor(const xray::metal::R4LocalShadowAtlasRecord& record)
{
    return {record.atlasX,record.atlasY,record.atlasSize,record.atlasSize};
}

bool beginShadowAtlasPass(IR4Backend& backend,
    const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    const std::shared_ptr<IR4Texture>& atlas, const std::shared_ptr<IR4Texture>& depth,
    const xray::metal::R4LocalShadowAtlasRecord& firstRecord, bool clearAtlas,
    std::string& error)
{
    R4RenderPassDesc pass;
    pass.label="E08 shadow atlas tile batch";
    pass.useDrawable=false;
    R4ColorAttachmentDesc color;
    color.texture=atlas; color.loadAction=clearAtlas?R4LoadAction::Clear:R4LoadAction::Load;
    color.storeAction=R4StoreAction::Store; color.clearColor={1,1,1,1};
    pass.colorAttachments={color};
    pass.depthStencilAttachment.texture=depth;
    pass.depthStencilAttachment.depthLoadAction=R4LoadAction::Clear;
    pass.depthStencilAttachment.depthStoreAction=R4StoreAction::Store;
    pass.hasViewport=true; pass.viewport=tileViewport(firstRecord);
    pass.hasScissor=true; pass.scissor=tileScissor(firstRecord);
    return backend.BeginRenderPass(pass,error) && backend.BindGraphicsPipeline(pipeline,error);
}

bool encodeShadowTile(IR4Backend& backend, const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    const std::shared_ptr<IR4Texture>& atlas, const std::shared_ptr<IR4Texture>& depth,
    const std::shared_ptr<IR4Buffer>& indices, const R4EngineLocalLight& light,
    const xray::metal::R4LocalShadowAtlasRecord& record, bool clearAtlas,
    std::string& error)
{
    if (!beginShadowAtlasPass(backend,pipeline,atlas,depth,record,clearAtlas,error) ||
        !encodeShadowTileDraw(backend,indices,light,record,error)) return false;
    return backend.EndRenderPass(error);
}

bool encodeGroupedShadowTiles(IR4Backend& backend,
    const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    const std::shared_ptr<IR4Texture>& atlas, const std::shared_ptr<IR4Texture>& depth,
    const std::shared_ptr<IR4Buffer>& indices,
    const std::vector<const R4EngineLocalLight*>& lights,
    const std::vector<xray::metal::R4LocalShadowAtlasRecord>& records,
    std::string& error)
{
    if (lights.empty() || lights.size()!=records.size())
    { error="grouped shadow tile/light records do not match"; return false; }
    if (!beginShadowAtlasPass(backend,pipeline,atlas,depth,records.front(),true,error)) return false;
    for (std::size_t tile=0;tile<lights.size();++tile)
    {
        if (tile!=0u && !backend.SetActiveRenderRegion(tileViewport(records[tile]),
                tileScissor(records[tile]),error)) return false;
        if (!encodeShadowTileDraw(backend,indices,*lights[tile],records[tile],error)) return false;
    }
    return backend.EndRenderPass(error);
}

struct ShadowAtlasPathStats
{
    std::size_t passes = 0u;
    std::size_t regionChanges = 0u;
};

enum class ShadowAtlasRenderPath
{
    GroupedBatch,
    LegacyMultiTileBatch,
    SerialPerLight,
};

bool beginReceiverSurface(IR4Backend& backend, const std::shared_ptr<IR4Texture>& color,
    const std::shared_ptr<IR4Texture>& depthStencil,
    const std::shared_ptr<IR4GraphicsPipeline>& geometry,
    const std::shared_ptr<IR4Buffer>& quadIndices, std::string& error)
{
    R4RenderPassDesc pass;
    pass.label="E08 offscreen planar receiver and preserved geometry stencil";
    pass.useDrawable=false;
    pass.colorAttachments.push_back({color,R4LoadAction::Clear,R4StoreAction::Store,{0,0,0,0}});
    pass.depthStencilAttachment.texture=depthStencil;
    pass.depthStencilAttachment.depthLoadAction=R4LoadAction::Clear;
    pass.depthStencilAttachment.depthStoreAction=R4StoreAction::Store;
    pass.depthStencilAttachment.stencilLoadAction=R4LoadAction::Clear;
    pass.depthStencilAttachment.stencilStoreAction=R4StoreAction::Store;
    pass.depthStencilAttachment.clearDepth=1.0; pass.depthStencilAttachment.clearStencil=0u;
    if (!backend.BeginRenderPass(pass,error) || !backend.BindGraphicsPipeline(geometry,error) ||
        !backend.SetStencilReference(1u,error)) return false;
    R4IndexedDrawDesc draw; draw.indexBuffer=quadIndices; draw.indexCount=6u; draw.indexType=R4IndexType::UInt16;
    if (!backend.DrawIndexed(draw,error)) return false;
    return backend.EndRenderPass(error);
}

bool beginAccumulatorLoad(IR4Backend& backend, const std::shared_ptr<IR4Texture>& color,
    const std::shared_ptr<IR4Texture>& depthStencil, std::string& error)
{
    R4RenderPassDesc pass;
    pass.label="E08 accumulate light batch while preserving accumulator/depth/stencil";
    pass.useDrawable=false;
    pass.colorAttachments.push_back({color,R4LoadAction::Load,R4StoreAction::Store,{0,0,0,0}});
    pass.depthStencilAttachment.texture=depthStencil;
    pass.depthStencilAttachment.depthLoadAction=R4LoadAction::Load;
    pass.depthStencilAttachment.depthStoreAction=R4StoreAction::Store;
    pass.depthStencilAttachment.stencilLoadAction=R4LoadAction::Load;
    pass.depthStencilAttachment.stencilStoreAction=R4StoreAction::Store;
    return backend.BeginRenderPass(pass,error);
}

bool renderBatchShadowed(IR4Backend& backend, MetalR4LocalLightAdapter& adapter,
    MetalR4LocalLightAdapter::GBufferInputs inputs, const MetalR4LocalLightAdapter::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const std::vector<std::vector<std::size_t>>& batches,
    const std::shared_ptr<IR4Texture>& atlas, const std::shared_ptr<IR4Texture>& atlasDepth,
    const std::shared_ptr<IR4GraphicsPipeline>& shadowPipeline,
    const std::shared_ptr<IR4Texture>& accumulator, const std::shared_ptr<IR4Texture>& sceneDepthStencil,
    const std::shared_ptr<IR4GraphicsPipeline>& geometry, const std::shared_ptr<IR4Buffer>& quadIndices,
    ShadowAtlasRenderPath renderPath, const std::string& validationDirectory,
    std::size_t& markerResets, std::size_t& submitted,
    ShadowAtlasPathStats& atlasPathStats, std::string& error)
{
    if (!beginReceiverSurface(backend,accumulator,sceneDepthStencil,geometry,quadIndices,error)) return false;
    MetalR4LocalLightAdapter::DrawState state;
    adapter.BeginFrame();
    std::vector<std::vector<std::size_t>> sequence;
    if (renderPath == ShadowAtlasRenderPath::SerialPerLight)
    {
        // Replay precisely the planner's batch/light order so FP16 additive
        // rounding is compared against the same production sequence.
        for (const auto& batch:batches)
            for (const std::size_t index:batch) sequence.push_back({index});
    }
    else sequence=batches;
    std::uint32_t batchId=0u;
    for (const auto& indices:sequence)
    {
        std::vector<xray::metal::R4LocalShadowAtlasRecord> records;
        records.reserve(indices.size());
        for (std::size_t tile=0;tile<indices.size();++tile)
        {
            xray::metal::R4LocalShadowAtlasRecord record;
            if (!makeShadowRecord(snapshot->lights[indices[tile]],snapshot->engineFrame,tile,batchId,
                    view.view,record,error)) return false;
            records.emplace_back(record);
        }
        if (renderPath == ShadowAtlasRenderPath::SerialPerLight)
        {
            // Keep the reference path as the previous one-render-pass-per-light
            // implementation; each serial record owns atlas tile zero.
            for (std::size_t tile=0;tile<indices.size();++tile)
            {
                if (!encodeShadowTile(backend,shadowPipeline,atlas,atlasDepth,quadIndices,
                        snapshot->lights[indices[tile]],records[tile],true,error)) return false;
                ++atlasPathStats.passes;
            }
        }
        else if (renderPath == ShadowAtlasRenderPath::LegacyMultiTileBatch)
        {
            // Preserve the former batched atlas algorithm: one render pass per
            // tile, clear color on tile zero, load it thereafter, and clear
            // the shared depth attachment on every tile pass.
            for (std::size_t tile=0;tile<indices.size();++tile)
            {
                if (!encodeShadowTile(backend,shadowPipeline,atlas,atlasDepth,quadIndices,
                        snapshot->lights[indices[tile]],records[tile],tile==0u,error)) return false;
                ++atlasPathStats.passes;
            }
        }
        else
        {
            std::vector<const R4EngineLocalLight*> groupedLights;
            groupedLights.reserve(indices.size());
            for (const std::size_t index:indices)
                groupedLights.push_back(&snapshot->lights[index]);
            if (!encodeGroupedShadowTiles(backend,shadowPipeline,atlas,atlasDepth,
                    quadIndices,groupedLights,records,error)) return false;
            ++atlasPathStats.passes;
            atlasPathStats.regionChanges += indices.size() - 1u;
        }
        inputs.shadowAtlas=atlas;
        inputs.shadowRecords=&records;
        if (!beginAccumulatorLoad(backend,accumulator,sceneDepthStencil,error)) return false;
        if (!adapter.DrawBatchIntoActiveAccumulator(backend,inputs,view,snapshot,indices,state,error) ||
            !backend.EndRenderPass(error)) return false;
        if (renderPath != ShadowAtlasRenderPath::SerialPerLight)
        {
            R4LocalShadowBatchValidationDesc validation;
            validation.records=records;
            for (const std::size_t index:indices)
                validation.parentIds.push_back(snapshot->lights[index].parentLightId);
            validation.view=view.view; validation.projection=view.projection;
            Matrix4 camera{}; std::memcpy(camera.e,view.view.data(),sizeof(camera.e));
            const Matrix4 inverse=inverseRigidView(camera);
            std::memcpy(validation.inverseView.data(),inverse.e,sizeof(inverse.e));
            // The G-buffer is a z=.5 plane under the fixture perspective projection. These
            // globals invert that exact projection, rather than feeding the
            // native witness the packed normal XY as a world position.
            validation.positionDecompressionParams={1.0f,-1.0f,2.0f/float(Width),-2.0f/float(Height)};
            validation.engineFrame=snapshot->engineFrame; validation.batchId=batchId;
            validation.outputDirectory=validationDirectory;
            if (!backend.ValidateLocalShadowBatch(validation,atlas,inputs.position,
                    sceneDepthStencil,accumulator,error)) return false;
        }
        ++batchId;
    }
    markerResets=adapter.LastTelemetry().markerResets;
    submitted=adapter.LastTelemetry().submitted;
    return true;
}

bool renderUnshadowed(IR4Backend& backend, MetalR4LocalLightAdapter& adapter,
    MetalR4LocalLightAdapter::GBufferInputs inputs, const MetalR4LocalLightAdapter::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const std::vector<std::vector<std::size_t>>& batches,
    const std::shared_ptr<IR4Texture>& accumulator, const std::shared_ptr<IR4Texture>& sceneDepthStencil,
    const std::shared_ptr<IR4GraphicsPipeline>& geometry, const std::shared_ptr<IR4Buffer>& quadIndices,
    std::size_t& markerResets, std::size_t& submitted, std::string& error)
{
    if (!beginReceiverSurface(backend,accumulator,sceneDepthStencil,geometry,quadIndices,error)) return false;
    MetalR4LocalLightAdapter::DrawState state; adapter.BeginFrame();
    for (const auto& indices:batches)
    {
        if (!beginAccumulatorLoad(backend,accumulator,sceneDepthStencil,error) ||
            !adapter.DrawBatchIntoActiveAccumulator(backend,inputs,view,snapshot,indices,state,error) ||
            !backend.EndRenderPass(error)) return false;
    }
    markerResets=adapter.LastTelemetry().markerResets;
    submitted=adapter.LastTelemetry().submitted;
    return true;
}

bool clearAtlasToLit(IR4Backend& backend, const std::shared_ptr<IR4Texture>& atlas,
    std::string& error)
{
    // This is a completed clear-only shadow-map pass: it leaves no caster
    // coverage, but the record remains valid for the negative shadow control.
    R4RenderPassDesc pass; pass.label="E08 empty-atlas lit control"; pass.useDrawable=false;
    pass.colorAttachments.push_back({atlas,R4LoadAction::Clear,R4StoreAction::Store,{1,1,1,1}});
    return backend.BeginRenderPass(pass,error)&&backend.EndRenderPass(error);
}

bool renderShadowedAgainstEmptyAtlas(IR4Backend& backend, MetalR4LocalLightAdapter& adapter,
    MetalR4LocalLightAdapter::GBufferInputs inputs, const MetalR4LocalLightAdapter::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const std::vector<std::vector<std::size_t>>& batches,
    const std::shared_ptr<IR4Texture>& emptyAtlas,
    const std::shared_ptr<IR4Texture>& accumulator, const std::shared_ptr<IR4Texture>& sceneDepthStencil,
    const std::shared_ptr<IR4GraphicsPipeline>& geometry, const std::shared_ptr<IR4Buffer>& quadIndices,
    std::size_t& markerResets, std::size_t& submitted, std::string& error)
{
    if (!beginReceiverSurface(backend,accumulator,sceneDepthStencil,geometry,quadIndices,error)) return false;
    MetalR4LocalLightAdapter::DrawState state; adapter.BeginFrame();
    std::uint32_t batchId=0u;
    for (const auto& indices:batches)
    {
        std::vector<xray::metal::R4LocalShadowAtlasRecord> records;
        records.reserve(indices.size());
        for (std::size_t tile=0;tile<indices.size();++tile)
        {
            xray::metal::R4LocalShadowAtlasRecord record;
            if (!makeShadowRecord(snapshot->lights[indices[tile]],snapshot->engineFrame,tile,batchId,
                    view.view,record,error)) return false;
            records.emplace_back(record);
        }
        inputs.shadowAtlas=emptyAtlas; inputs.shadowRecords=&records;
        if (!beginAccumulatorLoad(backend,accumulator,sceneDepthStencil,error) ||
            !adapter.DrawBatchIntoActiveAccumulator(backend,inputs,view,snapshot,indices,state,error) ||
            !backend.EndRenderPass(error)) return false;
        ++batchId;
    }
    markerResets=adapter.LastTelemetry().markerResets;
    submitted=adapter.LastTelemetry().submitted;
    return true;
}

bool renderDelta(IR4Backend& backend, const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    const std::shared_ptr<IR4Texture>& shadowed, const std::shared_ptr<IR4Texture>& unshadowed,
    const std::shared_ptr<IR4Texture>& output, const std::shared_ptr<IR4Sampler>& sampler,
    const std::shared_ptr<IR4Buffer>& triangleIndices, std::string& error)
{
    R4RenderPassDesc pass; pass.label="E08 shadowed versus unshadowed output delta"; pass.useDrawable=false;
    pass.colorAttachments.push_back({output,R4LoadAction::Clear,R4StoreAction::Store,{0,0,0,0}});
    if (!backend.BeginRenderPass(pass,error) || !backend.BindGraphicsPipeline(pipeline,error) ||
        !backend.BindFragmentTexture(0u,shadowed,error) || !backend.BindFragmentTexture(1u,unshadowed,error) ||
        !backend.BindFragmentSampler(0u,sampler,error)) return false;
    R4IndexedDrawDesc draw; draw.indexBuffer=triangleIndices; draw.indexCount=3u; draw.indexType=R4IndexType::UInt16;
    return backend.DrawIndexed(draw,error) && backend.EndRenderPass(error);
}

void encodeTextureReport(IR4Backend& backend, const std::string& phase, std::uint32_t frame,
    const std::string& outputDirectory, const std::vector<std::shared_ptr<IR4Texture>>& textures,
    const std::vector<std::string>& labels, bool pair, std::string& error)
{
    R4TemporalTextureValidationDesc descriptor;
    descriptor.textures=textures; descriptor.labels=labels; descriptor.engineFrame=frame;
    descriptor.previousFrame=frame; descriptor.phase=phase; descriptor.outputDirectory=outputDirectory;
    descriptor.expectEqualPair=pair;
    if (!backend.ValidateTemporalTextures(descriptor,error) && error.empty())
        error="R4 temporal readback encoding failed for phase "+phase;
}

bool reportHas(const fs::path& report, const std::string& needle)
{
    std::ifstream input(report);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)),{});
    return text.find(needle)!=std::string::npos;
}

bool readTileHashes(const fs::path& report, std::vector<std::string>& hashes)
{
    std::ifstream input(report);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)),{});
    constexpr std::string_view key="\"tile_hash_fnv1a64\": \"";
    std::size_t cursor=0u;
    while ((cursor=text.find(key,cursor))!=std::string::npos)
    {
        cursor+=key.size();
        const std::size_t end=text.find('"',cursor);
        if (end==std::string::npos || end-cursor!=16u) return false;
        const std::string hash=text.substr(cursor,end-cursor);
        if (!std::all_of(hash.begin(),hash.end(),[](const unsigned char c) { return std::isxdigit(c)!=0; }))
            return false;
        hashes.push_back(hash);
        cursor=end+1u;
    }
    return !hashes.empty();
}

bool exactTileHashesMatch(const fs::path& groupedReport, const fs::path& legacyReport,
    const std::size_t expectedTiles)
{
    std::vector<std::string> grouped,legacy;
    return readTileHashes(groupedReport,grouped) && readTileHashes(legacyReport,legacy) &&
        grouped.size()==expectedTiles && legacy.size()==expectedTiles && grouped==legacy;
}

fs::path nativeBatchReportPath(const fs::path& directory, std::uint32_t frame,std::uint32_t batch)
{
    std::ostringstream name;
    name<<"local-shadow-frame-"<<std::setw(8)<<std::setfill('0')<<frame
        <<"-batch-"<<std::setw(4)<<std::setfill('0')<<batch<<".json";
    return directory/name.str();
}

bool reportNonzeroPixels(const fs::path& report, const std::string& label)
{
    std::ifstream input(report);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)),{});
    const std::string labelKey="\"label\": \""+label+"\"";
    const auto start=text.find(labelKey);
    if (start==std::string::npos) return false;
    const auto end=text.find('}',start);
    const std::string record=text.substr(start,end==std::string::npos?std::string::npos:end-start);
    const auto key=record.find("\"nonzero_rgb_pixel_count\": ");
    if (key==std::string::npos) return false;
    const char* value=record.c_str()+key+std::strlen("\"nonzero_rgb_pixel_count\": ");
    return std::strtoull(value,nullptr,10)>0u;
}

bool validatePointFaces(const R4EngineLocalLightSnapshot& snapshot, std::string& error)
{
    if (snapshot.lights.size()<6u) { error="snapshot lacks the complete point-face group"; return false; }
    std::array<bool,6> faces{};
    const auto& first=snapshot.lights[0];
    for (std::size_t i=0;i<6u;++i)
    {
        const auto& light=snapshot.lights[i];
        if (light.type!=R4EngineLocalLightType::OmniFace || light.omniFace>=6u || faces[light.omniFace] ||
            light.parentLightId!=first.parentLightId || light.position!=first.position ||
            light.range!=first.range || light.lightId==0u)
        { error="point light OmniFace records are not a coherent six-face group"; return false; }
        faces[light.omniFace]=true;
    }
    if (!std::all_of(faces.begin(),faces.end(),[](bool value){return value;}))
    { error="one or more point faces are missing"; return false; }
    return true;
}

bool runProbe(const std::string& localShaderDirectory, const std::string& shadowShaderDirectory,
    const std::string& artifactDirectory, std::size_t lightCount, std::string& error)
{
    if (lightCount<17u || lightCount>400u || localShaderDirectory.empty() ||
        shadowShaderDirectory.empty() || artifactDirectory.empty())
    { error="shader directories, artifact directory, and 17..400 shadow records are required"; return false; }
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)!=0)
    { error=std::string("SDL video initialization failed: ")+SDL_GetError(); return false; }
    SDL_Window* window=SDL_CreateWindow("R4 local shadow batch probe",SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,Width,Height,SDL_WINDOW_HIDDEN|SDL_WINDOW_METAL|SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
    { error=std::string("hidden Metal window creation failed: ")+SDL_GetError(); SDL_QuitSubSystem(SDL_INIT_VIDEO); return false; }
    auto backend=CreateMetalR4Backend();
    bool success=false;
    do
    {
        if (!backend->Initialize(window,Width,Height,error)) break;
        IR4Resources& resources=backend->Resources();
        auto position=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,R4TextureUsage::ShaderRead,
            "E08 atlas batch packed position",error);
        auto diffuse=makeTexture(resources,Width,Height,R4PixelFormat::RGBA8Unorm,R4TextureUsage::ShaderRead,
            "E08 atlas batch diffuse",error);
        auto atlas=makeTexture(resources,AtlasExtent,AtlasExtent,R4PixelFormat::R32Float,
            R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 reusable local shadow atlas",error);
        auto emptyAtlas=makeTexture(resources,AtlasExtent,AtlasExtent,R4PixelFormat::R32Float,
            R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 empty shadow atlas positive control",error);
        auto atlasDepth=makeTexture(resources,AtlasExtent,AtlasExtent,R4PixelFormat::Depth32Float,
            R4TextureUsage::RenderTarget,"E08 local shadow tile depth",error);
        if (!position||!diffuse||!atlas||!emptyAtlas||!atlasDepth) { error="fixture texture allocation failed: "+error; break; }
        std::vector<__fp16> positionPixels(PixelCount*4u);
        std::vector<std::uint8_t> diffusePixels(PixelCount*4u,0u);
        std::array<std::shared_ptr<IR4Texture>,6> facePositions,faceDiffuses;
        const std::uint32_t packed=0x44000000u; float packedFloat; std::memcpy(&packedFloat,&packed,sizeof(packedFloat));
        for (std::uint32_t y=0;y<Height;++y)
            for (std::uint32_t x=0;x<Width/2u;++x)
            {
                const std::size_t o=(std::size_t(y)*Width+x)*4u;
                positionPixels[o]=__fp16(0.0f); positionPixels[o+1u]=__fp16(-1.0f);
                positionPixels[o+2u]=__fp16(0.5f); positionPixels[o+3u]=__fp16(packedFloat);
                diffusePixels[o]=diffusePixels[o+1u]=diffusePixels[o+2u]=180u; diffusePixels[o+3u]=255u;
            }
        if (!resources.UploadTexture2D(position,0u,0u,positionPixels.data(),
                positionPixels.size()*sizeof(__fp16),Width*4u*sizeof(__fp16),
                positionPixels.size()*sizeof(__fp16),error) ||
            !resources.UploadTexture2D(diffuse,0u,0u,diffusePixels.data(),diffusePixels.size(),
                Width*4u,diffusePixels.size(),error))
        { error="G-buffer fixture upload failed: "+error; break; }
        const std::uint32_t faceCenterX=Width/4u, faceCenterY=Height/2u;
        for (std::size_t face=0;face<6u;++face)
        {
            facePositions[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,
                R4TextureUsage::ShaderRead,"E08 isolated point-face packed position",error);
            faceDiffuses[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA8Unorm,
                R4TextureUsage::ShaderRead,"E08 isolated point-face diffuse",error);
            if (!facePositions[face]||!faceDiffuses[face])
            { error="isolated point-face G-buffer allocation failed: "+error; break; }
            std::vector<__fp16> facePositionPixels(PixelCount*4u);
            std::vector<std::uint8_t> faceDiffusePixels(PixelCount*4u,0u);
            for (std::uint32_t y=faceCenterY-4u;y<faceCenterY+4u;++y)
                for (std::uint32_t x=faceCenterX-4u;x<faceCenterX+4u;++x)
                {
                    const std::size_t o=(std::size_t(y)*Width+x)*4u;
                    const Vec3 normal={-CubeDirections[face].x,-CubeDirections[face].y,-CubeDirections[face].z};
                    const float angle=std::atan2(normal.y,normal.x)/3.14159265358979323846f;
                    facePositionPixels[o]=__fp16(angle); facePositionPixels[o+1u]=__fp16(normal.z);
                    facePositionPixels[o+2u]=__fp16(0.5f); facePositionPixels[o+3u]=__fp16(packedFloat);
                    faceDiffusePixels[o]=faceDiffusePixels[o+1u]=faceDiffusePixels[o+2u]=180u;
                    faceDiffusePixels[o+3u]=255u;
                }
            if (!resources.UploadTexture2D(facePositions[face],0u,0u,facePositionPixels.data(),
                    facePositionPixels.size()*sizeof(__fp16),Width*4u*sizeof(__fp16),
                    facePositionPixels.size()*sizeof(__fp16),error) ||
                !resources.UploadTexture2D(faceDiffuses[face],0u,0u,faceDiffusePixels.data(),
                    faceDiffusePixels.size(),Width*4u,faceDiffusePixels.size(),error))
            { error="isolated point-face G-buffer upload failed: "+error; break; }
        }
        if (!error.empty()) break;
        std::array<std::uint8_t,528> globals{};
        const float resolution[4]={float(Width),float(Height),1.0f/float(Width),1.0f/float(Height)};
        std::memcpy(globals.data()+416u,resolution,sizeof(resolution));
        const float decompression[4]={1.0f,-1.0f,2.0f/float(Width),-2.0f/float(Height)};
        std::memcpy(globals.data()+400u,decompression,sizeof(decompression));
        auto staticGlobals=makeBuffer(resources,globals.data(),globals.size(),R4BufferUsage::Uniform,
            "E08 atlas batch static globals",error);
        auto nearest=makeSampler(resources,R4SamplerFilter::Nearest,"E08 atlas batch point sampler",error);
        auto linear=makeSampler(resources,R4SamplerFilter::Linear,"E08 atlas batch linear sampler",error);
        auto shadowSampler=makeSampler(resources,R4SamplerFilter::Linear,"E08 atlas batch shadow sampler",error,
            R4SamplerCompareFunction::LessEqual);
        const std::array<std::uint8_t,4> white={{255u,255u,255u,255u}};
        auto cookie=makeTexture(resources,1u,1u,R4PixelFormat::RGBA8Unorm,R4TextureUsage::ShaderRead,
            "E08 atlas batch white spot cookie",error);
        if (!staticGlobals||!nearest||!linear||!shadowSampler||!cookie||
            !resources.UploadTexture2D(cookie,0u,0u,white.data(),white.size(),white.size(),white.size(),error))
        { error="local-light shared fixture resources unavailable: "+error; break; }

        const std::array<std::uint16_t,6> quadIndices={0,1,2,0,2,3};
        const std::array<std::uint16_t,3> triIndices={0,1,2};
        auto indexBuffer=makeBuffer(resources,quadIndices.data(),sizeof(quadIndices),R4BufferUsage::Index,
            "E08 atlas batch quad indices",error);
        auto triangleIndexBuffer=makeBuffer(resources,triIndices.data(),sizeof(triIndices),R4BufferUsage::Index,
            "E08 atlas batch diagnostic triangle indices",error);
        std::shared_ptr<IR4GraphicsPipeline> geometryPipeline,deltaPipeline;
        if (!indexBuffer||!triangleIndexBuffer||!makeGeometryPipelines(*backend,geometryPipeline,deltaPipeline,error))
        { error="receiver/delta pipeline setup failed: "+error; break; }
        const fs::path shadowDir(shadowShaderDirectory);
        const std::string shadowVs=(shadowDir/"shadow_direct_base.vs.metallib").string();
        const std::string shadowPs=(shadowDir/"shadow_direct_base_color.ps.metallib").string();
        if (!fs::is_regular_file(shadowVs)||!fs::is_regular_file(shadowPs))
        { error="converted source base shadow VS/PS bundle is missing"; break; }
        R4GraphicsPipelineDesc shadowDesc;
        shadowDesc.label="E08 actual MSC static base shadow caster";
        shadowDesc.shaderKey="e05-local-shadow-batch-base-shadow-msc-v1";
        shadowDesc.vertexMetallibPath=shadowVs.c_str(); shadowDesc.fragmentMetallibPath=shadowPs.c_str();
        shadowDesc.vertexAttributes={{11u,6u,0u,R4VertexFormat::Float3}};
        shadowDesc.vertexLayouts={{6u,sizeof(float)*3u}};
        shadowDesc.colorFormats={R4PixelFormat::R32Float};
        shadowDesc.depthFormat=R4PixelFormat::Depth32Float;
        shadowDesc.depthCompare=R4CompareFunction::Less; shadowDesc.depthWrite=true;
        shadowDesc.cullMode=R4CullMode::Back; shadowDesc.frontFace=R4Winding::Clockwise;
        auto shadowPipeline=backend->CreateGraphicsPipeline(shadowDesc,error);
        if (!shadowPipeline) { error="production MSC base shadow pipeline creation failed: "+error; break; }

        MetalR4LocalLightAdapter adapter;
        MetalR4LocalLightAdapter::Options adapterOptions;
        adapterOptions.shaderDirectory=fs::path(localShaderDirectory); adapterOptions.defaultSpotCookie=cookie;
        std::string adapterStatus;
        if (!adapter.Prepare(*backend,adapterOptions,adapterStatus)||!adapter.IsReady())
        { error="production local-light MSC adapter did not prepare: "+adapterStatus; break; }

        const auto batchesSnapshot=makeSnapshot(lightCount,599u,true);
        std::vector<std::vector<std::size_t>> batches;
        if (!makeBatches(*batchesSnapshot,batches,error)) break;
        const auto pointBatch=std::find_if(batches.begin(),batches.end(),[](const auto& batch){
            for (std::size_t face=0;face<6u;++face)
                if (std::find(batch.begin(),batch.end(),face)==batch.end()) return false;
            return true;
        });
        if (batches.empty()||pointBatch==batches.end())
        { error="batch planner split the coherent six-face point group"; break; }
        const fs::path groupedValidationDirectory=fs::path(artifactDirectory)/"grouped-atlas";
        const fs::path legacyValidationDirectory=fs::path(artifactDirectory)/"legacy-multitile-atlas";
        std::array<std::shared_ptr<IR4Texture>,3> batchColor,serialColor,legacyTileColor,unshadowedColor,deltaColor;
        std::array<std::shared_ptr<IR4Texture>,3> batchDepth,serialDepth,legacyTileDepth,unshadowedDepth;
        for (std::size_t pose=0;pose<3u;++pose)
        {
            batchColor[pose]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 batch accumulator",error);
            serialColor[pose]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 serial accumulator",error);
            legacyTileColor[pose]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 legacy multi-tile accumulator",error);
            unshadowedColor[pose]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 no-shadow positive control",error);
            deltaColor[pose]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 shadowed-unshadowed difference",error);
            batchDepth[pose]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,R4TextureUsage::RenderTarget,"E08 batch depth stencil",error);
            serialDepth[pose]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,R4TextureUsage::RenderTarget,"E08 serial depth stencil",error);
            legacyTileDepth[pose]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,R4TextureUsage::RenderTarget,"E08 legacy multi-tile depth stencil",error);
            unshadowedDepth[pose]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,R4TextureUsage::RenderTarget,"E08 unshadowed depth stencil",error);
            if (!batchColor[pose]||!serialColor[pose]||!legacyTileColor[pose]||!unshadowedColor[pose]||!deltaColor[pose]||
                !batchDepth[pose]||!serialDepth[pose]||!legacyTileDepth[pose]||!unshadowedDepth[pose])
            { error="offscreen batch/reference target allocation failed: "+error; break; }
        }
        if (!error.empty()) break;

        std::array<std::size_t,3> batchResets{},serialResets{},unshadowedResets{};
        std::array<std::size_t,3> batchSubmitted{},serialSubmitted{},unshadowedSubmitted{};
        std::array<std::size_t,3> legacyTileResets{},legacyTileSubmitted{};
        std::array<ShadowAtlasPathStats,3> batchAtlasPathStats{},serialAtlasPathStats{},legacyTileAtlasPathStats{};
        std::array<float,3> eyeX={0.0f,0.035f,0.0f};
        for (std::size_t pose=0;pose<3u;++pose)
        {
            const std::uint32_t frame=600u+static_cast<std::uint32_t>(pose);
            const auto snapshot=makeSnapshot(lightCount,frame,true);
            if (!validatePointFaces(*snapshot,error)) break;
            const auto noShadowSnapshot=makeSnapshot(lightCount,frame,false);
            MetalR4LocalLightAdapter::ViewState view;
            const Matrix4 v=cameraView(eyeX[pose]);
            std::memcpy(view.view.data(),v.e,sizeof(v.e));
            const Matrix4 p=perspectiveProjection();
            std::memcpy(view.projection.data(),p.e,sizeof(p.e));
            view.eye={eyeX[pose],0,0}; view.valid=true;
            if (backend->BeginFrame(error)!=R4FrameAcquireResult::Ready)
            { error="probe frame acquisition failed: "+error; break; }
            MetalR4LocalLightAdapter::GBufferInputs inputs;
            inputs.position=position; inputs.diffuse=diffuse; inputs.staticGlobals=staticGlobals;
            inputs.noFilterSampler=nearest; inputs.linearSampler=linear; inputs.shadowSampler=shadowSampler;
            auto makeCapture=[&](const std::shared_ptr<IR4Texture>& color,const std::shared_ptr<IR4Texture>& ds,
                ShadowAtlasRenderPath renderPath,const std::string& validationDirectory,
                std::size_t& resets,std::size_t& submitted,
                ShadowAtlasPathStats& atlasPathStats) {
                return renderBatchShadowed(*backend,adapter,inputs,view,snapshot,batches,atlas,atlasDepth,
                    shadowPipeline,color,ds,geometryPipeline,indexBuffer,renderPath,validationDirectory,
                    resets,submitted,atlasPathStats,error);
            };
            if (!makeCapture(batchColor[pose],batchDepth[pose],ShadowAtlasRenderPath::GroupedBatch,
                    groupedValidationDirectory.string(),batchResets[pose],batchSubmitted[pose],batchAtlasPathStats[pose])||
                !makeCapture(serialColor[pose],serialDepth[pose],ShadowAtlasRenderPath::SerialPerLight,
                    artifactDirectory,serialResets[pose],serialSubmitted[pose],serialAtlasPathStats[pose])||
                !makeCapture(legacyTileColor[pose],legacyTileDepth[pose],ShadowAtlasRenderPath::LegacyMultiTileBatch,
                    legacyValidationDirectory.string(),legacyTileResets[pose],legacyTileSubmitted[pose],
                    legacyTileAtlasPathStats[pose]))
            { if (error.empty()) error="grouped, legacy multi-tile, or serial shadow rendering failed"; backend->DiscardFrame(); break; }
            MetalR4LocalLightAdapter::GBufferInputs noShadowInputs=inputs;
            noShadowInputs.shadowAtlas.reset(); noShadowInputs.shadowRecords=nullptr; noShadowInputs.shadowSampler.reset();
            if (!renderUnshadowed(*backend,adapter,noShadowInputs,view,noShadowSnapshot,batches,
                    unshadowedColor[pose],unshadowedDepth[pose],geometryPipeline,indexBuffer,
                    unshadowedResets[pose],unshadowedSubmitted[pose],error) ||
                !renderDelta(*backend,deltaPipeline,batchColor[pose],unshadowedColor[pose],deltaColor[pose],
                    linear,triangleIndexBuffer,error))
            { if (error.empty()) error="no-shadow control/delta rendering failed"; backend->DiscardFrame(); break; }
            encodeTextureReport(*backend,"batch-serial",frame,artifactDirectory,
                {batchColor[pose],serialColor[pose]},{"shadowed-batch","shadowed-serial"},true,error);
            encodeTextureReport(*backend,"grouped-legacy",frame,artifactDirectory,
                {batchColor[pose],legacyTileColor[pose]},{"shadowed-grouped","shadowed-legacy-multitile"},true,error);
            if (pose==2u)
                encodeTextureReport(*backend,"camera-return",frame,artifactDirectory,
                    {batchColor[0],batchColor[2]},{"batch-pose-a","batch-return-a"},true,error);
            encodeTextureReport(*backend,"output",frame,artifactDirectory,
                {unshadowedColor[pose],deltaColor[pose]},{"unshadowed-control","shadow-vs-unshadowed-delta"},false,error);
            if (!error.empty()) { backend->DiscardFrame(); break; }
            if (!backend->SubmitFrame(error)) { error="batch probe frame submission failed: "+error; break; }
        }
        std::array<std::shared_ptr<IR4Texture>,6> faceShadowColor,faceControlColor,faceEmptyColor;
        std::array<std::shared_ptr<IR4Texture>,6> faceDeltaColor,faceCasterDeltaColor;
        std::array<std::shared_ptr<IR4Texture>,6> faceShadowDepth,faceControlDepth,faceEmptyDepth;
        if (!error.empty()) { backend->Shutdown(); break; }
        for (std::size_t face=0;face<6u;++face)
        {
            faceShadowColor[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,
                R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 isolated OmniFace shadowed output",error);
            faceControlColor[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,
                R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 isolated OmniFace control output",error);
            faceEmptyColor[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,
                R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 isolated OmniFace empty-atlas output",error);
            faceDeltaColor[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,
                R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 isolated OmniFace difference",error);
            faceCasterDeltaColor[face]=makeTexture(resources,Width,Height,R4PixelFormat::RGBA16Float,
                R4TextureUsage::RenderTarget|R4TextureUsage::ShaderRead,"E08 isolated OmniFace caster-vs-empty difference",error);
            faceShadowDepth[face]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,
                R4TextureUsage::RenderTarget,"E08 isolated OmniFace shadow depth stencil",error);
            faceControlDepth[face]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,
                R4TextureUsage::RenderTarget,"E08 isolated OmniFace control depth stencil",error);
            faceEmptyDepth[face]=makeTexture(resources,Width,Height,R4PixelFormat::Depth32FloatStencil8,
                R4TextureUsage::RenderTarget,"E08 isolated OmniFace empty-atlas depth stencil",error);
            if (!faceShadowColor[face]||!faceControlColor[face]||!faceEmptyColor[face]||
                !faceDeltaColor[face]||!faceCasterDeltaColor[face]||!faceShadowDepth[face]||
                !faceControlDepth[face]||!faceEmptyDepth[face])
            { error="isolated OmniFace output allocation failed: "+error; break; }
        }
        if (!error.empty()) { backend->Shutdown(); break; }
        if (backend->BeginFrame(error)!=R4FrameAcquireResult::Ready)
        { error="isolated point-face frame acquisition failed: "+error; backend->Shutdown(); break; }
        std::vector<std::shared_ptr<IR4Texture>> faceOutputs;
        std::vector<std::string> faceLabels;
        MetalR4LocalLightAdapter::ViewState faceView;
        faceView.view={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        faceView.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        faceView.eye={0.0f,0.0f,0.0f}; faceView.valid=true;
        const Matrix4 faceProjection=perspectiveProjection();
        std::memcpy(faceView.projection.data(),faceProjection.e,sizeof(faceProjection.e));
        for (std::size_t face=0;face<6u;++face)
        {
            const std::uint32_t faceFrame=700u+static_cast<std::uint32_t>(face);
            auto faceSnapshot=makeCanonicalPointFaceCase(faceFrame,face,true);
            auto faceControl=makeCanonicalPointFaceCase(faceFrame,face,false);
            // Keep all six canonical face records in the same atomic parent
            // group, while making this case's contribution come from exactly
            // one face. This exercises each cube-face matrix/binding without
            // allowing the other five faces to mask a broken selected face.
            for (std::size_t groupFace=0;groupFace<6u;++groupFace)
                if (groupFace!=face)
                {
                    faceSnapshot->lights[groupFace].color={0,0,0,1};
                    faceControl->lights[groupFace].color={0,0,0,1};
                    faceSnapshot->lights[groupFace].specular=0;
                    faceControl->lights[groupFace].specular=0;
                }
            if (!validatePointFaces(*faceSnapshot,error)) break;
            MetalR4LocalLightAdapter::GBufferInputs faceInputs;
            faceInputs.staticGlobals=staticGlobals; faceInputs.noFilterSampler=nearest;
            faceInputs.linearSampler=linear; faceInputs.shadowSampler=shadowSampler;
            faceInputs.position=facePositions[face]; faceInputs.diffuse=faceDiffuses[face];
            const std::vector<std::vector<std::size_t>> faceBatch={{0u,1u,2u,3u,4u,5u}};
            std::size_t faceResets=0u,faceSubmitted=0u,controlResets=0u,controlSubmitted=0u;
            ShadowAtlasPathStats faceAtlasPathStats;
            if (!renderBatchShadowed(*backend,adapter,faceInputs,faceView,faceSnapshot,faceBatch,
                    atlas,atlasDepth,shadowPipeline,faceShadowColor[face],faceShadowDepth[face],
                    geometryPipeline,indexBuffer,ShadowAtlasRenderPath::GroupedBatch,
                    artifactDirectory,faceResets,faceSubmitted,
                    faceAtlasPathStats,error) ||
                !renderUnshadowed(*backend,adapter,faceInputs,faceView,faceControl,faceBatch,
                    faceControlColor[face],faceControlDepth[face],geometryPipeline,indexBuffer,
                    controlResets,controlSubmitted,error) ||
                !clearAtlasToLit(*backend,emptyAtlas,error) ||
                !renderShadowedAgainstEmptyAtlas(*backend,adapter,faceInputs,faceView,faceSnapshot,
                    faceBatch,emptyAtlas,faceEmptyColor[face],faceEmptyDepth[face],geometryPipeline,indexBuffer,
                    controlResets,controlSubmitted,error) ||
                !renderDelta(*backend,deltaPipeline,faceShadowColor[face],faceControlColor[face],
                    faceDeltaColor[face],linear,triangleIndexBuffer,error) ||
                !renderDelta(*backend,deltaPipeline,faceShadowColor[face],faceEmptyColor[face],
                    faceCasterDeltaColor[face],linear,triangleIndexBuffer,error))
            { if (error.empty()) error="isolated OmniFace shadow/control rendering failed"; break; }
            if (faceSubmitted!=6u||controlSubmitted!=6u||faceResets!=0u||controlResets!=0u)
            { error="isolated OmniFace adapter did not submit its complete six-face group"; break; }
            if (faceAtlasPathStats.passes!=1u||faceAtlasPathStats.regionChanges!=5u)
            { error="grouped six-face atlas path did not use one encoder and five region changes"; break; }
            faceOutputs.push_back(faceShadowColor[face]); faceLabels.push_back("point-face-"+std::to_string(face)+"-shadowed");
            faceOutputs.push_back(faceControlColor[face]); faceLabels.push_back("point-face-"+std::to_string(face)+"-control");
            faceOutputs.push_back(faceEmptyColor[face]); faceLabels.push_back("point-face-"+std::to_string(face)+"-empty-atlas");
            faceOutputs.push_back(faceDeltaColor[face]); faceLabels.push_back("point-face-"+std::to_string(face)+"-delta");
            faceOutputs.push_back(faceCasterDeltaColor[face]); faceLabels.push_back("point-face-"+std::to_string(face)+"-caster-delta");
        }
        if (!error.empty()) { backend->DiscardFrame(); backend->Shutdown(); break; }
        encodeTextureReport(*backend,"output",700u,artifactDirectory,faceOutputs,faceLabels,false,error);
        if (!error.empty()) { backend->DiscardFrame(); backend->Shutdown(); break; }
        if (!backend->SubmitFrame(error))
        { error="isolated OmniFace control submission failed: "+error; backend->Shutdown(); break; }
        backend->Shutdown();
        if (!error.empty()) break;

        const std::size_t expectedResets=lightCount<125u?0u:1u+(lightCount-125u)/126u;
        for (std::size_t pose=0;pose<3u;++pose)
        {
            const std::uint32_t frame=600u+static_cast<std::uint32_t>(pose);
            const fs::path pairReport=fs::path(artifactDirectory)/("r4_temporal_batch-serial_frame_"+std::to_string(frame)+".json");
            const fs::path groupedLegacyReport=fs::path(artifactDirectory)/("r4_temporal_grouped-legacy_frame_"+std::to_string(frame)+".json");
            const fs::path outputReport=fs::path(artifactDirectory)/("r4_temporal_output_frame_"+std::to_string(frame)+".json");
            if (!reportHas(pairReport,"\"result\": \"PASS\"")||!reportHas(pairReport,"\"pair_exact\": true")||
                !reportHas(groupedLegacyReport,"\"result\": \"PASS\"")||!reportHas(groupedLegacyReport,"\"pair_exact\": true")||
                !reportHas(outputReport,"\"result\": \"PASS\"")||
                !reportNonzeroPixels(outputReport,"unshadowed-control")||
                !reportNonzeroPixels(outputReport,"shadow-vs-unshadowed-delta"))
            { error="readback did not prove exact batch/serial parity plus positive unshadowed and shadow-difference signals at pose "+std::to_string(pose); break; }
            if (batchSubmitted[pose]!=lightCount||serialSubmitted[pose]!=lightCount||
                legacyTileSubmitted[pose]!=lightCount||unshadowedSubmitted[pose]!=lightCount||
                batchResets[pose]!=expectedResets||serialResets[pose]!=expectedResets||
                legacyTileResets[pose]!=expectedResets||unshadowedResets[pose]!=expectedResets)
            { error="adapter batch admission or shared marker rollover count is incorrect"; break; }
            std::size_t expectedGroupedRegionChanges=0u;
            for (const auto& batch:batches)
                expectedGroupedRegionChanges+=batch.size()-1u;
            if (batchAtlasPathStats[pose].passes!=batches.size()||
                batchAtlasPathStats[pose].regionChanges!=expectedGroupedRegionChanges||
                legacyTileAtlasPathStats[pose].passes!=lightCount||
                legacyTileAtlasPathStats[pose].regionChanges!=0u||
                serialAtlasPathStats[pose].passes!=lightCount||
                serialAtlasPathStats[pose].regionChanges!=0u)
            { error="grouped, legacy multi-tile, or serial atlas pass counts are incorrect"; break; }
            for (std::size_t batch=0;batch<batches.size();++batch)
            {
                const auto batchId=static_cast<std::uint32_t>(batch);
                const fs::path groupedReport=nativeBatchReportPath(groupedValidationDirectory,frame,batchId);
                const fs::path legacyReport=nativeBatchReportPath(legacyValidationDirectory,frame,batchId);
                if (!reportHas(groupedReport,"\"pass\": true")||!reportHas(legacyReport,"\"pass\": true"))
                { error="native atlas/receiver/depth/stencil validation failed for grouped or legacy frame "+
                    std::to_string(frame)+" batch "+std::to_string(batch); break; }
                if (!exactTileHashesMatch(groupedReport,legacyReport,batches[batch].size()))
                { error="grouped atlas per-tile R32 hashes differ from legacy multi-tile atlas for frame "+
                    std::to_string(frame)+" batch "+std::to_string(batch); break; }
            }
        }
        const fs::path returnReport=fs::path(artifactDirectory)/"r4_temporal_camera-return_frame_602.json";
        if (error.empty()&&(!reportHas(returnReport,"\"result\": \"PASS\"")||
            !reportHas(returnReport,"\"pair_exact\": true")))
            error="A-to-B-to-A camera return did not reproduce the exact GPU accumulator";
        const fs::path faceOutputReport=fs::path(artifactDirectory)/"r4_temporal_output_frame_700.json";
        if (error.empty()&&!reportHas(faceOutputReport,"\"result\": \"PASS\""))
            error="isolated six-face readback report did not pass";
        for (std::size_t face=0;face<6u&&error.empty();++face)
            if (!reportNonzeroPixels(faceOutputReport,"point-face-"+std::to_string(face)+"-control")||
                !reportNonzeroPixels(faceOutputReport,"point-face-"+std::to_string(face)+"-empty-atlas")||
                !reportNonzeroPixels(faceOutputReport,"point-face-"+std::to_string(face)+"-delta")||
                !reportNonzeroPixels(faceOutputReport,"point-face-"+std::to_string(face)+"-caster-delta"))
                error="canonical OmniFace did not produce unshadowed, empty-atlas, and caster-shadow GPU signals: face "+std::to_string(face);
        for (std::size_t face=0;face<6u&&error.empty();++face)
            if (!reportHas(nativeBatchReportPath(artifactDirectory,700u+static_cast<std::uint32_t>(face),0u),
                    "\"pass\": true"))
                error="native atlas/receiver/depth/stencil validation failed for canonical point face "+std::to_string(face);
        if (!error.empty()) break;
        fs::create_directories(artifactDirectory);
        std::ofstream report(fs::path(artifactDirectory)/"local-shadow-batch-report.json");
        if (!report) { error="could not create local shadow batch report"; break; }
        report<<"{\n  \"status\": \"PASS\",\n  \"stage\": \"E08 grouped local shadow atlas\",\n  \"lights\": "<<lightCount
            <<",\n  \"batch_capacity\": 16,\n  \"batch_count\": "<<batches.size()
            <<",\n  \"camera_poses\": [\"origin\",\"out\",\"back\"],\n"
            <<"  \"shadowed_batch_exact_matches_serial\": 3,\n"
            <<"  \"shadowed_grouped_exact_matches_legacy_multitile\": 3,\n"
            <<"  \"per_tile_r32_hash_matches_legacy_multitile\": true,\n"
            <<"  \"grouped_atlas_passes_per_pose\": "<<batchAtlasPathStats[0].passes<<",\n"
            <<"  \"legacy_multitile_atlas_passes_per_pose\": "<<legacyTileAtlasPathStats[0].passes<<",\n"
            <<"  \"serial_atlas_passes_per_pose\": "<<serialAtlasPathStats[0].passes<<",\n"
            <<"  \"grouped_region_changes_per_pose\": "<<batchAtlasPathStats[0].regionChanges<<",\n"
            <<"  \"serial_reference_uses_per_light_passes\": true,\n"
            <<"  \"positive_unshadowed_and_shadow_difference_signals\": 3,\n"
            <<"  \"rotated_point_face_positive_controls\": 6,\n"
            <<"  \"point_omni_faces\": [0,1,2,3,4,5],\n"
            <<"  \"marker_resets\": "<<batchResets[0]<<",\n"
            <<"  \"atlas_tile_size\": "<<AtlasTile<<",\n"
            <<"  \"shadow_pipeline\": \"converted shadow_direct_base.vs + shadow_direct_base_color.ps\"\n}\n";
        success=static_cast<bool>(report);
        if (!success) error="writing final local shadow batch report failed";
    } while (false);
    backend->Shutdown();
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return success;
}
}

int main(int argc,char** argv)
{
    if (argc!=6||std::string(argv[1])!="--gpu")
    {
        std::fprintf(stderr,"usage: R4LocalShadowBatchGpuProbe --gpu <local-light-msc-dir> <shadow-caster-msc-dir> <light-count> <artifact-dir>\n");
        return 2;
    }
    char* end=nullptr;
    const unsigned long count=std::strtoul(argv[4],&end,10);
    if (end==argv[4]||*end!='\0')
    { std::fprintf(stderr,"light-count must be an integer\n"); return 2; }
    InitializeR4RolloverProbeRuntime();
    std::string error;
    const bool passed=runProbe(argv[2],argv[3],argv[5],static_cast<std::size_t>(count),error);
    std::fprintf(stderr,"E08_LOCAL_SHADOW_BATCH_GPU status=%s lights=%lu detail=\"%s\"\n",
        passed?"PASS":"FAIL",count,error.c_str());
    ShutdownR4RolloverProbeRuntime();
    return passed?0:1;
}
