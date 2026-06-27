// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/IChatEventSink.h"
#include "Chat/ChatSession.h"
#include "Async/Async.h"

/**
 * IChatEventSink implementation that forwards provider events to a FChatSession
 * via a weak pointer, marshalling to the game thread.
 *
 * The session owns the sink via TSharedPtr and hands out TSharedRef<IChatEventSink>
 * to providers when starting a stream. The weak pointer back to the session means
 * the sink gracefully becomes a no-op if the session is released while a stream
 * is still in flight — the provider finishes its work and the sink drops whatever
 * events arrive.
 */
class FChatSessionSink : public IChatEventSink
{
public:
	explicit FChatSessionSink(TWeakPtr<FChatSession> InSession, uint64 InTurnGeneration)
		: Session(InSession)
		, TurnGeneration(InTurnGeneration)
	{}

	virtual void OnEvent(const FChatEvent& Event) override
	{
		TWeakPtr<FChatSession> WeakSession = Session;
		const uint64 CapturedTurnGeneration = TurnGeneration;

		auto Dispatch = [WeakSession, CapturedTurnGeneration, Event]()
		{
			if (TSharedPtr<FChatSession> Pinned = WeakSession.Pin())
			{
				Pinned->HandleEvent(Event, CapturedTurnGeneration);
			}
		};

		if (IsInGameThread())
		{
			Dispatch();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(Dispatch));
		}
	}

private:
	TWeakPtr<FChatSession> Session;
	uint64 TurnGeneration = 0;
};
