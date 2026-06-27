// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Tools/NeoStackToolRegistry.h"
#include "NeoStackAIModule.h"
#include "RunOnMainThread.h"
#include "EntitlementClient.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

// Analytics & Crash Reporting
#include "NSAIAnalytics.h"
#include "NSAICrashReporter.h"

// Safety net: transaction rollback, blueprint validation, crash protection
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

// Tool headers — only the Lua executor remains as a directly-registered tool
#include "Tools/ExecuteNeoStackLuaTool.h"
#ifndef WITH_UE_TOOLSET_REGISTRY
#define WITH_UE_TOOLSET_REGISTRY 0
#endif

#if WITH_UE_TOOLSET_REGISTRY
#include "Tools/UEToolsetsBridgeTool.h"
#endif

// ---------------------------------------------------------------------------
// Singleton + registration
// ---------------------------------------------------------------------------

FNeoStackToolRegistry& FNeoStackToolRegistry::Get()
{
	static FNeoStackToolRegistry Instance;
	return Instance;
}

FNeoStackToolRegistry::FNeoStackToolRegistry()
{
	RegisterBuiltInTools();
}

void FNeoStackToolRegistry::RegisterBuiltInTools()
{
	RegisterNeoStackLuaTool();
#if WITH_UE_TOOLSET_REGISTRY
	Register(MakeShared<FUEToolsetsBridgeTool>());
#endif
	// Core MCP tools stay compact. Most NeoStack capabilities are exposed as Lua
	// bindings; UE 5.8 ToolsetRegistry is bridged as a single discovery/call tool.

	UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Tool registry initialized with %d tools"), Tools.Num());
}

void FNeoStackToolRegistry::RegisterNeoStackLuaTool()
{
	Register(MakeShared<FExecuteNeoStackLuaTool>());
}

void FNeoStackToolRegistry::Register(TSharedPtr<FNeoStackToolBase> Tool)
{
	RegisterOwned(TEXT("neostack.core"), MoveTemp(Tool));
}

FNeoStackExtensionHandle FNeoStackToolRegistry::RegisterOwned(const FString& OwnerExtensionId, TSharedPtr<FNeoStackToolBase> Tool)
{
	if (!Tool.IsValid())
	{
		return {};
	}

	FString Name = Tool->GetName();
	FRegisteredTool Entry;
	Entry.Tool = MoveTemp(Tool);
	Entry.OwnerExtensionId = OwnerExtensionId.IsEmpty() ? TEXT("neostack.core") : OwnerExtensionId;
	Entry.RegistrationId = FGuid::NewGuid();

	if (Tools.Contains(Name))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("[AIK] Tool '%s' already registered, overwriting"), *Name);
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("[AIK] Registered tool: %s"), *Name);
	Tools.Add(Name, MoveTemp(Entry));

	FNeoStackExtensionHandle Handle;
	Handle.RegistrationId = Tools[Name].RegistrationId;
	Handle.Kind = TEXT("tool");
	Handle.OwnerExtensionId = Tools[Name].OwnerExtensionId;
	return Handle;
}

bool FNeoStackToolRegistry::Unregister(const FGuid& RegistrationId)
{
	for (auto It = Tools.CreateIterator(); It; ++It)
	{
		if (It.Value().RegistrationId == RegistrationId)
		{
			It.RemoveCurrent();
			return true;
		}
	}

	return false;
}

int32 FNeoStackToolRegistry::UnregisterAllForOwner(const FString& OwnerExtensionId)
{
	int32 Removed = 0;
	for (auto It = Tools.CreateIterator(); It; ++It)
	{
		if (It.Value().OwnerExtensionId.Equals(OwnerExtensionId, ESearchCase::CaseSensitive))
		{
			It.RemoveCurrent();
			++Removed;
		}
	}

	return Removed;
}

bool FNeoStackToolRegistry::HasTool(const FString& ToolName) const
{
	return Tools.Contains(ToolName);
}

FNeoStackToolBase* FNeoStackToolRegistry::GetTool(const FString& ToolName) const
{
	const FRegisteredTool* Found = Tools.Find(ToolName);
	return Found && Found->Tool.IsValid() ? Found->Tool.Get() : nullptr;
}

TArray<FString> FNeoStackToolRegistry::GetToolNames() const
{
	TArray<FString> Names;
	Tools.GetKeys(Names);
	return Names;
}

// ---------------------------------------------------------------------------
// Internal helpers — file-local
// ---------------------------------------------------------------------------

namespace
{
	// Extract the target asset identifier from common argument patterns for audit logging
	FString ExtractAssetTarget(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid()) return TEXT("");

