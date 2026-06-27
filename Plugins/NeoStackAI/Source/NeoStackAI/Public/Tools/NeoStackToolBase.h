// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

/**
 * Image content block for tool results
 */
struct NEOSTACKAI_API FToolResultImage
{
	FString Base64Data;
	FString MimeType;
	int32 Width = 0;
	int32 Height = 0;
};

/**
 * Tool execution result - text output with optional image content blocks
 */
struct NEOSTACKAI_API FToolResult
{
	bool bSuccess = false;
	FString Output;
	TArray<FToolResultImage> Images;

	static FToolResult Ok(const FString& Message)
	{
		FToolResult R;
		R.bSuccess = true;
		R.Output = Message;
		return R;
	}

	static FToolResult Fail(const FString& Message)
	{
		FToolResult R;
		R.bSuccess = false;
		R.Output = Message;
		return R;
	}

	static FToolResult OkWithImage(const FString& Message, const FString& Base64Data, const FString& MimeType, int32 Width, int32 Height)
	{
		FToolResult R;
		R.bSuccess = true;
		R.Output = Message;
		FToolResultImage Img;
		Img.Base64Data = Base64Data;
		Img.MimeType = MimeType;
		Img.Width = Width;
		Img.Height = Height;
		R.Images.Add(Img);
		return R;
	}

	static FToolResult Cancelled()
	{
		return Fail(TEXT("Tool execution was cancelled."));
	}
};

/** Tool protocol version — bumped when the Execute/Schema contract changes.
 *  Agents can query this to detect incompatible tool hosts.
 *  Bumped to 8000 for the async-callback API change. */
constexpr int32 NEOSTACK_TOOL_PROTOCOL_VERSION = 8000;

/**
 * Base class for all NeoStack tools.
 *
 * Async-callback model (ported from UE 5.8 IModelContextProtocolTool::RunAsync):
 *   - Execute() returns immediately. The implementation MAY call OnComplete inline
 *     (sync tools — Lua, Python) or save it for later (async tools — Generate3D,
 *     GenerateImage, Screenshot).
 *   - OnComplete MUST fire exactly once on the game thread (success, error, or cancelled).
 *     Use UE::NeoStack::RunOnMainThread to marshal back from worker threads.
 *   - Cancel() is an opt-in hook. Implementations that override it MUST guarantee
 *     OnComplete still fires (typically with FToolResult::Cancelled()).
 */
class NEOSTACKAI_API FNeoStackToolBase : public TSharedFromThis<FNeoStackToolBase>
{
public:
	using FResultCallback = TFunction<void(const FToolResult&)>;

	virtual ~FNeoStackToolBase() = default;

	/** Tool name used for invocation (e.g., "execute_script", "generate_3d_model") */
	virtual FString GetName() const = 0;

	/** Human-readable description for AI context */
	virtual FString GetDescription() const = 0;

	/** Get JSON schema for tool parameters (for API function calling) */
	virtual TSharedPtr<FJsonObject> GetInputSchema() const = 0;

	/**
	 * Execute the tool with JSON arguments.
	 *
	 * Called on the game thread (the registry marshals before invoking).
	 * OnComplete MUST eventually be called with a result — exactly once.
	 *
	 * @param Args        Parsed JSON arguments (never null; empty object if none provided)
	 * @param OnComplete  Invoked with the tool result. May fire inline or later.
	 */
	virtual void Execute(const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete) = 0;

	/**
	 * Execute with the registry's opaque handle id. Tools that need per-call
	 * cancellation can override this; most tools should keep overriding Execute().
	 */
	virtual void ExecuteWithHandle(const FGuid& HandleId, const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete)
	{
		Execute(Args, OnComplete);
	}

	/**
	 * Optional cancellation hook.
	 *
	 * Default: no-op (sync tools complete inline so cancellation is a no-op).
	 * Async tools should override to abort outstanding HTTP / tickers / delegates,
	 * then fire OnComplete(FToolResult::Cancelled()).
	 *
	 * Cancel() is called on the game thread.
	 */
	virtual void Cancel() {}

	/** Cancel a specific registry call. Defaults to the legacy tool-wide cancel hook. */
	virtual void CancelHandle(const FGuid& HandleId)
	{
		Cancel();
	}

	/**
	 * Whether this tool completes synchronously inside Execute() (i.e., calls
	 * OnComplete inline before Execute returns).
	 *
	 * The registry uses this to decide whether to wrap the call in a transaction
	 * + blueprint-health validation + SEH crash protection. Async tools that defer
	 * OnComplete must NOT be wrapped in a transaction since the transaction would
	 * span unrelated editor activity.
	 *
	 * Default: true. Override to return false for async tools.
	 */
	virtual bool IsSynchronous() const { return true; }
};
