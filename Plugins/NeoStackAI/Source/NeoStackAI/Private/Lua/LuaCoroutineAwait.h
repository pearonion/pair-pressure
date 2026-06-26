// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Coroutine-based async-await for Lua bindings.
//
// A binding that needs to wait for an async operation (HTTP response, asset
// import, ticker callback, etc.) calls AwaitAsync. AwaitAsync:
//   1. finds the FNeoLuaState owning the coroutine,
//   2. registers a pending await,
//   3. invokes the binding's KickOff with a continuation,
//   4. yields the coroutine via lua_yield (long-jumps).
//
// Whoever called the continuation later (typically an HTTP completion delegate
// on the game thread) provides a function that pushes return values onto the
// coroutine's lua_State; the coroutine then resumes.
//
// Cancellation: if the parent FNeoLuaState is destroyed or cancelled before the
// continuation fires, the continuation becomes a no-op — the binding's async
// work may still complete, but its result is silently dropped.
//
// Usage from a binding:
//
//   Lua.set_function("my_async_op", [&Session](sol::this_state s, std::string arg) -> int
//   {
//       lua_State* L = s.lua_state();
//       return UE::NeoStack::Lua::AwaitAsync(L,
//           [arg](UE::NeoStack::Lua::FAwaitContinuation Continuation)
//           {
//               // Kick off async work — eventually call Continuation with a push fn.
//               StartAsyncWork(arg, [Continuation](FString Result)
//               {
//                   Continuation([Result](lua_State* CoroL) -> int
//                   {
//                       lua_pushstring(CoroL, TCHAR_TO_UTF8(*Result));
//                       return 1;  // pushed 1 value
//                   });
//               });
//           });
//   });

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

struct lua_State;

namespace UE::NeoStack::Lua
{
	/**
	 * Push values onto the coroutine's lua_State stack and return the number pushed.
	 * Called on the game thread, with the lua_State of the coroutine that yielded.
	 */
	using FPushValuesFn = TFunction<int(lua_State*)>;

	/**
	 * Continuation handed to a binding's KickOff. Calling it (with a push function)
	 * resumes the suspended coroutine. Safe to call from any thread; marshals to the
	 * game thread internally before touching Lua state.
	 *
	 * If the FNeoLuaState owning the coroutine has been destroyed or cancelled, the
	 * continuation is a no-op.
	 */
	using FAwaitContinuation = TFunction<void(FPushValuesFn)>;

	/**
	 * KickOff: invoked synchronously from inside the binding's call. Receives the
	 * Continuation to call when the async work completes.
	 */
	using FKickOffFn = TFunction<void(FAwaitContinuation)>;

	/**
	 * Yield the current coroutine and resume when the async work signals completion
	 * via the Continuation passed to KickOff.
	 *
	 * Long-jumps via lua_yield — the caller's frame doesn't return normally.
	 *
	 * @param L         The coroutine's lua_State, typically from sol::this_state.
	 * @param KickOff   Async-work setup. Called synchronously; must capture the
	 *                  Continuation and invoke it later (possibly from another thread)
	 *                  when the result is ready.
	 * @return          The lua_yield return value (always 0 — this function long-jumps).
	 *
	 * Errors via luaL_error if not in a coroutine context, or if no FNeoLuaState
	 * is attached to the state.
	 */
	int AwaitAsync(lua_State* L, FKickOffFn&& KickOff);

	/**
	 * Yield the current coroutine for the given duration in seconds, then resume
	 * with no return values. Implemented as a one-shot FTSTicker — the editor stays
	 * interactive throughout the wait.
	 *
	 * Use for poll-loop intervals between provider status checks.
	 *
	 * @param L        The coroutine's lua_State.
	 * @param Seconds  How long to wait. Negative or zero values resume immediately
	 *                 (next tick) but still yield once, so callers can use this in
	 *                 a uniform poll loop without special-casing the first iteration.
	 * @return         The lua_yield return value (always 0 — this function long-jumps).
	 */
	int AwaitDelay(lua_State* L, float Seconds);
}
