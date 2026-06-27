// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Ported from UE 5.8's UE::ToolsetRegistry::Internal::RunOnMainThread
// (Engine/Plugins/Experimental/ToolsetRegistry/.../RunOnMainThread.h).
//
// If already on the game thread, runs the callable inline and returns a fulfilled
// future. Otherwise dispatches via TaskGraphMainThread and returns a pending future.
// Used to marshal HTTP / IO completion callbacks back onto the game thread before
// touching editor state.

#pragma once

#include <type_traits>

#include "Async/Async.h"
#include "Async/Future.h"
#include "Templates/Function.h"
#include "Templates/UnrealTemplate.h"

namespace UE::NeoStack
{
	template<typename CallableT>
	inline auto RunOnMainThread(CallableT&& Callable)
		-> TFuture<decltype(Forward<CallableT>(Callable)())>
	{
		using ReturnT = decltype(Forward<CallableT>(Callable)());
		if (IsInGameThread())
		{
			if constexpr (std::is_void_v<ReturnT>)
			{
				Callable();
				return MakeFulfilledPromise<void>().GetFuture();
			}
			else
			{
				return MakeFulfilledPromise<ReturnT>(Callable()).GetFuture();
			}
		}
		else
		{
			return Async(EAsyncExecution::TaskGraphMainThread, Forward<CallableT>(Callable));
		}
	}

	// Fire-and-forget variant: dispatches to game thread without returning a future.
	// Use when the caller doesn't need to await completion.
	template<typename CallableT>
	inline void RunOnMainThreadDeferred(CallableT&& Callable)
	{
		if (IsInGameThread())
		{
			Callable();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, Forward<CallableT>(Callable));
		}
	}
}
