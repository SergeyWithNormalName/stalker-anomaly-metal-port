#include "MetalR4DefObjectsLodAdapter.h"

#include "MetalR4EngineFs.h"
#include "R4DefObjectsLodFoundation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr std::size_t kVsGlobals = 160u, kTransforms = 256u, kPsGlobals = 176u,
    kStaticGlobals = 528u;
struct Matrix4 { float values[16]{}; };

Matrix4 mul(const Matrix4& a, const Matrix4& b)
{
    Matrix4 r;
    for (std::size_t c = 0; c != 4u; ++c)
        for (std::size_t row = 0; row != 4u; ++row)
            for (std::size_t k = 0; k != 4u; ++k)
                r.values[c * 4u + row] += a.values[k * 4u + row] * b.values[c * 4u + k];
    return r;
}

bool outside(const xray::metal::R4PositionBounds& b, const Matrix4& m)
{
    if (!b.available) return false;
    for (float v : b.minimum) if (!std::isfinite(v)) return false;
    for (float v : b.maximum) if (!std::isfinite(v)) return false;
    for (float v : m.values) if (!std::isfinite(v)) return false;
    std::array<bool, 6> reject = {true, true, true, true, true, true};
    for (std::size_t i = 0; i != 8u; ++i)
    {
        const float x = i & 1u ? b.maximum[0] : b.minimum[0];
        const float y = i & 2u ? b.maximum[1] : b.minimum[1];
        const float z = i & 4u ? b.maximum[2] : b.minimum[2];
        const float X = m.values[0]*x + m.values[4]*y + m.values[8]*z + m.values[12];
        const float Y = m.values[1]*x + m.values[5]*y + m.values[9]*z + m.values[13];
        const float Z = m.values[2]*x + m.values[6]*y + m.values[10]*z + m.values[14];
        const float W = m.values[3]*x + m.values[7]*y + m.values[11]*z + m.values[15];
        if (!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z) || !std::isfinite(W)) return false;
        if (X + W >= 0) reject[0] = false;
        if (W - X >= 0) reject[1] = false;
        if (Y + W >= 0) reject[2] = false;
        if (W - Y >= 0) reject[3] = false;
        if (Z >= 0) reject[4] = false;
        if (W - Z >= 0) reject[5] = false;
    }
    return std::any_of(reject.begin(), reject.end(), [](bool value) { return value; });
}

bool sourceDds(const std::string& identity, std::filesystem::path& path, std::string& error)
{
    if (identity.empty() || identity.find("..") != std::string::npos) { error = "unsafe B_VERT base texture"; return false; }
    std::string name = identity;
    std::replace(name.begin(), name.end(), '\\', '/');
    path = name;
    if (path.is_absolute() || !path.extension().empty()) { error = "B_VERT base texture must be relative and extensionless"; return false; }
    path += ".dds";
    return true;
}

bool uniform(IR4Resources& resources, std::size_t bytes, const char* label,
    std::shared_ptr<IR4Buffer>& result, std::string& error)
{
    R4BufferDesc d; d.size = bytes; d.storage = R4ResourceStorage::Shared; d.usage = R4BufferUsage::Uniform; d.label = label;
    result = resources.CreateBuffer(d, nullptr, 0u, error);
    return static_cast<bool>(result);
}
} // namespace

struct MetalR4DefObjectsLodAdapter::Impl
{
    struct Texture { xray::metal::R4DdsTexture dds; std::shared_ptr<IR4Texture> gpu; };
    enum class Variant { Flat, Bump, Steep };
    struct Draw
    {
        Variant variant = Variant::Flat;
        std::uint32_t vertexBase = 0, indexBase = 0, indexCount = 0;
        Matrix4 model{};
        xray::metal::R4PositionBounds bounds;
        std::shared_ptr<IR4Buffer> vb, ib, vsGlobals, transforms, psGlobals;
        std::shared_ptr<IR4Texture> base, bump, bumpX;
    };
    std::shared_ptr<IR4GraphicsPipeline> flatPipeline, bumpPipeline, steepPipeline;
    std::shared_ptr<IR4Sampler> sampler;
    std::vector<Texture> textures;
    std::vector<Draw> draws;
    bool uploaded = false;
};

