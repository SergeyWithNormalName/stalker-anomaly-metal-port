#include "stdafx.h"
#include <dinput.h>
#include <typeinfo>
#include <functional>

#include "GamepadInput.h"
#include "GamepadInputRouting.h"

#include "../xrEngine/xr_input.h"
#include "../xrEngine/igame_persistent.h"
#include "xr_level_controller.h"
#include "level.h"
#include "Actor.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "UIGameCustom.h"
#include "MainMenu.h"

#include "GamePersistent.h"
#include "UICursor.h"
#include "ui/UIDialogWnd.h"
#include "ui/UIGameTutorial.h"
#include "ui/UIActorMenu.h"
#include "ui/UIPdaWnd.h"
#include "ui/UITaskWnd.h"
#include "ui/UIMapWnd.h"
#include "ui/UIMap.h"
#include "ui/UITalkWnd.h"
#include "ui/UITalkDialogWnd.h"
#include "ui/UIPropertiesBox.h"
#include "ui/UIDragDropListEx.h"
#include "ui/UITabControl.h"
#include "ui/UITabButton.h"
#include "ui/UICellItem.h"
#include "../xrEngine/xr_ioconsole.h"
#include "ActorCondition.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_engine_space.h"
extern CUISequencer* g_tutorial;
extern CUISequencer* g_tutorial2;

namespace
{
constexpr float gamepad_recoil_ms = 110.0f;

CGamepadInput* g_gamepad_input = nullptr;

CUIGameCustom* gamepad_safe_ui()
{
	if (!g_pGameLevel)
		return nullptr;
	return CurrentGameUI();
}

// The main menu keeps its own input stack and is selection based rather than
// cursor based: the arrow keys move a highlight and enter activates it. A
// synthetic mouse click at the cursor would land on whatever happens to be
// under the cursor instead of on the selection.
bool gamepad_main_menu_active()
{
	if (!g_pGamePersistent)
		return false;

	CMainMenu* menu = MainMenu();
	return menu && menu->IsActive();
}

// Sequencers always capture the engine stack, even for passive HUD prompts.
// Only the current item's GrabInput flag tells us whether input is consumed.
CUISequencer* gamepad_sequencer(IInputReceiver* ir)
{
	if (!ir)
		return nullptr;

	if (g_pGamePersistent)
	{
		CGamePersistent* gp = smart_cast<CGamePersistent*>(g_pGamePersistent);
		if (gp && gp->Intro() && ir == static_cast<IInputReceiver*>(gp->Intro()))
			return gp->Intro();
	}
	if (g_tutorial && ir == static_cast<IInputReceiver*>(g_tutorial))
		return g_tutorial;
	if (g_tutorial2 && ir == static_cast<IInputReceiver*>(g_tutorial2))
		return g_tutorial2;
	return nullptr;
}

SGamepadInputRoute<IInputReceiver> gamepad_route()
{
	CUIDialogWnd* top = nullptr;
	if (CUIGameCustom* ui = gamepad_safe_ui())
		top = ui->TopInputReceiver();

	return GamepadResolveInputRoute(pInput ? pInput->CurrentIR() : nullptr,
		g_pGameLevel ? static_cast<IInputReceiver*>(&Level()) : nullptr,
		g_pGamePersistent ? static_cast<IInputReceiver*>(MainMenu()) : nullptr,
		gamepad_main_menu_active(), top && top->IR_process(), gamepad_sequencer);
}

// An item is in the actor's hands. Used only to decide whether trigger
// resistance is worth showing; it never gates input.
bool gamepad_actor_holds_item()
{
	if (!g_pGameLevel)
		return false;

	CActor* actor = smart_cast<CActor*>(Level().CurrentControlEntity());
	if (!actor)
		return false;

	PIItem item = actor->inventory().ActiveItem();
	return item && item->cast_hud_item() != nullptr;
}

// Translates the mapping's logical controls into engine actions.
//
// The two enums have deliberately unrelated orderings, so this table is the
// only thing tying them together. Leaving it out - or growing the enum without
// extending it - makes a control drive whatever action happens to share its
// ordinal, which is how a stick silently ends up strafing instead of running.
EGameActions gamepad_action_for(EGamepadMappedAction mapped)
{
	switch (mapped)
	{
	case GPA_MoveForward: return kFWD;
	case GPA_MoveBack: return kBACK;
	case GPA_MoveLeft: return kL_STRAFE;
	case GPA_MoveRight: return kR_STRAFE;
	case GPA_Walk: return kACCEL;
	case GPA_Sprint: return kSPRINT_TOGGLE;
	case GPA_Jump: return kJUMP;
	case GPA_Crouch: return kCROUCH;
	case GPA_Fire: return kWPN_FIRE;
	case GPA_Aim: return kWPN_ZOOM;
	case GPA_LeanLeft: return kL_LOOKOUT;
	case GPA_LeanRight: return kR_LOOKOUT;
	case GPA_WeaponFunc: return kWPN_FUNC;
	case GPA_FireModeNext: return kWPN_FIREMODE_NEXT;
	case GPA_Torch: return kTORCH;
	case GPA_Pause: return kPAUSE;
	case GPA_DpadUp: return kWPN_NEXT;
	case GPA_DpadDown: return kDETECTOR;
	case GPA_DpadLeft: return kQUICK_USE_1;
	case GPA_DpadRight: return kQUICK_USE_2;
	case GPA_Reload: return kWPN_RELOAD;
	case GPA_Use: return kUSE;
	case GPA_Pda: return kACTIVE_JOBS;
	case GPA_Inventory: return kINVENTORY;
	case GPA_Back: return kQUIT;
	case GPA_Count: break;
	}
	return kNOTBINDED;
}

// Growing the mapping's action list without extending the table above is the
// one mistake this design cannot catch at run time, because a wrong action
// still resolves to a plausible key. Pinning the count forces a visit here.
static_assert(GPA_Count == 25,
	"EGamepadMappedAction changed: extend gamepad_action_for() to match");

// Resolves a logical gamepad control to the key its action is bound to right
// now. This is the whole reason no key code is hard coded anywhere in the
// mapping: the player's own bindings, including any rebinding, are what gets
// pressed.
int gamepad_resolve_action(int mappedAction)
{
	if (mappedAction == GPA_Reload && g_pGameLevel)
	{
		CActor* actor = smart_cast<CActor*>(Level().CurrentControlEntity());
		if (actor && actor->ObjectWeLookingAt())
		{
			// Contextual Square action: interact / talk / open / loot
			return get_action_dik(kUSE);
		}
	}

	const EGameActions action = gamepad_action_for(EGamepadMappedAction(mappedAction));
	if (action == kNOTBINDED)
		return 0;

	return get_action_dik(action);
}
}

