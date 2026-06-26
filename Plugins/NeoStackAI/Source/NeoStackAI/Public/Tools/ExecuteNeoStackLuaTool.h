// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

class FNeoLuaState;

/**
 * The single registered NeoStackAI MCP tool. Runs Lua scripts that drive the
 * 80+ Lua bindings (open_asset, find_assets, generate_image, screenshot, ...).
 *
 * Async: scripts run as coroutines (FNeoLuaState). Bindings can yield via
 * UE::NeoStack::Lua::AwaitAsync to wait on async work (HTTP, asset import, etc.)
 * without freezing the editor. OnComplete fires when the script finishes.
 *
 * Cancellation: forwarded by registry handle when available, so cancelling one
 * chat session does not tear down another session's queued/running script.
 */
class NEOSTACKAI_API FExecuteNeoStackLuaTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override { return TEXT("execute_script"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;

	virtual void Execute(const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete) override;
	virtual void ExecuteWithHandle(const FGuid& HandleId, const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete) override;
	virtual void Cancel() override;
	virtual void CancelHandle(const FGuid& HandleId) override;
	virtual bool IsSynchronous() const override { return false; }

private:
	/** Currently-running scripts, grouped by registry handle for scoped cancellation. */
	TMap<FGuid, TArray<TSharedPtr<FNeoLuaState>>> InProgressByHandle;
};
