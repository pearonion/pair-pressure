// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Tools/UEToolsetsBridgeTool.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#ifndef WITH_UE_TOOLSET_REGISTRY
#define WITH_UE_TOOLSET_REGISTRY 0
#endif

#if WITH_UE_TOOLSET_REGISTRY
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#endif

namespace
{
	FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	bool TrySerializeObjectField(const TSharedPtr<FJsonObject>& Args, const FString& FieldName, FString& OutJson)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Object>(FieldName))
		{
			return false;
		}

		const TSharedPtr<FJsonObject> Object = Args->GetObjectField(FieldName);
		if (!Object.IsValid())
		{
			return false;
		}

		OutJson = JsonObjectToString(Object.ToSharedRef());
		return true;
	}

	bool IsJsonObjectString(const FString& Json)
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
	}

	TSharedPtr<FJsonObject> MakeStringProperty(const FString& Description)
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), Description);
		return Prop;
	}

	void CompleteOnGameThread(FNeoStackToolBase::FResultCallback OnComplete, FToolResult Result)
	{
		AsyncTask(ENamedThreads::GameThread, [OnComplete = MoveTemp(OnComplete), Result = MoveTemp(Result)]() mutable
		{
			OnComplete(Result);
		});
	}

#if WITH_UE_TOOLSET_REGISTRY
	TValueOrError<TObjectPtr<UToolsetRegistrySubsystem>, FString> GetToolsetRegistrySubsystem()
	{
		return UToolsetRegistrySubsystem::Get(TEXT("NeoStack ue_toolsets"));
	}

	TValueOrError<UE::ToolsetRegistry::FToolDescriptor, FString> MakeToolDescriptor(FString ToolsetName, FString ToolName)
	{
		ToolsetName.TrimStartAndEndInline();
		ToolName.TrimStartAndEndInline();

		if (ToolsetName.IsEmpty())
		{
			return UE::ToolsetRegistry::FToolDescriptor::FromString(ToolName);
		}

		const FString Prefix = ToolsetName + TEXT(".");
		if (ToolName.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			ToolName = ToolName.RightChop(Prefix.Len());
		}

		if (ToolName.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("Tool name is empty for toolset '%s'."), *ToolsetName));
		}

		UE::ToolsetRegistry::FToolDescriptor Descriptor;
		Descriptor.ToolsetName = ToolsetName;
		Descriptor.ToolName = ToolName;
		return MakeValue(Descriptor);
	}

	FToolResult MakeToolsetListResult(UE::ToolsetRegistry::FToolsetRegistry& Registry)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ToolsetsJson;

		Registry.ForEachToolset([&ToolsetsJson](const FString& Name, const UE::ToolsetRegistry::FToolset& Toolset)
		{
			TSharedPtr<FJsonObject> ToolsetJson = MakeShared<FJsonObject>();
			ToolsetJson->SetStringField(TEXT("name"), Name);
			ToolsetJson->SetStringField(TEXT("version"), Toolset.GetToolsetVersion());
			ToolsetJson->SetStringField(TEXT("description"), Toolset.GetToolsetDescription());
			ToolsetJson->SetBoolField(TEXT("enabled"), Toolset.IsEnabled());

			TArray<TSharedPtr<FJsonValue>> ToolsJson;
			for (const FString& ToolName : Toolset.ListToolNames())
			{
				ToolsJson.Add(MakeShared<FJsonValueString>(ToolName));
			}
			ToolsetJson->SetArrayField(TEXT("tools"), MoveTemp(ToolsJson));

			ToolsetsJson.Add(MakeShared<FJsonValueObject>(ToolsetJson));
		});

		Root->SetBoolField(TEXT("available"), true);
		Root->SetArrayField(TEXT("toolsets"), MoveTemp(ToolsetsJson));
		return FToolResult::Ok(JsonObjectToString(Root));
	}

	FToolResult MakeToolsetDescribeResult(UE::ToolsetRegistry::FToolsetRegistry& Registry, const FString& ToolsetName)
	{
		if (ToolsetName.IsEmpty())
		{
			return FToolResult::Fail(TEXT("'toolset_name' is required for action 'describe'."));
		}

		FString ErrorMessage;
		TSharedPtr<UE::ToolsetRegistry::FToolset> Toolset = Registry.Find(ToolsetName, false, &ErrorMessage);
		if (!Toolset.IsValid())
		{
			return FToolResult::Fail(ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Toolset '%s' was not found."), *ToolsetName)
				: ErrorMessage);
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("name"), Toolset->GetToolsetName());
		Root->SetStringField(TEXT("version"), Toolset->GetToolsetVersion());
		Root->SetStringField(TEXT("description"), Toolset->GetToolsetDescription());
		Root->SetBoolField(TEXT("enabled"), Toolset->IsEnabled());
		Root->SetStringField(TEXT("schema_json"), Toolset->GetJsonSchema());

		TArray<TSharedPtr<FJsonValue>> ToolsJson;
		for (const FString& ToolName : Toolset->ListToolNames())
		{
			ToolsJson.Add(MakeShared<FJsonValueString>(ToolName));
		}
		Root->SetArrayField(TEXT("tools"), MoveTemp(ToolsJson));

		return FToolResult::Ok(JsonObjectToString(Root));
	}

	FToolResult MakeToolsetSchemasResult(UE::ToolsetRegistry::FToolsetRegistry& Registry)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schemas_json"), Registry.GetToolsetJsonSchemas());
		return FToolResult::Ok(JsonObjectToString(Root));
	}