CGamepadInput::CGamepadInput()
{
	m_lastReceiver = nullptr;
	m_lastRouteReceiver = nullptr;
	m_lastDialog = nullptr;
	m_uiFocusActive = false;
	m_debugUiRelease = 0;
	m_debugUiReplay = false;
	m_debugMs = 0.0f;
	m_debugContext = -1;
	m_lightApplied = false;
	m_adaptiveWeaponOn = false;
	m_recoilMs = 0.0f;
	m_lightThrottleMs = 0.0f;
	m_lastLightR = m_lastLightG = m_lastLightB = -1.0f;

	m_mapping.SetOutput(this);
	m_mapping.SetResolver(&gamepad_resolve_action);

	SGamepadMappingConfig config = m_mapping.Config();
	config.uiNavUp = DIK_UP;
	config.uiNavDown = DIK_DOWN;
	config.uiNavLeft = DIK_LEFT;
	config.uiNavRight = DIK_RIGHT;
	config.uiAccept = DIK_RETURN;
	m_mapping.Configure(config);
}

CGamepadInput::~CGamepadInput()
{
	m_mapping.Reset();
}

void CGamepadInput::Register()
{
	if (g_gamepad_input)
		return;

	// Deliberately a process lifetime singleton, like the input receiver stack
	// itself: releasing keys has to be able to reach pInput, so tearing this
	// down during shutdown would be strictly more dangerous than leaving it.
	g_gamepad_input = xr_new<CGamepadInput>();
	Gamepad()->SetSink(g_gamepad_input);
}

IInputReceiver* CGamepadInput::Receiver() const
{
	return pInput ? pInput->CurrentIR() : nullptr;
}

CUIDialogWnd* CGamepadInput::ActiveDialog() const
{
	// New-game setup, loadout and settings belong to the main menu's holder,
	// including before any level exists.
	if (gamepad_main_menu_active())
	{
		CUIDialogWnd* top = MainMenu()->TopInputReceiver();
		return top && top->IR_process() ? top : nullptr;
	}
	CUIGameCustom* ui = gamepad_safe_ui();
	if (!ui)
		return nullptr;

	CUIDialogWnd* top = ui->TopInputReceiver();
	if (top && top->IR_process())
		return top;

	return nullptr;
}

void CGamepadInput::OnGamepadReset()
{
	m_mapping.Reset();
	m_uiNavigation.Clear();
	m_lastDialog = nullptr;
	m_uiFocusActive = false;
	m_debugUiInputs.clear();
	m_debugUiRelease = 0;
	m_debugUiReplay = false;
	GetUICursor().SetGamepadFocus(false);
	m_haptics.Reset();

	// Nothing may be left buzzing or holding a trigger after focus loss.
	if (CGamepadManager* gp = Gamepad())
	{
		gp->ResetEffects();
		m_adaptiveWeaponOn = false;
		m_recoilMs = 0.0f;
	}
}

