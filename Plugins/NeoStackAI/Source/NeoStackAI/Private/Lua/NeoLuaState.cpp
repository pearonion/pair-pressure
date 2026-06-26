// Copyright 2026 Betide Studio. All Rights Reserved.
//
// FNeoLuaState — async coroutine-based Lua script execution.
//
// The previous version was a stack-allocated synchronous executor: build state,
// run safe_script, return result. This rewrite makes it heap-allocated and
// coroutine-based so bindings can yield while waiting for async work (HTTP,
// asset import, etc.) without freezing the editor.
//
// Each script gets its own sol::state and lua_State main thread. The script is
// loaded as a function on a child sol::thread (the coroutine), and lua_resume
// drives execution. Each lua_resume call is a "segment" — wrapped with a UE
// editor transaction, SEH crash protection, and post-segment Blueprint health
// validation. Segments end on yield (binding called AwaitAsync) or completion.
//
// A static state-map (lua main thread → TWeakPtr<FNeoLuaState>) lets binding
// helpers find their parent runner from any lua_State*. Game-thread only access.

#include "Lua/NeoLuaState.h"
#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphFinalizer.h"
#include "Lua/LuaCoroutineAwait.h"
#include "RunOnMainThread.h"
#include "NSAIAnalytics.h"
#include "Misc/Paths.h"

// Editor / safety wrapping
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

DEFINE_LOG_CATEGORY_STATIC(LogNeoLua, Log, All);

namespace
{
	// We attach the FNeoLuaState* to each lua_State via the per-state extra space
	// (LUA_EXTRASPACE bytes — 8 on 64-bit, exactly fits a pointer). Set on both
	// the main state and the coroutine thread state during Execute setup so
	// AwaitAsync can find the runner from any state of the script.
	void SetExtraSpacePtr(lua_State* L, FNeoLuaState* Self)
	{
		*static_cast<FNeoLuaState**>(lua_getextraspace(L)) = Self;
	}

	FNeoLuaState* GetExtraSpacePtr(lua_State* L)
	{
		if (!L) return nullptr;
		return *static_cast<FNeoLuaState**>(lua_getextraspace(L));
	}

	// SEH crash-protection wrapper. Same shape as the one in NeoStackToolRegistry —
	// MSVC C2712 forces __try into a function with no objects requiring unwinding.
#if PLATFORM_WINDOWS
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

	struct FSEHResumeContext
	{
		lua_State* L;
		int NArgs;
		int* OutStatus;
		int* OutNumResults;
	};

	void ExecuteResumeKickoff(FSEHResumeContext* Ctx)
	{
		// Lua 5.4: lua_resume(L, from, nargs, *nresults). nresults is an out-param
		// receiving the number of values yielded/returned.
		*Ctx->OutStatus = lua_resume(Ctx->L, nullptr, Ctx->NArgs, Ctx->OutNumResults);
	}

	bool TryResumeWithSEH(FSEHResumeContext* Ctx)
	{
		__try
		{
			ExecuteResumeKickoff(Ctx);
			return true;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
#endif

	// Validate a blueprint hasn't been corrupted. Skip GeneratedClass check for
	// blueprints that already had null GeneratedClass before script execution.
	bool ValidateBlueprintHealth(UBlueprint* BP, FString& OutErrors, bool bCheckGeneratedClass)
	{
		if (!BP) { OutErrors = TEXT("null blueprint pointer"); return false; }

		if (bCheckGeneratedClass && !BP->GeneratedClass)
		{
			OutErrors = FString::Printf(TEXT("'%s' GeneratedClass is null (compilation state corrupted)"), *BP->GetName());
			return false;
		}

		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || !IsValid(Graph)) continue;

			UBlueprint* OwnerBP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
			if (!OwnerBP)
			{
				OutErrors = FString::Printf(TEXT("Graph '%s' in '%s' has broken outer chain (orphaned)"),
					*Graph->GetName(), *BP->GetName());
				return false;
			}
		}

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || !IsValid(Graph)) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !IsValid(Node))
				{
					OutErrors = FString::Printf(TEXT("Graph '%s' in '%s' contains invalid/null node"),
						*Graph->GetName(), *BP->GetName());
					return false;
				}
			}
		}

		return true;
	}

	// Snapshot all blueprints currently in a "null GeneratedClass" state. These
	// are pre-existing oddities (e.g. Mover plugin movement modes) that we must
	// not flag as corruption after script execution.
	void SnapshotPreExistingNullGeneratedClass(TSet<UBlueprint*>& OutSet)
	{
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (IsValid(*It) && !(*It)->GeneratedClass)
			{
				OutSet.Add(*It);
			}
		}
	}
}