#endif
}

FString FUEToolsetsBridgeTool::GetName() const
{
	return TEXT("ue_toolsets");
}

FString FUEToolsetsBridgeTool::GetDescription() const
{
	return TEXT("Bridge to UE 5.8 ToolsetRegistry. Use action=list to discover toolsets, describe to inspect one toolset schema, schemas for all raw schemas, and call to execute a toolset tool.");
}

TSharedPtr<FJsonObject> FUEToolsetsBridgeTool::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
	ActionProp->SetStringField(TEXT("type"), TEXT("string"));
	ActionProp->SetStringField(TEXT("description"), TEXT("ToolsetRegistry bridge action."));
	TArray<TSharedPtr<FJsonValue>> ActionEnum;
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("list")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("describe")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("schemas")));
	ActionEnum.Add(MakeShared<FJsonValueString>(TEXT("call")));
	ActionProp->SetArrayField(TEXT("enum"), MoveTemp(ActionEnum));
	Props->SetObjectField(TEXT("action"), ActionProp);

	Props->SetObjectField(TEXT("toolset_name"), MakeStringProperty(TEXT("Toolset name for describe and call actions.")));
	Props->SetObjectField(TEXT("tool_name"), MakeStringProperty(TEXT("Tool name inside the selected toolset for call actions.")));

	TSharedPtr<FJsonObject> ArgumentsProp = MakeShared<FJsonObject>();
	ArgumentsProp->SetStringField(TEXT("type"), TEXT("object"));
	ArgumentsProp->SetStringField(TEXT("description"), TEXT("JSON object passed to the UE toolset tool for call actions."));
	Props->SetObjectField(TEXT("arguments"), ArgumentsProp);

	Props->SetObjectField(TEXT("arguments_json"), MakeStringProperty(TEXT("Raw JSON string passed to the UE toolset tool. Takes precedence over arguments.")));

	Schema->SetObjectField(TEXT("properties"), Props);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("action")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