void CGamepadInput::OnGamepadFrame(const SGamepadFrame& sourceFrame, float dtMs)
{
	if (!pInput)
		return;

	CGamepadManager* gp = Gamepad();
	const auto route = gamepad_route();
	const EGamepadInputContext context = route.context;
	SGamepadFrame frame = sourceFrame;
	// Tests enqueue button pulses from the console, then close it. Feed the
	// pulses only after the UI owns input so they exercise the real routing.
	if (psGamepadDebug && context == GAMEPAD_CONTEXT_UI && m_debugUiReplay)
	{
		frame.Reset();
		frame.connected = frame.extended = true;
		if (m_debugUiRelease || !m_debugUiInputs.empty())
		{
			const bool release = m_debugUiRelease != 0;
			const int control = release ? m_debugUiRelease : m_debugUiInputs.front();
			if (release)
				m_debugUiRelease = 0;
			else
			{
				m_debugUiInputs.erase(m_debugUiInputs.begin());
				m_debugUiRelease = control;
			}
			const u32 bits[] = {0, GAMEPAD_BIT_DPAD_UP, GAMEPAD_BIT_DPAD_DOWN,
				GAMEPAD_BIT_DPAD_LEFT, GAMEPAD_BIT_DPAD_RIGHT, GAMEPAD_BIT_A,
				GAMEPAD_BIT_B, GAMEPAD_BIT_X, GAMEPAD_BIT_Y, GAMEPAD_BIT_LB,
				GAMEPAD_BIT_RB, 0, 0, GAMEPAD_BIT_RS};
			if (control == 14 || control == 15)
			{
				frame.lookY = release ? 0.0f : (control == 14 ? -1.0f : 1.0f);
			}
			else if (control == 11 || control == 12)
			{
				frame.adsPressed = control == 11 && !release;
				frame.firePressed = control == 12 && !release;
			}
			else
			{
				frame.pressed = release ? 0 : bits[control];
				frame.held = frame.pressed;
				frame.released = release ? bits[control] : 0;
			}
		}
	}
	// Record ownership changes even when verbose sample logging is disabled.
	// A passive prompt can keep gameplay context while replacing CurrentIR().
	IInputReceiver* currentReceiver = pInput->CurrentIR();
	const bool routeChanged = m_debugContext != int(context) || m_lastReceiver != currentReceiver;
	if (routeChanged)
	{
		CUISequencer* sequencer = gamepad_sequencer(currentReceiver);
		CUIDialogWnd* top = ActiveDialog();
		Msg("* [gamepad ownership] context=%s receiver=%s tutorial=%s grab=%d modal=%d menu=%d top=%s",
			context == GAMEPAD_CONTEXT_GAMEPLAY ? "gameplay" : context == GAMEPAD_CONTEXT_UI ? "ui" : "suppressed",
			currentReceiver ? typeid(*currentReceiver).name() : "<none>",
			sequencer ? sequencer->m_name : "<none>", sequencer && sequencer->GrabInput(),
			route.blockingSequencer, gamepad_main_menu_active(),
			top && top->WindowName().size() ? top->WindowName().c_str() : "<none>");
		m_debugContext = int(context);
	}
	if (psGamepadDebug)
	{
		m_debugMs += dtMs;
		if (routeChanged || m_debugMs >= 500.0f)
		{
			m_debugMs = 0.0f;
			m_debugContext = int(context);
			CUIDialogWnd* top = ActiveDialog();
			const char* name = top ? top->WindowName().c_str() : nullptr;
			Msg("* [gamepad route] context=%s connected=%d neutral=%d menu=%d sequencer=%d paused=%d top=%s shown=%d process=%d cursor=%d",
				context == GAMEPAD_CONTEXT_GAMEPLAY ? "gameplay" : context == GAMEPAD_CONTEXT_UI ? "ui" : "suppressed",
				frame.connected, m_mapping.NeedsNeutral(), gamepad_main_menu_active(), route.blockingSequencer,
				Device.Paused(), name ? name : "<none>", top && top->IsShown(), top && top->IR_process(), GetUICursor().IsVisible());
			Msg("* [gamepad route] move=(%.3f, %.3f) look=(%.3f, %.3f) ads=%d fire=%d pressed=0x%x held=0x%x bindings: fwd=%d crouch=%d fire=%d aim=%d",
				frame.moveX, frame.moveY, frame.lookX, frame.lookY, frame.ads, frame.fire,
				unsigned(frame.pressed), unsigned(frame.held), gamepad_resolve_action(GPA_MoveForward),
				gamepad_resolve_action(GPA_Crouch), gamepad_resolve_action(GPA_Fire), gamepad_resolve_action(GPA_Aim));
		}
	}

	// Passive prompts forward to the same owner: preserve held movement and
	// avoid requiring the player to center both sticks on approaching a fire.
	// A real owner change still releases all actions and requires neutral.
	if (m_lastRouteReceiver != route.receiver)
	{
		if (m_lastRouteReceiver)
			m_mapping.Reset();
		m_lastRouteReceiver = route.receiver;
	}
	m_lastReceiver = currentReceiver;

	CUIDialogWnd* dialog = ActiveDialog();
	if (m_lastDialog != dialog)
	{
		// A popup and its owner use the same engine receiver and UI context.
		// Reset their repeats and held input just as on a receiver change.
		m_mapping.Reset();
		m_uiNavigation.Clear();
		m_lastDialog = dialog;
	}
	if (context == GAMEPAD_CONTEXT_UI && frame.connected && dialog)
	{
		// The adapter declares mod-created cell controls and their existing
		// actions. It is optional for profiles containing only native controls.
		if (FS.exist("$game_scripts$", "gamepad_ui_compat.script"))
		{
			luabind::functor<void> install;
			if (ai().script_engine().functor("gamepad_ui_compat.install", install))
				install();
		}
		const bool activity = frame.pressed || frame.held || frame.touchActive ||
			frame.moveFwd || frame.moveBack || frame.moveLeft || frame.moveRight ||
			std::fabs(frame.lookX) > 0.2f || std::fabs(frame.lookY) > 0.2f ||
			frame.adsPressed || frame.firePressed;
		if (activity)
			m_uiFocusActive = true;
		else if (!GetUICursor().GamepadFocusMode())
			m_uiFocusActive = false;
	}
	else
		m_uiFocusActive = false;

	// Mapping chooses between selection accept and pointer click after it has
	// processed R3 and the neutral latch in this frame.
	const int accept = DIK_RETURN;
	CUIPdaWnd* pda = smart_cast<CUIPdaWnd*>(dialog);
	const bool mapPan = pda && pda->pUITaskWnd && pda->pUITaskWnd->IsShown() &&
		pda->pUITaskWnd->GetMapWnd() && pda->pUITaskWnd->GetMapWnd()->IsShown();
	if (m_mapping.Config().uiAccept != accept || m_mapping.Config().uiMapPan != mapPan)
	{
		SGamepadMappingConfig config = m_mapping.Config();
		config.uiAccept = accept;
		config.uiMapPan = mapPan;
		m_mapping.Configure(config);
	}
	UpdateUiFocus(m_uiFocusActive && !m_mapping.FallbackCursorEnabled());

	if (route.blockingSequencer)
	{
		// Pressing Circle/B or Options/Start acts as Esc (skip/cancel intro/sequencer)
		if (frame.pressed & (GAMEPAD_BIT_B | GAMEPAD_BIT_START))
		{
			const int escDik = gamepad_resolve_action(GPA_Back);
			const int dik = escDik ? escDik : DIK_ESCAPE;
			GamepadKeyPress(dik);
			GamepadKeyRelease(dik);
			return;
		}

		// Pressing ANY button or pulling any trigger acts as "Press any key to continue"
		if (frame.pressed != 0 || frame.firePressed || frame.adsPressed)
		{
			GamepadKeyPress(DIK_RETURN);
			GamepadKeyRelease(DIK_RETURN);
			return;
		}
	}

	m_mapping.Process(frame, gp->Settings(), dtMs, context);
	UpdateUiFocus(m_uiFocusActive && context == GAMEPAD_CONTEXT_UI &&
		!m_mapping.FallbackCursorEnabled());

	if (frame.connected)
		UpdateEffects(frame, context != GAMEPAD_CONTEXT_GAMEPLAY, dtMs);
}

