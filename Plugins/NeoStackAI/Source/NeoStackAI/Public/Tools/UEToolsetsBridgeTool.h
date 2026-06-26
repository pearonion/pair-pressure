// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/NeoStackToolBase.h"

class NEOSTACKAI_API FUEToolsetsBridgeTool : public FNeoStackToolBase
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual void Execute(const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete) override;
	virtual bool IsSynchronous() const override { return false; }
};
