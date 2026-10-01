#include "stdafx.h"
#include "GamepadUiNavigation.h"

#include "UICursor.h"
#include "ui/UIButton.h"
#include "ui/UICheckButton.h"
#include "ui/UIComboBox.h"
#include "ui/UIScrollView.h"
#include "ui/UIScrollBar.h"
#include "ui/UIFixedScrollBar.h"
#include "ui/UITrackBar.h"
#include "ui/UICustomSpin.h"
#include "ui/UICustomEdit.h"
#include "ui/uiabstract.h"
#include "ui/UIWndCallback.h"
#include "ui/UIPropertiesBox.h"
#include "ui/UIListBoxItem.h"
#include "ui/UIScriptWnd.h"
#include "ui/UITabControl.h"
#include "ui/UITabButton.h"
#include "ui/UIMMShniaga.h"
#include "../xrEngine/xr_gamepad_mapping.h"
#include "../xrEngine/xr_input.h"
#include "ui_defs.h"

CGamepadUiNavigation::CGamepadUiNavigation()
	: m_dialog(nullptr), m_focused(nullptr), m_hasLastFocusRect(false)
{
}

void CGamepadUiNavigation::Clear()
{
	// The dialog may have been destroyed by a script rebuild. Do not dereference
	// either saved pointer here; the next Synchronize clears markers in its fresh tree.
	m_focused = nullptr;
	m_dialog = nullptr;
	m_hasLastFocusRect = false;
}

void CGamepadUiNavigation::Synchronize(CUIDialogWnd* dialog)
{
	if (dialog != m_dialog)
	{
		m_focused = nullptr;
		m_dialog = dialog;
		m_hasLastFocusRect = false;
	}
	if (!m_dialog)
		return;

	xr_vector<CUIWindow*> candidates;
	CollectCandidates(CandidateRoot(), candidates);
	auto focusedIt = std::find(candidates.begin(), candidates.end(), m_focused);
	if (focusedIt != candidates.end() && (*focusedIt)->GetGamepadFocused())
	{
		Frect rect;
		(*focusedIt)->GetAbsoluteRect(rect);
		const bool rectChanged = !m_hasLastFocusRect ||
			!fsimilar(rect.x1, m_lastFocusRect.x1) || !fsimilar(rect.y1, m_lastFocusRect.y1) ||
			!fsimilar(rect.x2, m_lastFocusRect.x2) || !fsimilar(rect.y2, m_lastFocusRect.y2);
		if (rectChanged)
		{
			EnsureVisible(*focusedIt);
			SnapCursor(*focusedIt);
		}
		return;
	}

	// The old pointer may have been deleted by a script rebuild. Clear focus
	// only on objects in this fresh tree, never through that old pointer.
	xr_vector<CUIWindow*> dialogCandidates;
	CollectCandidates(m_dialog, dialogCandidates);
	for (CUIWindow* candidate : dialogCandidates)
		candidate->SetGamepadFocused(false);
	m_focused = nullptr;
	m_hasLastFocusRect = false;
	if (!candidates.empty())
	{
		std::sort(candidates.begin(), candidates.end(), [this](CUIWindow* a, CUIWindow* b)
		{
			const bool av = IsCandidateVisible(a);
			const bool bv = IsCandidateVisible(b);
			if (av != bv) return av;
			Frect ar, br;
			a->GetAbsoluteRect(ar);
			b->GetAbsoluteRect(br);
			if (ar.top != br.top) return ar.top < br.top;
			return ar.left < br.left;
		});
		SetFocus(candidates.front());
	}
}

bool CGamepadUiNavigation::HasScriptClickCallback(CUIWindow* window) const
{
	if (!window || !window->GetMessageTarget())
		return false;
	CUIWindow* target = window->GetMessageTarget();
	if (CUIDialogWndEx* scriptDialog = smart_cast<CUIDialogWndEx*>(target))
		return scriptDialog->HasCallback(window, BUTTON_CLICKED);
	if (CUIWndCallback* callback = smart_cast<CUIWndCallback*>(target))
		return callback->HasCallback(window, BUTTON_CLICKED);
	return false;
}

