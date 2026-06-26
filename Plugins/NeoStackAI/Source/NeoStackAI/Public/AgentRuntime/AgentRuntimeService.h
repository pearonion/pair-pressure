// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class ENeoStackAgentRuntimeKind : uint8
{
	Unknown,
	ACP,
	ChatGateway
};

struct NEOSTACKAI_API FNeoStackAgentRuntimeCreateOptions
{
	FString SessionId;
	FString AgentName;
	FString WorkingDirectory;
	bool bForceFreshProcess = false;
};

struct NEOSTACKAI_API FNeoStackAgentRuntimeResumeOptions
{
	FString SessionId;
	FString AgentName;
	FString WorkingDirectory;
	bool bLaunchResume = false;
};

struct NEOSTACKAI_API FNeoStackAgentRuntimeResumeResult
{
	bool bStarted = false;
	bool bLoading = false;
	FString Error;
};

/**
 * Provider-shaped runtime facade for live agent interactions.
 *
 * The WebUI and future local chat store should deal in NeoStack session IDs.
 * This service owns the transport split underneath those sessions: ACP
 * subprocess agents, Chat Gateway providers, and future native runtimes.
 */
class NEOSTACKAI_API FNeoStackAgentRuntimeService
{
public:
	static FNeoStackAgentRuntimeService& Get();

	ENeoStackAgentRuntimeKind GetRuntimeKindForAgent(const FString& AgentName) const;
	bool HasLiveSession(const FString& SessionId) const;

	bool CreateSession(const FNeoStackAgentRuntimeCreateOptions& Options);
	FNeoStackAgentRuntimeResumeResult ResumeSession(const FNeoStackAgentRuntimeResumeOptions& Options);
	void SendPrompt(const FString& SessionId, const FString& AgentName, const FString& PromptText);
	void CancelPrompt(const FString& SessionId);
	void CloseSession(const FString& SessionId);

private:
	FNeoStackAgentRuntimeService() = default;
};
