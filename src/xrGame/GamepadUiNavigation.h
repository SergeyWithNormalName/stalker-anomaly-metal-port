#pragma once

#include "ui/UIDialogWnd.h"

class CUITabControl;

class CGamepadUiNavigation
{
public:
	// Commands reserved for controller-aware script dialogs.
	enum EDispatchCommand
	{
		DISPATCH_ACCEPT = 0x4701,
		DISPATCH_CONTEXT = 0x4702,
		DISPATCH_TRANSFER = 0x4703,
		DISPATCH_TAB_PREV = 0x4704,
		DISPATCH_TAB_NEXT = 0x4705,
		DISPATCH_PANEL_PREV = 0x4706,
		DISPATCH_PANEL_NEXT = 0x4707
	};

	CGamepadUiNavigation();
	void Synchronize(CUIDialogWnd* dialog);
	bool HandleKey(int dik, EUIMessages message);
	bool UiAction(int action);
	bool Move(int dik);
	bool Scroll(int direction);
	void Clear();
	bool GetFocusRect(Frect& rect) const;
	CUIWindow* FocusedWindow() const;

private:
	void CollectCandidates(CUIWindow* window, xr_vector<CUIWindow*>& out) const;
	void CollectGroups(CUIWindow* window, xr_vector<CUIWindow*>& out) const;
	void CollectTabs(CUIWindow* window, xr_vector<CUITabControl*>& out) const;
	bool IsCandidate(CUIWindow* window) const;
	bool IsCandidateVisible(CUIWindow* window) const;
	bool HasScriptClickCallback(CUIWindow* window) const;
	void SetFocus(CUIWindow* window);
	void EnsureVisible(CUIWindow* window);
	bool DispatchToDialog(int command);
	bool MovePanel(int direction);
	bool CycleTab(int direction);
	void SnapCursor(CUIWindow* window);
	CUIWindow* CandidateRoot() const;

	CUIDialogWnd* m_dialog;
	CUIWindow* m_focused;
	Frect m_lastFocusRect;
	bool m_hasLastFocusRect;
};
