// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Tools/ExecuteNeoStackLuaTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Lua/LuaBindingRegistry.h"
#include "Lua/NeoLuaState.h"

FString FExecuteNeoStackLuaTool::GetDescription() const
{
	return FLuaBindingRegistry::Get().BuildDescription();
}

TSharedPtr<FJsonObject> FExecuteNeoStackLuaTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ScriptProp = MakeShared<FJsonObject>();
	ScriptProp->SetStringField(TEXT("type"), TEXT("string"));
	ScriptProp->SetStringField(TEXT("description"), TEXT("Lua script to execute"));
	Props->SetObjectField(TEXT("script"), ScriptProp);

	Schema->SetObjectField(TEXT("properties"), Props);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("script")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

void FExecuteNeoStackLuaTool::Execute(const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete)
{
	ExecuteWithHandle(FGuid(), Args, OnComplete);
}

void FExecuteNeoStackLuaTool::ExecuteWithHandle(const FGuid& HandleId, const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete)
{
	if (!Args.IsValid())
	{
		OnComplete(FToolResult::Fail(TEXT("Invalid arguments")));
		return;
	}

	FString Script;
	if (!Args->TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
	{
		OnComplete(FToolResult::Fail(TEXT("'script' parameter is required")));
		return;
	}

	// Spawn a fresh runner. Held in InProgress so it stays alive across coroutine
	// yields until completion (or until Cancel tears everything down).
	TSharedPtr<FNeoLuaState> Runner = MakeShared<FNeoLuaState>();
	InProgressByHandle.FindOrAdd(HandleId).Add(Runner);

	// Weak captures: if the tool itself is destroyed (module shutdown) or the
	// runner is force-cancelled, the lambda still runs but skips dangling access.
	TWeakPtr<FNeoStackToolBase> WeakSelf = AsWeak();
	TWeakPtr<FNeoLuaState> WeakRunner = Runner;

	Runner->Execute(Script,
		[WeakSelf, WeakRunner, OnComplete, this, HandleId](FScriptResult R)
		{
			// Match the old behavior: Output is the joined Trace, not the script's
			// return value. The agent reads tool output as the run log.
			FString Output;
			for (const FString& Line : R.Trace)
			{
				Output += Line + TEXT("\n");
			}

			FToolResult Tool = R.bSuccess ? FToolResult::Ok(Output) : FToolResult::Fail(Output);
			Tool.Images = MoveTemp(R.Images);

			// Drop the runner from InProgressByHandle — but only if `this` is still alive.
			if (auto SelfPin = WeakSelf.Pin())
			{
				if (auto RunnerPin = WeakRunner.Pin())
				{
					if (TArray<TSharedPtr<FNeoLuaState>>* Runners = this->InProgressByHandle.Find(HandleId))
					{
						Runners->Remove(RunnerPin);
						if (Runners->Num() == 0)
						{
							this->InProgressByHandle.Remove(HandleId);
						}
					}
				}
			}

			OnComplete(Tool);
		});
}

void FExecuteNeoStackLuaTool::Cancel()
{
	// Snapshot — each Cancel() drives the runner to fire its OnComplete, which
	// re-enters InProgressByHandle removal. Iterating a copy avoids invalidation.
	TArray<TSharedPtr<FNeoLuaState>> Snapshot;
	for (const auto& Pair : InProgressByHandle)
	{
		Snapshot.Append(Pair.Value);
	}
	for (TSharedPtr<FNeoLuaState>& Runner : Snapshot)
	{
		if (Runner.IsValid())
		{
			Runner->Cancel();
		}
	}
}

void FExecuteNeoStackLuaTool::CancelHandle(const FGuid& HandleId)
{
	if (!HandleId.IsValid())
	{
		Cancel();
		return;
	}

	TArray<TSharedPtr<FNeoLuaState>> Snapshot;
	if (const TArray<TSharedPtr<FNeoLuaState>>* Runners = InProgressByHandle.Find(HandleId))
	{
		Snapshot = *Runners;
	}

	for (TSharedPtr<FNeoLuaState>& Runner : Snapshot)
	{
		if (Runner.IsValid())
		{
			Runner->Cancel();
		}
	}
}
