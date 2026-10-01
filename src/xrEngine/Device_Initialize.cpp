#include "stdafx.h"
#include "resource.h"
#include "dedicated_server_only.h"
#include "../Include/xrRender/R4BackendSelection.h"

#ifdef INGAME_EDITOR
# include "../include/editor/ide.hpp"
# include "engine_impl.hpp"
#endif // #ifdef INGAME_EDITOR

extern LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

#if defined(XR_PLATFORM_APPLE)
extern "C" SDL_Window* g_mainSdlWindow = nullptr;
#endif

#ifdef INGAME_EDITOR
void CRenderDevice::initialize_editor()
{
    m_editor_module = LoadLibrary("editor.dll");
    if (!m_editor_module)
    {
        Msg("! cannot load library \"editor.dll\"");
        return;
    }

    m_editor_initialize = (initialize_function_ptr)GetProcAddress(m_editor_module, "initialize");
    VERIFY(m_editor_initialize);

    m_editor_finalize = (finalize_function_ptr)GetProcAddress(m_editor_module, "finalize");
    VERIFY(m_editor_finalize);

    m_engine = xr_new<engine_impl>();
    m_editor_initialize(m_editor, m_engine);
    VERIFY(m_editor);

    m_hWnd = m_editor->view_handle();
    VERIFY(m_hWnd != INVALID_HANDLE_VALUE);
}
#endif // #ifdef INGAME_EDITOR

PROTECT_API void CRenderDevice::Initialize()
{
	Log("Initializing Engine...");
	TimerGlobal.Start();
	TimerMM.Start();

#ifdef INGAME_EDITOR
    if (Core.ParamsData.test(ECoreParams::editor))
        initialize_editor();
#endif // #ifdef INGAME_EDITOR

#if defined(_WIN32)
	// Unless a substitute hWnd has been specified, create a window to render into
	if (m_hWnd == NULL)
	{
		const char* wndclass = "_XRAY_1.5";

		// Register the windows class
		HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(0);
		WNDCLASS wndClass = {
			0, WndProc, 0, 0, hInstance,
			LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)),
			LoadCursor(NULL, IDC_ARROW),
			(HBRUSH)GetStockObject(BLACK_BRUSH),
			NULL, wndclass
		};
		RegisterClass(&wndClass);

		// Set the window's initial style
		m_dwWindowStyle = WS_BORDER | WS_DLGFRAME;

		// Set the window's initial width
		RECT rc;
		SetRect(&rc, 0, 0, 640, 480);
		AdjustWindowRect(&rc, m_dwWindowStyle, FALSE);

		// Create the render window
		m_hWnd = CreateWindowEx(WS_EX_TOPMOST,
		                        wndclass, "S.T.A.L.K.E.R.: Anomaly", m_dwWindowStyle,
		                        /*rc.left, rc.top, */CW_USEDEFAULT, CW_USEDEFAULT,
		                        (rc.right - rc.left), (rc.bottom - rc.top), 0L,
		                        0, hInstance, 0L);
	}

	// Save window properties
	m_dwWindowStyle = GetWindowLong(m_hWnd, GWL_STYLE);
	GetWindowRect(m_hWnd, &m_rcWindowBounds);
	GetClientRect(m_hWnd, &m_rcWindowClient);
#else
	if (m_hWnd == NULL)
	{
		if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
		{
			Msg("! [SDL] SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s", SDL_GetError());
		}
		Uint32 windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(XR_PLATFORM_APPLE)
		if (SelectedR4Backend() == R4BackendKind::Metal)
			windowFlags |= SDL_WINDOW_SHOWN | SDL_WINDOW_METAL;
		else
			windowFlags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN;
#else
		windowFlags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN;
#endif

		SDL_Window* sdlWnd = SDL_CreateWindow("S.T.A.L.K.E.R.: Anomaly",
		                                      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                                      640, 480, windowFlags);
		m_hWnd = (HWND)sdlWnd;
#if defined(XR_PLATFORM_APPLE)
		g_mainSdlWindow = sdlWnd;
#endif
	}
	m_dwWindowStyle = 0;
	GetWindowRect(m_hWnd, &m_rcWindowBounds);
	GetClientRect(m_hWnd, &m_rcWindowClient);
#endif

	/*
	if (strstr(lpCmdLine,"-gpu_sw")!=NULL) HW.Caps.bForceGPU_SW = TRUE;
	else HW.Caps.bForceGPU_SW = FALSE;
	if (strstr(lpCmdLine,"-gpu_nopure")!=NULL) HW.Caps.bForceGPU_NonPure = TRUE;
	else HW.Caps.bForceGPU_NonPure = FALSE;
	if (strstr(lpCmdLine,"-gpu_ref")!=NULL) HW.Caps.bForceGPU_REF = TRUE;
	else HW.Caps.bForceGPU_REF = FALSE;
	*/

	Device.seqAppStart.Add(&m_imgui);
	Device.seqAppEnd.Add(&m_imgui);
}
