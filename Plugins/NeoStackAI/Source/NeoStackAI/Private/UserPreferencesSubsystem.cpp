// Copyright 2026 Betide Studio. All Rights Reserved.

#include "UserPreferencesSubsystem.h"

#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogUserPrefs, Log, All);

namespace
{
	FString GetPreferencesDir()
	{
		FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
		if (HomeDir.IsEmpty())
		{
			HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		}
		if (HomeDir.IsEmpty())
		{
			// Last-resort fallback when no user home is available (CI, sandboxed envs).
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NeoStackAI"));
		}
		return FPaths::Combine(HomeDir, TEXT(".agentintegrationkit"));
	}

	FString GetPreferencesFilePath()
	{
		return FPaths::Combine(GetPreferencesDir(), TEXT("user_prefs.json"));
	}
}

UUserPreferencesSubsystem* UUserPreferencesSubsystem::Get()
{
	if (!GEditor)
	{
		return nullptr;
	}
	return GEditor->GetEditorSubsystem<UUserPreferencesSubsystem>();
}

void UUserPreferencesSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoadPreferences();
}

void UUserPreferencesSubsystem::Deinitialize()
{
	SavePreferences();
	Super::Deinitialize();
}

void UUserPreferencesSubsystem::SavePreferences()
{
	const FString FilePath = GetPreferencesFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	// Notifications
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("onlyWhenUnfocused"), bOnlyNotifyWhenUnfocused);
		Obj->SetBoolField(TEXT("notifyOnComplete"), bNotifyOnTaskComplete);
		Obj->SetBoolField(TEXT("flashTaskbar"), bFlashTaskbarOnComplete);
		Obj->SetBoolField(TEXT("playSound"), bPlayCompletionSound);
		Obj->SetNumberField(TEXT("soundVolume"), CompletionSoundVolume);
		Obj->SetStringField(TEXT("completionSound"), CompletionSound.ToString());
		Obj->SetStringField(TEXT("errorSound"), ErrorSound.ToString());
		Obj->SetBoolField(TEXT("playPermissionSound"), bPlayPermissionRequestSound);
		Obj->SetNumberField(TEXT("permissionSoundVolume"), PermissionRequestSoundVolume);
		Obj->SetStringField(TEXT("permissionRequestSound"), PermissionRequestSound.ToString());
		Root->SetObjectField(TEXT("notifications"), Obj);
	}

	// Agent / Tool Execution
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("systemPromptAppend"), ACPSystemPromptAppend);
		Obj->SetNumberField(TEXT("toolTimeout"), ToolExecutionTimeoutSeconds);
		Obj->SetNumberField(TEXT("agentResponseTimeout"), AgentResponseTimeoutSeconds);
		Root->SetObjectField(TEXT("agentExecution"), Obj);
	}

	// AI Generation defaults
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("imageModel"), ImageGenerationDefaultModel);
		Obj->SetStringField(TEXT("meshyArtStyle"), MeshyDefaultArtStyle);
		Root->SetObjectField(TEXT("generation"), Obj);
	}

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Root, Writer);

	// Atomic write: stage to .tmp, then rename. Prevents truncated/corrupt file
	// if the editor crashes mid-write — a partial write would silently wipe
	// every preference on next load (see LoadPreferences error path).
	const FString TempPath = FilePath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(JsonString, *TempPath))
	{
		UE_LOG(LogUserPrefs, Error, TEXT("Failed to write user preferences temp file %s"), *TempPath);
		return;
	}

	if (!IFileManager::Get().Move(*FilePath, *TempPath, /*bReplace=*/true))
	{
		UE_LOG(LogUserPrefs, Error, TEXT("Failed to rename %s -> %s"), *TempPath, *FilePath);
		IFileManager::Get().Delete(*TempPath);
		return;
	}

	UE_LOG(LogUserPrefs, Verbose, TEXT("Saved user preferences to %s"), *FilePath);
}

void UUserPreferencesSubsystem::LoadPreferences()
{
	const FString FilePath = GetPreferencesFilePath();

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		UE_LOG(LogUserPrefs, Log, TEXT("No user_prefs.json found at %s — using defaults"), *FilePath);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUserPrefs, Warning, TEXT("Failed to parse user_prefs.json"));
		return;
	}

	// Notifications
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Root->TryGetObjectField(TEXT("notifications"), Obj))
		{
			(*Obj)->TryGetBoolField(TEXT("onlyWhenUnfocused"), bOnlyNotifyWhenUnfocused);
			(*Obj)->TryGetBoolField(TEXT("notifyOnComplete"), bNotifyOnTaskComplete);
			(*Obj)->TryGetBoolField(TEXT("flashTaskbar"), bFlashTaskbarOnComplete);
			(*Obj)->TryGetBoolField(TEXT("playSound"), bPlayCompletionSound);
			double Vol = CompletionSoundVolume;
			if ((*Obj)->TryGetNumberField(TEXT("soundVolume"), Vol)) CompletionSoundVolume = static_cast<float>(Vol);
			FString Path;
			if ((*Obj)->TryGetStringField(TEXT("completionSound"), Path)) CompletionSound = FSoftObjectPath(Path);
			if ((*Obj)->TryGetStringField(TEXT("errorSound"), Path)) ErrorSound = FSoftObjectPath(Path);
			(*Obj)->TryGetBoolField(TEXT("playPermissionSound"), bPlayPermissionRequestSound);
			double PVol = PermissionRequestSoundVolume;
			if ((*Obj)->TryGetNumberField(TEXT("permissionSoundVolume"), PVol)) PermissionRequestSoundVolume = static_cast<float>(PVol);
			if ((*Obj)->TryGetStringField(TEXT("permissionRequestSound"), Path)) PermissionRequestSound = FSoftObjectPath(Path);
		}
	}

	// Agent / Tool Execution
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Root->TryGetObjectField(TEXT("agentExecution"), Obj))
		{
			(*Obj)->TryGetStringField(TEXT("systemPromptAppend"), ACPSystemPromptAppend);
			int32 Timeout = ToolExecutionTimeoutSeconds;
			if ((*Obj)->TryGetNumberField(TEXT("toolTimeout"), Timeout)) ToolExecutionTimeoutSeconds = FMath::Clamp(Timeout, 0, 600);
			int32 AgentTimeout = AgentResponseTimeoutSeconds;
			if ((*Obj)->TryGetNumberField(TEXT("agentResponseTimeout"), AgentTimeout)) AgentResponseTimeoutSeconds = FMath::Clamp(AgentTimeout, 0, 86400);
		}
	}

	// AI Generation defaults
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Root->TryGetObjectField(TEXT("generation"), Obj))
		{
			(*Obj)->TryGetStringField(TEXT("imageModel"), ImageGenerationDefaultModel);
			(*Obj)->TryGetStringField(TEXT("meshyArtStyle"), MeshyDefaultArtStyle);
		}
	}

	UE_LOG(LogUserPrefs, Log, TEXT("Loaded user preferences from %s"), *FilePath);
}