bool CGamepadUiNavigation::IsCandidate(CUIWindow* window) const
{
	if (!window || !window->IsShown() || !window->IsEnabled())
		return false;
	bool insideScrollContent = false;
	for (CUIWindow* parent = window->GetParent(); parent; parent = parent->GetParent())
	{
		// Scrollbar arrows/thumbs are implementation details of a scroll view,
		// not useful controller stops. Inventory content below the same view
		// remains eligible even while it is outside the viewport.
		if (smart_cast<CUIScrollBar*>(parent) || smart_cast<CUIFixedScrollBar*>(parent))
			return false;
		if (smart_cast<CUIScrollView*>(parent))
			insideScrollContent = true;
	}
	Frect screen;
	screen.set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT);
	Frect windowRect;
	window->GetAbsoluteRect(windowRect);
	if (!screen.intersected(windowRect) && !insideScrollContent)
		return false;
	if (window->GetGamepadFocusable() || window->ui_cast_selectable())
	{
		return windowRect.width() > EPS_L && windowRect.height() > EPS_L;
	}
	const bool actionable = smart_cast<CUIButton*>(window) || smart_cast<CUIComboBox*>(window) ||
		smart_cast<CUITrackBar*>(window) || smart_cast<CUICustomSpin*>(window) ||
		smart_cast<CUICustomEdit*>(window) || HasScriptClickCallback(window);
	if (!actionable)
		return false;
	return windowRect.width() > EPS_L && windowRect.height() > EPS_L;
}

bool CGamepadUiNavigation::IsCandidateVisible(CUIWindow* window) const
{
	if (!window)
		return false;
	Frect visible;
	visible.set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT);
	Frect rect;
	window->GetAbsoluteRect(rect);
	if (!visible.intersected(rect))
		return false;
	for (CUIWindow* parent = window->GetParent(); parent; parent = parent->GetParent())
	{
		CUIScrollView* scroll = smart_cast<CUIScrollView*>(parent);
		if (!scroll)
			continue;
		Frect clip;
		scroll->GetAbsoluteRect(clip);
		clip.top += scroll->GetVertIndent() * 0.5f;
		clip.bottom -= scroll->GetVertIndent() * 0.5f;
		if (!clip.intersected(rect))
			return false;
	}
	return true;
}

void CGamepadUiNavigation::CollectCandidates(CUIWindow* window, xr_vector<CUIWindow*>& out) const
{
	if (!window || !window->IsShown() || !window->IsEnabled())
		return;
	// These controls already own a focused row and keyboard behavior. Let the
	// main-menu list keep its primary selection, and let the properties popup
	// keep its selected item/submenu semantics while mouse capture is active.
	if (smart_cast<CUIMMShniaga*>(window) || smart_cast<CUIPropertiesBox*>(window))
		return;
	if (IsCandidate(window))
	{
		out.push_back(window);
		// Composite widgets own their internal controls. Scroll and selectable
		// rows own their items, so do not expose decorative descendants either.
		if (window->GetGamepadRole() != 0 || smart_cast<CUIButton*>(window) ||
			smart_cast<CUIComboBox*>(window) || smart_cast<CUITrackBar*>(window) ||
			smart_cast<CUICustomSpin*>(window) || smart_cast<CUICustomEdit*>(window) ||
			window->ui_cast_selectable())
			return;
	}

	xrCriticalSectionGuard guard(window->csUi);
	for (CUIWindow* child : window->GetChildWndList())
		CollectCandidates(child, out);
}

void CGamepadUiNavigation::CollectGroups(CUIWindow* window, xr_vector<CUIWindow*>& out) const
{
	if (!window || !window->IsShown() || !window->IsEnabled())
		return;
	if (window->GetGamepadRole() == 2)
		out.push_back(window);
	xrCriticalSectionGuard guard(window->csUi);
	for (CUIWindow* child : window->GetChildWndList())
		CollectGroups(child, out);
}

