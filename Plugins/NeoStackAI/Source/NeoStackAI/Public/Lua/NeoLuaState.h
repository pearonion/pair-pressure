// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Misc/Guid.h"

struct lua_State;

struct FScriptResult
{
	bool bSuccess = false;
	TArray<FString> Trace;
	TArray<FToolResultImage> Images;
	FString Error;
	FString ReturnValue;
};

/**
 * Per-script Lua execution context with coroutine support.
 *
 * Heavyweight — heap-allocated (via MakeShared) and held alive across coroutine
 * yields. The state lives until the coroutine completes or is cancelled, then
 * fires its completion callback once.
 *
 * Lifecycle:
 *   1. `auto S = MakeShared<FNeoLuaState>();`
 *   2. `S->Execute(Script, OnComplete);` — script begins running as a coroutine.
 *   3. If a binding calls UE::NeoStack::Lua::AwaitAsync, the coroutine yields and
 *      the call to Execute returns without firing OnComplete. The async work
 *      eventually resumes the coroutine.
 *   4. When the coroutine finishes (success, error, cancelled), OnComplete fires
 *      on the game thread exactly once.
 *
 * Cancellation: Cancel() aborts the script. Any pending awaits are dropped.
 * OnComplete fires with bSuccess=false, Error="Cancelled."
 *
 * Safety wrapping: each coroutine "segment" (initial start or any resume, until
 * the next yield or end) runs inside a UE editor transaction with SEH crash
 * protection and post-segment Blueprint health validation. Transactions don't
 * span yields, so unrelated editor activity during an async wait isn't captured
 * in the script's transaction.
 */
class NEOSTACKAI_API FNeoLuaState : public TSharedFromThis<FNeoLuaState>
{
public:
	using FCompletionCallback = TFunction<void(FScriptResult)>;

	FNeoLuaState();
	~FNeoLuaState();

	FNeoLuaState(const FNeoLuaState&) = delete;
	FNeoLuaState& operator=(const FNeoLuaState&) = delete;

	/**
	 * Begin executing Script as a coroutine.
	 *
	 * Game thread only. OnComplete fires on the game thread when the coroutine
	 * finishes — possibly inline (script never yielded) or much later (script
	 * yielded for async work).
	 */
	void Execute(const FString& Script, FCompletionCallback&& OnComplete);

	/**
	 * Convenience: synchronously execute a script and return the result.
	 *
	 * Used by internal callers that run trusted scripts not expected to call
	 * any async bindings (level serializer, content loader, etc.). If the
	 * script DOES yield, this helper cancels it and returns an error — async
	 * scripts must use the callback Execute instead.
	 *
	 * Constructs and owns its own FNeoLuaState internally. Game thread only.
	 */
	static FScriptResult ExecuteSyncBlocking(const FString& Script);

	/**
	 * Abort the running coroutine. If a callback is pending, fires it with
	 * bSuccess=false, Error="Cancelled". Pending awaits are dropped — their
	 * continuations become no-ops.
	 */
	void Cancel();

	// ──────────────────────────────────────────────────────────────────────
	// Internal — used by UE::NeoStack::Lua::AwaitAsync. Not for direct callers.
	// ──────────────────────────────────────────────────────────────────────

	/** Allocate a token marking that the coroutine is yielding pending an async result. */
	FGuid RegisterAwait();

	/** Fire a pending await: pushes values via PushFn onto the coroutine state, then resumes.
	 *  Safe to call from any thread; marshals to the game thread internally. */
	void FireAwait(const FGuid& Token, TFunction<int(lua_State*)>&& PushFn);

	/** Drop a pending await without resuming. Used when the coroutine is being cancelled
	 *  but the binding's async work might still fire its continuation. */
	void CancelAwait(const FGuid& Token);

	/** Look up the FNeoLuaState owning the given coroutine state. Game thread only.
	 *  Returns nullptr if no FNeoLuaState is attached (e.g., bare Lua console). */
	static TSharedPtr<FNeoLuaState> FindForState(lua_State* L);

	/** Pointer to the script's session (Trace / Images / NodeMaps / etc.). Stays
	 *  valid as long as this FNeoLuaState is alive. Returns nullptr if Impl was
	 *  torn down (shouldn't happen in normal use — defensive only).
	 *
	 *  Bindings that capture this pointer inside async callbacks MUST also hold
	 *  a TWeakPtr<FNeoLuaState> alongside it and pin before each access, otherwise
	 *  the pointer will dangle if the runner is cancelled mid-flight. See
	 *  LuaBinding_Generate.cpp for the pin-and-access pattern. */
	struct FLuaSessionData* GetSession() const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;

	void StartCoroutine();
	void ResumeSegment(int NArgs);
	void Finish(FScriptResult Result);
};
