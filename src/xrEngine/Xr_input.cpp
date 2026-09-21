#include "stdafx.h"
#pragma hdrstop

#include "xr_input.h"
#include "IInputReceiver.h"
#include "XR_IOConsole.h"
//#include "../include/editor/ide.hpp"

#ifndef _EDITOR
# include "xr_input_xinput.h"
#endif
CInput* pInput = NULL;
IInputReceiver dummyController;

ENGINE_API float psMouseSens = 1.f;
ENGINE_API float psMouseSensScale = 1.f;
ENGINE_API float psMouseSensVerticalK = 1.f;
ENGINE_API Flags32 psMouseInvert = {FALSE};

float stop_vibration_time = flt_max;

// demonized: configurable buffer sizes for mouse and kb
int MOUSEBUFFERSIZE = 1024;
int KEYBOARDBUFFERSIZE = 128;

#define _KEYDOWN(name,key) ( name[key] & 0x80 )

static bool g_exclusive = false;
extern u32 g_screenmode;

static void on_error_dialog(bool before)
{
#ifdef INGAME_EDITOR
    if (Device.editor())
        return;
#endif // #ifdef INGAME_EDITOR
	if (!pInput || !g_exclusive)
		return;

	if (before)
	{
		pInput->unacquire();
		return;
	}

	pInput->acquire(true);
}

