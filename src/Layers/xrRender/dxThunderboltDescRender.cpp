#include "stdafx.h"
#include "dxThunderboltDescRender.h"
#include "../../Include/xrRender/R4BackendSelection.h"

void dxThunderboltDescRender::Copy(IThunderboltDescRender& _in)
{
	*this = *((dxThunderboltDescRender*)&_in);
}

void dxThunderboltDescRender::CreateModel(LPCSTR m_name)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		l_model = nullptr;
		return;
	}

	IReader* F = 0;
	F = FS.r_open("$game_meshes$", m_name);
	R_ASSERT3(F, "Cannot open 'lightning_model' path=[%s].", m_name);
	l_model = ::RImplementation.model_CreateDM(F);
	FS.r_close(F);
}

void dxThunderboltDescRender::DestroyModel()
{
	::RImplementation.model_Delete(l_model);
}