void FUEToolsetsBridgeTool::Execute(const TSharedPtr<FJsonObject>& Args, const FResultCallback& OnComplete)
{
	if (!Args.IsValid())
	{
		OnComplete(FToolResult::Fail(TEXT("Invalid arguments.")));
		return;
	}

	FString Action;
	if (!Args->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
	{
		OnComplete(FToolResult::Fail(TEXT("'action' is required.")));
		return;
	}

#if WITH_UE_TOOLSET_REGISTRY
	TValueOrError<TObjectPtr<UToolsetRegistrySubsystem>, FString> SubsystemResult = GetToolsetRegistrySubsystem();
	if (SubsystemResult.HasError())
	{
		OnComplete(FToolResult::Fail(SubsystemResult.GetError()));
		return;
	}

	UToolsetRegistrySubsystem* Subsystem = SubsystemResult.GetValue();
	if (!Subsystem)
	{
		OnComplete(FToolResult::Fail(TEXT("ToolsetRegistry subsystem is unavailable.")));
		return;
	}

	UE::ToolsetRegistry::FToolsetRegistry& Registry = Subsystem->ToolsetRegistry;

	if (Action.Equals(TEXT("list"), ESearchCase::IgnoreCase))
	{
		OnComplete(MakeToolsetListResult(Registry));
		return;
	}

	if (Action.Equals(TEXT("describe"), ESearchCase::IgnoreCase))
	{
		FString ToolsetName;
		Args->TryGetStringField(TEXT("toolset_name"), ToolsetName);
		OnComplete(MakeToolsetDescribeResult(Registry, ToolsetName));
		return;
	}

	if (Action.Equals(TEXT("schemas"), ESearchCase::IgnoreCase))
	{
		OnComplete(MakeToolsetSchemasResult(Registry));
		return;
	}

	if (Action.Equals(TEXT("call"), ESearchCase::IgnoreCase))
	{
		FString ToolsetName;
		FString ToolName;
		Args->TryGetStringField(TEXT("toolset_name"), ToolsetName);
		Args->TryGetStringField(TEXT("tool_name"), ToolName);

		if (ToolsetName.IsEmpty())
		{
			if (!ToolName.Contains(TEXT(".")))
			{
				OnComplete(FToolResult::Fail(TEXT("'toolset_name' is required for action 'call' unless 'tool_name' is a fully qualified Toolset.Tool name.")));
				return;
			}
		}
		if (ToolName.IsEmpty())
		{
			OnComplete(FToolResult::Fail(TEXT("'tool_name' is required for action 'call'.")));
			return;
		}

		FString ArgumentsJson;
		if (!Args->TryGetStringField(TEXT("arguments_json"), ArgumentsJson) || ArgumentsJson.IsEmpty())
		{
			if (!TrySerializeObjectField(Args, TEXT("arguments"), ArgumentsJson))
			{
				ArgumentsJson = TEXT("{}");
			}
		}
		else if (!IsJsonObjectString(ArgumentsJson))
		{
			OnComplete(FToolResult::Fail(TEXT("'arguments_json' must be a valid JSON object string.")));
			return;
		}

		TValueOrError<UE::ToolsetRegistry::FToolDescriptor, FString> DescriptorResult = MakeToolDescriptor(ToolsetName, ToolName);
		if (DescriptorResult.HasError())
		{
			OnComplete(FToolResult::Fail(DescriptorResult.GetError()));
			return;
		}

		Registry.ExecuteTool(DescriptorResult.GetValue(), ArgumentsJson).Next(
			[OnComplete](TValueOrError<FString, FString>&& ToolResult) mutable
			{
				if (ToolResult.HasError())
				{
					CompleteOnGameThread(OnComplete, FToolResult::Fail(ToolResult.StealError()));
				}
				else
				{
					CompleteOnGameThread(OnComplete, FToolResult::Ok(ToolResult.StealValue()));
				}
			});
		return;
	}

	OnComplete(FToolResult::Fail(FString::Printf(TEXT("Unknown ue_toolsets action: %s"), *Action)));
#else
	OnComplete(FToolResult::Fail(TEXT("UE ToolsetRegistry is not available in this engine build.")));
#endif
}
