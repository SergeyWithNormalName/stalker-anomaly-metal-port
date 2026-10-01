#include "StdAfx.h"
#include <dinput.h>
#include "UIComboBox.h"
#include "UITextureMaster.h"
#include "UIScrollBar.h"
#include "uilistboxitem.h"
#include "../string_table.h"

#define CB_HEIGHT 20.0f

CUIComboBox::CUIComboBox()
{
	AttachChild(&m_frameLine);
	AttachChild(&m_text);

	AttachChild(&m_list_frame);
	m_list_frame.AttachChild(&m_list_box);

	m_iListHeight = 0;
	m_bInited = false;
	m_eState = LIST_FONDED;
	m_textColor[0] = 0xff00ff00;
}

CUIComboBox::~CUIComboBox()
{
}

void CUIComboBox::SetListLength(int length)
{
	R_ASSERT(0 == m_iListHeight);
	m_iListHeight = length;
}

void CUIComboBox::InitComboBox(Fvector2 pos, float width)
{
	float lb_text_offset = 5.0f;

	m_bInited = true;
	if (0 == m_iListHeight)
		m_iListHeight = 4;

	CUIWindow::SetWndPos(pos);
	CUIWindow::SetWndSize(Fvector2().set(width, CB_HEIGHT));

	m_frameLine.InitIB(Fvector2().set(0, 0), Fvector2().set(width, CB_HEIGHT));

	m_frameLine.InitState(S_Enabled, "ui_inGame2_combobox_linetext"); // horizontal by default
	m_frameLine.InitState(S_Highlighted, "ui_inGame2_combobox_linetext");

	for (int s = 0; s < S_Total; ++s)
		if (CUIFrameLineWnd* fl = m_frameLine.Get((IBState)s))
			fl->SetCapScaled(true);

	// Edit Box on left side of frame line
	m_text.SetWndPos(Fvector2().set(lb_text_offset, 0.0f));
	m_text.SetWndSize(Fvector2().set(width - lb_text_offset, CB_HEIGHT));

	m_text.SetVTextAlignment(valCenter);
	m_text.SetTextColor(m_textColor[0]);
	m_text.Enable(false);

	// height of list equal to height of ONE element
	float item_height = CUITextureMaster::GetTextureHeight("ui_inGame2_combobox_line_b");

	m_list_box.SetWndPos(Fvector2().set(lb_text_offset, 0.0f));
	m_list_box.SetWndSize(Fvector2().set(width - lb_text_offset, item_height * m_iListHeight));
	m_list_box.InitScrollView();
	m_list_box.SetTextColor(m_textColor[0]);
	m_list_box.SetSelectionTexture("ui_inGame2_combobox_line");
	m_list_box.SetItemHeight(CUITextureMaster::GetTextureHeight("ui_inGame2_combobox_line_b"));
	// frame(texture) for list
	m_list_frame.InitTexture("ui_inGame2_combobox");
	m_list_frame.SetWndSize(Fvector2().set(width, m_list_box.GetItemHeight() * m_iListHeight));
	m_list_frame.SetWndPos(Fvector2().set(0.0f, CB_HEIGHT));

	m_list_box.Show(true);
	m_list_frame.Show(false);
	m_list_box.SetMessageTarget(this);
}

CUIListBoxItem* CUIComboBox::AddItem_(LPCSTR str, int _data)
{
	R_ASSERT2(m_bInited, "Can't add item to ComboBox before Initialization");
	CUIListBoxItem* itm = m_list_box.AddTextItem(str);
	itm->SetData((void*)(__int64)_data);
	return itm;
}


void CUIComboBox::OnListItemSelect()
{
	m_text.SetText(m_list_box.GetSelectedText());
	CUIListBoxItem* itm = m_list_box.GetSelectedItem();

	int bk_itoken_id = m_itoken_id;

	m_itoken_id = (int)(__int64)itm->GetData();
	ShowList(false);

	if (bk_itoken_id != m_itoken_id)
		GetMessageTarget()->SendMessage(this, LIST_ITEM_SELECT, NULL);
}

void CUIComboBox::SetText(LPCSTR text)
{
	if (!text)
		return;

	m_text.SetText(text);
}

void CUIComboBox::disable_id(int id)
{
	if (m_disabled.end() == std::find(m_disabled.begin(), m_disabled.end(), id))
		m_disabled.push_back(id);
}

void CUIComboBox::enable_id(int id)
{
	xr_vector<int>::iterator it = std::find(m_disabled.begin(), m_disabled.end(), id);

	if (m_disabled.end() != it)
		m_disabled.erase(it);
}