MetalR4DefObjectsLodAdapter::MetalR4DefObjectsLodAdapter() = default;
MetalR4DefObjectsLodAdapter::~MetalR4DefObjectsLodAdapter() = default;

bool MetalR4DefObjectsLodAdapter::Prepare(IR4Backend& backend,
    const xray::metal::R4LevelGeometry& geometry,
    const std::vector<std::shared_ptr<IR4Buffer>>& vbs,
    const std::vector<std::shared_ptr<IR4Buffer>>& ibs,
    const Options& options, std::string& status)
{
    m_ready = false; m_status.clear(); m_telemetry = {}; m_impl = std::make_unique<Impl>();
    if (options.shaderDirectory.empty() || !options.ddsBytesProvider) { m_status = "B_VERT adapter requires MSC directory and XRFS provider"; status=m_status; return true; }
    std::string error;
    if (!xray::metal::ValidateR4DefObjectsLodFlatMscBundle(options.shaderDirectory, error) ||
        !xray::metal::ValidateR4DefObjectsLodBumpMscBundle(options.shaderDirectory, error) ||
        !xray::metal::ValidateR4DefObjectsLodSteepMscBundle(options.shaderDirectory, error)) { m_status=error; status=m_status; return true; }
    const auto pipeline = [&backend, &error](const char* label, const char* key,
        const std::filesystem::path& vs, const std::filesystem::path& ps) {
        const std::string v = vs.string(), p = ps.string();
        R4GraphicsPipelineDesc d; d.label=label; d.shaderKey=key; d.vertexMetallibPath=v.c_str(); d.fragmentMetallibPath=p.c_str();
        d.vertexAttributes={{11u,6u,12u,R4VertexFormat::UChar4Normalized},{12u,6u,16u,R4VertexFormat::UChar4Normalized},
            {13u,6u,20u,R4VertexFormat::UChar4Normalized},{14u,6u,24u,R4VertexFormat::Short2},{15u,6u,0u,R4VertexFormat::Float3}};
        d.vertexLayouts={{6u,32u}}; d.colorFormats={R4PixelFormat::RGBA16Float,R4PixelFormat::RGBA8Unorm,R4PixelFormat::RGBA8Unorm,R4PixelFormat::RGBA16Float};
        d.depthFormat=R4PixelFormat::Depth32FloatStencil8; d.depthCompare=R4CompareFunction::Less; d.depthWrite=true; d.cullMode=R4CullMode::Back; d.frontFace=R4Winding::Clockwise;
        d.stencil.enabled=true; d.stencil.frontFace.compare=R4CompareFunction::Always; d.stencil.frontFace.depthStencilPass=R4StencilOperation::Replace;
        d.stencil.frontFace.readMask=0xffu; d.stencil.frontFace.writeMask=0x7fu; d.stencil.backFace=d.stencil.frontFace;
        return backend.CreateGraphicsPipeline(d,error);
    };
    m_impl->flatPipeline = pipeline("R4 B_VERT flat MSC G-buffer", "r4-b-vert-flat-msc-v1",
        options.shaderDirectory / "deffer_base_flat_lod.vs.metallib", options.shaderDirectory / "deffer_base_flat_lod.ps.metallib");
    m_impl->bumpPipeline = pipeline("R4 B_VERT bump MSC G-buffer", "r4-b-vert-bump-msc-v1",
        options.shaderDirectory / "deffer_base_bump_lod.vs.metallib", options.shaderDirectory / "deffer_base_bump_lod.ps.metallib");
    m_impl->steepPipeline = pipeline("R4 B_VERT steep HQ MSC G-buffer", "r4-b-vert-steep-hq-msc-v1",
        options.shaderDirectory / "deffer_base_steep_hq_lod.vs.metallib", options.shaderDirectory / "deffer_base_steep_hq_lod.ps.metallib");
    if (!m_impl->flatPipeline || !m_impl->bumpPipeline || !m_impl->steepPipeline) { m_status="B_VERT MSC pipeline creation failed: "+error; status=m_status; return true; }
    R4SamplerDesc sampler; sampler.minFilter=R4SamplerFilter::Linear; sampler.magFilter=R4SamplerFilter::Linear; sampler.mipFilter=R4SamplerFilter::Linear;
    sampler.addressU=R4SamplerAddressMode::Repeat; sampler.addressV=R4SamplerAddressMode::Repeat; sampler.maxAnisotropy=8; sampler.label="R4 B_VERT MSC sampler";
    m_impl->sampler=backend.Resources().CreateSampler(sampler,error);
    if (!m_impl->sampler) { m_status="B_VERT sampler creation failed: "+error; status=m_status; return true; }
    std::unordered_map<std::string,std::shared_ptr<IR4Texture>> cache;
    const auto load = [&](const std::filesystem::path& path, std::shared_ptr<IR4Texture>& out) -> bool {
        const auto found=cache.find(path.generic_string()); if(found!=cache.end()){out=found->second; return true;}
        Impl::Texture asset;
        if(!xray::metal::LoadR4DdsTexture(path,options.ddsBytesProvider,asset.dds,error) || asset.dds.descriptor.type!=R4TextureType::Texture2D || asset.dds.descriptor.arrayLength!=1u) { if(error.empty())error="B_VERT texture is not a 2D DDS"; return false; }
        asset.dds.descriptor.label="R4 B_VERT XRFS texture"; asset.gpu=backend.Resources().CreateTexture2D(asset.dds.descriptor,error); if(!asset.gpu)return false;
        out=asset.gpu; cache.emplace(path.generic_string(),out); m_impl->textures.emplace_back(std::move(asset)); return true;
    };
    std::unordered_set<std::uint32_t> unique;
    for(const auto& visual:geometry.visuals)
    {
        if(visual.shaderId>=geometry.shaders.size() || !xray::metal::IsR4DefObjectsLodShaderFamily(geometry.shaders[visual.shaderId].shaderName)) continue;
        ++m_telemetry.requestedVisuals;
        if(!unique.emplace(visual.visualId).second || !visual.valid || !visual.referencedPositionBounds.available || visual.vertexBufferId>=geometry.vertexBuffers.size() || visual.indexBufferId>=geometry.indexBuffers.size() || visual.vertexBufferId>=vbs.size() || visual.indexBufferId>=ibs.size() || !vbs[visual.vertexBufferId] || !ibs[visual.indexBufferId] || visual.vertexCount==0u || visual.indexCount==0u || visual.indexCount%3u!=0u || !xray::metal::ValidateR4DefObjectsLodVertexAbi(geometry.vertexBuffers[visual.vertexBufferId],error)) { ++m_telemetry.rejectedVisuals; if(error.empty())error="invalid B_VERT geometry"; break; }
        const auto& sourceVertices=geometry.vertexBuffers[visual.vertexBufferId]; const auto& sourceIndices=geometry.indexBuffers[visual.indexBufferId];
        if(visual.vertexBase>sourceVertices.vertexCount || visual.vertexCount>sourceVertices.vertexCount-visual.vertexBase || visual.indexBase>sourceIndices.indexCount || visual.indexCount>sourceIndices.indexCount-visual.indexBase){++m_telemetry.rejectedVisuals;error="B_VERT visual range exceeds source level buffer";break;}
        std::filesystem::path baseIdentity, basePath, bumpPath, bumpXPath; if(!sourceDds(geometry.shaders[visual.shaderId].baseTexture,baseIdentity,error) || !ResolveMetalR4EngineTextureDdsPath(baseIdentity,basePath,error)) {++m_telemetry.rejectedVisuals;break;}
        R4EngineTextureBumpKind kind; if(!ClassifyMetalR4EngineTextureBump(baseIdentity,kind,bumpPath,bumpXPath,error)) {++m_telemetry.rejectedVisuals;break;}
        Impl::Draw d; d.variant=kind==R4EngineTextureBumpKind::Steep?Impl::Variant::Steep:(kind==R4EngineTextureBumpKind::Bump?Impl::Variant::Bump:Impl::Variant::Flat); d.vertexBase=visual.vertexBase; d.indexBase=visual.indexBase; d.indexCount=visual.indexCount; d.bounds=visual.referencedPositionBounds; std::memcpy(d.model.values,visual.modelTransform.data(),sizeof(d.model.values)); d.vb=vbs[visual.vertexBufferId]; d.ib=ibs[visual.indexBufferId];
        if(!load(basePath,d.base) || (d.variant!=Impl::Variant::Flat && !load(bumpPath,d.bump)) || (d.variant==Impl::Variant::Steep && !load(bumpXPath,d.bumpX)) || !uniform(backend.Resources(),kVsGlobals,"R4 B_VERT VS globals",d.vsGlobals,error) || !uniform(backend.Resources(),kTransforms,"R4 B_VERT transforms",d.transforms,error) || !uniform(backend.Resources(),kPsGlobals,"R4 B_VERT PS globals",d.psGlobals,error)){++m_telemetry.rejectedVisuals;break;}
        d.variant==Impl::Variant::Steep ? ++m_telemetry.steepVisuals : (d.variant==Impl::Variant::Bump ? ++m_telemetry.bumpVisuals : ++m_telemetry.flatVisuals); m_impl->draws.emplace_back(std::move(d));
    }
    m_telemetry.textureAssets=m_impl->textures.size();
    if(m_telemetry.rejectedVisuals || (m_telemetry.requestedVisuals > 0u && m_impl->draws.size()!=m_telemetry.requestedVisuals)){if(error.empty())error="B_VERT configured-level classification/admission is incomplete";m_status=error;m_impl.reset();status=m_status;return true;}
    m_ready=true;m_status="B_VERT flat/bump/steep MSC adapter ready";status=m_status;return true;
}