void CGamepadInput::UpdateUiFocus(bool active)
{
	CUIDialogWnd* dialog = ActiveDialog();
	if (!active || !dialog)
	{
		GetUICursor().SetGamepadFocus(false);
		return;
	}
	// Native talk/inventory already own selection semantics. Other dialogs,
	// including all scripted GAMMA screens, use focused-control dispatch.
	if (smart_cast<CUIActorMenu*>(dialog) || smart_cast<CUITalkWnd*>(dialog))
	{
		CUIActorMenu* actorMenu = smart_cast<CUIActorMenu*>(dialog);
		Frect rect;
		if (actorMenu && actorMenu->CurrentItem())
		{
			actorMenu->CurrentItem()->GetAbsoluteRect(rect);
			GetUICursor().SetGamepadFocus(true, &rect);
		}
		else
			GetUICursor().SetGamepadFocus(true);
		return;
	}
	m_uiNavigation.Synchronize(dialog);
	Frect rect;
	const bool hasFocus = m_uiNavigation.GetFocusRect(rect);
	GetUICursor().SetGamepadFocus(true, hasFocus ? &rect : nullptr);
}

void CGamepadInput::DebugUiInput(int control)
{
	if (!psGamepadDebug || !g_gamepad_input || control < 0 || control > 15)
		return;
	if (control == 0)
	{
		CGamepadInput* input = g_gamepad_input;
		input->m_uiNavigation.Synchronize(input->ActiveDialog());
		CUIWindow* focused = input->m_uiNavigation.FocusedWindow();
		Frect rect;
		const bool hasRect = input->m_uiNavigation.GetFocusRect(rect);
		Msg("* [gamepad focus] pointer=%d focused=%s role=%d rect=(%.1f,%.1f,%.1f,%.1f)",
			input->m_mapping.FallbackCursorEnabled(), focused && focused->WindowName().size()
			? focused->WindowName().c_str() : "<unnamed>", focused ? focused->GetGamepadRole() : -1,
			hasRect ? rect.x1 : 0.0f, hasRect ? rect.y1 : 0.0f,
			hasRect ? rect.x2 : 0.0f, hasRect ? rect.y2 : 0.0f);

		CUIDialogWnd* dialog = input->ActiveDialog();
		Msg("* [gamepad tree] root=%p capture=%p", dialog, dialog ? dialog->GetMouseCapturer() : nullptr);
		std::function<void(CUIWindow*, int)> audit = [&](CUIWindow* window, int depth)
		{
			if (!window || depth > 12) return;
			const char* name = window->WindowName().c_str();
			if ((name && *name) || window->GetGamepadRole() == 3)
			{
				Frect r; window->GetAbsoluteRect(r);
				CUIWindow* parent = window->GetParent();
				Msg("* [gamepad tree] depth=%d window=%p name=%s type=%s shown=%d enabled=%d role=%d parent=%p ptype=%s pshown=%d penabled=%d rect=(%.1f,%.1f,%.1f,%.1f)",
					depth, window, name ? name : "", typeid(*window).name(), window->IsShown(), window->IsEnabled(), window->GetGamepadRole(),
					parent, parent ? typeid(*parent).name() : "none", parent && parent->IsShown(), parent && parent->IsEnabled(), r.x1,r.y1,r.x2,r.y2);
			}
			xrCriticalSectionGuard guard(window->csUi);
			for (CUIWindow* child : window->GetChildWndList()) audit(child, depth + 1);
		};
		audit(dialog, 0);
		return;
	}
	if (g_gamepad_input->m_debugUiInputs.size() < 64)
	{
		g_gamepad_input->m_debugUiReplay = true;
		g_gamepad_input->m_debugUiInputs.push_back(control);
	}
}