#if defined(XR_PLATFORM_APPLE)
namespace
{
u32 apple_dik_from_scancode(SDL_Scancode scancode)
{
	switch (scancode)
	{
	case SDL_SCANCODE_ESCAPE: return DIK_ESCAPE;
	case SDL_SCANCODE_1: return DIK_1;
	case SDL_SCANCODE_2: return DIK_2;
	case SDL_SCANCODE_3: return DIK_3;
	case SDL_SCANCODE_4: return DIK_4;
	case SDL_SCANCODE_5: return DIK_5;
	case SDL_SCANCODE_6: return DIK_6;
	case SDL_SCANCODE_7: return DIK_7;
	case SDL_SCANCODE_8: return DIK_8;
	case SDL_SCANCODE_9: return DIK_9;
	case SDL_SCANCODE_0: return DIK_0;
	case SDL_SCANCODE_MINUS: return DIK_MINUS;
	case SDL_SCANCODE_EQUALS: return DIK_EQUALS;
	case SDL_SCANCODE_BACKSPACE: return DIK_BACK;
	case SDL_SCANCODE_TAB: return DIK_TAB;
	case SDL_SCANCODE_Q: return DIK_Q;
	case SDL_SCANCODE_W: return DIK_W;
	case SDL_SCANCODE_E: return DIK_E;
	case SDL_SCANCODE_R: return DIK_R;
	case SDL_SCANCODE_T: return DIK_T;
	case SDL_SCANCODE_Y: return DIK_Y;
	case SDL_SCANCODE_U: return DIK_U;
	case SDL_SCANCODE_I: return DIK_I;
	case SDL_SCANCODE_O: return DIK_O;
	case SDL_SCANCODE_P: return DIK_P;
	case SDL_SCANCODE_LEFTBRACKET: return DIK_LBRACKET;
	case SDL_SCANCODE_RIGHTBRACKET: return DIK_RBRACKET;
	case SDL_SCANCODE_RETURN: return DIK_RETURN;
	case SDL_SCANCODE_LCTRL: return DIK_LCONTROL;
	case SDL_SCANCODE_A: return DIK_A;
	case SDL_SCANCODE_S: return DIK_S;
	case SDL_SCANCODE_D: return DIK_D;
	case SDL_SCANCODE_F: return DIK_F;
	case SDL_SCANCODE_G: return DIK_G;
	case SDL_SCANCODE_H: return DIK_H;
	case SDL_SCANCODE_J: return DIK_J;
	case SDL_SCANCODE_K: return DIK_K;
	case SDL_SCANCODE_L: return DIK_L;
	case SDL_SCANCODE_SEMICOLON: return DIK_SEMICOLON;
	case SDL_SCANCODE_APOSTROPHE: return DIK_APOSTROPHE;
	case SDL_SCANCODE_GRAVE: return DIK_GRAVE;
	case SDL_SCANCODE_LSHIFT: return DIK_LSHIFT;
	case SDL_SCANCODE_BACKSLASH: return DIK_BACKSLASH;
	case SDL_SCANCODE_Z: return DIK_Z;
	case SDL_SCANCODE_X: return DIK_X;
	case SDL_SCANCODE_C: return DIK_C;
	case SDL_SCANCODE_V: return DIK_V;
	case SDL_SCANCODE_B: return DIK_B;
	case SDL_SCANCODE_N: return DIK_N;
	case SDL_SCANCODE_M: return DIK_M;
	case SDL_SCANCODE_COMMA: return DIK_COMMA;
	case SDL_SCANCODE_PERIOD: return DIK_PERIOD;
	case SDL_SCANCODE_SLASH: return DIK_SLASH;
	case SDL_SCANCODE_RSHIFT: return DIK_RSHIFT;
	case SDL_SCANCODE_KP_MULTIPLY: return DIK_MULTIPLY;
	case SDL_SCANCODE_LALT: return DIK_LMENU;
	case SDL_SCANCODE_SPACE: return DIK_SPACE;
	case SDL_SCANCODE_CAPSLOCK: return DIK_CAPITAL;
	case SDL_SCANCODE_F1: return DIK_F1;
	case SDL_SCANCODE_F2: return DIK_F2;
	case SDL_SCANCODE_F3: return DIK_F3;
	case SDL_SCANCODE_F4: return DIK_F4;
	case SDL_SCANCODE_F5: return DIK_F5;
	case SDL_SCANCODE_F6: return DIK_F6;
	case SDL_SCANCODE_F7: return DIK_F7;
	case SDL_SCANCODE_F8: return DIK_F8;
	case SDL_SCANCODE_F9: return DIK_F9;
	case SDL_SCANCODE_F10: return DIK_F10;
	case SDL_SCANCODE_NUMLOCKCLEAR: return DIK_NUMLOCK;
	case SDL_SCANCODE_SCROLLLOCK: return DIK_SCROLL;
	case SDL_SCANCODE_KP_7: return DIK_NUMPAD7;
	case SDL_SCANCODE_KP_8: return DIK_NUMPAD8;
	case SDL_SCANCODE_KP_9: return DIK_NUMPAD9;
	case SDL_SCANCODE_KP_MINUS: return DIK_SUBTRACT;
	case SDL_SCANCODE_KP_4: return DIK_NUMPAD4;
	case SDL_SCANCODE_KP_5: return DIK_NUMPAD5;
	case SDL_SCANCODE_KP_6: return DIK_NUMPAD6;
	case SDL_SCANCODE_KP_PLUS: return DIK_ADD;
	case SDL_SCANCODE_KP_1: return DIK_NUMPAD1;
	case SDL_SCANCODE_KP_2: return DIK_NUMPAD2;
	case SDL_SCANCODE_KP_3: return DIK_NUMPAD3;
	case SDL_SCANCODE_KP_0: return DIK_NUMPAD0;
	case SDL_SCANCODE_KP_PERIOD: return DIK_DECIMAL;
	case SDL_SCANCODE_F11: return DIK_F11;
	case SDL_SCANCODE_F12: return DIK_F12;
	case SDL_SCANCODE_KP_ENTER: return DIK_NUMPADENTER;
	case SDL_SCANCODE_RCTRL: return DIK_RCONTROL;
	case SDL_SCANCODE_KP_DIVIDE: return DIK_DIVIDE;
	case SDL_SCANCODE_RALT: return DIK_RMENU;
	case SDL_SCANCODE_PAUSE: return DIK_PAUSE;
	case SDL_SCANCODE_HOME: return DIK_HOME;
	case SDL_SCANCODE_UP: return DIK_UP;
	case SDL_SCANCODE_PAGEUP: return DIK_PRIOR;
	case SDL_SCANCODE_LEFT: return DIK_LEFT;
	case SDL_SCANCODE_RIGHT: return DIK_RIGHT;
	case SDL_SCANCODE_END: return DIK_END;
	case SDL_SCANCODE_DOWN: return DIK_DOWN;
	case SDL_SCANCODE_PAGEDOWN: return DIK_NEXT;
	case SDL_SCANCODE_INSERT: return DIK_INSERT;
	case SDL_SCANCODE_DELETE: return DIK_DELETE;
	case SDL_SCANCODE_LGUI: return DIK_LWIN;
	case SDL_SCANCODE_RGUI: return DIK_RWIN;
	case SDL_SCANCODE_APPLICATION: return DIK_APPS;
	default: return 0;
	}
}

DWORD apple_mouse_offset(Uint8 button)
{
	switch (button)
	{
	case SDL_BUTTON_LEFT: return DIMOFS_BUTTON0;
	case SDL_BUTTON_RIGHT: return DIMOFS_BUTTON1;
	case SDL_BUTTON_MIDDLE: return DIMOFS_BUTTON2;
	case SDL_BUTTON_X1: return DIMOFS_BUTTON3;
	case SDL_BUTTON_X2: return DIMOFS_BUTTON4;
	default: return 0xffffffffu;
	}
}

DIDEVICEOBJECTDATA apple_input_event(DWORD offset, DWORD data)
{
	DIDEVICEOBJECTDATA result = {};
	result.dwOfs = offset;
	result.dwData = data;
	result.dwTimeStamp = SDL_GetTicks();
	return result;
}

constexpr ULONG_PTR apple_text_input_marker = 1;

bool apple_is_text_input_event(const DIDEVICEOBJECTDATA& event)
{
	return (event.uAppData & apple_text_input_marker) != 0;
}

xr_string* apple_text_from_event(const DIDEVICEOBJECTDATA& event)
{
	return reinterpret_cast<xr_string*>(event.uAppData & ~apple_text_input_marker);
}
}
#endif