bool MetalR4DefObjectsLodAdapter::Upload(IR4Backend& backend,std::string& error)
{
    if(!m_ready||!m_impl||m_impl->uploaded)return true;
    for (auto& asset : m_impl->textures)
    {
        for (const auto& mip : asset.dds.mipLevels)
        {
            if (mip.arraySlice || mip.dataOffset > asset.dds.bytes.size() ||
                mip.bytesPerImage > asset.dds.bytes.size() - mip.dataOffset ||
                !backend.Resources().UploadTexture2D(asset.gpu, mip.mipLevel, 0u,
                    asset.dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                    mip.bytesPerRow, mip.bytesPerImage, error))
                return false;
            m_telemetry.uploadedBytes += mip.bytesPerImage;
        }
    }
    for(auto& asset:m_impl->textures){asset.dds.bytes.clear();asset.dds.bytes.shrink_to_fit();}
    m_impl->uploaded=true;std::fprintf(stderr,"METAL_B_VERT_UPLOAD visuals=%zu flat=%zu bump=%zu steep=%zu textures=%zu bytes=%llu\n",m_telemetry.requestedVisuals,m_telemetry.flatVisuals,m_telemetry.bumpVisuals,m_telemetry.steepVisuals,m_telemetry.textureAssets,static_cast<unsigned long long>(m_telemetry.uploadedBytes));return true;
}