// --- output ----------------------------------------------------------------

void CGamepadInput::GamepadKeyPress(int dik)
{
	CUIDialogWnd* dialog = ActiveDialog();
	m_uiNavigation.Synchronize(dialog);
	if (!m_mapping.FallbackCursorEnabled() && dialog &&
		!smart_cast<CUIActorMenu*>(dialog) && !smart_cast<CUITalkWnd*>(dialog) &&
		m_uiNavigation.HandleKey(dik, WINDOW_KEY_PRESSED))
		return;
	if (IInputReceiver* ir = Receiver())
		ir->IR_OnKeyboardPress(dik);
}

void CGamepadInput::GamepadKeyRelease(int dik)
{
	CUIDialogWnd* dialog = ActiveDialog();
	m_uiNavigation.Synchronize(dialog);
	if (!m_mapping.FallbackCursorEnabled() && dialog &&
		!smart_cast<CUIActorMenu*>(dialog) && !smart_cast<CUITalkWnd*>(dialog) &&
		m_uiNavigation.HandleKey(dik, WINDOW_KEY_RELEASED))
		return;
	if (IInputReceiver* ir = Receiver())
		ir->IR_OnKeyboardRelease(dik);
}

void CGamepadInput::GamepadKeyHold(int dik)
{
	if (!m_mapping.FallbackCursorEnabled() && ActiveDialog() &&
		(dik == DIK_RETURN || dik == DIK_UP || dik == DIK_DOWN || dik == DIK_LEFT || dik == DIK_RIGHT))
		return;
	if (IInputReceiver* ir = Receiver())
		ir->IR_OnKeyboardHold(dik);
}