		FString Target;
		if (Args->TryGetStringField(TEXT("name"),       Target)) return Target;
		if (Args->TryGetStringField(TEXT("asset"),      Target)) return Target;
		if (Args->TryGetStringField(TEXT("path"),       Target)) return Target;
		if (Args->TryGetStringField(TEXT("asset_path"), Target)) return Target;
		if (Args->TryGetStringField(TEXT("operation"),  Target)) return Target;
		return TEXT("");
	}

	FString ExtractStringFieldAny(const TSharedPtr<FJsonObject>& Args, const TArray<FString>& FieldNames)
	{
		if (!Args.IsValid()) return TEXT("");

		for (const FString& FieldName : FieldNames)
		{
			FString Value;
			if (Args->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}

		return TEXT("");
	}

	FString NormalizeAssetType(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ReplaceInline(TEXT(" "), TEXT("_"), ESearchCase::CaseSensitive);
		return Value.ToLower();
	}

	FString InferAssetTypeFromPath(const FString& Target)
	{
		const FString Extension = FPaths::GetExtension(Target).ToLower();
		if (Extension == TEXT("uasset")) return TEXT("unreal_asset");
		if (Extension == TEXT("umap")) return TEXT("map");
		if (Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("exr")) return TEXT("texture");
		if (Extension == TEXT("fbx") || Extension == TEXT("obj") || Extension == TEXT("glb") || Extension == TEXT("gltf")) return TEXT("model");
		if (Extension == TEXT("wav") || Extension == TEXT("mp3") || Extension == TEXT("ogg")) return TEXT("audio");
		return TEXT("");
	}

	FString InferAssetType(const TSharedPtr<FJsonObject>& Args, const FString& Target)
	{
		const FString ExplicitType = ExtractStringFieldAny(Args, {
			TEXT("asset_type"),
			TEXT("assetType"),
			TEXT("asset_class"),
			TEXT("assetClass"),
			TEXT("class"),
			TEXT("type")
		});
		if (!ExplicitType.IsEmpty())
		{
			return NormalizeAssetType(ExplicitType);
		}

		return InferAssetTypeFromPath(Target);
	}

	FString InferToolKind(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
	{
		const FString ExplicitKind = ExtractStringFieldAny(Args, {
			TEXT("tool_kind"),
			TEXT("toolKind"),
			TEXT("kind")
		});
		if (!ExplicitKind.IsEmpty())
		{
			FString Normalized = ExplicitKind;
			Normalized.TrimStartAndEndInline();
			Normalized.ReplaceInline(TEXT(" "), TEXT("_"), ESearchCase::CaseSensitive);
			return Normalized.ToLower();
		}

		if (ToolName.Contains(TEXT("script"), ESearchCase::IgnoreCase)) return TEXT("script");
		if (ToolName.Contains(TEXT("python"), ESearchCase::IgnoreCase)) return TEXT("python");
		if (ToolName.Contains(TEXT("lua"), ESearchCase::IgnoreCase)) return TEXT("lua");
		if (ToolName.Contains(TEXT("image"), ESearchCase::IgnoreCase)) return TEXT("image");
		if (ToolName.Contains(TEXT("3d"), ESearchCase::IgnoreCase) || ToolName.Contains(TEXT("model"), ESearchCase::IgnoreCase)) return TEXT("model");
		if (ToolName.Contains(TEXT("audio"), ESearchCase::IgnoreCase) || ToolName.Contains(TEXT("sound"), ESearchCase::IgnoreCase)) return TEXT("audio");
		if (ToolName.Contains(TEXT("screenshot"), ESearchCase::IgnoreCase)) return TEXT("screenshot");
		return TEXT("editor");
	}

	// Write a line to the tool audit log (Saved/Logs/NSAI_ToolAudit.log)
	void WriteAuditLog(const FString& ToolName, const FString& Target, bool bSuccess, const FString& Summary)
	{
		static FString AuditLogPath = FPaths::ProjectSavedDir() / TEXT("Logs") / TEXT("NSAI_ToolAudit.log");

		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
		FString Status = bSuccess ? TEXT("OK") : TEXT("FAIL");

		// Truncate summary to keep log readable (173 chars fits a single log viewer line at 1920px)
		FString ShortSummary = Summary.Left(173);
		ShortSummary.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);
		ShortSummary.ReplaceInline(TEXT("\r"), TEXT(""),  ESearchCase::CaseSensitive);

		FString Line = FString::Printf(TEXT("[%s] %s | %s | %s | %s\n"),
			*Timestamp, *Status, *ToolName, *Target, *ShortSummary);

		FFileHelper::SaveStringToFile(Line, *AuditLogPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
	}

	// Validate that a blueprint hasn't been corrupted by a tool that just modified it.
	// Skip the GeneratedClass check for blueprints that already had null GeneratedClass
	// before execution (e.g. Mover plugin movement modes — that's normal for them).
	bool ValidateBlueprintHealth(UBlueprint* BP, FString& OutErrors, bool bCheckGeneratedClass = true)
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

	// SEH crash protection (Windows only). Wraps the synchronous call to
	// Tool->Execute. MSVC C2712 forces the __try block to live in a function with
	// no objects requiring unwinding, so we split into two functions:
	//   ExecuteToolKickoff — calls Execute (C++ objects with destructors OK)
	//   TryKickoffWithSEH  — only POD types in scope; wraps in __try/__except
#if PLATFORM_WINDOWS
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

	struct FSEHKickoffContext
	{
		FNeoStackToolBase* Tool;
		const TSharedPtr<FJsonObject>* ArgsPtr;
		const FNeoStackToolBase::FResultCallback* OnCompletePtr;
	};

	void ExecuteToolKickoff(FSEHKickoffContext* Ctx)
	{
		Ctx->Tool->Execute(*Ctx->ArgsPtr, *Ctx->OnCompletePtr);
	}

	bool TryKickoffWithSEH(FSEHKickoffContext* Ctx)
	{
		__try
		{
			ExecuteToolKickoff(Ctx);
			return true;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
#endif

	// Run a synchronous tool with full safety wrapping: transaction, blueprint
	// health validation, SEH crash protection. The tool MUST fire OnComplete
	// inline before returning from Execute (sync contract — IsSynchronous()==true).
	//
	// Returns the tool's result via OutResult and whether it crashed via OutCrashed.
	// Game thread only.
	void ExecuteSyncWithSafety(
		FNeoStackToolBase* Tool,
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Args,
		FToolResult& OutResult,
		bool& OutCrashed)
	{
		check(IsInGameThread());
		OutCrashed = false;

		// 0. Snapshot blueprints with pre-existing null GeneratedClass — we must not
		//    flag these as "corrupted" after execution.
		TSet<UBlueprint*> PreExistingNullGeneratedClass;
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (IsValid(*It) && !(*It)->GeneratedClass)
			{
				PreExistingNullGeneratedClass.Add(*It);
			}
		}

		// 1. Track packages modified during this tool execution.
		TSet<UPackage*> ModifiedPackages;
		FDelegateHandle DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
			[&ModifiedPackages](UPackage* Pkg, bool /*bWasDirty*/)
			{
				if (Pkg) ModifiedPackages.Add(Pkg);
			});

		// 2. Begin transaction for rollback capability.
		int32 TransIndex = INDEX_NONE;
		if (GEditor)
		{
			TransIndex = GEditor->BeginTransaction(
				FText::FromString(FString::Printf(TEXT("AIK: %s"), *ToolName)));
		}

		// 3. Execute with crash protection. The capture lambda fires inline (sync contract).
		bool bResultCaptured = false;
		FToolResult CapturedResult;
		FNeoStackToolBase::FResultCallback Capture =
			[&CapturedResult, &bResultCaptured](const FToolResult& R)
			{
				CapturedResult = R;
				bResultCaptured = true;
			};

#if PLATFORM_WINDOWS
		FSEHKickoffContext Ctx = { Tool, &Args, &Capture };
		const bool bKickoffOk = TryKickoffWithSEH(&Ctx);
		if (!bKickoffOk)
		{
			OutCrashed = true;
			// In-place reconstruct: a partial FString assignment may have been
			// mid-flight when the crash hit; calling its destructor would crash again.
			new (&OutResult) FToolResult();
			OutResult.bSuccess = false;
			OutResult.Output = FString::Printf(
				TEXT("Tool '%s' crashed (access violation caught). Changes rolled back. "
				     "This usually indicates blueprint compilation during PIE or stale engine state."),
				*ToolName);
		}
		else if (bResultCaptured)
		{
			OutResult = MoveTemp(CapturedResult);
		}
		else
		{
			OutResult = FToolResult::Fail(FString::Printf(
				TEXT("Sync tool '%s' did not call OnComplete before returning. "
				     "Either fire OnComplete inline or override IsSynchronous() to return false."),
				*ToolName));
		}
#else
		Tool->Execute(Args, Capture);
		if (bResultCaptured)
		{
			OutResult = MoveTemp(CapturedResult);
		}
		else
		{
			OutResult = FToolResult::Fail(FString::Printf(
				TEXT("Sync tool '%s' did not call OnComplete before returning."),
				*ToolName));
		}
#endif

		// 4. Unsubscribe from package dirty events.
		UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);

		// 5. Validate any modified blueprints for corruption.
		bool bNeedsRollback = OutCrashed;
		if (!bNeedsRollback && OutResult.bSuccess && ModifiedPackages.Num() > 0)
		{
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				UBlueprint* BP = *It;
				if (BP && IsValid(BP) && ModifiedPackages.Contains(BP->GetPackage()))
				{
					const bool bHadGeneratedClass = PreExistingNullGeneratedClass.Find(BP) == nullptr;
					FString ValidationErrors;
					if (!ValidateBlueprintHealth(BP, ValidationErrors, bHadGeneratedClass))
					{
						bNeedsRollback = true;
						OutResult = FToolResult::Fail(FString::Printf(
							TEXT("Blueprint corruption detected after '%s': %s. Changes rolled back."),
							*ToolName, *ValidationErrors));
						break;
					}
				}
			}
		}

		// 6. Rollback or commit.
		if (GEditor && TransIndex != INDEX_NONE)
		{
			if (bNeedsRollback)
			{
				GEditor->CancelTransaction(TransIndex);
				UE_LOG(LogNeoStackAI, Error,
					TEXT("[AIK] Transaction rolled back for tool '%s'"), *ToolName);
			}
			else
			{
				GEditor->EndTransaction();
			}
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// Async dispatch + completion
// ---------------------------------------------------------------------------

FNeoStackToolHandle FNeoStackToolRegistry::Execute(const FString& ToolName, const FString& ArgsJson, const FResultCallback& OnComplete)
{
	return Execute(ToolName, ArgsJson, OnComplete, FNeoStackToolExecuteOptions{});
}

FNeoStackToolHandle FNeoStackToolRegistry::Execute(const FString& ToolName, const FString& ArgsJson, const FResultCallback& OnComplete, const FNeoStackToolExecuteOptions& Options)
{
	TSharedPtr<FJsonObject> Args;

	if (!ArgsJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(Reader, Args) || !Args.IsValid())
		{
			// Invalid JSON — fail immediately on the game thread.
			FNeoStackToolHandle Handle{ FGuid::NewGuid() };
			const FString Err = FString::Printf(TEXT("Failed to parse arguments for tool '%s'"), *ToolName);
			UE::NeoStack::RunOnMainThreadDeferred([OnComplete, Err]()
			{
				OnComplete(FToolResult::Fail(Err));
			});
			return Handle;
		}
	}
	else
	{
		Args = MakeShared<FJsonObject>();
	}

	return Execute(ToolName, Args, OnComplete, Options);
}

FNeoStackToolHandle FNeoStackToolRegistry::Execute(const FString& ToolName, const TSharedPtr<FJsonObject>& InArgs, const FResultCallback& OnComplete)
{
	return Execute(ToolName, InArgs, OnComplete, FNeoStackToolExecuteOptions{});
}

FNeoStackToolHandle FNeoStackToolRegistry::Execute(const FString& ToolName, const TSharedPtr<FJsonObject>& InArgs, const FResultCallback& OnComplete, const FNeoStackToolExecuteOptions& Options)
{
	const TSharedPtr<FJsonObject> Args = InArgs.IsValid() ? InArgs : MakeShared<FJsonObject>();
	const FNeoStackToolHandle Handle{ FGuid::NewGuid() };

	// Subscription gate. Strict for binary builds — must reach neostack.dev
	// every editor launch (user-confirmed). Distinguish "still verifying"
	// (request in flight) from "verified and denied" so the user knows
	// whether to wait or to take action.
	if (!FEntitlementClient::Get().IsEntitled())
	{
		const FString Err = FEntitlementClient::Get().IsCheckPending()
			? TEXT("Verifying NeoStack subscription… try again in a moment.")
			: TEXT("NeoStack subscription not active. Renew at https://neostack.dev/account "
				"or set a valid API key in Settings > Chat & Agents > Chat Providers > NeoStack Cloud.");
		UE::NeoStack::RunOnMainThreadDeferred([OnComplete, Err]()
		{
			OnComplete(FToolResult::Fail(Err));
		});
		return Handle;
	}

	// Look up tool + grab a shared owner so it stays alive across async boundaries.
	TSharedPtr<FNeoStackToolBase> ToolOwner;
	if (const FRegisteredTool* RT = Tools.Find(ToolName))
	{
		ToolOwner = RT->Tool;
	}

	if (!ToolOwner.IsValid())
	{
		const FString Err = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
		UE_LOG(LogNeoStackAI, Warning, TEXT("[AIK] %s"), *Err);
		WriteAuditLog(ToolName, ExtractAssetTarget(Args), false, Err);
		UE::NeoStack::RunOnMainThreadDeferred([OnComplete, Err]()
		{
			OnComplete(FToolResult::Fail(Err));
		});
		return Handle;
	}

	// Build in-flight state. Used for both sync and async paths so FinishCall's
	// audit/analytics logic is uniform.
	//
	// HandleId MUST be set before DispatchOnGameThread because if the caller is
	// already on the game thread, RunOnMainThreadDeferred runs the dispatch
	// inline — the dispatch lambda registers InFlight[Call->HandleId], which
	// would be a default-constructed Guid if we assigned it afterwards.
	TSharedPtr<FInFlightCall> Call = MakeShared<FInFlightCall>();
	Call->HandleId     = Handle.Id;
	Call->ToolName     = ToolName;
	Call->Target       = ExtractAssetTarget(Args);
	Call->ToolKind     = InferToolKind(ToolName, Args);
	Call->AssetType    = InferAssetType(Args, Call->Target);
	Call->Provider     = ExtractStringFieldAny(Args, { TEXT("provider"), TEXT("provider_id"), TEXT("providerId") });
	Call->Model        = ExtractStringFieldAny(Args, { TEXT("model"), TEXT("model_id"), TEXT("modelId") });
	Call->OriginatingSessionId = Options.OriginatingSessionId;
	Call->Tool         = ToolOwner;
	Call->Args         = Args;
	Call->UserCallback = OnComplete;
	Call->StartTime    = FPlatformTime::Seconds();

	UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] Tool: %s | Target: %s"), *ToolName, *Call->Target);

	// Note on race: if a caller invokes Execute from a non-game thread and immediately
	// calls CancelExecute(Handle), the cancel may race ahead of dispatch. Both are queued
	// onto TaskGraphMainThread from the same source thread, so they execute in FIFO order
	// — dispatch first, then cancel. From different source threads ordering is undefined,
	// but cancellation against an unstarted call is currently a no-op (won't find InFlight
	// entry) and the tool will run to completion. Acceptable for foundation phase.
	DispatchOnGameThread(Call, Args);
	return Handle;
}