void CGamepadUiNavigation::CollectTabs(CUIWindow* window, xr_vector<CUITabControl*>& out) const
{
	if (!window || !window->IsShown() || !window->IsEnabled())
		return;
	if (CUITabControl* tabs = smart_cast<CUITabControl*>(window))
		out.push_back(tabs);
	xrCriticalSectionGuard guard(window->csUi);
	for (CUIWindow* child : window->GetChildWndList())
		CollectTabs(child, out);
}

CUIWindow* CGamepadUiNavigation::CandidateRoot() const
{
	if (!m_dialog)
		return nullptr;
	CUIWindow* current = m_dialog;
	// Capture propagates one parent edge at a time. Follow the chain so nested
	// popup/list controls own navigation while active.
	for (u32 depth = 0; depth < 32; ++depth)
	{
		CUIWindow* capture = current->GetMouseCapturer();
		if (!capture || capture == current)
			break;
		current = capture;
	}
	return current;
}

void CGamepadUiNavigation::SnapCursor(CUIWindow* window)
{
	if (!window)
		return;
	Frect rect;
	window->GetAbsoluteRect(rect);
	GetUICursor().SetUICursorPosition(Fvector2().set((rect.x1 + rect.x2) * 0.5f, (rect.y1 + rect.y2) * 0.5f));
	if (window == m_focused)
	{
		m_lastFocusRect = rect;
		m_hasLastFocusRect = true;
	}
	// Existing XML/Lua controls use cursor-derived hover state internally. Snap
	// once on focus change and refresh that state synchronously without virtual
	// cursor motion.
	m_dialog->CUIWindow::Update();
}

void CGamepadUiNavigation::SetFocus(CUIWindow* window)
{
	if (!m_dialog || !window)
		return;
	xr_vector<CUIWindow*> candidates;
	CollectCandidates(CandidateRoot(), candidates);
	if (std::find(candidates.begin(), candidates.end(), window) == candidates.end())
		return;
	if (m_focused == window)
		return;
	for (CUIWindow* candidate : candidates)
		candidate->SetGamepadFocused(candidate == window);
	m_focused = window;
	if (window->ui_cast_selectable())
	{
		for (CUIWindow* parent = window->GetParent(); parent; parent = parent->GetParent())
		{
			if (CUIScrollView* scroll = smart_cast<CUIScrollView*>(parent))
			{
				// Keep the list's own selection model in sync without generating a
				// click or script callback during focus movement.
				scroll->SetSelected(window);
				break;
			}
		}
	}
	EnsureVisible(window);
	SnapCursor(window);
}

void CGamepadUiNavigation::EnsureVisible(CUIWindow* window)
{
	if (!window)
		return;
	Frect itemRect;
	window->GetAbsoluteRect(itemRect);
	for (CUIWindow* parent = window->GetParent(); parent && parent != m_dialog; parent = parent->GetParent())
	{
		CUIScrollView* scroll = smart_cast<CUIScrollView*>(parent);
		if (!scroll)
			continue;
		Frect viewRect;
		scroll->GetAbsoluteRect(viewRect);
		const float inset = scroll->GetVertIndent() * 0.5f;
		viewRect.top += inset;
		viewRect.bottom -= inset;
		int next = scroll->GetCurrentScrollPos();
		if (itemRect.top < viewRect.top)
			next -= iFloor(viewRect.top - itemRect.top);
		else if (itemRect.bottom > viewRect.bottom)
			next += iCeil(itemRect.bottom - viewRect.bottom);
		if (next != scroll->GetCurrentScrollPos())
		{
			scroll->SetScrollPos(next);
			window->GetAbsoluteRect(itemRect);
		}
	}
}

bool CGamepadUiNavigation::DispatchToDialog(int command)
{
	return m_dialog && m_focused && m_dialog->Dispatch(command, m_focused->GetGamepadRole());
}