void CGamepadInput::GamepadMouseMove(int dx, int dy)
{
	// The engine's own relative look path: on macOS this is the same call the
	// SDL mouse motion handler makes, so the camera behaves identically.
	if (IInputReceiver* ir = Receiver())
		ir->IR_OnMouseMove(dx, dy);
}

void CGamepadInput::GamepadMouseWheel(int direction)
{
	if (IInputReceiver* ir = Receiver())
		ir->IR_OnMouseWheel(direction);
}

void CGamepadInput::GamepadCursorMove(float dx, float dy)
{
	CUIDialogWnd* dialog = ActiveDialog();

	// Continuous pointer input belongs exclusively to the explicit R3 mode.
	if (!m_mapping.FallbackCursorEnabled())
		return;

	// Only move a cursor that is actually on screen and wanted by the top
	// dialog; the rest of the time the right stick is a camera.
	CUICursor& cursor = GetUICursor();
	if (!cursor.IsVisible() && !(dialog && !dialog->NeedCursor()))
		return;

	cursor.MoveCursorGamepad(dx, dy);
}

void CGamepadInput::GamepadMapPan(float dx, float dy)
{
	CUIPdaWnd* pda = smart_cast<CUIPdaWnd*>(ActiveDialog());
	if (!pda || !m_mapping.Config().uiMapPan || !pda->pUITaskWnd || !pda->pUITaskWnd->GetMapWnd())
		return;
	CUIMapWnd* map = pda->pUITaskWnd->GetMapWnd();
	if (map->GlobalMap() && !map->GlobalMap()->Locked())
		map->MoveMap(Fvector2().set(dx, dy));
}

void CGamepadInput::GamepadMapZoom(bool zoomIn)
{
	CUIPdaWnd* pda = smart_cast<CUIPdaWnd*>(ActiveDialog());
	if (pda && m_mapping.Config().uiMapPan && pda->pUITaskWnd && pda->pUITaskWnd->GetMapWnd())
		pda->pUITaskWnd->GetMapWnd()->UpdateZoom(zoomIn, false);
}

void CGamepadInput::GamepadChordEvent(int chord)
{
	if (chord == CHORD_QUICKSAVE)
	{
		const int saveDik = get_action_dik(kQUICK_SAVE);
		if (saveDik)
		{
			GamepadKeyPress(saveDik);
			GamepadKeyRelease(saveDik);
		}
		else if (Console)
		{
			Console->Execute("save");
		}
	}
	else if (chord == CHORD_QUICKLOAD)
	{
		const int loadDik = get_action_dik(kQUICK_LOAD);
		if (loadDik)
		{
			GamepadKeyPress(loadDik);
			GamepadKeyRelease(loadDik);
		}
		else if (Console)
		{
			Console->Execute("load_last_save");
		}
	}
}