void FNeoStackToolRegistry::DispatchOnGameThread(TSharedPtr<FInFlightCall> Call, const TSharedPtr<FJsonObject>& Args)
{
	UE::NeoStack::RunOnMainThreadDeferred([this, Call, Args]()
	{
		check(IsInGameThread());
		Call->Args = Args.IsValid() ? Args : MakeShared<FJsonObject>();

		if (bToolExecutionBusy)
		{
			WaitingCalls.Add(Call);
			UE_LOG(LogNeoStackAI, Log,
				TEXT("[AIK] Queued tool behind active execution: %s | Target: %s"),
				*Call->ToolName, *Call->Target);
			return;
		}

		bToolExecutionBusy = true;
		Call->bStarted = true;

		const FString ToolName = Call->ToolName;
		const FString Target   = Call->Target;

		// Crash context — best-effort. If multiple tools are concurrently in flight,
		// the most recent dispatch's context is what crash dumps see. Cleared in
		// FinishCall when InFlight goes empty.
		FNSAICrashReporter::SetCrashContext(TEXT("CurrentTool"), ToolName);
		FNSAICrashReporter::SetCrashContext(TEXT("Status"),      TEXT("ExecutingTool"));
		if (!Target.IsEmpty())
		{
			FNSAICrashReporter::SetCrashContext(TEXT("Target"),  Target);
		}

		FNeoStackToolBase* Tool = Call->Tool.Get();
		check(Tool);

		if (Tool->IsSynchronous())
		{
			// ── Synchronous tool path ─────────────────────────────────────────
			// Wrap in transaction + blueprint health + SEH protection. Tool must
			// fire OnComplete inline before Execute returns (enforced by ExecuteSyncWithSafety).
			FToolResult Result;
			bool bToolCrashed = false;
			ExecuteSyncWithSafety(Tool, ToolName, Call->Args, Result, bToolCrashed);

			Call->bCompleted = true;
			FinishCall(Call, Result, bToolCrashed);
		}
		else
		{
			// ── Asynchronous tool path ────────────────────────────────────────
			// Register for cancellation, then kick off. Tool fires Wrapper later
			// (potentially from a worker thread; Wrapper marshals back to GT).
			InFlight.Add(Call->HandleId, Call);

			TWeakPtr<FInFlightCall> WeakCall = Call;
			FNeoStackToolBase::FResultCallback Wrapper =
				[this, WeakCall](const FToolResult& Result)
				{
					UE::NeoStack::RunOnMainThreadDeferred([this, WeakCall, Result]()
					{
						TSharedPtr<FInFlightCall> Pinned = WeakCall.Pin();
						if (!Pinned || Pinned->bCompleted)
						{
							return;
						}
						Pinned->bCompleted = true;
						InFlight.Remove(Pinned->HandleId);
						FinishCall(Pinned, Result, /*bToolCrashed*/false);
					});
				};

			// SEH-wrap the kickoff. During foundation phase async tools still block
			// inside Execute(), so the kickoff = the whole tool body — SEH coverage
			// is meaningful. Once tools become truly async (step 3 of the refactor),
			// kickoff is just creating an HTTP request / ticker, and continuations
			// run outside SEH; that's an accepted tradeoff (matches engine behavior).
			bool bKickoffCrashed = false;
#if PLATFORM_WINDOWS
			FSEHKickoffContext Ctx = { Tool, &Call->Args, &Wrapper };
			bKickoffCrashed = !TryKickoffWithSEH(&Ctx);
#else
			Tool->ExecuteWithHandle(Call->HandleId, Call->Args, Wrapper);
#endif

			if (bKickoffCrashed && !Call->bCompleted)
			{
				// Force-fire so the user's callback runs exactly once. Wrapper went
				// out of scope without being invoked; we synthesize the result.
				Call->bCompleted = true;
				InFlight.Remove(Call->HandleId);
				FToolResult CrashResult;
				CrashResult.bSuccess = false;
				CrashResult.Output = FString::Printf(
					TEXT("Tool '%s' crashed during async kickoff (access violation caught)."),
					*ToolName);
				FinishCall(Call, CrashResult, /*bToolCrashed*/true);
			}
		}
	});
}

