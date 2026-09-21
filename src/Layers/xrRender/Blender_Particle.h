// Blender_Screen_SET.h: interface for the Blender_Screen_SET class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_BLENDER_SCREEN_SET_H__A215FA40_D885_4D06_9032_ED934AE295E3__INCLUDED_P)
#define AFX_BLENDER_SCREEN_SET_H__A215FA40_D885_4D06_9032_ED934AE295E3__INCLUDED_P
#pragma once

class CBlender_Particle : public IBlender
{
	xrP_TOKEN oBlend;
	xrP_Integer oAREF;
	xrP_BOOL oClamp;
public:
	// Value-only introspection for the Direct Metal particle capture path. The
	// values are loaded from this exact blender instance; no D3D state object is
	// created or retained.
	u32 BlendMode() const { return oBlend.IDselected; }
	BOOL BaseSamplerClamp() const { return oClamp.value; }

	virtual LPCSTR getComment() { return "particles"; }
	virtual BOOL canBeLMAPped() { return FALSE; }

	virtual void Save(IWriter& fs);
	virtual void Load(IReader& fs, u16 version);

	virtual void Compile(CBlender_Compile& C);

	CBlender_Particle();
	virtual ~CBlender_Particle();
};

#endif // !defined(AFX_BLENDER_SCREEN_SET_H__A215FA40_D885_4D06_9032_ED934AE295E3__INCLUDED_P)
