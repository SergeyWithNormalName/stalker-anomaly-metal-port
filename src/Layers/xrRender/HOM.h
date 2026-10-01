// HOM.h: interface for the CHOM class.
//
//////////////////////////////////////////////////////////////////////
#pragma once

#include "../../xrEngine/IGame_Persistent.h"

class occTri;

class CHOM
#ifdef DEBUG
	: public pureRender
#endif
{
private:
	xrXRC xrc;
	CDB::MODEL* m_pModel;
	occTri* m_pTris;
	BOOL bEnabled;
	bool diagnostic_only = false;
	Fmatrix m_xform;
	Fmatrix m_xform_01;
#ifdef DEBUG
	u32						tris_in_frame_visible	;
	u32						tris_in_frame			;
#endif

	xr_atomic_u32 MT_frame_rendered;
	xrCriticalSection m_mt_render_guard;
	u32 diagnostic_frame = u32(-1);
	Fvector diagnostic_camera;

	void Render_DB(CFrustum& base, const Fmatrix& fullTransform,
		const Fvector& camera, u32 frame, bool diagnostic = false);
public:
	void Load(bool scheduleParallel = true);
	void Unload();
	void Render(CFrustum& base);
	void Render_ZB();
	// Same CPU HOM raster, explicit same-frame camera, no scheduler, RNG or
	// cached vis_data. Diagnostic only: callers must keep rendering all work.
	bool RenderDiagnostic(const Fmatrix& fullTransform, const Fvector& camera, u32 frame);
	BOOL visibleDiagnostic(Fbox3& box, u32 frame);
	//	void					Debug		();

	void occlude(Fbox2& space)
	{
	}

	void Disable();
	void Enable();

	void __stdcall MT_RENDER();

	BOOL visible(vis_data& vis);
	BOOL visible(Fbox3& B);
	BOOL visible(Fsphere& S);
	BOOL visible(sPoly& P);
	BOOL visible(Fbox2& B, float depth); // viewport-space (0..1)

	CHOM();
	~CHOM();

#ifdef DEBUG
	virtual void			OnRender	();
			void			stats		();
#endif
};
