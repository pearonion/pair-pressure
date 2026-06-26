// Copyright 2026 Betide Studio. All Rights Reserved.

#include "AgentRuntime/AgentRuntimeService.h"

#include "ACPAgentManager.h"
#include "ACPSettings.h"
#include "ACPClient.h"
#include "Chat/ChatModelRegistry.h"
#include "Chat/ChatSessionManager.h"
#include "Chat/ChatStore.h"
#include "Misc/Paths.h"
#include "NeoStackAIModule.h"

FNeoStackAgentRuntimeService& FNeoStackAgentRuntimeService::Get()
{
	static FNeoStackAgentRuntimeService Instance;
	return Instance;
}

namespace
{
FString BuildChatGatewayRuntimePayload(const FChatSessionAttachOptions& Options)
{
	const FString EscapedModelId = Options.PrefixedModelId.ReplaceCharWithEscapedChar();
	const FString EscapedReasoning = Options.ReasoningLevel.ReplaceCharWithEscapedChar();
	return FString::Printf(TEXT("{\"modelId\":\"%s\",\"reasoning\":\"%s\"}"),
		*EscapedModelId,
		*EscapedReasoning);
}

FChatSessionAttachOptions BuildChatGatewayAttachOptions(const FString& AgentName, const FString& StoredModelId = FString())
{
	FChatSessionAttachOptions Options;
	Options.PrefixedModelId = StoredModelId.IsEmpty()
		? FChatModelRegistry::Get().GetSelectedModel()
		: StoredModelId;

	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		Options.ReasoningLevel = Settings->GetSavedReasoningForAgent(AgentName);
	}

	return Options;
}
}

ENeoStackAgentRuntimeKind FNeoStackAgentRuntimeService::GetRuntimeKindForAgent(const FString& AgentName) const
{
	if (AgentName.IsEmpty())
	{
		return ENeoStackAgentRuntimeKind::Unknown;
	}

	const FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	return AgentMgr.IsChatGatewayAgent(AgentName)
		? ENeoStackAgentRuntimeKind::ChatGateway
		: ENeoStackAgentRuntimeKind::ACP;
}

bool FNeoStackAgentRuntimeService::HasLiveSession(const FString& SessionId) const
{
	if (SessionId.IsEmpty())
	{
		return false;
	}

	if (FChatSessionManager::Get().HasSession(SessionId))
	{
		return true;
	}

	return !FACPAgentManager::Get().GetSessionAgent(SessionId).IsEmpty();
}

bool FNeoStackAgentRuntimeService::CreateSession(const FNeoStackAgentRuntimeCreateOptions& Options)
{
	if (Options.SessionId.IsEmpty() || Options.AgentName.IsEmpty())
	{
		return false;
	}

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	AgentMgr.RegisterSession(Options.SessionId, Options.AgentName);
	FString RegistryId;
	if (const FACPAgentConfig* Config = AgentMgr.GetAgentConfig(Options.AgentName))
	{
		RegistryId = Config->RegistryId;
	}

	if (AgentMgr.IsChatGatewayAgent(Options.AgentName))
	{
		const FChatSessionAttachOptions AttachOptions = BuildChatGatewayAttachOptions(Options.AgentName);
		FNeoStackChatStore::Get().UpsertProviderBinding(
			Options.SessionId, Options.AgentName, TEXT("chat_gateway"), RegistryId, TEXT("{}"),
			BuildChatGatewayRuntimePayload(AttachOptions),
			TEXT("active"));
		const bool bAttached = FChatSessionManager::Get().AttachSession(Options.SessionId, AttachOptions);
		return bAttached;
	}

	FNeoStackChatStore::Get().UpsertProviderBinding(
		Options.SessionId, Options.AgentName, TEXT("acp"), RegistryId,
		FString::Printf(TEXT("{\"sessionId\":\"%s\"}"), *Options.SessionId),
		TEXT("{}"), TEXT("active"));

	AgentMgr.EnsureSessionClient(Options.SessionId, Options.AgentName);
	AgentMgr.StartSessionConversation(Options.SessionId, Options.AgentName);

	return true;
}

