#ifndef dxLensFlareRender_included
#define dxLensFlareRender_included
#pragma once

#include "..\..\Include\xrRender\LensFlareRender.h"

class dxFlareRender : public IFlareRender
{
public:
	virtual void Copy(IFlareRender& _in);

	virtual void CreateShader(LPCSTR sh_name, LPCSTR tex_name);
	virtual void DestroyShader();

	//private:
public:
	ref_shader hShader;
	// Direct Metal intentionally never constructs the legacy shader object,
	// but its native flare pass still needs the authored descriptor identities.
	shared_str metalShaderName;
	shared_str metalTextureName;
};

class dxLensFlareRender : public ILensFlareRender
{
public:
	virtual void Copy(ILensFlareRender& _in);

	virtual void Render(CLensFlare& owner, BOOL bSun, BOOL bFlares, BOOL bGradient);

	virtual void OnDeviceCreate();
	virtual void OnDeviceDestroy();
private:
	ref_geom hGeom;
};

#endif	//	LensFlareRender_included