// ──────────────────────────────────────────────────────────────────────────────
// FImpl
// ──────────────────────────────────────────────────────────────────────────────

struct FNeoLuaState::FImpl
{
	// Owned Lua state. The script and all bindings live on this state's globals.
	sol::state Lua;

	// Shared session: bindings capture this by reference. Outlives the script.
	FLuaSessionData Session;

	// Coroutine thread. Owns a child lua_State that shares Lua's global state.
	// The script function is pushed onto this thread's stack and resumed via
	// lua_resume.
	sol::thread Thread;

	// Cached pointer to the thread's lua_State. Equivalent to Thread.thread_state(),
	// stored to avoid repeated calls.
	lua_State* CoroL = nullptr;

	// Cached pointer to the main lua_State of Lua. Used as the key in the static
	// state map (so any thread of this state can look up the FNeoLuaState).
	lua_State* MainL = nullptr;

	// Final result is built up here as the script runs.
	FCompletionCallback OnComplete;

	// Pending awaits (one entry per outstanding AwaitAsync that hasn't fired yet).
	TSet<FGuid> PendingAwaits;

	// Snapshot taken once at script start; reused across all segments for blueprint
	// validation (don't false-positive blueprints that always had null GeneratedClass).
	TSet<UBlueprint*> PreExistingNullGeneratedClass;

	bool bStarted = false;
	bool bFinished = false;
	bool bCancelled = false;
};

// ──────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ──────────────────────────────────────────────────────────────────────────────

FNeoLuaState::FNeoLuaState()
	: Impl(MakeUnique<FImpl>())
{
}

FNeoLuaState::~FNeoLuaState()
{
	// Defensive cleanup: if we never finished, fire the callback with an error so
	// no consumer is left hanging. Extra-space pointers naturally die with the
	// sol::state (which closes the lua_State) — nothing to clean up explicitly.
	if (Impl)
	{
		if (!Impl->bFinished && Impl->OnComplete)
		{
			FScriptResult R;
			R.bSuccess = false;
			R.Error = TEXT("Lua state destroyed without completion (this is a bug — Cancel should have been called).");
			FCompletionCallback Cb = MoveTemp(Impl->OnComplete);
			Cb(MoveTemp(R));
		}
	}
}

// ──────────────────────────────────────────────────────────────────────────────
// Static lookup
// ──────────────────────────────────────────────────────────────────────────────

TSharedPtr<FNeoLuaState> FNeoLuaState::FindForState(lua_State* L)
{
	check(IsInGameThread());
	FNeoLuaState* Raw = GetExtraSpacePtr(L);
	if (!Raw) return nullptr;
	// Raw is alive — bindings only run while the script is running, and the
	// FNeoLuaState is held in InProgress on the owning tool throughout. Convert
	// to TSharedPtr via shared-from-this so callers get safe lifetime semantics.
	return Raw->AsShared();
}

FLuaSessionData* FNeoLuaState::GetSession() const
{
	return Impl.IsValid() ? &Impl->Session : nullptr;
}

// ──────────────────────────────────────────────────────────────────────────────
// Execute / Cancel
// ──────────────────────────────────────────────────────────────────────────────