void CUIComboBox::SetCurrentOptValue()
{
	CUIOptionsItem::SetCurrentOptValue();

	m_list_box.Clear();
	xr_token* tok = GetOptToken();

	while (tok->name)
	{
		if (m_disabled.end() == std::find(m_disabled.begin(), m_disabled.end(), tok->id))
		{
			AddItem_(tok->name, tok->id);
		}
		tok++;
	}

	LPCSTR cur_val = *CStringTable().translate(GetOptTokenValue());
	m_text.SetText(cur_val);
	m_list_box.SetSelectedText(cur_val);

	CUIListBoxItem* itm = m_list_box.GetSelectedItem();
	if (itm)
		m_itoken_id = (int)(__int64)itm->GetData();
	else
		m_itoken_id = 1; //first
}

void CUIComboBox::SaveBackUpOptValue()
{
	CUIOptionsItem::SaveBackUpOptValue();
	m_opt_backup_value = m_itoken_id;
}

void CUIComboBox::UndoOptValue()
{
	m_itoken_id = m_opt_backup_value;
	OnChangedOptValue();
	SetItemToken(m_itoken_id);
	CUIOptionsItem::UndoOptValue();
}

void CUIComboBox::SaveOptValue()
{
	CUIOptionsItem::SaveOptValue();
	xr_token* tok = GetOptToken();
	if (tok)
	{
		LPCSTR cur_val = get_token_name(tok, m_itoken_id);
		SaveOptStringValue(cur_val);
	}
}

bool CUIComboBox::IsChangedOptValue() const
{
	return (m_opt_backup_value != m_itoken_id);
}

LPCSTR CUIComboBox::GetText()
{
	return m_text.GetText();
}

u32 CUIComboBox::GetSize()
{
	return m_list_box.GetSize();
}

LPCSTR CUIComboBox::GetTextOf(int index)
{
	if (u32(index) >= GetSize())
		return "";

	return m_list_box.GetText(index);
}


void CUIComboBox::SetItemIDX(int idx)
{
	m_list_box.SetSelectedIDX(idx);
	CUIListBoxItem* itm = m_list_box.GetSelectedItem();
	m_itoken_id = (int)(__int64)itm->GetData();

	m_text.SetText(m_list_box.GetSelectedText());

	OnChangedOptValue();
}

void CUIComboBox::SetItemToken(int tok_id)
{
	int idx = m_list_box.GetIdxByTAG(tok_id);
	SetItemIDX(idx);
}

void CUIComboBox::OnBtnClicked()
{
	ShowList(!m_list_frame.IsShown());
}

void CUIComboBox::ShowList(bool bShow)
{
	if (bShow)
	{
		SetHeight(m_text.GetHeight() + m_list_box.GetHeight());
		m_list_frame.Show(true);
		m_eState = LIST_EXPANDED;
		GetParent()->SetCapture(this, true);
	}
	else
	{
		m_list_frame.Show(false);
		SetHeight(m_frameLine.GetHeight());
		m_eState = LIST_FONDED;
		GetParent()->SetCapture(this, false);
	}
}

void CUIComboBox::Update()
{
	CUIWindow::Update();
	if (!m_bIsEnabled)
	{
		m_frameLine.SetCurrentState(S_Disabled);
		m_text.SetTextColor(m_textColor[1]);
	}
	else
	{
		m_text.SetTextColor(m_textColor[0]);

		if (m_list_frame.IsShown())
		{
			Device.seqRender.Remove(this);
			Device.seqRender.Add(this, 3);
		}
	}
}

void CUIComboBox::OnFocusLost()
{
	CUIWindow::OnFocusLost();
	if (m_bIsEnabled)
		m_frameLine.SetCurrentState(S_Enabled);
}

void CUIComboBox::OnFocusReceive()
{
	CUIWindow::OnFocusReceive();
	if (m_bIsEnabled)
		m_frameLine.SetCurrentState(S_Highlighted);
}

bool CUIComboBox::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
	if (CUIWindow::OnMouseAction(x, y, mouse_action))
		return true;

	if (mouse_action == WINDOW_LBUTTON_DOWN)
	{
		switch (m_eState)
		{
		case LIST_EXPANDED:
			if (!m_list_box.ScrollBar()->CursorOverWindow())
			{
				ShowList(false);
				return true;
			}
			break;
		case LIST_FONDED:
			OnBtnClicked();
			return true;
		default:
			break;
		}
	}

	return false;
}

