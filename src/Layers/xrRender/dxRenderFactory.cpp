#include "stdafx.h"
#include "dxRenderFactory.h"

#include "dxStatGraphRender.h"
#include "dxConsoleRender.h"
#ifndef _EDITOR
#include "dxThunderboltRender.h"
#include "dxThunderboltDescRender.h"
#include "dxRainRender.h"
#include "dxLensFlareRender.h"
#include "dxImGuiRender.h"
#include "dxEnvironmentRender.h"

#include "dxRenderDeviceRender.h"
#include "dxObjectSpaceRender.h"
#if defined(XR_PLATFORM_APPLE)
#include "../../Include/xrRender/R4BackendSelection.h"
#include "../xrRenderMetal/MetalRenderDeviceRender.h"
#endif
#endif // _EDITOR

#include "dxFontRender.h"
#include "dxApplicationRender.h"
#include "dxWallMarkArray.h"
#include "dxStatsRender.h"
#include "dxUISequenceVideoItem.h"
#include "dxUIShader.h"

dxRenderFactory RenderFactoryImpl;

//# include "dx##Class.h" \

#define RENDER_FACTORY_IMPLEMENT(Class) \
	I##Class* dxRenderFactory::Create##Class() \
{ \
	return xr_new<dx##Class>(); \
} \
	void dxRenderFactory::Destroy##Class(I##Class *pObject)\
{ \
	xr_delete((dx##Class*&)pObject); \
}
#ifndef _EDITOR
RENDER_FACTORY_IMPLEMENT(UISequenceVideoItem)
RENDER_FACTORY_IMPLEMENT(UIShader)
RENDER_FACTORY_IMPLEMENT(StatGraphRender)
RENDER_FACTORY_IMPLEMENT(ConsoleRender)
IRenderDeviceRender* dxRenderFactory::CreateRenderDeviceRender()
{
#if defined(XR_PLATFORM_APPLE)
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		Msg("* RENDER_DEVICE_FACTORY backend=metal implementation=MetalRenderDeviceRender");
		return xr_new<MetalRenderDeviceRender>();
	}
#endif
	Msg("* RENDER_DEVICE_FACTORY backend=dxvk implementation=dxRenderDeviceRender");
	return xr_new<dxRenderDeviceRender>();
}

void dxRenderFactory::DestroyRenderDeviceRender(IRenderDeviceRender* pObject)
{
	xr_delete((dxRenderDeviceRender*&)pObject);
}
#	ifdef DEBUG
		RENDER_FACTORY_IMPLEMENT(ObjectSpaceRender)
#	endif // DEBUG
RENDER_FACTORY_IMPLEMENT(ApplicationRender)
RENDER_FACTORY_IMPLEMENT(WallMarkArray)
RENDER_FACTORY_IMPLEMENT(StatsRender)
#endif // _EDITOR

#ifndef _EDITOR
RENDER_FACTORY_IMPLEMENT(ThunderboltRender)
RENDER_FACTORY_IMPLEMENT(ThunderboltDescRender)
RENDER_FACTORY_IMPLEMENT(RainRender)
RENDER_FACTORY_IMPLEMENT(LensFlareRender)
RENDER_FACTORY_IMPLEMENT(ImGuiRender)
RENDER_FACTORY_IMPLEMENT(EnvironmentRender)
RENDER_FACTORY_IMPLEMENT(EnvDescriptorMixerRender)
RENDER_FACTORY_IMPLEMENT(EnvDescriptorRender)
RENDER_FACTORY_IMPLEMENT(FlareRender)
#endif
RENDER_FACTORY_IMPLEMENT(FontRender)
