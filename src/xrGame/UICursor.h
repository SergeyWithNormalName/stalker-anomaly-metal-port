#pragma once

#include "ui_base.h"
class CUIStatic;

class CUICursor : public pureRender,
                  public pureScreenResolutionChanged
{
	bool bVisible;
	Fvector2 vPos;
	Fvector2 vPrevPos;
	bool m_b_use_win_cursor;
	CUIStatic* m_static;
	CUIStatic* m_gamepadFocusStatic;
	bool m_gamepadFocusMode;
	bool m_gamepadFocusRectValid;
	Frect m_gamepadFocusRect;
	void InitInternal();
public:
	CUICursor();
	virtual ~CUICursor();
	virtual void OnRender();

	Fvector2 GetCursorPositionDelta();

	Fvector2 GetCursorPosition();
	void SetUICursorPosition(Fvector2 pos);
	void UpdateCursorPosition(int _dx, int _dy);

	// Relative move for a gamepad driven cursor. Unlike UpdateCursorPosition
	// this always applies the delta, even on platforms whose cursor normally
	// follows the real mouse, and it keeps the previous position in step so
	// drag and delta based code still works.
	void MoveCursorGamepad(float dx, float dy);
	// Keep the logical position for legacy mod hover callbacks, while showing
	// a selection frame instead of a pointer in controller focus mode.
	void SetGamepadFocus(bool enabled, const Frect* rect = nullptr);
	bool GamepadFocusMode() const { return m_gamepadFocusMode; }
	virtual void OnScreenResolutionChanged();

	bool IsVisible() { return bVisible; }
	void Show();
	void Hide() { bVisible = false; }
};