void FNeoLuaState::Execute(const FString& Script, FCompletionCallback&& OnComplete)
{
	check(IsInGameThread());

	if (Impl->bStarted)
	{
		// Double-execute protection — caller bug.
		FScriptResult R;
		R.bSuccess = false;
		R.Error = TEXT("FNeoLuaState::Execute called twice on the same instance.");
		OnComplete(MoveTemp(R));
		return;
	}

	Impl->bStarted = true;
	Impl->OnComplete = MoveTemp(OnComplete);

	// Open standard libs on the main state.
	Impl->Lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::coroutine);

	// Built-in helpers.
	{
		FLuaSessionData& Session = Impl->Session;
		Impl->Lua.set_function("log", [&Session](const std::string& Msg)
		{
			Session.Log(UTF8_TO_TCHAR(Msg.c_str()));
		});

		Impl->Lua.set_function("print", [&Session](sol::variadic_args Va, sol::this_state S)
		{
			sol::state_view LuaView(S);
			sol::function Tostring = LuaView["tostring"];
			FString Line;
			for (auto Arg : Va)
			{
				if (Line.Len() > 0) Line += TEXT("\t");
				std::string Str = Tostring(Arg).get<std::string>();
				Line += UTF8_TO_TCHAR(Str.c_str());
			}
			Session.Log(Line);
		});

		Impl->Lua.set_function("project_dir", []() -> std::string
		{
			FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			return TCHAR_TO_UTF8(*Dir);
		});
	}

	// Apply all registered bindings.
	for (const FLuaBinding& Binding : FLuaBindingRegistry::Get().GetAll())
	{
		Binding.Bind.Execute(Impl->Lua, Impl->Session);
	}

	// Cache main lua_State and stash a self-pointer in its extra space so
	// bindings can look us up via FindForState.
	Impl->MainL = Impl->Lua.lua_state();
	SetExtraSpacePtr(Impl->MainL, this);

	// Snapshot pre-existing-null-GeneratedClass blueprints (once, reused per segment).
	SnapshotPreExistingNullGeneratedClass(Impl->PreExistingNullGeneratedClass);

	// Build the coroutine thread and stash the self-pointer in its extra space too.
	// Each lua_State has its own extra space (LUA_EXTRASPACE bytes); main and
	// coroutine don't share, so set both.
	Impl->Thread = sol::thread::create(Impl->Lua);
	Impl->CoroL = Impl->Thread.thread_state();
	SetExtraSpacePtr(Impl->CoroL, this);

	// Infinite loop protection. Hooks are per-thread in Lua, so set this on the
	// coroutine state — the main state never executes user script directly.
	lua_sethook(Impl->CoroL, [](lua_State* InL, lua_Debug*)
	{
		luaL_error(InL, "Script exceeded instruction limit (possible infinite loop)");
	}, LUA_MASKCOUNT, 1047382);

	// Load the script as a function ON the coroutine state. luaL_loadstring leaves
	// the function on the stack, ready for lua_resume to invoke.
	const FTCHARToUTF8 ScriptUtf8(*Script);
	const int LoadStatus = luaL_loadstring(Impl->CoroL, ScriptUtf8.Get());
	if (LoadStatus != LUA_OK)
	{
		// Compile error — top of stack has the error message.
		FString Err;
		if (lua_isstring(Impl->CoroL, -1))
		{
			Err = UTF8_TO_TCHAR(lua_tostring(Impl->CoroL, -1));
		}
		else
		{
			Err = TEXT("Failed to compile Lua script (no error message)");
		}
		lua_pop(Impl->CoroL, 1);

		FScriptResult R;
		R.bSuccess = false;
		R.Error = Err;
		Impl->Session.Log(FString::Printf(TEXT("[ERROR] %s"), *Err));
		UE_LOG(LogNeoLua, Warning, TEXT("Lua compile error: %s"), *Err);
		FNSAIAnalytics::Get().RecordLuaError(TEXT("lua_compile"), Err);

		Finish(MoveTemp(R));
		return;
	}

	// Start the coroutine. Runs until first yield or completion.
	StartCoroutine();
}