CInput::CInput(BOOL bExclusive, int deviceForInit)
{
	g_exclusive = !!bExclusive;

	Log("Starting INPUT device...");

	pDI = NULL;
	pMouse = NULL;
	pKeyboard = NULL;

	//=====================Mouse
	mouse_property.mouse_dt = 25;

	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));

	//===================== Dummy pack
	iCapture(&dummyController);

#if !defined(XR_PLATFORM_APPLE)
	if (!pDI)
		CHK_DX(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&pDI, NULL));

	//. u32 kb_input_flags = ((bExclusive)?DISCL_EXCLUSIVE:DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;
	u32 kb_input_flags = ((bExclusive) ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;

	//. u32 mouse_input_flags = ((bExclusive)?DISCL_EXCLUSIVE:DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY,
	u32 mouse_input_flags = ((bExclusive) ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY;

	// KEYBOARD
	if (deviceForInit & keyboard_device_key)
		CHK_DX(CreateInputDevice(
		&pKeyboard, GUID_SysKeyboard, &c_dfDIKeyboard,
		kb_input_flags,
		KEYBOARDBUFFERSIZE));

	// MOUSE
	if (deviceForInit & mouse_device_key)
		CHK_DX(CreateInputDevice(
		&pMouse, GUID_SysMouse, &c_dfDIMouse2,
		mouse_input_flags,
		MOUSEBUFFERSIZE));
#endif

	Debug.set_on_dialog(&on_error_dialog);

#ifdef ENGINE_BUILD
	Device.seqAppActivate.Add(this);
	Device.seqAppDeactivate.Add(this, REG_PRIORITY_HIGH);
	Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
#endif
}

CInput::~CInput(void)
{
#if defined(XR_PLATFORM_APPLE)
	SetRelativeMouseMode(false);
#endif
#ifdef ENGINE_BUILD
	Device.seqFrame.Remove(this);
	Device.seqAppDeactivate.Remove(this);
	Device.seqAppActivate.Remove(this);
#endif
	//_______________________

	// Unacquire and release the device's interfaces
	if (pMouse)
	{
		pMouse->Unacquire();
		_RELEASE(pMouse);
	}

	if (pKeyboard)
	{
		pKeyboard->Unacquire();
		_RELEASE(pKeyboard);
	}

	_SHOW_REF("Input: ", pDI);
	_RELEASE(pDI);
}

//-----------------------------------------------------------------------------
// Name: CreateInputDevice()
// Desc: Create a DirectInput device.
//-----------------------------------------------------------------------------
HRESULT CInput::CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice, const DIDATAFORMAT* pdidDataFormat,
                                  u32 dwFlags, u32 buf_size)
{
	// Obtain an interface to the input device
	//. CHK_DX( pDI->CreateDeviceEx( guidDevice, IID_IDirectInputDevice8, (void**)device, NULL ) );
	CHK_DX(pDI->CreateDevice(guidDevice, /*IID_IDirectInputDevice8,*/ device, NULL));

	// Set the device data format. Note: a data format specifies which
	// controls on a device we are interested in, and how they should be
	// reported.
	CHK_DX((*device)->SetDataFormat(pdidDataFormat));

	// Set the cooperativity level to let DirectInput know how this device
	// should interact with the system and with other DirectInput applications.
#ifdef INGAME_EDITOR
    if (!Device.editor())
#endif // #ifdef INGAME_EDITOR
	{
		HRESULT _hr = (*device)->SetCooperativeLevel(RDEVICE.m_hWnd, dwFlags);
		if (FAILED(_hr) && (_hr == E_NOTIMPL)) Msg("! INPUT: Can't set coop level. Emulation???");
		else
			R_CHK(_hr);
	}

	// setup the buffer size for the keyboard data
	DIPROPDWORD dipdw;
	dipdw.diph.dwSize = sizeof(DIPROPDWORD);
	dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	dipdw.diph.dwObj = 0;
	dipdw.diph.dwHow = DIPH_DEVICE;
	dipdw.dwData = buf_size;

	CHK_DX((*device)->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph));

	return S_OK;
}

