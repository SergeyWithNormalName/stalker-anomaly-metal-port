#ifndef dxFontRender_included
#define dxFontRender_included
#pragma once

#include "..\..\Include\xrRender\FontRender.h"
#include "MetalUICapture.h"

class dxFontRender : public IFontRender
{
public:
	dxFontRender();
	virtual ~dxFontRender();

	virtual void Initialize(LPCSTR cShader, LPCSTR cTexture);
	virtual void OnRender(CGameFont& owner);

private:
	ref_shader pShader;
	ref_geom pGeom;
	xray::metal::ui::ShaderIdentity m_metalShader;
};

#endif	//	FontRender_included