void FNeoLuaState::StartCoroutine()
{
	check(IsInGameThread());
	check(Impl->bStarted && !Impl->bFinished);

	// Initial resume: 0 args (script function takes no parameters).
	ResumeSegment(0);
}

void FNeoLuaState::Cancel()
{
	check(IsInGameThread());

	if (Impl->bFinished || Impl->bCancelled)
	{
		return;
	}
	Impl->bCancelled = true;

	// Drop all pending awaits — their continuations become no-ops.
	Impl->PendingAwaits.Empty();

	// We don't try to lua_resume the coroutine into an error state — we simply
	// abandon it. When this FNeoLuaState destructs, sol::state's destructor
	// closes the global state, which closes all threads. Lua GC handles cleanup.

	FScriptResult R;
	R.bSuccess = false;
	R.Error = TEXT("Cancelled");
	Finish(MoveTemp(R));
}

// ──────────────────────────────────────────────────────────────────────────────
// Segment execution with safety wrapping
// ──────────────────────────────────────────────────────────────────────────────

void FNeoLuaState::ResumeSegment(int NArgs)
{
	check(IsInGameThread());
	check(Impl->bStarted);
	if (Impl->bFinished || Impl->bCancelled)
	{
		// Defensive: shouldn't be resumed after finish/cancel.
		return;
	}

	// 1. Track packages modified during this segment for blueprint health validation.
	TSet<UPackage*> ModifiedPackages;
	FDelegateHandle DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
		[&ModifiedPackages](UPackage* Pkg, bool /*bWasDirty*/)
		{
			if (Pkg) ModifiedPackages.Add(Pkg);
		});

	// 2. Begin transaction for this segment. Each segment is its own undo entry —
	//    transactions don't span yields, so unrelated editor activity during an
	//    async wait isn't captured in the script's transaction.
	int32 TransIndex = INDEX_NONE;
	if (GEditor)
	{
		TransIndex = GEditor->BeginTransaction(FText::FromString(TEXT("AIK: execute_script")));
	}

	// 3. Resume the coroutine, SEH-wrapped on Windows.
	int Status = LUA_OK;
	int NumResults = 0;  // out-param of lua_resume in Lua 5.4
	bool bSegmentCrashed = false;

#if PLATFORM_WINDOWS
	{
		FSEHResumeContext Ctx = { Impl->CoroL, NArgs, &Status, &NumResults };
		if (!TryResumeWithSEH(&Ctx))
		{
			bSegmentCrashed = true;
		}
	}
#else
	Status = lua_resume(Impl->CoroL, nullptr, NArgs, &NumResults);
