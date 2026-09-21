#include "stdafx.h"
#include "dxUIRender.h"

#include "dxUIShader.h"
#include "../../Include/xrRender/R4BackendSelection.h"
#if defined(XR_PLATFORM_APPLE) && (RENDER == R_R4)
#include "../xrRenderMetal/MetalR4EngineFs.h"
#endif

dxUIRender UIRenderImpl;

void dxUIRender::CreateUIGeom()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		xray::metal::ui::ResetFrameBatch();
		return;
	}
	hGeom_TL.create(FVF::F_TL, RCache.Vertex.Buffer(), 0);
	hGeom_LIT.create(FVF::F_LIT, RCache.Vertex.Buffer(), 0);
}

void dxUIRender::DestroyUIGeom()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		xray::metal::ui::ResetFrameBatch();
		m_metalVertices.clear();
		m_metalTextureResolutions.clear();
		m_metalPrimitiveOverflow = false;
	}

    for (auto& it : g_UIShadersCache)
        it.second.destroy();
    g_UIShadersCache.clear();

	hGeom_TL = NULL;
	hGeom_LIT = NULL;
}

void dxUIRender::SetShader(IUIShader& shader)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		dxUIShader* pShader = (dxUIShader*)&shader;
		m_metalShader = pShader->MetalIdentity();
		return;
	}
	dxUIShader* pShader = (dxUIShader*)&shader;
	VERIFY(&pShader);
	VERIFY(pShader->hShader);
	RCache.set_Shader(pShader->hShader);
}

void dxUIRender::SetAlphaRef(int aref)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		m_metalAlphaRef = aref;
		return;
	}
	//CHK_DX(HW.pDevice->SetRenderState(D3DRS_ALPHAREF,aref));
	RCache.set_AlphaRef(aref);
}

/*
void dxUIRender::StartTriList(u32 iMaxVerts)
{
	VERIFY(PrimitiveType==ptNone);
	m_PointType = pttLIT;
	m_iMaxVerts = iMaxVerts;
	start_pv	= (FVF::LIT*)RCache.Vertex.Lock	(m_iMaxVerts,hGeom_fan.stride(),vOffset);
	pv			= start_pv;
	PrimitiveType = ptTriList;
}

void dxUIRender::FlushTriList()
{
	VERIFY(PrimitiveType==ptTriList);
	VERIFY(u32(pv-start_pv)<=m_iMaxVerts);

	std::ptrdiff_t p_cnt		= (pv-start_pv)/3;							
	RCache.Vertex.Unlock		(u32(pv-start_pv),hGeom_fan.stride());
	RCache.set_Geometry			(hGeom_fan);
	if (p_cnt!=0)RCache.Render	(D3DPT_TRIANGLELIST,vOffset,u32(p_cnt));

	PrimitiveType = ptNone;
}

void dxUIRender::StartTriFan(u32 iMaxVerts)
{
	VERIFY(PrimitiveType==ptNone);
	m_iMaxVerts = iMaxVerts;
	start_pv	= (FVF::LIT*)RCache.Vertex.Lock	(m_iMaxVerts,hGeom_fan.stride(),vOffset);
	pv			= start_pv;
	PrimitiveType = ptTriFan;
	m_PointType	= pttLIT;

}

void dxUIRender::FlushTriFan()
{
	VERIFY(PrimitiveType==ptTriFan);
	VERIFY(u32(pv-start_pv)<=m_iMaxVerts);

	std::ptrdiff_t p_cnt		= pv-start_pv;
	RCache.Vertex.Unlock		(u32(p_cnt),hGeom_fan.stride());
	RCache.set_Geometry	 		(hGeom_fan);
	if (p_cnt>2) RCache.Render	(D3DPT_TRIANGLEFAN,vOffset,u32(p_cnt-2));

	PrimitiveType = ptNone;
}

void dxUIRender::StartTriStrip(u32 iMaxVerts)
{
	VERIFY(PrimitiveType==ptNone);
	m_iMaxVerts = iMaxVerts;
	start_pv	= (FVF::TL*)RCache.Vertex.Lock	(m_iMaxVerts,hGeom_fan.stride(),vOffset);
	pv			= start_pv;
	PrimitiveType = ptTriStrip;
}

void dxUIRender::FlushTriStrip()
{
}


void dxUIRender::StartLineStrip(u32 iMaxVerts)
{
	VERIFY(PrimitiveType==ptNone);
	m_iMaxVerts = iMaxVerts;
	start_pv	= (FVF::LIT*)RCache.Vertex.Lock	(m_iMaxVerts,hGeom_fan.stride(),vOffset);
	pv			= start_pv;
	PrimitiveType = ptLineStrip;
	m_PointType = pttLIT;
}

void dxUIRender::FlushLineStrip()
{
	VERIFY(PrimitiveType==ptLineStrip);
	VERIFY(u32(pv-start_pv)<=m_iMaxVerts);

	std::ptrdiff_t p_cnt		= pv-start_pv;
	RCache.Vertex.Unlock		(u32(p_cnt),hGeom_fan.stride());
	RCache.set_Geometry	 		(hGeom_fan);
	if (p_cnt>1) RCache.Render	(D3DPT_LINESTRIP,vOffset,u32(p_cnt-1));

	PrimitiveType = ptNone;
}

void dxUIRender::StartLineList(u32 iMaxVerts)
{
	VERIFY(PrimitiveType==ptNone);
	m_iMaxVerts = iMaxVerts;
	start_pv	= (FVF::LIT*)RCache.Vertex.Lock	(m_iMaxVerts,hGeom_fan.stride(),vOffset);
	pv			= start_pv;
	PrimitiveType = ptLineList;
}

void dxUIRender::FlushLineList()
{
	VERIFY(PrimitiveType==ptLineList);
	VERIFY(u32(pv-start_pv)<=m_iMaxVerts);

	std::ptrdiff_t p_cnt		= pv-start_pv;
	RCache.Vertex.Unlock		(u32(p_cnt),hGeom_fan.stride());
	RCache.set_Geometry	 		(hGeom_fan);
	if (p_cnt>1) RCache.Render	(D3DPT_LINELIST,vOffset,u32(p_cnt)/2);

	PrimitiveType = ptNone;
}
*/
void dxUIRender::SetScissor(Irect* rect)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		m_metalScissor.enabled = rect != nullptr;
		if (rect)
		{
			m_metalScissor.x1 = rect->x1;
			m_metalScissor.y1 = rect->y1;
			m_metalScissor.x2 = rect->x2;
			m_metalScissor.y2 = rect->y2;
		}
		return;
	}
