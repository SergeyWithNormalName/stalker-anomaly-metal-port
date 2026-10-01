#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreA.h"
#if defined(__APPLE__)
#include "SoundRender_CoreApple.h"
#endif

XRSOUND_API xr_token* snd_devices_token = NULL;
XRSOUND_API xr_string snd_device_name;

void CSound_manager_interface::_create(int stage)
{
	if (stage == 0)
	{
		bool use_apple = false;
#if defined(__APPLE__)
		if (strstr(Core.Params, "-sound_backend apple") ||
		    strstr(Core.Params, "-sound_backend_apple") ||
		    strstr(Core.Params, "-sound_apple") ||
		    strstr(Core.Params, "-apple_sound") ||
		    strstr(Core.Params, "-apple_audio"))
		{
			use_apple = true;
		}
#endif

		if (use_apple)
		{
#if defined(__APPLE__)
			Msg("* Sound: Selected Apple Spatial Audio backend via command line");
			SoundRenderApple = xr_new<CSoundRender_CoreApple>();
			SoundRender = SoundRenderApple;
			SoundRenderA = nullptr;
			Sound = SoundRender;
#endif
		}
		else
		{
			Msg("* Sound: Selected OpenAL backend");
			SoundRenderA = xr_new<CSoundRender_CoreA>();
			SoundRender = SoundRenderA;
			Sound = SoundRender;
		}

		if (Core.ParamsData.test(ECoreParams::nosound))
		{
			SoundRender->bPresent = FALSE;
			return;
		}
		else
			SoundRender->bPresent = TRUE;
	}

	if (!SoundRender->bPresent) return;
	Sound->_initialize(stage);

#if defined(__APPLE__)
	// If Apple backend failed stage 1 initialization, fallback to OpenAL safely
	if (stage == 1 && SoundRenderApple && !SoundRenderApple->is_ready())
	{
		Msg("! Sound: Apple Spatial Audio backend failed to become ready; falling back to OpenAL...");
		Sound->_clear();
		xr_delete(SoundRender);
		SoundRenderApple = nullptr;

		SoundRenderA = xr_new<CSoundRender_CoreA>();
		SoundRender = SoundRenderA;
		Sound = SoundRender;
		SoundRender->bPresent = TRUE;
		Sound->_initialize(0);
		Sound->_initialize(1);
	}
#endif
}

void CSound_manager_interface::_destroy()
{
	if (Sound)
		Sound->_clear();
	xr_delete(SoundRender);
	SoundRenderA = nullptr;
#if defined(__APPLE__)
	SoundRenderApple = nullptr;
#endif
	Sound = 0;
}