#endif

	// 4. Unsubscribe from package dirty events.
	UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);

	// 5. Validate any modified blueprints for corruption.
	bool bNeedsRollback = bSegmentCrashed;
	FString ValidationError;
	if (!bNeedsRollback && Status == LUA_OK && ModifiedPackages.Num() > 0)
	{
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* BP = *It;
			if (BP && IsValid(BP) && ModifiedPackages.Contains(BP->GetPackage()))
			{
				const bool bHadGeneratedClass = Impl->PreExistingNullGeneratedClass.Find(BP) == nullptr;
				if (!ValidateBlueprintHealth(BP, ValidationError, bHadGeneratedClass))
				{
					bNeedsRollback = true;
					break;
				}
			}
		}
	}

	// 6. Rollback or commit this segment's transaction.
	if (GEditor && TransIndex != INDEX_NONE)
	{
		if (bNeedsRollback)
		{
			GEditor->CancelTransaction(TransIndex);
			UE_LOG(LogNeoLua, Error, TEXT("Lua segment rolled back: %s"),
				bSegmentCrashed ? TEXT("crash detected") : *ValidationError);
		}
		else
		{
			GEditor->EndTransaction();
		}
	}

	// 7. Decide what to do next based on status.
	if (bSegmentCrashed)
	{
		FScriptResult R;
		R.bSuccess = false;
		R.Error = TEXT("Lua segment crashed (access violation caught). Changes rolled back. "
		               "This usually indicates blueprint compilation during PIE or stale engine state.");
		Impl->Session.Log(FString::Printf(TEXT("[ERROR] %s"), *R.Error));
		UE_LOG(LogNeoLua, Error, TEXT("%s"), *R.Error);
		FNSAIAnalytics::Get().RecordLuaError(TEXT("lua_crash"), R.Error);
		Finish(MoveTemp(R));
		return;
	}

	if (bNeedsRollback)
	{
		FScriptResult R;
		R.bSuccess = false;
		R.Error = FString::Printf(TEXT("Blueprint corruption detected after Lua segment: %s. Changes rolled back."),
			*ValidationError);
		Impl->Session.Log(FString::Printf(TEXT("[ERROR] %s"), *R.Error));
		UE_LOG(LogNeoLua, Error, TEXT("%s"), *R.Error);
		FNSAIAnalytics::Get().RecordLuaError(TEXT("lua_blueprint_corruption"), R.Error);
		Finish(MoveTemp(R));
		return;
	}

	if (Status == LUA_OK)
	{
		// Coroutine completed normally. Capture return value (top of stack) if present.
		FScriptResult R;
		R.bSuccess = true;
		const int Top = lua_gettop(Impl->CoroL);
		if (Top >= 1 && lua_isstring(Impl->CoroL, -1))
		{
			R.ReturnValue = UTF8_TO_TCHAR(lua_tostring(Impl->CoroL, -1));
		}
		// Pop everything the coroutine left on the stack.
		lua_settop(Impl->CoroL, 0);

		Finish(MoveTemp(R));
		return;
	}

	if (Status == LUA_YIELD)
	{
		// Coroutine yielded. Whatever the script left on the stack at the yield point
		// is irrelevant to us — when it resumes, we'll push fresh values via FireAwait.
		lua_settop(Impl->CoroL, 0);

		// If no awaits are pending (e.g., script called bare coroutine.yield()), the
		// coroutine will hang forever. Log a warning but otherwise leave it — the
		// outer Cancel() path will clean up if the parent tool is cancelled.
		if (Impl->PendingAwaits.IsEmpty())
		{
			UE_LOG(LogNeoLua, Warning,
				TEXT("Lua coroutine yielded with no pending await. Script will hang until cancelled."));
		}
		return;
	}

	// Any other status is a Lua-level error. Top of stack has the message.
	FString Err;
	if (lua_isstring(Impl->CoroL, -1))
	{
		Err = UTF8_TO_TCHAR(lua_tostring(Impl->CoroL, -1));
	}
	else
	{
		Err = FString::Printf(TEXT("Lua resume returned status %d (no error message)"), Status);
	}
	lua_settop(Impl->CoroL, 0);

	{
		FScriptResult R;
		R.bSuccess = false;
		R.Error = Err;
		Impl->Session.Log(FString::Printf(TEXT("[ERROR] %s"), *Err));
		UE_LOG(LogNeoLua, Warning, TEXT("Lua error: %s"), *Err);
		FNSAIAnalytics::Get().RecordLuaError(TEXT("lua_runtime"), Err);
		Finish(MoveTemp(R));
	}
}

// ──────────────────────────────────────────────────────────────────────────────
// Finish — fire OnComplete exactly once
// ──────────────────────────────────────────────────────────────────────────────

void FNeoLuaState::Finish(FScriptResult Result)
{
	check(IsInGameThread());

	if (Impl->bFinished)
	{
		return;
	}
	Impl->bFinished = true;

	// Clear the extra-space self-pointer so any stale binding lookup post-finish
	// returns null. The lua_State itself dies with sol::state.
	if (Impl->MainL) { SetExtraSpacePtr(Impl->MainL, nullptr); Impl->MainL = nullptr; }
	if (Impl->CoroL) { SetExtraSpacePtr(Impl->CoroL, nullptr); Impl->CoroL = nullptr; }

	// Finalize all graphs that were mutated during script execution.
	// Only safe if we're not in a crashed/cancelled state — finalizing a
	// half-mutated graph could compound the damage.
	if (Result.bSuccess)
	{
		for (auto& Pair : Impl->Session.DirtyGraphs)
		{
			LuaGraphFinalizer::FinalizeGraph(Pair.Key, Pair.Value);
		}
	}

	Result.Trace = MoveTemp(Impl->Session.Trace);
	Result.Images = MoveTemp(Impl->Session.Images);

	FCompletionCallback Cb = MoveTemp(Impl->OnComplete);
	if (Cb)
	{
		Cb(MoveTemp(Result));
	}
}

