#include "stdafx.h"
#include "MonitorList.h"

// Window/display placement is currently owned by SDL on macOS.  The engine's
// HMONITOR-based live-switch path is a Win32/GDI contract, so advertise only
// the truthful automatic mode until that path has a native equivalent.
ENGINE_API xr_token* vid_monitor_token = nullptr;
ENGINE_API xr_string vid_monitor_name = "Auto";
ENGINE_API volatile long g_monitor_list_dirty = 0;

void fill_vid_monitor_list()
{
    if (vid_monitor_token)
        return;

    vid_monitor_token = xr_alloc<xr_token>(2);
    vid_monitor_token[0].name = xr_strdup("Auto");
    vid_monitor_token[0].id = 0;
    vid_monitor_token[1].name = nullptr;
    vid_monitor_token[1].id = -1;
}

void free_vid_monitor_list()
{
    if (!vid_monitor_token)
        return;

    xr_free(vid_monitor_token[0].name);
    xr_free(vid_monitor_token);
    vid_monitor_token = nullptr;
}

void refresh_vid_monitor_list()
{
    free_vid_monitor_list();
    fill_vid_monitor_list();
}

HMONITOR ResolveSelectedMonitor()
{
    if (!vid_monitor_name.empty() && vid_monitor_name != "Auto")
        Msg("! vid_monitor: selecting a display is not available on macOS yet; using Auto");
    return nullptr;
}