void CGamepadInput::GamepadUiAction(int uiAction)
{
	CUIDialogWnd* dlg = ActiveDialog();
	if (!dlg)
	{
		if (uiAction == UI_ACTION_TAB_PREV || uiAction == UI_ACTION_TAB_NEXT)
			GamepadMouseWheel(uiAction == UI_ACTION_TAB_NEXT ? -1 : 1);
		return;
	}

	if (uiAction == UI_ACTION_SCROLL_UP || uiAction == UI_ACTION_SCROLL_DOWN)
	{
		const int direction = uiAction == UI_ACTION_SCROLL_UP ? 1 : -1;
		if (!m_mapping.FallbackCursorEnabled())
		{
			if (smart_cast<CUIActorMenu*>(dlg) || smart_cast<CUITalkWnd*>(dlg))
				GamepadMouseWheel(direction);
			else
			{
				m_uiNavigation.Synchronize(dlg);
				m_uiNavigation.Scroll(direction);
			}
		}
		else
			GamepadMouseWheel(direction);
		return;
	}
	const bool nativePdaShortcut = smart_cast<CUIPdaWnd*>(dlg) &&
		(uiAction == UI_ACTION_TAB_PREV || uiAction == UI_ACTION_TAB_NEXT ||
		 uiAction == UI_ACTION_PANEL_PREV || uiAction == UI_ACTION_PANEL_NEXT);
	if (!m_mapping.FallbackCursorEnabled() && !smart_cast<CUIActorMenu*>(dlg) &&
		!smart_cast<CUITalkWnd*>(dlg) && !nativePdaShortcut)
	{
		m_uiNavigation.Synchronize(dlg);
		if (m_uiNavigation.UiAction(uiAction))
			return;
	}

	// 1. Context menu (Square / X)
	if (uiAction == UI_ACTION_CONTEXT_MENU)
	{
		CUIActorMenu* actorMenu = smart_cast<CUIActorMenu*>(dlg);
		if (actorMenu && !m_mapping.FallbackCursorEnabled())
		{
			if (actorMenu->CurrentItem())
				actorMenu->OnItemRButtonClick(actorMenu->CurrentItem());
			return;
		}
		if (!m_mapping.FallbackCursorEnabled())
			return;
		GamepadKeyPress(MOUSE_2);
		GamepadKeyRelease(MOUSE_2);
		return;
	}

	// 2. Direct Transfer / Quick Action (Triangle / Y)
	if (uiAction == UI_ACTION_TRANSFER)
	{
		CUIActorMenu* actorMenu = smart_cast<CUIActorMenu*>(dlg);
		if (actorMenu)
		{
			if (actorMenu->CurrentItem())
				actorMenu->OnItemDbClick(actorMenu->CurrentItem());
			return;
		}

		CUITalkWnd* talkWnd = smart_cast<CUITalkWnd*>(dlg);
		if (talkWnd)
		{
			if (talkWnd->UITalkDialogWnd && talkWnd->UITalkDialogWnd->mechanic_mode)
				talkWnd->SwitchToUpgrade();
			else
				talkWnd->SwitchToTrade();
			return;
		}
	}

	// 3. Tab cycling (LB / RB)
	if (uiAction == UI_ACTION_TAB_PREV || uiAction == UI_ACTION_TAB_NEXT)
	{
		const bool next = (uiAction == UI_ACTION_TAB_NEXT);

		CUIActorMenu* actorMenu = smart_cast<CUIActorMenu*>(dlg);
		if (actorMenu)
		{
			int curTab = actorMenu->current_sort_mode();
			int numTabs = (int)actorMenu->m_sort_buttons.size();
			if (numTabs > 1)
			{
				int nextTab = next ? (curTab + 1) % numTabs : (curTab - 1 + numTabs) % numTabs;
				actorMenu->SelectInventoryTab(nextTab);
			}
			return;
		}

		CUIPdaWnd* pdaWnd = smart_cast<CUIPdaWnd*>(dlg);
		if (pdaWnd && pdaWnd->GetTabControl())
		{
			CUITabControl* tabCtrl = pdaWnd->GetTabControl();
			u32 count = tabCtrl->GetTabsCount();
			if (count > 1)
			{
				const shared_str& activeId = tabCtrl->GetActiveId();
				u32 curIdx = 0;
				for (u32 i = 0; i < count; ++i)
				{
					if (tabCtrl->GetButtonById(activeId) == (*tabCtrl->GetButtonsVector())[i])
					{
						curIdx = i;
						break;
					}
				}
				u32 nextIdx = next ? (curIdx + 1) % count : (curIdx + count - 1) % count;
				tabCtrl->SetActiveTab((*tabCtrl->GetButtonsVector())[nextIdx]->m_btn_id);
			}
			return;
		}

		// Fallback for other dialogs: scroll list
		GamepadMouseWheel(next ? -1 : 1);
		return;
	}

	// 4. Panel navigation / Map Zoom (L2 / R2)
	if (uiAction == UI_ACTION_PANEL_PREV || uiAction == UI_ACTION_PANEL_NEXT)
	{
		const bool next = (uiAction == UI_ACTION_PANEL_NEXT);

		CUIPdaWnd* pdaWnd = smart_cast<CUIPdaWnd*>(dlg);
		if (pdaWnd && pdaWnd->pUITaskWnd && pdaWnd->pUITaskWnd->IsShown() &&
			pdaWnd->pUITaskWnd->GetMapWnd() && pdaWnd->pUITaskWnd->GetMapWnd()->IsShown())
		{
			pdaWnd->pUITaskWnd->GetMapWnd()->UpdateZoom(next, false);
			return;
		}

		CUIActorMenu* actorMenu = smart_cast<CUIActorMenu*>(dlg);
		if (actorMenu)
		{
			// Switch focus between Actor Bag and Partner/Loot Bag
			CUIDragDropListEx* targetList = actorMenu->m_currMenuMode == mmTrade
				? (next ? actorMenu->m_pTradePartnerBagList : actorMenu->m_pTradeActorBagList)
				: (next ? actorMenu->m_pDeadBodyBagList : actorMenu->m_pInventoryBagList);

			if (targetList && targetList->IsShown() && targetList->ItemsCount() > 0)
			{
				CUICellItem* first = targetList->GetItemIdx(0);
				if (first)
				{
					Frect r;
					first->GetAbsoluteRect(r);
					GetUICursor().SetUICursorPosition(Fvector2().set(r.x1 + r.width() * 0.5f, r.y1 + r.height() * 0.5f));
					actorMenu->SetCurrentItem(first);
				}
			}
			return;
		}
	}
}