// ──────────────────────────────────────────────────────────────────────────────
// Await registration / firing / cancellation
// ──────────────────────────────────────────────────────────────────────────────

FGuid FNeoLuaState::RegisterAwait()
{
	check(IsInGameThread());
	const FGuid Token = FGuid::NewGuid();
	Impl->PendingAwaits.Add(Token);
	return Token;
}

void FNeoLuaState::FireAwait(const FGuid& Token, TFunction<int(lua_State*)>&& PushFn)
{
	// CRITICAL: always defer the resume via AsyncTask, even when the caller is
	// already on the game thread. Reason: a provider's continuation can fire
	// SYNCHRONOUSLY inside the binding's KickOff (e.g., default GetBalance/CancelJob
	// impls that immediately invoke their callback, or our own auth pre-flight that
	// fails fast). If we resumed inline, the chain would be:
	//
	//   AwaitAsync → KickOff → Continuation → FireAwait → ResumeSegment → lua_resume
	//
	// ...all before AwaitAsync's own lua_yield() has executed. lua_resume on a
	// non-suspended coroutine would crash or fail. Deferring forces the resume to
	// a fresh game-thread tick, after lua_yield has long-jumped out and the
	// coroutine is properly in the suspended state.
	TWeakPtr<FNeoLuaState> Weak = AsWeak();
	AsyncTask(ENamedThreads::GameThread, [Weak, Token, PushFn = MoveTemp(PushFn)]() mutable
	{
		TSharedPtr<FNeoLuaState> Pinned = Weak.Pin();
		if (!Pinned) return;

		if (Pinned->Impl->bFinished || Pinned->Impl->bCancelled)
		{
			return;
		}

		if (!Pinned->Impl->PendingAwaits.Contains(Token))
		{
			// Already cancelled or fired — silently drop.
			return;
		}
		Pinned->Impl->PendingAwaits.Remove(Token);

		// Push the values onto the coroutine's stack, then resume.
		const int NArgs = PushFn(Pinned->Impl->CoroL);
		Pinned->ResumeSegment(NArgs);
	});
}

void FNeoLuaState::CancelAwait(const FGuid& Token)
{
	check(IsInGameThread());
	Impl->PendingAwaits.Remove(Token);
}

// ──────────────────────────────────────────────────────────────────────────────
// Synchronous convenience for trusted internal scripts (no async bindings expected)
// ──────────────────────────────────────────────────────────────────────────────

FScriptResult FNeoLuaState::ExecuteSyncBlocking(const FString& Script)
{
	check(IsInGameThread());

	const TSharedRef<FNeoLuaState> Runner = MakeShared<FNeoLuaState>();

	FScriptResult Captured;
	bool bDone = false;

	Runner->Execute(Script, [&Captured, &bDone](FScriptResult R)
	{
		Captured = MoveTemp(R);
		bDone = true;
	});

	if (!bDone)
	{
		// Script yielded but the caller doesn't support async — cancel and return
		// an error. Internal scripts run by ExecuteSyncBlocking aren't supposed
		// to call any async bindings (generate_image, etc.); if one slipped in,
		// the caller needs to migrate to the async Execute API.
		UE_LOG(LogNeoLua, Error,
			TEXT("ExecuteSyncBlocking: script yielded; cancelling. Sync helper does not support async bindings."));
		Runner->Cancel();
		// Cancel runs Finish synchronously, which fires OnComplete and sets bDone=true.
		check(bDone);
	}

	return Captured;
}