bool CGamepadUiNavigation::Move(int dik)
{
	if (!m_dialog)
		return false;
	xr_vector<CUIWindow*> candidates;
	CollectCandidates(CandidateRoot(), candidates);
	if (candidates.empty())
		return false;
	if (std::find(candidates.begin(), candidates.end(), m_focused) == candidates.end())
	{
		SetFocus(candidates.front());
		return true;
	}

	Frect currentRect;
	m_focused->GetAbsoluteRect(currentRect);
	const float cx = (currentRect.x1 + currentRect.x2) * 0.5f;
	const float cy = (currentRect.y1 + currentRect.y2) * 0.5f;
	CUIWindow* best = nullptr;
	float bestScore = flt_max;
	for (CUIWindow* candidate : candidates)
	{
		if (candidate == m_focused)
			continue;
		Frect rect;
		candidate->GetAbsoluteRect(rect);
		const float dx = (rect.x1 + rect.x2) * 0.5f - cx;
		const float dy = (rect.y1 + rect.y2) * 0.5f - cy;
		const float primary = (dik == DIK_LEFT) ? -dx : (dik == DIK_RIGHT) ? dx : (dik == DIK_UP) ? -dy : dy;
		if (primary <= 0.5f)
			continue;
		const float secondary = (dik == DIK_LEFT || dik == DIK_RIGHT) ? std::abs(dy) : std::abs(dx);
		const float score = primary + secondary * 2.0f;
		if (score < bestScore)
		{
			bestScore = score;
			best = candidate;
		}
	}
	if (!best)
	{
		// Edge wrap uses visual reading order, which keeps long script forms
		// usable even when their layout is not a strict grid.
		std::sort(candidates.begin(), candidates.end(), [this](CUIWindow* a, CUIWindow* b)
		{
			const bool av = IsCandidateVisible(a);
			const bool bv = IsCandidateVisible(b);
			if (av != bv) return av;
			Frect ar, br;
			a->GetAbsoluteRect(ar); b->GetAbsoluteRect(br);
			if (ar.top != br.top) return ar.top < br.top;
			return ar.left < br.left;
		});
		auto it = std::find(candidates.begin(), candidates.end(), m_focused);
		if (it != candidates.end())
		{
			if (dik == DIK_LEFT || dik == DIK_UP)
				best = it == candidates.begin() ? candidates.back() : *(it - 1);
			else
				best = it + 1 == candidates.end() ? candidates.front() : *(it + 1);
		}
	}
	if (!best)
		return false;
	SetFocus(best);
	return true;
}

bool CGamepadUiNavigation::Scroll(int direction)
{
	if (!m_focused || direction == 0)
		return false;
	xr_vector<CUIWindow*> candidates;
	if (!m_dialog)
		return false;
	CollectCandidates(CandidateRoot(), candidates);
	if (std::find(candidates.begin(), candidates.end(), m_focused) == candidates.end())
		return false;
	for (CUIWindow* parent = m_focused->GetParent(); parent && parent != m_dialog; parent = parent->GetParent())
	{
		if (CUIScrollView* scroll = smart_cast<CUIScrollView*>(parent))
		{
			Frect focusedRect;
			m_focused->GetAbsoluteRect(focusedRect);
			const int step = _max(24, iCeil(focusedRect.height()));
			scroll->SetScrollPos(scroll->GetCurrentScrollPos() + (direction > 0 ? -step : step));

			// Keep the same item focused while it remains visible. Once it scrolls
			// away, transfer focus to the nearest visible candidate in this pane.
			xr_vector<CUIWindow*> visibleCandidates;
			CollectCandidates(CandidateRoot(), visibleCandidates);
			CUIWindow* nearest = nullptr;
			float nearestDistance = flt_max;
			for (CUIWindow* candidate : visibleCandidates)
			{
				bool inPane = false;
				for (CUIWindow* ancestor = candidate->GetParent(); ancestor; ancestor = ancestor->GetParent())
				{
					if (ancestor == scroll)
					{
						inPane = true;
						break;
					}
				}
				if (!inPane || !IsCandidateVisible(candidate))
					continue;
				Frect candidateRect;
				candidate->GetAbsoluteRect(candidateRect);
				const float dx = (candidateRect.left + candidateRect.right - focusedRect.left - focusedRect.right) * 0.5f;
				const float dy = (candidateRect.top + candidateRect.bottom - focusedRect.top - focusedRect.bottom) * 0.5f;
				const float distance = dx * dx + dy * dy;
				if (distance < nearestDistance)
				{
					nearestDistance = distance;
					nearest = candidate;
				}
			}
			if (nearest == m_focused)
				SnapCursor(nearest);
			else if (nearest)
				SetFocus(nearest);
			return true;
		}
	}
	return false;
}

