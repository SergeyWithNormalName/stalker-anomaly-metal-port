#pragma once

#include "../xrEngine/xr_gamepad_mapping.h"

template <typename Receiver>
struct SGamepadInputRoute
{
	EGamepadInputContext context = GAMEPAD_CONTEXT_SUPPRESSED;
	bool blockingSequencer = false;
	Receiver* receiver = nullptr;
};

// Resolve the same forwarding chain used by CUISequencer's input handlers.
// Events still go through the real receiver so tutorial action filters apply.
template <typename Receiver, typename SequencerLookup>
SGamepadInputRoute<Receiver> GamepadResolveInputRoute(Receiver* receiver,
	Receiver* level, Receiver* menu, bool menuActive, bool dialogActive,
	SequencerLookup sequencerFor)
{
	SGamepadInputRoute<Receiver> route;
	for (int depth = 0; receiver && depth < 16; ++depth)
	{
		auto* sequencer = sequencerFor(receiver);
		if (sequencer)
		{
			if (sequencer->GrabInput())
			{
				route.context = GAMEPAD_CONTEXT_UI;
				route.blockingSequencer = true;
				route.receiver = receiver;
				return route;
			}
			receiver = sequencer->m_pStoredInputReceiver;
			continue;
		}
		route.receiver = receiver;
		if (menuActive)
			route.context = receiver == menu ? GAMEPAD_CONTEXT_UI : GAMEPAD_CONTEXT_SUPPRESSED;
		else if (level && receiver == level)
			route.context = dialogActive ? GAMEPAD_CONTEXT_UI : GAMEPAD_CONTEXT_GAMEPLAY;
		return route;
	}
	// Missing receivers and malformed forwarding cycles must never drive gameplay.
	return route;
}
