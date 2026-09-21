// dxRender_Visual.cpp: implementation of the dxRender_Visual class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#ifndef _EDITOR
#	include "../../xrEngine/render.h"
#endif // #ifndef _EDITOR

#include "fbasicvisual.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/bone.h"
#include "dxRenderDeviceRender.h"

#include <limits>

namespace
{
u32 cpuFvfVertexStride(u32 fvf)
{
	constexpr u32 PositionMask = 0x000e;
	constexpr u32 XYZ = 0x0002;
	constexpr u32 XYZRHW = 0x0004;
	constexpr u32 XYZB1 = 0x0006;
	constexpr u32 XYZB2 = 0x0008;
	constexpr u32 XYZB3 = 0x000a;
	constexpr u32 XYZB4 = 0x000c;
	constexpr u32 XYZB5 = 0x000e;
	constexpr u32 Normal = 0x0010;
	constexpr u32 PSize = 0x0020;
	constexpr u32 Diffuse = 0x0040;
	constexpr u32 Specular = 0x0080;
	constexpr u32 TexCountMask = 0x0f00;
	constexpr u32 TexCountShift = 8;

	u32 stride = 0;
	switch (fvf & PositionMask)
	{
	case XYZ: stride = 12; break;
	case XYZRHW: stride = 16; break;
	case XYZB1: stride = 16; break;
	case XYZB2: stride = 20; break;
	case XYZB3: stride = 24; break;
	case XYZB4: stride = 28; break;
	case XYZB5: stride = 32; break;
	default: return 0;
	}
	if (fvf & Normal) stride += 12;
	if (fvf & PSize) stride += 4;
	if (fvf & Diffuse) stride += 4;
	if (fvf & Specular) stride += 4;
	const u32 texCount = (fvf & TexCountMask) >> TexCountShift;
	if (texCount > 8)
		return 0;
	for (u32 index = 0; index < texCount; ++index)
	{
		const u32 dimension = (fvf >> (16 + index * 2)) & 0x3;
		stride += dimension == 0 ? 8 : dimension == 1 ? 4 : dimension == 2 ? 8 : 16;
	}
	return stride;
}

u32 cpuSkinnedVertexStride(u32 format)
{
	switch (format)
	{
	case OGF_VERTEXFORMAT_FVF_1L:
	case 1: return sizeof(vertBoned1W);
	case OGF_VERTEXFORMAT_FVF_2L:
	case 2: return sizeof(vertBoned2W);
	case OGF_VERTEXFORMAT_FVF_3L:
	case 3: return sizeof(vertBoned3W);
	case OGF_VERTEXFORMAT_FVF_4L:
	case 4: return sizeof(vertBoned4W);
	default: return 0;
	}
}

u64 cpuHashAppend(u64 hash, const void* bytes, size_t size)
{
	const u8* data = static_cast<const u8*>(bytes);
	for (size_t index = 0; index < size; ++index)
	{
		hash ^= data[index];
		hash *= 1099511628211ull;
	}
	return hash;
}

bool captureCpuLeafGeometry(IReader& reader, MetalCpuVisualAsset& asset)
{
	ogf_header header{};
	if (!reader.r_chunk_safe(OGF_HEADER, &header, sizeof(header)) ||
		header.format_version != xrOGF_FormatVersion)
		return false;

	MetalCpuGeometry geometry;
	geometry.visualType = header.type;
	geometry.skinned = header.type == MT_SKELETON_GEOMDEF_PM || header.type == MT_SKELETON_GEOMDEF_ST;
	if (reader.find_chunk(OGF_TEXTURE))
	{
		reader.r_stringZ(geometry.textureName);
		reader.r_stringZ(geometry.shaderName);
	}
	asset.contentHash = cpuHashAppend(asset.contentHash, &geometry.visualType, sizeof(geometry.visualType));
	asset.contentHash = cpuHashAppend(asset.contentHash, geometry.textureName.c_str(), geometry.textureName.size());
	asset.contentHash = cpuHashAppend(asset.contentHash, geometry.shaderName.c_str(), geometry.shaderName.size());

	if (!reader.find_chunk(OGF_VERTICES))
		return false;
	geometry.vertexFormat = reader.r_u32();
	geometry.vertexCount = reader.r_u32();
	geometry.vertexStride = geometry.skinned ? cpuSkinnedVertexStride(geometry.vertexFormat) :
		cpuFvfVertexStride(geometry.vertexFormat);
	if (geometry.vertexStride == 0 ||
		geometry.vertexCount > std::numeric_limits<u32>::max() / geometry.vertexStride)
		return false;
	const u32 vertexBytes = geometry.vertexCount * geometry.vertexStride;
	if (reader.elapsed() < static_cast<int>(vertexBytes))
		return false;
	auto vertices = std::make_shared<std::vector<u8>>(vertexBytes);
	if (vertexBytes)
		reader.r(vertices->data(), vertexBytes);
	geometry.vertexBytes = std::move(vertices);

	if (!reader.find_chunk(OGF_INDICES))
		return false;
	geometry.indexCount = reader.r_u32();
	if (geometry.indexCount > std::numeric_limits<u32>::max() / sizeof(u16) ||
		reader.elapsed() < static_cast<int>(geometry.indexCount * sizeof(u16)))
		return false;
	auto indices = std::make_shared<std::vector<u16>>(geometry.indexCount);
	if (geometry.indexCount)
		reader.r(indices->data(), geometry.indexCount * sizeof(u16));
	geometry.indexData = std::move(indices);
	asset.contentHash = cpuHashAppend(asset.contentHash, geometry.vertexBytes->data(), geometry.vertexBytes->size());
	asset.contentHash = cpuHashAppend(asset.contentHash, geometry.indexData->data(), geometry.indexData->size() * sizeof(u16));
	asset.geometry.push_back(std::move(geometry));
	return true;
}

void captureCpuGeometry(IReader& reader, MetalCpuVisualAsset& asset)
{
	if (reader.find_chunk(OGF_CHILDREN_L))
	{
		asset.hasUnresolvedGeometryLinks = true;
		return;
	}
	IReader* children = reader.open_chunk(OGF_CHILDREN);
	if (!children)
	{
		captureCpuLeafGeometry(reader, asset);
		return;
	}
	for (u32 index = 0;; ++index)
	{
		IReader* child = children->open_chunk(index);
		if (!child)
			break;
		captureCpuGeometry(*child, asset);
		child->close();
	}
	children->close();
}
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

IRender_Mesh::~IRender_Mesh()
{
	_RELEASE(p_rm_Vertices);
	_RELEASE(p_rm_Indices);
}

dxRender_Visual::dxRender_Visual()
{
	Type = 0;
	shader = 0;
	vis.clear();
	cpu_only = false;
	metalHeatVisionHot = false;
}

dxRender_Visual::~dxRender_Visual()
{
}

void dxRender_Visual::Release()
{
}

//CStatTimer						tscreate;

void dxRender_Visual::Load(const char* N, IReader* data, u32 dwFlags)
{
	dbg_name = N;
	dbg_id = 1;
	cpu_only = (dwFlags & VLOAD_CPU_ONLY) != 0;
	skinning = Engine.External.GetSkinningMode();
	hud = ::Render->hud_loading;

	// header
	VERIFY(data);
	ogf_header hdr;
	if (data->r_chunk_safe(OGF_HEADER, &hdr, sizeof(hdr)))
	{
		R_ASSERT2(hdr.format_version==xrOGF_FormatVersion, "Invalid visual version");
		Type = hdr.type;
		//if (hdr.shader_id)	shader	= ::Render->getShader	(hdr.shader_id);
		if (!cpu_only && hdr.shader_id) shader = ::RImplementation.getShader(hdr.shader_id);
		vis.box.set(hdr.bb.min, hdr.bb.max);
		vis.sphere.set(hdr.bs.c, hdr.bs.r);
	}
	else
	{
		FATAL("Invalid visual");
	}

	// Shader
	if (data->find_chunk(OGF_TEXTURE))
	{
		string256 fnT, fnS;
		data->r_stringZ(fnT, sizeof(fnT));
		data->r_stringZ(fnS, sizeof(fnS));
		dbg_shader_def = fnS;
		dbg_texture_def = fnT;
		ResetShaderTexture();
	}
	if (cpu_only)
	{
		auto asset = std::make_shared<MetalCpuVisualAsset>();
		asset->sourceName = N;
		asset->contentHash = cpuHashAppend(1469598103934665603ull, N, xr_strlen(N));
		captureCpuGeometry(*data, *asset);
		metalCpuAsset = std::move(asset);
		Msg("* METAL_CPU_ASSET_READY model=%s geometry=%u unresolved_links=%u", N,
		    u32(metalCpuAsset->geometry.size()), metalCpuAsset->hasUnresolvedGeometryLinks ? 1u : 0u);
	}

	// desc
#ifdef _EDITOR
    if (data->find_chunk(OGF_S_DESC)) 
	    desc.Load		(*data);
#endif
}

//--DSR-- HeatVision_start
CTexture* dxRender_Visual::GetTexture()
{
	Shader* pSh = shader._get();
	if (pSh == 0) 
		return 0;

	ShaderElement* pShE = pSh->E[0]._get();
	if (pShE == 0 || pShE->passes.empty()) 
		return 0;

	SPass* pPass = pShE->passes[0]._get();
	if (pPass == 0)
		return 0;

	STextureList* pTexList = pPass->T._get();
	if (pTexList == 0 || pTexList->empty()) 
		return 0;

	return pTexList->at(0).second._get();
}

void dxRender_Visual::MarkAsHot(bool is_hot) 
{
	// The legacy renderer gates L_hotness through CTexture::m_is_hot. Direct
	// Metal's CPU-only model pool has no CTexture, so retain the same authoring
	// decision as a value-only visual property for its later world capture.
	metalHeatVisionHot = is_hot;
	auto texture = GetTexture();
	if (texture)
		texture->m_is_hot = is_hot;
}
//--DSR-- HeatVision_end

//--DSR-- SilencerOverheat_start
void dxRender_Visual::MarkAsGlowing(bool is_glowing)
{
	auto texture = GetTexture();
	if (texture)
		texture->m_is_glowing = is_glowing;
}
//--DSR-- SilencerOverheat_end

void dxRender_Visual::SetShaderTexture(LPCSTR s_shader, LPCSTR s_texture)
{
	if (s_shader && strlen(s_shader))
	{
		char* shader = xr_strdup(s_shader);
		char* no_shadow = strstr(shader, "$no_shadows");

		if (no_shadow)
		{
			flags.set(IRenderVisualFlags::eNoShadow, TRUE);
			*no_shadow = 0;
		}
		else
			flags.set(IRenderVisualFlags::eNoShadow, FALSE);

		dbg_shader = shader;
		xr_delete(shader);
	}

	if (s_texture && strlen(s_texture))
	{
		dbg_texture = s_texture;
	}

	if (cpu_only)
		return;

	Engine.External.SetSkinningMode(skinning);
    	bool prev_hud = ::Render->hud_loading;
    	::Render->hud_loading = hud;
    	shader.create(*dbg_shader, *dbg_texture);
    	::Render->hud_loading = prev_hud;
}

void dxRender_Visual::ResetShaderTexture()
{
	if (!dbg_shader.equal(dbg_shader_def) || !dbg_texture.equal(dbg_texture_def))
		SetShaderTexture(*dbg_shader_def, *dbg_texture_def);
}

#define PCOPY(a)	a = pFrom->a

void dxRender_Visual::Copy(dxRender_Visual* pFrom)
{
	PCOPY(Type);
	PCOPY(shader);
	PCOPY(vis);
#ifdef _EDITOR
	PCOPY(desc);
#endif
	PCOPY(flags);
	PCOPY(dbg_id);
	PCOPY(dbg_name);
	PCOPY(dbg_shader);
	PCOPY(dbg_shader_def);
	PCOPY(dbg_texture);
	PCOPY(dbg_texture_def);
	PCOPY(skinning);
    PCOPY(hud);
	PCOPY(cpu_only);
	PCOPY(metalHeatVisionHot);
	PCOPY(metalCpuAsset);
}