#if (RENDER == R_R3) || (RENDER == R_R4)
	RCache.set_Scissor(rect);
	StateManager.OverrideScissoring(rect ? true : false, TRUE);
#else	//	(RENDER == R_R3) || (RENDER == R_R4)
	RCache.set_Scissor(rect);
#endif	//	(RENDER == R_R3) || (RENDER == R_R4)
}

void dxUIRender::GetActiveTextureResolution(Fvector2& res)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		const auto cached = m_metalTextureResolutions.find(m_metalShader.texture);
		if (cached != m_metalTextureResolutions.end())
		{
			res = cached->second;
			return;
		}
		res.set(1.0f, 1.0f);
#if defined(XR_PLATFORM_APPLE) && (RENDER == R_R4)
		if (!m_metalShader.texture.empty())
		{
			std::uint32_t width = 0u;
			std::uint32_t height = 0u;
			std::string error;
			if (LoadMetalR4EngineTextureDimensions(m_metalShader.texture, width, height, error))
			{
				res.set(static_cast<float>(width), static_cast<float>(height));
				m_metalTextureResolutions.emplace(m_metalShader.texture, res);
			}
			else
			{
				Msg("! METAL_UI_TEXTURE_SIZE_SKIP texture=\"%s\" error=\"%s\"",
					m_metalShader.texture.c_str(), error.c_str());
				m_metalTextureResolutions.emplace(m_metalShader.texture, res);
			}
		}
#endif
		return;
	}
	CTexture* T = RCache.get_ActiveTexture(0);
	res.set(float(T->get_Width()), float(T->get_Height()));
}

LPCSTR dxUIRender::UpdateShaderName(LPCSTR tex_name, LPCSTR sh_name)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return sh_name;
	string_path buff;
	u32 v_dev = CAP_VERSION(HW.Caps.raster_major, HW.Caps.raster_minor);
	u32 v_need = CAP_VERSION(2, 0);
	if ((v_dev >= v_need) && FS.exist(buff, "$game_textures$", tex_name, ".ogm"))
		return "hud\\movie";
	else
		return sh_name;
}

/*
void dxUIRender::PushPoint(float x, float y, u32 c, float u, float v)
{
	VERIFY(m_PointType==pttNone);
	pv->set(x, y, 0.0f, c, u, v);
	++pv;
}
*/
/*
void dxUIRender::PushPoint(int x, int y, u32 c, float u, float v)
{
	VERIFY(m_PointType==pttNone);
	pv->set(x, y, 0, c, u, v);
	++pv;
}
*/

void dxUIRender::PushPoint(float x, float y, float z, u32 C, float u, float v)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		if (PrimitiveType == ptNone || m_PointType == pttNone)
			return;
		if (m_metalVertices.size() >= m_iMaxVerts)
		{
			m_metalPrimitiveOverflow = true;
			return;
		}
		m_metalVertices.push_back({x, y, m_PointType == pttTL ? 0.0001f : z, C, u, v});
		return;
	}
	//.	VERIFY(m_PointType==pttLIT);
	switch (m_PointType)
	{
	case pttLIT:
		LIT_pv->set(x, y, z, C, u, v);
		++LIT_pv;
		break;
	case pttTL:
		TL_pv->set(x, y, C, u, v);
		++TL_pv;
		break;
	}
}

