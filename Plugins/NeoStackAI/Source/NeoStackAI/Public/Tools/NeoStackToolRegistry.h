// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Extensions/NeoStackExtensionTypes.h"
#include "Tools/NeoStackToolBase.h"
#include "Misc/Guid.h"

/**
 * Opaque handle to an in-flight tool call. Use with FNeoStackToolRegistry::CancelExecute.
 */
struct NEOSTACKAI_API FNeoStackToolHandle
{
	FGuid Id;

	bool IsValid() const { return Id.IsValid(); }

	bool operator==(const FNeoStackToolHandle& Other) const { return Id == Other.Id; }
	friend uint32 GetTypeHash(const FNeoStackToolHandle& H) { return GetTypeHash(H.Id); }
};

struct NEOSTACKAI_API FNeoStackToolExecuteOptions
{
	FString OriginatingSessionId;
};

/**
 * Central registry for all NeoStack tools.
 *
 * Async-callback API (ported from UE 5.8 ToolsetRegistry / MCP plugins):
 *   - Execute(Name, Args, OnComplete) returns immediately with a handle.
 *   - OnComplete fires on the game thread when the tool finishes (or is cancelled).
 *   - CancelExecute(Handle) calls Tool->Cancel() and ensures OnComplete still fires.
 *
 * The registry marshals Execute calls onto the game thread before invoking the tool,
 * so callers may invoke from any thread (HTTP server, RPC handler, chat session).
 */
class NEOSTACKAI_API FNeoStackToolRegistry
{
public:
	using FResultCallback = FNeoStackToolBase::FResultCallback;

	/** Get singleton instance */
	static FNeoStackToolRegistry& Get();

	/** Register a tool (takes shared ownership) */
	void Register(TSharedPtr<FNeoStackToolBase> Tool);
	FNeoStackExtensionHandle RegisterOwned(const FString& OwnerExtensionId, TSharedPtr<FNeoStackToolBase> Tool);
	bool Unregister(const FGuid& RegistrationId);
	int32 UnregisterAllForOwner(const FString& OwnerExtensionId);

	/** Register the NeoStack Lua-backed tool */
	void RegisterNeoStackLuaTool();

	/**
	 * Execute a tool by name with JSON args string.
	 * Returns immediately with a handle. OnComplete fires on the game thread.
	 */
	FNeoStackToolHandle Execute(const FString& ToolName, const FString& ArgsJson, const FResultCallback& OnComplete);
	FNeoStackToolHandle Execute(const FString& ToolName, const FString& ArgsJson, const FResultCallback& OnComplete, const FNeoStackToolExecuteOptions& Options);

	/**
	 * Execute a tool by name with parsed JSON args.
	 * Returns immediately with a handle. OnComplete fires on the game thread.
	 */
	FNeoStackToolHandle Execute(const FString& ToolName, const TSharedPtr<class FJsonObject>& Args, const FResultCallback& OnComplete);
	FNeoStackToolHandle Execute(const FString& ToolName, const TSharedPtr<class FJsonObject>& Args, const FResultCallback& OnComplete, const FNeoStackToolExecuteOptions& Options);

	/**
	 * Cancel an in-flight tool call. Calls Tool->Cancel() on the underlying tool.
	 * No-op if the handle is invalid or the call already completed.
	 *
	 * The pending OnComplete is still guaranteed to fire (with FToolResult::Cancelled()
	 * if the tool itself doesn't override Cancel to fire its own result).
	 */
	void CancelExecute(const FNeoStackToolHandle& Handle);
	void CancelAllForSession(const FString& SessionId);
	bool IsHandleQueued(const FNeoStackToolHandle& Handle) const;
	bool IsHandleInFlight(const FNeoStackToolHandle& Handle) const;

	/** Check if a tool exists */
	bool HasTool(const FString& ToolName) const;

	/** Get tool by name (can be null) */
	FNeoStackToolBase* GetTool(const FString& ToolName) const;

	/** Get all registered tool names */
	TArray<FString> GetToolNames() const;

	/** Get tool count */
	int32 GetToolCount() const { return Tools.Num(); }

private:
	FNeoStackToolRegistry();
	~FNeoStackToolRegistry() = default;