bool CGamepadUiNavigation::MovePanel(int direction)
{
	if (!m_dialog || !m_focused || direction == 0)
		return false;
	xr_vector<CUIWindow*> groups;
	CollectGroups(CandidateRoot(), groups);
	CUIWindow* current = nullptr;
	for (CUIWindow* parent = m_focused; parent; parent = parent->GetParent())
	{
		if (parent->GetGamepadRole() == 2)
		{
			current = parent;
			break;
		}
	}
	Frect focusRect;
	m_focused->GetAbsoluteRect(focusRect);
	const float focusX = (focusRect.left + focusRect.right) * 0.5f;
	const float focusY = (focusRect.top + focusRect.bottom) * 0.5f;
	// Some legacy inventory panels keep their cell list beside the decorative
	// `prof` window instead of parenting the cells beneath it. Use the closest
	// visible role-2 anchor when no ancestor anchor exists.
	if (!current && !groups.empty())
	{
		float nearest = flt_max;
		for (CUIWindow* group : groups)
		{
			Frect rect;
			group->GetAbsoluteRect(rect);
			const float dx = (rect.left + rect.right) * 0.5f - focusX;
			const float dy = (rect.top + rect.bottom) * 0.5f - focusY;
			const float distance = dx * dx + dy * dy;
			if (distance < nearest)
			{
				nearest = distance;
				current = group;
			}
		}
	}
	if (groups.size() > 1 && current)
	{
		auto it = std::find(groups.begin(), groups.end(), current);
		if (it != groups.end())
		{
			const auto index = std::distance(groups.begin(), it);
			for (size_t step = 1; step < groups.size(); ++step)
			{
				const int count = int(groups.size());
				const int next = (int(index) + (direction > 0 ? int(step) : -int(step)) + count * 2) % count;
				xr_vector<CUIWindow*> candidates;
				CollectCandidates(groups[next], candidates);
				if (candidates.empty())
				{
					CollectCandidates(CandidateRoot(), candidates);
					candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](CUIWindow* candidate)
					{
						return candidate->GetGamepadRole() != 1;
					}), candidates.end());
					if (!candidates.empty())
					{
						std::sort(candidates.begin(), candidates.end(), [&](CUIWindow* a, CUIWindow* b)
						{
							Frect ar, br;
							a->GetAbsoluteRect(ar); b->GetAbsoluteRect(br);
							Frect gr; groups[next]->GetAbsoluteRect(gr);
							const float gx = (gr.left + gr.right) * 0.5f;
							const float gy = (gr.top + gr.bottom) * 0.5f;
							const float adx = (ar.left + ar.right) * 0.5f - gx;
							const float ady = (ar.top + ar.bottom) * 0.5f - gy;
							const float bdx = (br.left + br.right) * 0.5f - gx;
							const float bdy = (br.top + br.bottom) * 0.5f - gy;
							return adx * adx + ady * ady < bdx * bdx + bdy * bdy;
						});
						candidates.resize(1);
					}
				}
				if (!candidates.empty())
				{
					SetFocus(candidates.front());
					return true;
				}
			}
		}
	}

	// Fallback for native and generic script layouts that have no explicit
	// role-2 anchor: switch between scroll-view panes sharing the current parent.
	CUIScrollView* currentScroll = nullptr;
	for (CUIWindow* parent = m_focused->GetParent(); parent && parent != m_dialog; parent = parent->GetParent())
	{
		if ((currentScroll = smart_cast<CUIScrollView*>(parent)))
			break;
	}
	if (!currentScroll)
		return false;
	xr_vector<CUIWindow*> siblings;
	CUIWindow* parent = currentScroll->GetParent();
	if (!parent)
		return false;
	for (CUIWindow* child : parent->GetChildWndList())
		if (smart_cast<CUIScrollView*>(child) && child->IsShown() && child->IsEnabled())
			siblings.push_back(child);
	if (siblings.size() < 2)
		return false;
	auto it = std::find(siblings.begin(), siblings.end(), currentScroll);
	if (it == siblings.end())
		return false;
	const int count = int(siblings.size());
	for (size_t step = 1; step < siblings.size(); ++step)
	{
		const int next = (int(std::distance(siblings.begin(), it)) + (direction > 0 ? int(step) : -int(step)) + count * 2) % count;
		xr_vector<CUIWindow*> candidates;
		CollectCandidates(siblings[next], candidates);
		if (!candidates.empty())
		{
			SetFocus(candidates.front());
			return true;
		}
	}
	return false;
}