FNeoStackAgentRuntimeResumeResult FNeoStackAgentRuntimeService::ResumeSession(const FNeoStackAgentRuntimeResumeOptions& Options)
{
	FNeoStackAgentRuntimeResumeResult Result;
	if (Options.SessionId.IsEmpty() || Options.AgentName.IsEmpty())
	{
		Result.Error = TEXT("Missing session id or agent name");
		return Result;
	}

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	const FString WorkingDirectory = Options.WorkingDirectory.IsEmpty() ? FPaths::ProjectDir() : Options.WorkingDirectory;

	AgentMgr.RegisterSession(Options.SessionId, Options.AgentName);
	FString RegistryId;
	if (const FACPAgentConfig* Config = AgentMgr.GetAgentConfig(Options.AgentName))
	{
		RegistryId = Config->RegistryId;
	}

	if (AgentMgr.IsChatGatewayAgent(Options.AgentName))
	{
		FACPSessionMetadata StoredMetadata;
		FNeoStackChatStore::Get().LoadSession(Options.SessionId, StoredMetadata);
		FChatSessionAttachOptions AttachOptions = BuildChatGatewayAttachOptions(Options.AgentName, StoredMetadata.ModelId);
		FNeoStackChatStore::Get().UpsertProviderBinding(
			Options.SessionId, Options.AgentName, TEXT("chat_gateway"), RegistryId, TEXT("{}"),
			BuildChatGatewayRuntimePayload(AttachOptions),
			TEXT("active"));
		FNeoStackChatStore::Get().LoadMessages(Options.SessionId, AttachOptions.RestoredMessages);
		Result.bStarted = FChatSessionManager::Get().AttachSession(Options.SessionId, AttachOptions);
		Result.bLoading = false;
		return Result;
	}

	FNeoStackChatStore::Get().UpsertProviderBinding(
		Options.SessionId, Options.AgentName, TEXT("acp"), RegistryId,
		FString::Printf(TEXT("{\"sessionId\":\"%s\"}"), *Options.SessionId),
		FString::Printf(TEXT("{\"cwd\":\"%s\"}"), *WorkingDirectory.ReplaceCharWithEscapedChar()),
		TEXT("active"));

	if (Options.bLaunchResume)
	{
		AgentMgr.SetLaunchResumeSession(Options.AgentName, Options.SessionId);
		AgentMgr.EnsureSessionClient(Options.SessionId, Options.AgentName);
		Result.bStarted = true;
		Result.bLoading = false;
		return Result;
	}

	AgentMgr.EnsureSessionClient(Options.SessionId, Options.AgentName);
	AgentMgr.StartConversationOnSessionClient(
		Options.SessionId,
		WorkingDirectory,
		/*bForceNewSession=*/false);

	Result.bStarted = true;
	Result.bLoading = true;
	return Result;
}

void FNeoStackAgentRuntimeService::SendPrompt(const FString& SessionId, const FString& AgentName, const FString& PromptText)
{
	if (SessionId.IsEmpty() || PromptText.IsEmpty())
	{
		return;
	}

	if (FChatSessionManager::Get().HasSession(SessionId))
	{
		FChatSessionManager::Get().SendPrompt(SessionId, PromptText);
		return;
	}

	FString ResolvedAgentName = AgentName;
	if (ResolvedAgentName.IsEmpty())
	{
		ResolvedAgentName = FACPAgentManager::Get().GetSessionAgent(SessionId);
	}

	if (!ResolvedAgentName.IsEmpty())
	{
		FACPAgentManager::Get().SendPromptToSession(SessionId, ResolvedAgentName, PromptText);
	}
}

void FNeoStackAgentRuntimeService::CancelPrompt(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		return;
	}

	if (FChatSessionManager::Get().HasSession(SessionId))
	{
		FChatSessionManager::Get().CancelPrompt(SessionId);
		return;
	}

	FACPAgentManager::Get().CancelSessionPrompt(SessionId);
}

void FNeoStackAgentRuntimeService::CloseSession(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		return;
	}

	const bool bClosedChatSession = FChatSessionManager::Get().CloseSession(SessionId);
	if (!bClosedChatSession)
	{
		FACPAgentManager::Get().CancelSessionPrompt(SessionId);
	}
	FACPAgentManager::Get().UnregisterSession(SessionId);
}
