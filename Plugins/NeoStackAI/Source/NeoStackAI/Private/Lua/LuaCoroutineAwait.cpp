// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaCoroutineAwait.h"
#include "Lua/NeoLuaState.h"
#include "Containers/Ticker.h"

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace UE::NeoStack::Lua
{
	int AwaitAsync(lua_State* L, FKickOffFn&& KickOff)
	{
		check(IsInGameThread());

		if (!L)
		{
			// Defensive — should never happen, but better than crashing.
			return 0;
		}

		if (!lua_isyieldable(L))
		{
			// Not running inside a coroutine — can't yield. This happens if a
			// binding that needs async is called outside a coroutine context
			// (e.g., a hypothetical Lua REPL not running scripts as coroutines).
			return luaL_error(L,
				"This binding requires async execution context (must be called "
				"from inside a coroutine via FNeoLuaState).");
		}

		TSharedPtr<FNeoLuaState> Owner = FNeoLuaState::FindForState(L);
		if (!Owner.IsValid())
		{
			return luaL_error(L,
				"No FNeoLuaState attached to this Lua state — cannot await.");
		}

		// Allocate the await token now so the continuation can capture it. We
		// register with the owner before kicking off so a continuation that
		// fires synchronously inside KickOff (rare but legal) finds the token.
		const FGuid Token = Owner->RegisterAwait();

		// Build the continuation. The owner is held weakly — if the parent
		// FNeoLuaState dies before the async work fires, the continuation
		// becomes a no-op (the binding's async result is silently dropped).
		TWeakPtr<FNeoLuaState> WeakOwner = Owner;
		FAwaitContinuation Continuation =
			[WeakOwner, Token](FPushValuesFn PushFn)
			{
				TSharedPtr<FNeoLuaState> Pinned = WeakOwner.Pin();
				if (!Pinned)
				{
					return;
				}
				Pinned->FireAwait(Token, MoveTemp(PushFn));
			};

		// Hand the continuation to the binding's KickOff. KickOff is expected
		// to start the async work synchronously and capture the continuation
		// for invocation when the result is ready.
		KickOff(MoveTemp(Continuation));

		// Yield. lua_yield long-jumps; control returns to the caller of lua_resume
		// (which is FNeoLuaState::ResumeSegment).
		return lua_yield(L, 0);
	}

	int AwaitDelay(lua_State* L, float Seconds)
	{
		// Negative/zero durations still yield — the ticker fires next frame,
		// keeping a uniform poll-loop shape.
		const float Delay = FMath::Max(Seconds, 0.0f);

		return AwaitAsync(L,
			[Delay](FAwaitContinuation Continuation)
			{
				FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda(
						[Continuation = MoveTemp(Continuation)](float /*DeltaTime*/) -> bool
						{
							Continuation([](lua_State*) -> int { return 0; });
							return false; // one-shot
						}),
					Delay);
			});
	}
}