bool CGamepadUiNavigation::CycleTab(int direction)
{
	if (!m_dialog || direction == 0)
		return false;
	xr_vector<CUITabControl*> tabs;
	CollectTabs(CandidateRoot(), tabs);
	CUITabControl* control = nullptr;
	for (CUIWindow* parent = m_focused; parent; parent = parent->GetParent())
	{
		if ((control = smart_cast<CUITabControl*>(parent)))
			break;
	}
	if (!control && !tabs.empty())
		control = tabs.front();
	if (!control || control->GetTabsCount() < 2)
		return false;
	TABS_VECTOR* buttons = control->GetButtonsVector();
	if (!buttons || buttons->empty())
		return false;
	CUITabButton* active = control->GetButtonById(control->GetActiveId());
	auto it = std::find(buttons->begin(), buttons->end(), active);
	const int current = it == buttons->end() ? 0 : int(std::distance(buttons->begin(), it));
	const int count = int(buttons->size());
	const int next = (current + (direction > 0 ? 1 : -1) + count) % count;
	control->SetActiveTab((*buttons)[next]->m_btn_id);
	return true;
}

bool CGamepadUiNavigation::HandleKey(int dik, EUIMessages message)
{
	if (!m_dialog)
		return false;
	Synchronize(m_dialog);
	if (!m_focused)
		return false;
	xr_vector<CUIWindow*> candidates;
	CollectCandidates(CandidateRoot(), candidates);
	if (std::find(candidates.begin(), candidates.end(), m_focused) == candidates.end())
		return false;
	if (message == WINDOW_KEY_PRESSED &&
		(dik == DIK_UP || dik == DIK_DOWN || dik == DIK_LEFT || dik == DIK_RIGHT))
	{
		if (CUIComboBox* combo = smart_cast<CUIComboBox*>(m_focused))
		{
			if (combo->IsListExpanded() || dik == DIK_LEFT || dik == DIK_RIGHT)
			{
				if (m_focused->OnKeyboardAction(dik, message))
					return true;
				if (combo->IsListExpanded())
					return true;
			}
		}
		if (smart_cast<CUITrackBar*>(m_focused) && m_focused->OnKeyboardAction(dik, message))
			return true;
		if (CUICustomSpin* spin = smart_cast<CUICustomSpin*>(m_focused))
		{
			if (dik == DIK_RIGHT)
				spin->OnBtnUpClick();
			else if (dik == DIK_LEFT)
				spin->OnBtnDownClick();
			else
				return Move(dik);
			return true;
		}
		return Move(dik);
	}
	if (message == WINDOW_KEY_PRESSED && dik == DIK_ESCAPE)
	{
		if (CUIComboBox* combo = smart_cast<CUIComboBox*>(m_focused); combo && combo->IsListExpanded())
		{
			m_focused->OnKeyboardAction(dik, message);
			return true;
		}
	}
	if (message == WINDOW_KEY_RELEASED &&
		(dik == DIK_UP || dik == DIK_DOWN || dik == DIK_LEFT || dik == DIK_RIGHT ||
		 dik == DIK_RETURN || dik == DIK_NUMPADENTER || dik == DIK_SPACE))
		return true;

	if (message == WINDOW_KEY_PRESSED && (dik == DIK_RETURN || dik == DIK_NUMPADENTER || dik == DIK_SPACE))
	{
		if (m_focused->GetGamepadRole() == 1)
		{
			// The callback may rebuild/delete this cell even when it reports
			// no action. Its focused accept must never fall through to a freed
			// widget or to a different control's Enter handler.
			DispatchToDialog(DISPATCH_ACCEPT);
			return true;
		}
		if (CUIComboBox* combo = smart_cast<CUIComboBox*>(m_focused))
		{
			m_focused->OnKeyboardAction(dik, message);
			return true;
		}
		if (CUICheckButton* check = smart_cast<CUICheckButton*>(m_focused))
			return check->OnMouseDown(MOUSE_1);
		if (CUIButton* button = smart_cast<CUIButton*>(m_focused))
		{
			button->OnClick();
			return true;
		}
		if (HasScriptClickCallback(m_focused))
		{
			m_focused->GetMessageTarget()->SendMessage(m_focused, BUTTON_CLICKED, nullptr);
			return true;
		}
		if (CUIListBoxItem* row = smart_cast<CUIListBoxItem*>(m_focused))
			return row->OnMouseDown(MOUSE_1);
	}
	return m_focused->OnKeyboardAction(dik, message);
}