bool CUIComboBox::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
	if (keyboard_action == WINDOW_KEY_PRESSED && IsShown() &&
		(CursorOverWindow() || m_eState == LIST_EXPANDED))
	{
		if (m_eState == LIST_EXPANDED)
		{
			if (dik == DIK_UP)
			{
				u32 cur = m_list_box.GetSelectedIDX();
				u32 total = m_list_box.GetSize();
				if (total > 0)
				{
					if (cur == u32(-1) || cur == 0)
						m_list_box.SetSelectedIDX(total - 1);
					else
						m_list_box.SetSelectedIDX(cur - 1);
				}
				return true;
			}
			else if (dik == DIK_DOWN)
			{
				u32 cur = m_list_box.GetSelectedIDX();
				u32 total = m_list_box.GetSize();
				if (total > 0)
				{
					if (cur == u32(-1) || cur + 1 >= total)
						m_list_box.SetSelectedIDX(0);
					else
						m_list_box.SetSelectedIDX(cur + 1);
				}
				return true;
			}
			else if (dik == DIK_RETURN || dik == (0xed + 100) /* MOUSE_1 */)
			{
				OnListItemSelect();
				return true;
			}
			else if (dik == DIK_ESCAPE)
			{
				ShowList(false);
				return true;
			}
		}
		else
		{
			if (dik == DIK_RETURN || dik == (0xed + 100) /* MOUSE_1 */)
			{
				OnBtnClicked();
				return true;
			}
			else if (dik == DIK_LEFT)
			{
				u32 cur = m_list_box.GetSelectedIDX();
				if (cur != u32(-1) && cur > 0)
				{
					SetItemIDX(cur - 1);
					GetMessageTarget()->SendMessage(this, LIST_ITEM_SELECT, NULL);
				}
				return true;
			}
			else if (dik == DIK_RIGHT)
			{
				u32 cur = m_list_box.GetSelectedIDX();
				if (cur != u32(-1) && cur + 1 < m_list_box.GetSize())
				{
					SetItemIDX(cur + 1);
					GetMessageTarget()->SendMessage(this, LIST_ITEM_SELECT, NULL);
				}
				return true;
			}
		}
	}

	return CUIWindow::OnKeyboardAction(dik, keyboard_action);
}

void CUIComboBox::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
	CUIWindow::SendMessage(pWnd, msg, pData);

	switch (msg)
	{
	case LIST_ITEM_CLICKED:
		if (pWnd == &m_list_box)
			OnListItemSelect();
		break;
	default:
		break;
	}
}

void CUIComboBox::OnRender()
{
	if (IsShown())
	{
		if (m_list_frame.IsShown())
		{
			m_list_frame.Draw();
			Device.seqRender.Remove(this);
		}
	}
}

void CUIComboBox::Draw()
{
	CUIWindow::Draw();
}

void CUIComboBox::ClearList()
{
	m_list_box.Clear();
	m_text.SetText("");
	m_itoken_id = 0;
	ShowList(false);
	m_disabled.clear();
}

void CUIComboBox::SetSelectedIDX(u32 idx)
{
	m_list_box.SetSelectedIDX(idx);
}

u32 CUIComboBox::GetSelectedIDX()
{
	return m_list_box.GetSelectedIDX();
}

bool CUIComboBox::GetSelectedItemRect(Frect& rect)
{
	if (m_eState != LIST_EXPANDED || !m_list_frame.IsShown())
		return false;

	CUIListBoxItem* item = m_list_box.GetSelectedItem();
	if (!item || !item->IsShown())
		return false;

	Frect viewport;
	Frect item_rect;
	m_list_box.GetAbsoluteRect(viewport);
	item->GetAbsoluteRect(item_rect);

	// Keyboard selection can advance beyond the current scroll window. Bring
	// the selected row fully into the list viewport before exposing its rect to
	// the gamepad focus overlay.
	int scroll_pos = m_list_box.GetCurrentScrollPos();
	if (item_rect.y1 < viewport.y1)
		scroll_pos += iFloor(item_rect.y1 - viewport.y1);
	else if (item_rect.y2 > viewport.y2)
		scroll_pos += iCeil(item_rect.y2 - viewport.y2);
	if (scroll_pos != m_list_box.GetCurrentScrollPos())
	{
		m_list_box.SetScrollPos(scroll_pos);
		item->GetAbsoluteRect(item_rect);
	}

	// Keep the focus outline inside the same scissor region as the list rows.
	// This also handles a list viewport shorter than one item.
	rect.set(_max(item_rect.x1, viewport.x1), _max(item_rect.y1, viewport.y1),
		_min(item_rect.x2, viewport.x2), _min(item_rect.y2, viewport.y2));
	return rect.x1 < rect.x2 && rect.y1 < rect.y2;
}