void FNeoStackToolRegistry::FinishCall(TSharedPtr<FInFlightCall> Call, const FToolResult& Result, bool bToolCrashed)
{
	check(IsInGameThread());
	check(Call.IsValid());

	// Audit log
	WriteAuditLog(Call->ToolName, Call->Target, Result.bSuccess,
		bToolCrashed ? FString::Printf(TEXT("CRASH in %s"), *Call->ToolName) : Result.Output);

	// Analytics — count [FAIL] occurrences in output for granular failure tracking.
	// Even successful Lua scripts may report individual binding failures via [FAIL] lines.
	TArray<FString> FailMessages;
	int32 FailCount = 0;
	{
		int32 SearchFrom = 0;
		while (true)
		{
			const int32 Pos = Result.Output.Find(TEXT("[FAIL]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Pos == INDEX_NONE) break;
			FailCount++;
			const int32 LineEnd = Result.Output.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Pos);
			FString FailLine = LineEnd != INDEX_NONE
				? Result.Output.Mid(Pos, LineEnd - Pos)
				: Result.Output.Mid(Pos);
			FailMessages.Add(FailLine);
			SearchFrom = Pos + 6;
		}
	}

	const FString ErrorForAnalytics = !Result.bSuccess
		? Result.Output
		: (FailMessages.Num() > 0 ? FString::Join(FailMessages, TEXT("\n")) : TEXT(""));

	{
		const double DurationMs = (FPlatformTime::Seconds() - Call->StartTime) * 1000.0;
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("tool"),        Call->ToolName);
		Props->SetStringField(TEXT("tool_kind"),   Call->ToolKind);
		Props->SetBoolField  (TEXT("success"),     Result.bSuccess);
		Props->SetNumberField(TEXT("duration_ms"), DurationMs);
		if (!Call->AssetType.IsEmpty())
		{
			Props->SetStringField(TEXT("asset_type"), Call->AssetType);
		}
		if (!Call->Provider.IsEmpty())
		{
			Props->SetStringField(TEXT("provider"), Call->Provider);
		}
		if (!Call->Model.IsEmpty())
		{
			Props->SetStringField(TEXT("model"), Call->Model);
		}
		if (FailCount > 0)
		{
			Props->SetNumberField(TEXT("fail_count"), FailCount);
		}
		if (!ErrorForAnalytics.IsEmpty())
		{
			Props->SetStringField(TEXT("error"), FNSAIAnalytics::SanitizeErrorForAnalytics(ErrorForAnalytics));
		}
		FNSAIAnalytics::Get().RecordEvent(TEXT("tool_executed"), Props);

		if (!Call->AssetType.IsEmpty())
		{
			TSharedPtr<FJsonObject> AssetProps = MakeShared<FJsonObject>();
			AssetProps->SetStringField(TEXT("operation"), Call->ToolName);
			AssetProps->SetStringField(TEXT("tool"), Call->ToolName);
			AssetProps->SetStringField(TEXT("tool_kind"), Call->ToolKind);
			AssetProps->SetStringField(TEXT("asset_type"), Call->AssetType);
			if (!Call->Provider.IsEmpty())
			{
				AssetProps->SetStringField(TEXT("provider"), Call->Provider);
			}
			if (!Call->Model.IsEmpty())
			{
				AssetProps->SetStringField(TEXT("model"), Call->Model);
			}
			AssetProps->SetBoolField(TEXT("success"), Result.bSuccess);
			AssetProps->SetNumberField(TEXT("duration_ms"), DurationMs);
			if (FailCount > 0)
			{
				AssetProps->SetNumberField(TEXT("fail_count"), FailCount);
			}
			FNSAIAnalytics::Get().RecordEvent(TEXT("asset_operation"), AssetProps);
		}
	}

	if (!Result.bSuccess)
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("[AIK] Tool '%s' failed: %s"), *Call->ToolName, *Result.Output);
	}

	// Fire the user's callback. Move the TFunction so we don't accidentally invoke it
	// twice if FinishCall is somehow re-entered.
	FResultCallback UserCb = MoveTemp(Call->UserCallback);
	if (UserCb)
	{
		UserCb(Result);
	}

	// Clear crash context only if no other in-flight tools remain (best-effort —
	// concurrent tools share the global context).
	if (InFlight.IsEmpty())
	{
		FNSAICrashReporter::ClearCrashContext(TEXT("CurrentTool"));
		FNSAICrashReporter::ClearCrashContext(TEXT("Status"));
		FNSAICrashReporter::ClearCrashContext(TEXT("Target"));
	}

	if (Call->bStarted)
	{
		bToolExecutionBusy = false;
		if (WaitingCalls.Num() > 0)
		{
			TSharedPtr<FInFlightCall> NextCall = WaitingCalls[0];
			WaitingCalls.RemoveAt(0);
			if (NextCall.IsValid() && !NextCall->bCompleted)
			{
				DispatchOnGameThread(NextCall, NextCall->Args);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Foundation-phase shims (UE::NeoStack::ExecuteToolBlocking / ExecuteToolDirectBlocking)
//
// These are transitional. They give existing synchronous callers a drop-in path
// onto the new async-callback API without forcing each caller to be rewritten.
// Removed once consumers (MCP server, ChatSession, LocalClient, ProjectIndexManager,
// Lua bindings) migrate in steps 4–5 of the refactor.
// ---------------------------------------------------------------------------

namespace UE::NeoStack
{
	// Heap-allocated capture state — keeps the lambda's references alive even if
	// this function returns before the tool's callback fires (which happens when
	// a game-thread caller invokes a tool that genuinely defers OnComplete, e.g.
	// the Lua tool with a script that yields via AwaitAsync). The lambda holds
	// the state via TSharedRef; when the chain eventually completes (or is torn
	// down via Cancel), the lambda's TFunction storage is destroyed and the
	// last reference drops cleanly.
	struct FBlockingCaptureState
	{
		FToolResult Result;
		bool bCaptured = false;
		FEvent* DoneEvent = nullptr;
	};

	FToolResult ExecuteToolBlocking(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
	{
		TSharedRef<FBlockingCaptureState> State = MakeShared<FBlockingCaptureState>();

		const bool bWorkerCaller = !IsInGameThread();
		if (bWorkerCaller)
		{
			State->DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		}

		FNeoStackToolBase::FResultCallback Capture =
			[State](const FToolResult& R)
			{
				State->Result = R;
				State->bCaptured = true;
				if (State->DoneEvent)
				{
					State->DoneEvent->Trigger();
				}
			};

		FNeoStackToolRegistry::Get().Execute(ToolName, Args, Capture);

		if (bWorkerCaller)
		{
			State->DoneEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(State->DoneEvent);
			State->DoneEvent = nullptr;
		}

		if (!State->bCaptured)
		{
			// Game-thread caller, tool genuinely deferred OnComplete (e.g. the
			// Lua tool with a yielding script). The lambda is still in flight in
			// the tool's callback chain — it'll fire later and write to the
			// heap-allocated State, which nobody reads. Safe (no UB) but the
			// caller doesn't get the actual result.
			//
			// Callers in this situation MUST migrate to the async API:
			//   FNeoStackToolRegistry::Get().Execute(Name, Args, OnComplete)
			// Where OnComplete handles the result on the game thread.
			return FToolResult::Fail(FString::Printf(
				TEXT("Tool '%s' did not complete inline on the game thread. "
				     "Migrate this caller to FNeoStackToolRegistry::Execute(name, args, OnComplete)."),
				*ToolName));
		}
		return State->Result;
	}

	FToolResult ExecuteToolBlocking(const FString& ToolName, const FString& ArgsJson)
	{
		TSharedPtr<FJsonObject> Args;
		if (!ArgsJson.IsEmpty())
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
			if (!FJsonSerializer::Deserialize(Reader, Args) || !Args.IsValid())
			{
				return FToolResult::Fail(FString::Printf(
					TEXT("Failed to parse arguments for tool '%s'"), *ToolName));
			}
		}
		else
		{
			Args = MakeShared<FJsonObject>();
		}
		return ExecuteToolBlocking(ToolName, Args);
	}

	FToolResult ExecuteToolDirectBlocking(FNeoStackToolBase& Tool, const TSharedPtr<FJsonObject>& Args)
	{
		TSharedRef<FBlockingCaptureState> State = MakeShared<FBlockingCaptureState>();

		const bool bWorkerCaller = !IsInGameThread();
		if (bWorkerCaller)
		{
			State->DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		}

		FNeoStackToolBase::FResultCallback Capture =
			[State](const FToolResult& R)
			{
				State->Result = R;
				State->bCaptured = true;
				if (State->DoneEvent)
				{
					State->DoneEvent->Trigger();
				}
			};

		Tool.Execute(Args.IsValid() ? Args : MakeShared<FJsonObject>(), Capture);

		if (bWorkerCaller)
		{
			State->DoneEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(State->DoneEvent);
			State->DoneEvent = nullptr;
		}

		if (!State->bCaptured)
		{
			return FToolResult::Fail(FString::Printf(
				TEXT("Tool '%s' did not complete inline on the game thread. "
				     "Use Tool.Execute(Args, OnComplete) directly with an async callback."),
				*Tool.GetName()));
		}
		return State->Result;
	}
}

void FNeoStackToolRegistry::CancelExecute(const FNeoStackToolHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	const FGuid Id = Handle.Id;
	UE::NeoStack::RunOnMainThreadDeferred([this, Id]()
	{
		check(IsInGameThread());

		for (int32 Index = 0; Index < WaitingCalls.Num(); ++Index)
		{
			TSharedPtr<FInFlightCall> QueuedCall = WaitingCalls[Index];
			if (QueuedCall.IsValid() && QueuedCall->HandleId == Id)
			{
				WaitingCalls.RemoveAt(Index);
				if (!QueuedCall->bCompleted)
				{
					QueuedCall->bCompleted = true;
					FinishCall(QueuedCall, FToolResult::Cancelled(), /*bToolCrashed*/false);
				}
				return;
			}
		}

		TSharedPtr<FInFlightCall> Call;
		if (TSharedPtr<FInFlightCall>* Found = InFlight.Find(Id))
		{
			Call = *Found;
		}
		if (!Call.IsValid() || Call->bCompleted)
		{
			return;
		}

		UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] Cancelling tool: %s"), *Call->ToolName);

		// Ask the tool to tear down. A well-behaved tool fires OnComplete in response,
		// which routes through the Wrapper and lands in FinishCall. If the tool's Cancel
		// is the default no-op, we synthesize a cancelled result below.
		if (Call->Tool.IsValid())
		{
			Call->Tool->CancelHandle(Id);
		}

		// Force-complete if the tool didn't honor Cancel().
		if (!Call->bCompleted)
		{
			Call->bCompleted = true;
			InFlight.Remove(Id);
			FinishCall(Call, FToolResult::Cancelled(), /*bToolCrashed*/false);
		}
	});
}

void FNeoStackToolRegistry::CancelAllForSession(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		return;
	}

	UE::NeoStack::RunOnMainThreadDeferred([this, SessionId]()
	{
		check(IsInGameThread());

		TArray<FNeoStackToolHandle> Handles;
		for (const TSharedPtr<FInFlightCall>& QueuedCall : WaitingCalls)
		{
			if (QueuedCall.IsValid()
				&& !QueuedCall->bCompleted
				&& QueuedCall->OriginatingSessionId == SessionId)
			{
				Handles.Add(FNeoStackToolHandle{ QueuedCall->HandleId });
			}
		}
		for (const auto& Pair : InFlight)
		{
			const TSharedPtr<FInFlightCall>& Call = Pair.Value;
			if (Call.IsValid()
				&& !Call->bCompleted
				&& Call->OriginatingSessionId == SessionId)
			{
				Handles.Add(FNeoStackToolHandle{ Call->HandleId });
			}
		}

		for (const FNeoStackToolHandle& Handle : Handles)
		{
			CancelExecute(Handle);
		}
	});
}

bool FNeoStackToolRegistry::IsHandleQueued(const FNeoStackToolHandle& Handle) const
{
	if (!Handle.IsValid())
	{
		return false;
	}
	for (const TSharedPtr<FInFlightCall>& QueuedCall : WaitingCalls)
	{
		if (QueuedCall.IsValid() && !QueuedCall->bCompleted && QueuedCall->HandleId == Handle.Id)
		{
			return true;
		}
	}
	return false;
}

bool FNeoStackToolRegistry::IsHandleInFlight(const FNeoStackToolHandle& Handle) const
{
	if (!Handle.IsValid())
	{
		return false;
	}
	const TSharedPtr<FInFlightCall>* Found = InFlight.Find(Handle.Id);
	return Found && Found->IsValid() && !(*Found)->bCompleted;
}
