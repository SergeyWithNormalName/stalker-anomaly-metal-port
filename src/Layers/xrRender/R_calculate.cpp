#include "stdafx.h"
#include "../../xrEngine/customhud.h"
#include "../../Include/xrRender/R4BackendSelection.h"

float g_fSCREEN;

extern float r_dtex_range;
extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaLOD_A;
extern float r_ssaLOD_B;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

void CRender::Calculate()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		// The regular R4 render path establishes these CPU-only selection values
		// before dynamic/HUD graph insertion.  Direct Metal returns before that
		// legacy D3D render path, but the HUD capture still calls the exact
		// rimp_select_sh_dynamic() gate.  Without PHASE_NORMAL it selects
		// SE_R2_SHADOW and every opaque normal-HQ HUD leaf is rejected.
		const float fovFactor = _sqr(90.f / Device.fFOV);
		g_fSCREEN = float(Device.dwWidth) * float(Device.dwHeight) * fovFactor * (EPS_S + ps_r__LOD);
		r_ssaDISCARD = _sqr(ps_r__ssaDISCARD) / g_fSCREEN;
		r_ssaDONTSORT = _sqr(ps_r__ssaDONTSORT / 3.f) / g_fSCREEN;
		r_ssaLOD_A = _sqr(ps_r2_ssaLOD_A / 3.f) / g_fSCREEN;
		r_ssaLOD_B = _sqr(ps_r2_ssaLOD_B / 3.f) / g_fSCREEN;
		r_ssaGLOD_start = _sqr(ps_r__GLOD_ssa_start / 3.f) / g_fSCREEN;
		r_ssaGLOD_end = _sqr(ps_r__GLOD_ssa_end / 3.f) / g_fSCREEN;
		r_ssaHZBvsTEX = _sqr(ps_r__ssaHZBvsTEX / 3.f) / g_fSCREEN;
		r_dtex_range = ps_r2_df_parallax_range * g_fSCREEN / (1024.f * 768.f);
		// Calculate runs before CRender::Render().  R4 therefore updates light
		// ownership, spatial entries, and the six point-light shadow faces here,
		// then the normal D3D Render path captures them later.  Direct Metal has
		// no D3D Render path, but it must preserve that ordering: taking the
		// snapshot before Lights.Update() races first-frame level construction.
		Lights.Update();

		// Mirror the exact scene-admission boundary at the head of
		// r4_R_render.cpp.  Calculate is also called for the main menu and while
		// a level is being constructed; those frames have no valid graph/spatial
		// contract to capture. End() deliberately consumes an empty same-frame
		// lighting snapshot in that case.
		IMainMenu* mainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : nullptr;
		const bool menuSkipsScene = mainMenu && mainMenu->CanSkipSceneRendering();
		const bool sceneReady = g_pGameLevel && g_hud && !menuSkipsScene;
		if (!sceneReady)
			return;

		// r4_R_render normally establishes the portal/sector visibility graph
		// immediately before its D3D capture.  Direct Metal returns before that
		// function, so it must reproduce this CPU-only part here or every light
		// is rejected as belonging to an invisible sector. detectLastSector is
		// safe now because sceneReady guarantees a live CLevel.
		if (g_pGameLevel && g_pGamePersistent)
		{
			if (!vLastCameraPos.similar(Device.vCameraPosition, EPS_S))
			{
				CSector* pSector = (CSector*)detectLastSector(Device.vCameraPosition);
				if (pSector && pSector != pLastSector)
					g_pGamePersistent->OnSectorChanged(translateSector(pSector));
				if (!pSector)
					pSector = pLastSector;
				pLastSector = pSector;
				vLastCameraPos.set(Device.vCameraPosition);
			}
			ViewBase.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
			GMBase.traverse(pLastSector, ViewBase, Device.vCameraPosition, Device.mFullTransform);
		}
		RImplementation.phase = PHASE_NORMAL;
		GMBase.r_dsgraph_capture_metal_world();
		GMBase.r_dsgraph_capture_metal_hud();
		// The value-only local-light snapshot is captured at the start of the
		// Metal End() call. That is still before frame encoding, but follows the
		// engine object-update phase; newly spawned lights are otherwise mutated
		// while Calculate() is iterating the registry during New Game loading.
		return;
	}
	// Transfer to global space to avoid deep pointer access
	IRender_Target* T = getTarget();
	float fov_factor = _sqr(90.f / Device.fFOV);
	g_fSCREEN = float(T->get_width() * T->get_height()) * fov_factor * (EPS_S + ps_r__LOD);
	r_ssaDISCARD = _sqr(ps_r__ssaDISCARD) / g_fSCREEN;
	r_ssaDONTSORT = _sqr(ps_r__ssaDONTSORT / 3) / g_fSCREEN;
	r_ssaLOD_A = _sqr(ps_r2_ssaLOD_A / 3) / g_fSCREEN;
	r_ssaLOD_B = _sqr(ps_r2_ssaLOD_B / 3) / g_fSCREEN;
	r_ssaGLOD_start = _sqr(ps_r__GLOD_ssa_start / 3) / g_fSCREEN;
	r_ssaGLOD_end = _sqr(ps_r__GLOD_ssa_end / 3) / g_fSCREEN;
	r_ssaHZBvsTEX = _sqr(ps_r__ssaHZBvsTEX / 3) / g_fSCREEN;
	r_dtex_range = ps_r2_df_parallax_range * g_fSCREEN / (1024.f * 768.f);

	// Detect camera-sector
	if (!vLastCameraPos.similar(Device.vCameraPosition, EPS_S))
	{
		CSector* pSector = (CSector*)detectLastSector(Device.vCameraPosition);
		if (pSector && (pSector != pLastSector))
			g_pGamePersistent->OnSectorChanged(translateSector(pSector));

		if (0 == pSector) pSector = pLastSector;
		pLastSector = pSector;
		vLastCameraPos.set(Device.vCameraPosition);
	}

	Lights.Update();
}