bool MetalR4DefObjectsLodAdapter::DrawIntoActiveGBuffer(IR4Backend& backend,const R4DefObjectsLodViewState& view,std::string& error)
{
    if(!m_ready||!m_impl||!m_impl->uploaded)return true;
    if (m_telemetry.steepVisuals && !view.staticGlobals) { error="B_VERT steep parallax requires live static_globals b2"; return false; }
    Matrix4 v,p;std::memcpy(v.values,view.view.data(),sizeof(v.values));std::memcpy(p.values,view.projection.data(),sizeof(p.values));const Matrix4 vp=mul(p,v);m_telemetry.submittedVisuals=m_telemetry.culledVisuals=0;std::shared_ptr<IR4Buffer> last; Impl::Variant lastVariant=Impl::Variant::Flat; bool hasVariant=false;
    for(auto& d:m_impl->draws){const Matrix4 wvp=mul(vp,d.model);if(view.valid&&outside(d.bounds,wvp)){++m_telemetry.culledVisuals;continue;}const Matrix4 wv=mul(v,d.model);std::array<std::uint8_t,kVsGlobals> vg{};std::array<std::uint8_t,kTransforms> tr{};std::array<std::uint8_t,kPsGlobals> pg{};std::memcpy(vg.data(),&wvp,sizeof(wvp));std::memcpy(vg.data()+64u,&vp,sizeof(vp));std::memcpy(tr.data(),&wvp,sizeof(wvp));std::memcpy(tr.data()+64u,&wv,sizeof(wv));std::memcpy(tr.data()+128u,&d.model,sizeof(d.model));const float material[4]={0,0,0,.25f};std::memcpy(tr.data()+192u,material,sizeof(material));std::memcpy(pg.data(),&wvp,sizeof(wvp));std::memcpy(pg.data()+64u,&vp,sizeof(vp));if(!backend.Resources().UpdateBuffer(d.vsGlobals,0,vg.data(),vg.size(),error)||!backend.Resources().UpdateBuffer(d.transforms,0,tr.data(),tr.size(),error)||!backend.Resources().UpdateBuffer(d.psGlobals,0,pg.data(),pg.size(),error))return false;
        if(!hasVariant||lastVariant!=d.variant){const auto& pipeline=d.variant==Impl::Variant::Flat?m_impl->flatPipeline:(d.variant==Impl::Variant::Bump?m_impl->bumpPipeline:m_impl->steepPipeline);if(!backend.BindGraphicsPipeline(pipeline,error))return false;lastVariant=d.variant;hasVariant=true;}
        R4MscStageBindings vs{R4ShaderStage::Vertex,48u,{{R4MscResourceType::ConstantBuffer,0u,d.vsGlobals,0u,kVsGlobals},{R4MscResourceType::ConstantBuffer,24u,d.transforms,0u,kTransforms}}};R4MscStageBindings ps;ps.stage=R4ShaderStage::Fragment;
        if(d.variant==Impl::Variant::Flat)ps={R4ShaderStage::Fragment,96u,{{R4MscResourceType::Texture,0u,nullptr,0u,0u,d.base},{R4MscResourceType::ConstantBuffer,24u,d.psGlobals,0u,kPsGlobals},{R4MscResourceType::ConstantBuffer,48u,d.transforms,0u,kTransforms},{R4MscResourceType::Sampler,72u,nullptr,0u,0u,nullptr,m_impl->sampler}}};
        else if(d.variant==Impl::Variant::Bump)ps={R4ShaderStage::Fragment,120u,{{R4MscResourceType::Texture,0u,nullptr,0u,0u,d.base},{R4MscResourceType::Texture,24u,nullptr,0u,0u,d.bump},{R4MscResourceType::ConstantBuffer,48u,d.psGlobals,0u,kPsGlobals},{R4MscResourceType::ConstantBuffer,72u,d.transforms,0u,kTransforms},{R4MscResourceType::Sampler,96u,nullptr,0u,0u,nullptr,m_impl->sampler}}};
        else ps={R4ShaderStage::Fragment,168u,{{R4MscResourceType::Texture,0u,nullptr,0u,0u,d.base},{R4MscResourceType::Texture,24u,nullptr,0u,0u,d.bump},{R4MscResourceType::Texture,48u,nullptr,0u,0u,d.bumpX},{R4MscResourceType::ConstantBuffer,72u,d.psGlobals,0u,kPsGlobals},{R4MscResourceType::ConstantBuffer,96u,d.transforms,0u,kTransforms},{R4MscResourceType::ConstantBuffer,120u,view.staticGlobals,0u,kStaticGlobals},{R4MscResourceType::Sampler,144u,nullptr,0u,0u,nullptr,m_impl->sampler}}};
        if(!backend.BindMscResources(vs,error)||!backend.BindMscResources(ps,error))return false;if(d.vb!=last){if(!backend.BindVertexBuffer(6u,d.vb,0u,error))return false;last=d.vb;}R4IndexedDrawDesc draw;draw.indexBuffer=d.ib;draw.indexBufferOffset=static_cast<std::size_t>(d.indexBase)*sizeof(std::uint16_t);draw.indexCount=d.indexCount;draw.indexType=R4IndexType::UInt16;draw.baseVertex=static_cast<std::int32_t>(d.vertexBase);if(!backend.DrawIndexed(draw,error))return false;++m_telemetry.submittedVisuals;}
    return true;
}

bool RunMetalR4DefObjectsLodAdapterSelfTest(std::string& error)
{
    xray::metal::R4PositionBounds b; b.available=true;b.minimum={-.5f,-.5f,.2f};b.maximum={.5f,.5f,.8f};Matrix4 i;i.values[0]=i.values[5]=i.values[10]=i.values[15]=1;Matrix4 right=i;right.values[12]=3;Matrix4 bad=i;bad.values[0]=std::numeric_limits<float>::quiet_NaN();if(outside(b,i)||!outside(b,right)||outside(b,bad)){error="B_VERT culling is not conservative";return false;}std::filesystem::path path;if(sourceDds("../bad",path,error)){error="B_VERT texture parser accepted traversal";return false;}return true;
}