//-----------------------------------------------------------------------

void CInput::SetAllAcquire(BOOL bAcquire)
{
	if (pMouse) bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
	if (pKeyboard) bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

void CInput::SetMouseAcquire(BOOL bAcquire)
{
	if (pMouse) bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
}

void CInput::SetKBDAcquire(BOOL bAcquire)
{
	if (pKeyboard) bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

//-----------------------------------------------------------------------
BOOL b_altF4 = FALSE;

void CInput::KeyUpdate()
{
	if (b_altF4) return;

	HRESULT hr;
	DWORD dwElements = KEYBOARDBUFFERSIZE;
	auto od = std::make_unique<DIDEVICEOBJECTDATA[]>(KEYBOARDBUFFERSIZE);
	DWORD key = 0;

#if defined(XR_PLATFORM_APPLE)
	dwElements = std::min<DWORD>(dwElements, sdlKeyboardEvents.size());
	std::copy_n(sdlKeyboardEvents.begin(), dwElements, od.get());
	sdlKeyboardEvents.erase(sdlKeyboardEvents.begin(), sdlKeyboardEvents.begin() + dwElements);
#else
	VERIFY(pKeyboard);

	hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
	if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
	{
		hr = pKeyboard->Acquire();
		if (hr != S_OK)
			return;

		hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
		if (hr != S_OK)
			return;
	}
#endif

	bool b_dik_pause_was_pressed = false;
	for (u32 idx = 0; idx < dwElements; idx++)
	{
#if defined(XR_PLATFORM_APPLE)
		if (apple_is_text_input_event(od[idx]))
			continue;
#endif
		if (od[idx].dwOfs == DIK_PAUSE)
		{
			if (od[idx].dwData & 0x80)
				b_dik_pause_was_pressed = true;

			if (b_dik_pause_was_pressed && !(od[idx].dwData & 0x80))
			{
				od[idx].uAppData = 666;
				continue; //skip one-frame pause key on-off switch
			}
		}
		KBState[od[idx].dwOfs] = od[idx].dwData & 0x80;
	}

#ifndef _EDITOR
	bool b_alt_tab = false;

	if (!b_altF4 && KBState[DIK_F4] && (KBState[DIK_RMENU] || KBState[DIK_LMENU]))
	{
		b_altF4 = TRUE;
		Engine.Event.Defer("KERNEL:disconnect");
		Engine.Event.Defer("KERNEL:quit");
	}


#endif
	if (b_altF4)
	{
#if defined(XR_PLATFORM_APPLE)
		for (u32 i = 0; i < dwElements; ++i)
			if (apple_is_text_input_event(od[i]))
				xr_delete(apple_text_from_event(od[i]));
#endif
		return;
	}

#ifndef _EDITOR
	if (Device.dwPrecacheFrame == 0)
#endif
	{
		for (u32 i = 0; i < dwElements; i++)
		{
		#if defined(XR_PLATFORM_APPLE)
			if (apple_is_text_input_event(od[i]))
			{
				xr_string* text = apple_text_from_event(od[i]);
				// The legacy console is the only active native text widget. Keep
				// physical-key input on IInputReceiver so this does not alter the
				// ABI of every gameplay input receiver.
				if (Console && Console->bVisible)
					Console->IR_OnTextInput(text->c_str());
				xr_delete(text);
				continue;
			}
		#endif

			if (od[i].uAppData == 666) //ignored action
				continue;

			key = od[i].dwOfs;
			if (od[i].dwData & 0x80)
				cbStack.back()->IR_OnKeyboardPress(key);
			else
			{
				cbStack.back()->IR_OnKeyboardRelease(key);
#ifndef _EDITOR
				if (key == DIK_TAB && (iGetAsyncKeyState(DIK_RMENU) || iGetAsyncKeyState(DIK_LMENU)))
					b_alt_tab = true;
#endif
			}
		}

		for (u32 i = 0; i < COUNT_KB_BUTTONS; i++)
			if (KBState[i])
				cbStack.back()->IR_OnKeyboardHold(i);
	}

#if defined(XR_PLATFORM_APPLE)
	// Precache frames intentionally suppress input dispatch; release queued
	// committed text rather than carrying stale input into the loaded level.
	if (Device.dwPrecacheFrame)
	{
		for (u32 i = 0; i < dwElements; ++i)
			if (apple_is_text_input_event(od[i]))
				xr_delete(apple_text_from_event(od[i]));
	}
#endif

#ifndef _EDITOR
	if (b_alt_tab) {
		BOOL fullscreen = (g_screenmode == 2);
		if (fullscreen)
			SendMessage(Device.m_hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
	}
#endif
	/*
	#ifndef _EDITOR
	//update xinput if exist
	for( DWORD iUserIndex=0; iUserIndex<DXUT_MAX_CONTROLLERS; iUserIndex++ )
	{
	DXUTGetGamepadState( iUserIndex, &g_GamePads[iUserIndex], true, false );

	if( !g_GamePads[iUserIndex].bConnected )
	continue; // unplugged?

	bool new_b, old_b;
	new_b = !!(g_GamePads[iUserIndex].wPressedButtons & XINPUT_GAMEPAD_A);
	old_b = !!(g_GamePads[iUserIndex].wLastButtons & XINPUT_GAMEPAD_A);

	if(new_b != old_b)
	{
	if(old_b)
	cbStack.back()->IR_OnMousePress(0);
	else
	cbStack.back()->IR_OnMouseRelease(0);
	}
	int dx,dy;
	dx = iFloor(g_GamePads[iUserIndex].fThumbRX*6);
	dy = iFloor(g_GamePads[iUserIndex].fThumbRY*6);
	if(dx || dy)
	cbStack.back()->IR_OnMouseMove ( dx, dy );
	}

	if(Device.fTimeGlobal > stop_vibration_time)
	{
	stop_vibration_time = flt_max;
	set_vibration (0, 0);
	}
	//xinput
	#endif
	*/
}

void CInput::resetMouseState()
{
	for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++) {
		mouseState[i] = 0;
	}
}

bool CInput::get_dik_name(int dik, LPSTR dest_str, int dest_sz)
{
#if defined(XR_PLATFORM_APPLE)
	// SDL's scancode names describe native keys; mapping the reverse DirectInput
	// table is only needed for the key-binding UI and is not part of event input.
	return false;
#else
	DIPROPSTRING keyname;
	keyname.diph.dwSize = sizeof(DIPROPSTRING);
	keyname.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	keyname.diph.dwObj = static_cast<DWORD>(dik);
	keyname.diph.dwHow = DIPH_BYOFFSET;
	HRESULT hr = pKeyboard->GetProperty(DIPROP_KEYNAME, &keyname.diph);
	if (FAILED(hr))
		return false;

	const wchar_t* wct = keyname.wsz;
	if (0 == wcslen(wct))
		return false;

	int cnt = WideCharToMultiByte(CP_ACP, 0, keyname.wsz, -1, dest_str, dest_sz, NULL, NULL);
	if (cnt == -1)
	{
		Msg("! cant convert dik_name for dik[%d], prop=[%S]", dik, keyname.wsz);
		return false;
	}
	return (cnt != -1);
#endif
}

bool CInput::dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz)
{
	if (!dest || dest_sz <= 1)
		return false;

	dest[0] = 0;

	const UINT scan = static_cast<UINT>(dik);
	const HKL hkl = GetKeyboardLayout(0);

	// Convert scan code -> virtual key according to current layout
	const UINT vk = MapVirtualKeyEx(scan, MAPVK_VSC_TO_VK_EX, hkl);
	if (vk == 0)
		return false;

	BYTE ks[256] = {};
	if (shift) ks[VK_SHIFT] = 0x80;
	if (ctrl)  ks[VK_CONTROL] = 0x80;
	if (alt)   ks[VK_MENU] = 0x80;
	if (caps)  ks[VK_CAPITAL] = 0x01;

	// AltGr (usually Right Alt) on Windows behaves like Ctrl+Alt
	if (altgr)
	{
		ks[VK_CONTROL] = 0x80;
		ks[VK_MENU] = 0x80;
	}
    
	wchar_t wbuf[8] = {};
	const int rc = ToUnicodeEx(vk, scan, ks, wbuf, (int)std::size(wbuf), 0, hkl);

	if (rc <= 0)
	{
		// Dead key: clear keyboard state in ToUnicodeEx internal buffer
		if (rc == -1)
		{
			BYTE ks0[256] = {};
			wchar_t dummy[8] = {};
			ToUnicodeEx(vk, scan, ks0, dummy, (int)std::size(dummy), 0, hkl);
		}
		return false;
	}

	// Filter control chars
	if (wbuf[0] < 0x20)
		return false;

	// Convert produced Unicode to CP1251
	BOOL usedDefault = FALSE;
	const int mb = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wbuf, rc, dest, dest_sz - 1, "?", &usedDefault);
	if (mb <= 0)
		return false;

	// Null-terminate
	dest[mb] = 0;

	// If UI cannot represent it in CP1251, prevent cyrillic output
	// Return true to insert '?'
	if (usedDefault)
		return false;

	return true;
}