	/** Register all built-in tools */
	void RegisterBuiltInTools();

	struct FRegisteredTool
	{
		TSharedPtr<FNeoStackToolBase> Tool;
		FString OwnerExtensionId;
		FGuid RegistrationId;
	};

	/** Map of tool name -> tool instance */
	TMap<FString, FRegisteredTool> Tools;

	/** State for an in-flight tool call (used for cancellation + finish-time bookkeeping) */
	struct FInFlightCall
	{
		FGuid HandleId;
		FString ToolName;
		FString Target;
		FString ToolKind;
		FString AssetType;
		FString Provider;
		FString Model;
		FString OriginatingSessionId;
		double StartTime = 0.0;
		TSharedPtr<FNeoStackToolBase> Tool;
		TSharedPtr<class FJsonObject> Args;
		FResultCallback UserCallback;
		bool bStarted = false;
		bool bCompleted = false;
	};

	/** Map of handle id -> in-flight state for ASYNC tools only.
	 *  Sync tools complete inline so they never enter this map.
	 *  Game-thread-only access. */
	TMap<FGuid, TSharedPtr<FInFlightCall>> InFlight;

	/** FIFO queue for tool calls waiting behind another active tool execution.
	 *  All tool bodies touch shared editor state, so concurrent agents may think
	 *  in parallel but their local tool calls are serialized here. */
	TArray<TSharedPtr<FInFlightCall>> WaitingCalls;
	bool bToolExecutionBusy = false;

	/** Internal: marshals to game thread, then either runs the sync wrapping path
	 *  or kicks off the async tool with a Wrapper that routes back to FinishCall. */
	void DispatchOnGameThread(TSharedPtr<FInFlightCall> Call, const TSharedPtr<class FJsonObject>& Args);

	/** Internal: writes audit log, records analytics, fires user callback, clears
	 *  crash context. Always called on the game thread. */
	void FinishCall(TSharedPtr<FInFlightCall> Call, const FToolResult& Result, bool bToolCrashed);
};

namespace UE::NeoStack
{
	/**
	 * Foundation-phase shim: synchronously execute a registered tool by blocking
	 * the calling thread until the result is available.
	 *
	 * Behavior:
	 *   - Game-thread caller: dispatch runs inline. The result is captured inline as
	 *     long as the tool fires OnComplete during Execute (sync tools always do; async
	 *     tools currently still block-then-fire-inline during foundation phase).
	 *   - Worker-thread caller: blocks on FEvent::Wait until the registry's
	 *     game-thread callback fires.
	 *
	 * Total wall-clock matches the old synchronous API. The editor still freezes
	 * during long tools — that's not fixed until step 3 of the refactor (the actual
	 * tool rewrites). This shim only exists so callers compile against the new async
	 * API without their bodies being rewritten yet.
	 *
	 * @deprecated Transitional. Each caller will be rewritten to use the proper
	 *   async callback API in step 5 of the refactor; this helper will be removed
	 *   once all callers migrate.
	 */
	NEOSTACKAI_API FToolResult ExecuteToolBlocking(const FString& ToolName, const TSharedPtr<class FJsonObject>& Args);
	NEOSTACKAI_API FToolResult ExecuteToolBlocking(const FString& ToolName, const FString& ArgsJson);

	/**
	 * Foundation-phase shim for direct tool-instance calls (e.g., Lua bindings that
	 * construct a local FScreenshotTool rather than going through the registry).
	 * Same blocking semantics as ExecuteToolBlocking but skips the registry's safety
	 * wrapping (no transaction, no blueprint health, no audit log, no analytics).
	 *
	 * Used by LuaBinding_Screenshot and the NSAI_Python extension's binding. The
	 * remaining callers don't need true async (their freezes are bounded — a
	 * few seconds for screenshot warmup, sync Python execution); coroutine-yielding
	 * via AwaitAsync is the eventual fix if their freezes ever become a problem.
	 *
	 * @deprecated Transitional. Generation paths now go through the async
	 *   `generate()` Lua binding + provider system.
	 */
	NEOSTACKAI_API FToolResult ExecuteToolDirectBlocking(FNeoStackToolBase& Tool, const TSharedPtr<class FJsonObject>& Args);
}
