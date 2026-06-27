// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/ChatTypes.h"

namespace ChatTypes
{
	bool SplitPrefixedModelId(const FString& PrefixedId, FString& OutProviderId, FString& OutModelId)
	{
		int32 ColonIdx = INDEX_NONE;
		if (!PrefixedId.FindChar(TEXT(':'), ColonIdx))
		{
			return false;
		}

		OutProviderId = PrefixedId.Left(ColonIdx);
		OutModelId = PrefixedId.Mid(ColonIdx + 1);

		return !OutProviderId.IsEmpty() && !OutModelId.IsEmpty();
	}

	FString RoleToOpenAIString(EChatRole Role)
	{
		switch (Role)
		{
			case EChatRole::System:    return TEXT("system");
			case EChatRole::User:      return TEXT("user");
			case EChatRole::Assistant: return TEXT("assistant");
			case EChatRole::Tool:      return TEXT("tool");
		}
		return TEXT("user");
	}

	FString ReasoningEffortToString(EReasoningEffort Effort)
	{
		switch (Effort)
		{
			case EReasoningEffort::None:   return TEXT("none");
			case EReasoningEffort::Low:    return TEXT("low");
			case EReasoningEffort::Medium: return TEXT("medium");
			case EReasoningEffort::High:   return TEXT("high");
		}
		return TEXT("none");
	}

	EReasoningEffort ReasoningEffortFromString(const FString& Str)
	{
		if (Str.Equals(TEXT("low"),    ESearchCase::IgnoreCase)) return EReasoningEffort::Low;
		if (Str.Equals(TEXT("medium"), ESearchCase::IgnoreCase)) return EReasoningEffort::Medium;
		if (Str.Equals(TEXT("high"),   ESearchCase::IgnoreCase)) return EReasoningEffort::High;
		return EReasoningEffort::None;
	}
}