void CGamepadInput::OnWeaponShot(bool isAutomatic)
{
	if (g_gamepad_input)
		g_gamepad_input->m_haptics.OnRealShot(isAutomatic);
}

void CGamepadInput::OnWeaponDryFire()
{
	if (g_gamepad_input)
		g_gamepad_input->m_haptics.OnDryFire();
}

// --- DualSense extras ------------------------------------------------------

void CGamepadInput::UpdateEffects(const SGamepadFrame& frame, bool uiActive, float dtMs)
{
	CGamepadManager* gp = Gamepad();
	if (!gp || !gp->IsSupported())
		return;

	// Dynamic DualSense Lightbar based on Actor health and radiation
	if (gp->Capabilities().light)
	{
		m_lightThrottleMs += dtMs;
		if (m_lightThrottleMs >= 100.0f || !m_lightApplied)
		{
			m_lightThrottleMs = 0.0f;
			float r = 0.05f, g = 0.12f, b = 0.25f; // calm blue default

			if (g_pGameLevel && !uiActive)
			{
				CActor* actor = smart_cast<CActor*>(Level().CurrentControlEntity());
				if (actor)
				{
					float health = actor->GetfHealth();
					float radiation = actor->conditions().GetRadiation();

					if (radiation > 0.05f)
					{
						// Radiation alert: blend yellow/amber
						float radFactor = std::min(1.0f, radiation * 3.0f);
						r = 0.9f * radFactor + 0.1f;
						g = 0.8f * radFactor;
						b = 0.0f;
					}
					else if (health < 0.35f)
					{
						// Critical / Low health: red
						float critFactor = (0.35f - health) / 0.35f;
						r = 0.9f;
						g = 0.05f * (1.0f - critFactor);
						b = 0.05f * (1.0f - critFactor);
					}
					else
					{
						// Normal health: stalker green
						r = (1.0f - health) * 0.4f;
						g = 0.5f * health + 0.1f;
						b = 0.15f;
					}
				}
			}

			// Only update if color changed significantly
			if (!m_lightApplied || std::fabs(r - m_lastLightR) > 0.04f ||
			    std::fabs(g - m_lastLightG) > 0.04f || std::fabs(b - m_lastLightB) > 0.04f)
			{
				m_lightApplied = true;
				m_lastLightR = r;
				m_lastLightG = g;
				m_lastLightB = b;
				gp->SetLight(r, g, b);
			}
		}
	}

	// Real weapon shot haptics from CWeaponMagazined
	SHapticFeedbackRequest hapticReq = m_haptics.Process(dtMs);
	if (hapticReq.triggerHaptic)
		gp->PlayHaptic(hapticReq.amplitude, hapticReq.durationSec);

	// Adaptive triggers. Only R2, the firing trigger, carries resistance, and
	// only while something is in the actor's hands. L2 stays free so that
	// aiming is never fought by the trigger itself. The effect is switched off
	// whenever the interface owns input, because a menu pull is not a weapon pull.
	const bool wantWeapon = !uiActive && gamepad_actor_holds_item();

	if (wantWeapon != m_adaptiveWeaponOn)
	{
		m_adaptiveWeaponOn = wantWeapon;
		gp->SetAdaptiveTrigger(GAMEPAD_HAND_RIGHT,
			wantWeapon ? GAMEPAD_TRIGGER_RESISTANCE : GAMEPAD_TRIGGER_OFF,
			gp->Settings().adaptiveStrength);
	}

	// A brief kick on the firing trigger, then back to its resting resistance.
	if (m_recoilMs > 0.0f)
	{
		m_recoilMs -= dtMs;
		if (m_recoilMs <= 0.0f)
		{
			m_recoilMs = 0.0f;
			gp->SetAdaptiveTrigger(GAMEPAD_HAND_RIGHT,
				m_adaptiveWeaponOn ? GAMEPAD_TRIGGER_RESISTANCE : GAMEPAD_TRIGGER_OFF,
				gp->Settings().adaptiveStrength);
		}
	}
	else if ((hapticReq.triggerAdaptiveKick || frame.firePressed) && m_adaptiveWeaponOn)
	{
		m_recoilMs = gamepad_recoil_ms;
		gp->SetAdaptiveTrigger(GAMEPAD_HAND_RIGHT, GAMEPAD_TRIGGER_RECOIL,
			gp->Settings().adaptiveStrength);
	}

	// Losing the weapon mid recoil must not leave the effect stuck on.
	if (!wantWeapon && m_recoilMs > 0.0f)
	{
		m_recoilMs = 0.0f;
		gp->SetAdaptiveTrigger(GAMEPAD_HAND_RIGHT, GAMEPAD_TRIGGER_OFF, 0.0f);
	}
}
