// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/ChatTypes.h"

/**
 * Callback target a provider streams FChatEvent into during a completion.
 *
 * Thread contract: OnEvent may be invoked on any thread — typically the HTTP
 * worker thread for HTTP-based providers. Implementations that touch UE game
 * state or broadcast delegates must marshal to the game thread themselves.
 *
 * The session's built-in sink implementation handles this marshalling; external
 * consumers that need direct access to the event stream are expected to go
 * through the session, not implement their own sink.
 */
class NEOSTACKAI_API IChatEventSink
{
public:
	virtual ~IChatEventSink() = default;

	virtual void OnEvent(const FChatEvent& Event) = 0;
};