#define MOUSE_1 (0xED + 100)
#define MOUSE_8 (0xED + 107)

BOOL CInput::iGetAsyncKeyState(int dik)
{
	if (dik < COUNT_KB_BUTTONS)
		return !!KBState[dik];
	else if (dik >= MOUSE_1 && dik <= MOUSE_8)
	{
		int mk = dik - MOUSE_1;
		return iGetAsyncBtnState(mk);
	}
	else
		return FALSE; //unknown key ???
}

BOOL CInput::iGetAsyncBtnState(int btn)
{
	if ((btn == 0 || btn == 1) && !!GetSystemMetrics(SM_SWAPBUTTON))
		btn = btn == 0 ? 1 : 0;
	return !!mouseState[btn];
}

void CInput::MouseUpdate()
{
	HRESULT hr;
	DWORD dwElements = MOUSEBUFFERSIZE;
	auto od = std::make_unique<DIDEVICEOBJECTDATA[]>(MOUSEBUFFERSIZE);

#if defined(XR_PLATFORM_APPLE)
	dwElements = std::min<DWORD>(dwElements, sdlMouseEvents.size());
	std::copy_n(sdlMouseEvents.begin(), dwElements, od.get());
	sdlMouseEvents.erase(sdlMouseEvents.begin(), sdlMouseEvents.begin() + dwElements);
#else
	VERIFY(pMouse);

	hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
	if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
	{
		hr = pMouse->Acquire();
		if (hr != S_OK) return;
		hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
		if (hr != S_OK) return;
	};
#endif

#ifndef _EDITOR
	if (Device.dwPrecacheFrame)
		return;
#endif
	BOOL mouse_prev[COUNT_MOUSE_BUTTONS];

	mouse_prev[0] = mouseState[0];
	mouse_prev[1] = mouseState[1];
	mouse_prev[2] = mouseState[2];
	mouse_prev[3] = mouseState[3];
	mouse_prev[4] = mouseState[4];
	mouse_prev[5] = mouseState[5];
	mouse_prev[6] = mouseState[6];
	mouse_prev[7] = mouseState[7];

	bool bSwitched = !!GetSystemMetrics(SM_SWAPBUTTON);

	offs[0] = offs[1] = offs[2] = 0;
	for (u32 i = 0; i < dwElements; i++)
	{
		switch (od[i].dwOfs)
		{
		case DIMOFS_X:
			offs[0] += od[i].dwData;
			timeStamp[0] = od[i].dwTimeStamp;
			break;
		case DIMOFS_Y:
			offs[1] += od[i].dwData;
			timeStamp[1] = od[i].dwTimeStamp;
			break;
		case DIMOFS_Z:
			offs[2] += od[i].dwData;
			timeStamp[2] = od[i].dwTimeStamp;
			break;
		case DIMOFS_BUTTON0:
			if (od[i].dwData & 0x80)
			{
				mouseState[0] = TRUE;
				cbStack.back()->IR_OnMousePress(bSwitched ? 1 : 0);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[0] = FALSE;
				cbStack.back()->IR_OnMouseRelease(bSwitched ? 1 : 0);
			}
			break;
		case DIMOFS_BUTTON1:
			if (od[i].dwData & 0x80)
			{
				mouseState[1] = TRUE;
				cbStack.back()->IR_OnMousePress(bSwitched ? 0 : 1);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[1] = FALSE;
				cbStack.back()->IR_OnMouseRelease(bSwitched ? 0 : 1);
			}
			break;
		case DIMOFS_BUTTON2:
			if (od[i].dwData & 0x80)
			{
				mouseState[2] = TRUE;
				cbStack.back()->IR_OnMousePress(2);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[2] = FALSE;
				cbStack.back()->IR_OnMouseRelease(2);
			}
			break;
		case DIMOFS_BUTTON3:
			if (od[i].dwData & 0x80)
			{
				mouseState[3] = TRUE;
				cbStack.back()->IR_OnMousePress(3);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[3] = FALSE;
				cbStack.back()->IR_OnMouseRelease(3);
			}
			break;
		case DIMOFS_BUTTON4:
			if (od[i].dwData & 0x80)
			{
				mouseState[4] = TRUE;
				cbStack.back()->IR_OnMousePress(4);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[4] = FALSE;
				cbStack.back()->IR_OnMouseRelease(4);
			}
			break;
		case DIMOFS_BUTTON5:
			if (od[i].dwData & 0x80)
			{
				mouseState[5] = TRUE;
				cbStack.back()->IR_OnMousePress(5);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[5] = FALSE;
				cbStack.back()->IR_OnMouseRelease(5);
			}
			break;
		case DIMOFS_BUTTON6:
			if (od[i].dwData & 0x80)
			{
				mouseState[6] = TRUE;
				cbStack.back()->IR_OnMousePress(6);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[6] = FALSE;
				cbStack.back()->IR_OnMouseRelease(6);
			}
			break;
		case DIMOFS_BUTTON7:
			if (od[i].dwData & 0x80)
			{
				mouseState[7] = TRUE;
				cbStack.back()->IR_OnMousePress(7);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[7] = FALSE;
				cbStack.back()->IR_OnMouseRelease(7);
			}
			break;
		}
	}

	if (mouseState[0] && mouse_prev[0])
	{
		cbStack.back()->IR_OnMouseHold(bSwitched ? 1 : 0);
	}

	if (mouseState[1] && mouse_prev[1])
	{
		cbStack.back()->IR_OnMouseHold(bSwitched ? 0 : 1);
	}

	if (mouseState[2] && mouse_prev[2])
	{
		cbStack.back()->IR_OnMouseHold(2);
	}

	if (mouseState[3] && mouse_prev[3])
	{
		cbStack.back()->IR_OnMouseHold(3);
	}

	if (mouseState[4] && mouse_prev[4])
	{
		cbStack.back()->IR_OnMouseHold(4);
	}

	if (mouseState[5] && mouse_prev[5])
	{
		cbStack.back()->IR_OnMouseHold(5);
	}

	if (mouseState[6] && mouse_prev[6])
	{
		cbStack.back()->IR_OnMouseHold(6);
	}

	if (mouseState[7] && mouse_prev[7])
	{
		cbStack.back()->IR_OnMouseHold(7);
	}

	if (dwElements)
	{
		if (offs[0] || offs[1]) cbStack.back()->IR_OnMouseMove(offs[0], offs[1]);
		if (offs[2]) cbStack.back()->IR_OnMouseWheel(offs[2]);
	}
	else
	{
		if (timeStamp[1] && ((dwCurTime - timeStamp[1]) >= mouse_property.mouse_dt)) cbStack
		                                                                             .back()->IR_OnMouseStop(
			                                                                             DIMOFS_Y, timeStamp[1] = 0);
		if (timeStamp[0] && ((dwCurTime - timeStamp[0]) >= mouse_property.mouse_dt)) cbStack
		                                                                             .back()->IR_OnMouseStop(
			                                                                             DIMOFS_X, timeStamp[0] = 0);
	}
}