void dxUIRender::StartPrimitive(u32 iMaxVerts, ePrimitiveType primType, ePointType pointType)
{
	VERIFY(PrimitiveType==ptNone);
	VERIFY(m_PointType==pttNone);
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		m_iMaxVerts = iMaxVerts;
		PrimitiveType = primType;
		m_PointType = pointType;
		m_metalPrimitiveOverflow = iMaxVerts > xray::metal::ui::FrameCapture::MaxVertices;
		m_metalVertices.clear();
		if (!m_metalPrimitiveOverflow)
			m_metalVertices.reserve(iMaxVerts);
		return;
	}
	//.	R_ASSERT(pointType==pttLIT);

	m_iMaxVerts = iMaxVerts;
	PrimitiveType = primType;
	m_PointType = pointType;

	switch (m_PointType)
	{
	case pttLIT:
		LIT_start_pv = (FVF::LIT*)RCache.Vertex.Lock(m_iMaxVerts, hGeom_LIT.stride(), vOffset);
		LIT_pv = LIT_start_pv;
		break;
	case pttTL:
		TL_start_pv = (FVF::TL*)RCache.Vertex.Lock(m_iMaxVerts, hGeom_TL.stride(), vOffset);
		TL_pv = TL_start_pv;
		break;
	}
}

void dxUIRender::FlushPrimitive()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		if (!m_metalPrimitiveOverflow)
		{
			xray::metal::ui::DrawRecord record;
			record.shader = m_metalShader;
			record.pointType = m_PointType == pttLIT ? xray::metal::ui::PointType::LIT : xray::metal::ui::PointType::TL;
			record.scissor = m_metalScissor;
			record.alphaRef = m_metalAlphaRef;
			record.cullMode = m_metalCullMode;
			switch (PrimitiveType)
			{
			case ptTriList:
				record.primitive = xray::metal::ui::PrimitiveType::TriangleList;
				break;
			case ptTriStrip:
				record.primitive = xray::metal::ui::PrimitiveType::TriangleStrip;
				break;
			case ptLineStrip:
				record.primitive = xray::metal::ui::PrimitiveType::LineStrip;
				break;
			case ptLineList:
				record.primitive = xray::metal::ui::PrimitiveType::LineList;
				break;
			default:
				m_metalPrimitiveOverflow = true;
				break;
			}
			const u32 vertexCount = static_cast<u32>(m_metalVertices.size());
			const bool primitiveValid =
				(PrimitiveType == ptTriList && vertexCount >= 3u) ||
				(PrimitiveType == ptTriStrip && vertexCount >= 3u) ||
				(PrimitiveType == ptLineStrip && vertexCount >= 2u) ||
				(PrimitiveType == ptLineList && vertexCount >= 2u);
			if (!m_metalPrimitiveOverflow && primitiveValid)
				xray::metal::ui::AppendSequentialDraw(std::move(record), m_metalVertices);
		}
		else
			xray::metal::ui::RecordDroppedDraw(m_iMaxVerts);
		m_metalVertices.clear();
		PrimitiveType = ptNone;
		m_PointType = pttNone;
		return;
	}
	u32 primCount = 0;
	_D3DPRIMITIVETYPE d3dPrimType = D3DPT_FORCE_DWORD;
	std::ptrdiff_t p_cnt = 0;

	switch (m_PointType)
	{
	case pttLIT:
		p_cnt = LIT_pv - LIT_start_pv;
		VERIFY(u32(p_cnt)<=m_iMaxVerts);

		RCache.Vertex.Unlock(u32(p_cnt), hGeom_LIT.stride());
		RCache.set_Geometry(hGeom_LIT);
		break;
	case pttTL:
		p_cnt = TL_pv - TL_start_pv;
		VERIFY(u32(p_cnt)<=m_iMaxVerts);

		RCache.Vertex.Unlock(u32(p_cnt), hGeom_TL.stride());
		RCache.set_Geometry(hGeom_TL);
		break;
	default:
		NODEFAULT;
	}

	//	Update data for primitive type
	switch (PrimitiveType)
	{
	case ptTriStrip:
		primCount = (u32)(p_cnt - 2);
		d3dPrimType = D3DPT_TRIANGLESTRIP;
		break;
	case ptTriList:
		primCount = (u32)(p_cnt / 3);
		d3dPrimType = D3DPT_TRIANGLELIST;
		break;
	case ptLineStrip:
		primCount = (u32)(p_cnt - 1);
		d3dPrimType = D3DPT_LINESTRIP;
		break;
	case ptLineList:
		primCount = (u32)(p_cnt / 2);
		d3dPrimType = D3DPT_LINELIST;
		break;
	default:
		NODEFAULT;
	}

	if (primCount > 0)
		RCache.Render(d3dPrimType, vOffset, primCount);

	PrimitiveType = ptNone;
	m_PointType = pttNone;
}

void dxUIRender::CacheSetXformWorld(const Fmatrix& M)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	RCache.set_xform_world(M);
}

void dxUIRender::CacheSetCullMode(CullMode m)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		m_metalCullMode = static_cast<std::uint8_t>(m);
		return;
	}
	RCache.set_CullMode(CULL_NONE + m);
}