bool CGamepadUiNavigation::UiAction(int action)
{
	if (!m_dialog)
		return false;
	Synchronize(m_dialog);
	if (!m_focused)
		return false;
	int command = 0;
	switch (action)
	{
	case UI_ACTION_CONTEXT_MENU: command = DISPATCH_CONTEXT; break;
	case UI_ACTION_TRANSFER: command = DISPATCH_TRANSFER; break;
	case UI_ACTION_TAB_PREV: command = DISPATCH_TAB_PREV; break;
	case UI_ACTION_TAB_NEXT: command = DISPATCH_TAB_NEXT; break;
	case UI_ACTION_PANEL_PREV: command = DISPATCH_PANEL_PREV; break;
	case UI_ACTION_PANEL_NEXT: command = DISPATCH_PANEL_NEXT; break;
	default: return false;
	}
	if ((action == UI_ACTION_CONTEXT_MENU || action == UI_ACTION_TRANSFER) &&
		m_focused->GetGamepadRole() == 1 && DispatchToDialog(command))
		return true;
	if ((action == UI_ACTION_TAB_PREV || action == UI_ACTION_TAB_NEXT) &&
		(m_focused->GetGamepadRole() == 1 || m_dialog->GetGamepadRole() == 3) && DispatchToDialog(command))
		return true;
	if (action == UI_ACTION_TAB_PREV || action == UI_ACTION_TAB_NEXT)
		return CycleTab(action == UI_ACTION_TAB_NEXT ? 1 : -1) || Scroll(action == UI_ACTION_TAB_NEXT ? -1 : 1);
	if (action == UI_ACTION_PANEL_PREV || action == UI_ACTION_PANEL_NEXT)
	{
		if (m_focused->GetGamepadRole() == 1 && DispatchToDialog(command))
			return true;
		return MovePanel(action == UI_ACTION_PANEL_NEXT ? 1 : -1);
	}
	return false;
}

bool CGamepadUiNavigation::GetFocusRect(Frect& rect) const
{
	if (!m_dialog || !m_focused)
		return false;
	xr_vector<CUIWindow*> candidates;
	CollectCandidates(CandidateRoot(), candidates);
	if (std::find(candidates.begin(), candidates.end(), m_focused) == candidates.end())
		return false;
	if (CUIComboBox* combo = smart_cast<CUIComboBox*>(m_focused))
		if (combo->GetSelectedItemRect(rect))
			return true;
	m_focused->GetAbsoluteRect(rect);
	return true;
}

CUIWindow* CGamepadUiNavigation::FocusedWindow() const
{
	if (!m_dialog || !m_focused)
		return nullptr;
	xr_vector<CUIWindow*> candidates;
	CollectCandidates(CandidateRoot(), candidates);
	return std::find(candidates.begin(), candidates.end(), m_focused) == candidates.end() ? nullptr : m_focused;
}