//-------------------------------------------------------
void CInput::iCapture(IInputReceiver* p)
{
	VERIFY(p);
	if (pMouse) MouseUpdate();
	if (pKeyboard) KeyUpdate();

	// change focus
	if (!cbStack.empty())
		cbStack.back()->IR_OnDeactivate();
	cbStack.push_back(p);
	cbStack.back()->IR_OnActivate();

	// prepare for _new_ controller
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::iRelease(IInputReceiver* p)
{
	if (p == cbStack.back())
	{
		cbStack.back()->IR_OnDeactivate();
		cbStack.pop_back();
		IInputReceiver* ir = cbStack.back();
		ir->IR_OnActivate();
	}
	else
	{
		// we are not topmost receiver, so remove the nearest one
		u32 cnt = cbStack.size();
		for (; cnt > 0; --cnt)
			if (cbStack[cnt - 1] == p)
			{
				xr_vector<IInputReceiver*>::iterator it = cbStack.begin();
				std::advance(it, cnt - 1);
				cbStack.erase(it);
				break;
			}
	}
}

void CInput::OnAppActivate(void)
{
	if (CurrentIR())
		CurrentIR()->IR_OnActivate();

	SetAllAcquire(true);
	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::OnAppDeactivate(void)
{
#if defined(XR_PLATFORM_APPLE)
	SetRelativeMouseMode(false);
#endif
	if (CurrentIR())
		CurrentIR()->IR_OnDeactivate();

	SetAllAcquire(false);
	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::DeactivateSoft()
{
	if (CurrentIR())
		CurrentIR()->IR_OnDeactivate();

	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

#if defined(XR_PLATFORM_APPLE)
void CInput::SetRelativeMouseMode(bool enabled)
{
	const SDL_bool requested = enabled ? SDL_TRUE : SDL_FALSE;
	if (SDL_GetRelativeMouseMode() == requested)
		return;

	if (SDL_SetRelativeMouseMode(requested) != 0)
	{
		Msg("! [SDL] SDL_SetRelativeMouseMode(%d) failed: %s", enabled, SDL_GetError());
		return;
	}

	// SDL flushes motion while changing modes. Drop any queued coordinates as
	// well so an old absolute event cannot affect the newly selected receiver.
	sdlMouseEvents.clear();
}

void CInput::OnSdlEvent(const SDL_Event& event)
{
	switch (event.type)
	{
	case SDL_KEYDOWN:
	case SDL_KEYUP:
	{
		const u32 dik = apple_dik_from_scancode(event.key.keysym.scancode);
		if (dik < COUNT_KB_BUTTONS)
			sdlKeyboardEvents.push_back(apple_input_event(dik, event.type == SDL_KEYDOWN ? 0x80 : 0));
		break;
	}
	case SDL_TEXTINPUT:
		if (event.text.text[0])
		{
			xr_string* text = xr_new<xr_string>(event.text.text);
			DIDEVICEOBJECTDATA textEvent = apple_input_event(0, 0);
			textEvent.uAppData = reinterpret_cast<ULONG_PTR>(text) | apple_text_input_marker;
			sdlKeyboardEvents.push_back(textEvent);
		}
		break;
	case SDL_MOUSEMOTION:
		if (event.motion.xrel)
			sdlMouseEvents.push_back(apple_input_event(DIMOFS_X, static_cast<DWORD>(event.motion.xrel)));
		if (event.motion.yrel)
			sdlMouseEvents.push_back(apple_input_event(DIMOFS_Y, static_cast<DWORD>(event.motion.yrel)));
		break;
	case SDL_MOUSEWHEEL:
	{
		const int direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
		if (event.wheel.y)
			sdlMouseEvents.push_back(apple_input_event(DIMOFS_Z, static_cast<DWORD>(direction * event.wheel.y * 120)));
		break;
	}
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	{
		const DWORD offset = apple_mouse_offset(event.button.button);
		if (offset != 0xffffffffu)
			sdlMouseEvents.push_back(apple_input_event(offset, event.type == SDL_MOUSEBUTTONDOWN ? 0x80 : 0));
		break;
	}
	case SDL_WINDOWEVENT:
		if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
			Device.OnWM_Activate(WA_ACTIVE, 0);
		else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
			Device.OnWM_Activate(WA_INACTIVE, 0);
		break;
	default:
		break;
	}
}
#endif

void CInput::OnFrame(void)
{
	PROF_EVENT("CInput::OnFrame");
	RDEVICE.Statistic->Input.Begin();
	dwCurTime = RDEVICE.TimerAsync_MMT();
#if defined(XR_PLATFORM_APPLE)
	KeyUpdate();
	MouseUpdate();
#else
	if (pKeyboard) KeyUpdate();
	if (pMouse) MouseUpdate();
#endif
	RDEVICE.Statistic->Input.End();
}

IInputReceiver* CInput::CurrentIR()
{
	if (cbStack.size())
		return cbStack.back();
	else
		return NULL;
}

void CInput::unacquire()
{
	#if defined(XR_PLATFORM_APPLE)
	return;
	#else
	pKeyboard->Unacquire();
	pMouse->Unacquire();
	#endif
}

void CInput::acquire(const bool& exclusive)
{
	#if defined(XR_PLATFORM_APPLE)
	return;
	#else
	pKeyboard->SetCooperativeLevel(
#ifdef INGAME_EDITOR
        Device.editor() ? Device.editor()->main_handle() :
#endif // #ifdef INGAME_EDITOR
		RDEVICE.m_hWnd,
		(exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND
	);
	pKeyboard->Acquire();

	pMouse->SetCooperativeLevel(
#ifdef INGAME_EDITOR
        Device.editor() ? Device.editor()->main_handle() :
#endif // #ifdef INGAME_EDITOR
		RDEVICE.m_hWnd,
		(exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY
	);
	pMouse->Acquire();
	#endif
}

void CInput::exclusive_mode(const bool& exclusive)
{
	g_exclusive = exclusive;
	unacquire();
	acquire(exclusive);
}

bool CInput::get_exclusive_mode()
{
	return g_exclusive;
}

void CInput::feedback(u16 s1, u16 s2, float time)
{
	stop_vibration_time = RDEVICE.fTimeGlobal + time;
#ifndef _EDITOR
	//. set_vibration (s1, s2);
#endif
}
