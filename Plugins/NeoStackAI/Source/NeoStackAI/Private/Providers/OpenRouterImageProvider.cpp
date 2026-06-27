// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "ACPSettings.h"
#include "Tools/AssetImportUtils.h"
#include "UserPreferencesSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
FString ORString(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, const FString& Default = FString())
{
	FString Value;
	return Json.IsValid() && Json->TryGetStringField(Key, Value) ? Value : Default;
}

void AddStringIfPresent(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Source, const TCHAR* Key)
{
	const FString Value = ORString(Source, Key);
	if (!Value.IsEmpty())
	{
		Obj->SetStringField(Key, Value);
	}
}

FString ExtensionForDataUrl(const FString& DataUrl)
{
	if (DataUrl.StartsWith(TEXT("data:image/jpeg")) || DataUrl.StartsWith(TEXT("data:image/jpg")))
	{
		return TEXT("jpg");
	}
	if (DataUrl.StartsWith(TEXT("data:image/webp")))
	{
		return TEXT("webp");
	}
	return TEXT("png");
}

bool ExtractImageUrlFromImageObject(const TSharedPtr<FJsonObject>& ImageObject, FString& OutUrl)
{
	if (!ImageObject.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ImageUrlObject = nullptr;
	if (ImageObject->TryGetObjectField(TEXT("image_url"), ImageUrlObject) && ImageUrlObject && (*ImageUrlObject).IsValid())
	{
		OutUrl = ORString(*ImageUrlObject, TEXT("url"));
		return !OutUrl.IsEmpty();
	}

	if (ImageObject->TryGetObjectField(TEXT("imageUrl"), ImageUrlObject) && ImageUrlObject && (*ImageUrlObject).IsValid())
	{
		OutUrl = ORString(*ImageUrlObject, TEXT("url"));
		return !OutUrl.IsEmpty();
	}

	OutUrl = ORString(ImageObject, TEXT("url"));
	return !OutUrl.IsEmpty();
}

FGenerativeJob ImageResultFromOpenRouterResponse(const TSharedPtr<FJsonObject>& Json)
{
	if (!Json.IsValid())
	{
		return FGenerativeJob::MakeFail(TEXT("OpenRouter image generation returned an empty response"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
	if (!Json->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
	{
		return FGenerativeJob::MakeFail(TEXT("OpenRouter image generation returned no choices"));
	}

	FGenerativeJob Job;
	Job.ProviderId = TEXT("openrouter");
	Job.ActionId = TEXT("text_to_image");
	Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Job.Status = EGenerativeJobStatus::Succeeded;
	Job.Progress = 100;
	Job.RawResponse = Json;

	for (const TSharedPtr<FJsonValue>& ChoiceValue : *Choices)
	{
		const TSharedPtr<FJsonObject> Choice = ChoiceValue.IsValid() ? ChoiceValue->AsObject() : nullptr;
		if (!Choice.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* Message = nullptr;
		if (!Choice->TryGetObjectField(TEXT("message"), Message) || !Message || !(*Message).IsValid())
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
		if (!(*Message)->TryGetArrayField(TEXT("images"), Images) || !Images)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& ImageValue : *Images)
		{
			FString ImageUrl;
			if (!ExtractImageUrlFromImageObject(ImageValue.IsValid() ? ImageValue->AsObject() : nullptr, ImageUrl))
			{
				continue;
			}

			if (ImageUrl.StartsWith(TEXT("data:image/")))
			{
				FString SaveError;
				const FString TempFile = AssetImportUtils::SaveBase64ToTempFile(ImageUrl, ExtensionForDataUrl(ImageUrl), SaveError);
				if (TempFile.IsEmpty())
				{
					return FGenerativeJob::MakeFail(FString::Printf(TEXT("OpenRouter image decode failed: %s"), *SaveError));
				}
				ImageUrl = TempFile;
			}

			Job.ImageUrls.Add(ImageUrl);
			if (Job.ResultUrl.IsEmpty())
			{
				Job.ResultUrl = ImageUrl;
			}
		}
	}

	if (Job.ResultUrl.IsEmpty())
	{
		return FGenerativeJob::MakeFail(TEXT("OpenRouter response did not contain generated images"));
	}

	return Job;
}
}

class FOpenRouterImageProvider : public FGenerativeProviderBase
{
public:
	FString GetId() const override { return TEXT("openrouter"); }
	FString GetDisplayName() const override { return TEXT("OpenRouter Images"); }
	FString GetWebsite() const override { return TEXT("https://openrouter.ai"); }
	FString GetDirectBaseUrl() const override { return TEXT("https://openrouter.ai/api/v1"); }
	FString GetApiKeySettingName() const override { return TEXT("OpenRouterApiKey"); }

	FString GetAuthToken() const override
	{
		const UACPSettings* Settings = UACPSettings::Get();
		return Settings ? Settings->GetOpenRouterAuthToken() : FString();
	}

	FString GetBaseUrl() const override
	{
		const UACPSettings* Settings = UACPSettings::Get();
		return Settings ? Settings->GetOpenRouterImageGenerationUrl() : TEXT("https://openrouter.ai/api/v1/chat/completions");
	}

	TArray<FProviderActionDescriptor> GetActions() const override
	{
		return {
			{TEXT("text_to_image"),
				TEXT("Generate an image through OpenRouter image-capable chat models. "
					"Uses chat completions with image output modalities and returns a temp image file when the model returns base64."),
				{TEXT("text")}, {TEXT("image")}, BuildTextToImageSchema(), TEXT("varies"), true},
		};
	}

	void Submit(const FString& ActionId,
		const TSharedPtr<FJsonObject>& Params,
		FGenerativeJobCallback OnComplete) override
	{
		if (ActionId != TEXT("text_to_image"))
		{
			OnComplete(FGenerativeJob::MakeFail(FString::Printf(TEXT("Unknown OpenRouter image action: %s"), *ActionId)));
			return;
		}

		const FString Prompt = ORString(Params, TEXT("prompt"), ORString(Params, TEXT("text")));
		if (Prompt.IsEmpty())
		{
			OnComplete(FGenerativeJob::MakeFail(TEXT("OpenRouter text_to_image requires 'prompt'")));
			return;
		}

		FString DefaultModel = TEXT("black-forest-labs/flux.2-flex");
		if (const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get())
		{
			if (!Prefs->ImageGenerationDefaultModel.IsEmpty())
			{
				DefaultModel = Prefs->ImageGenerationDefaultModel;
			}
		}

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), ORString(Params, TEXT("model"), DefaultModel));
		Body->SetBoolField(TEXT("stream"), false);

		TArray<TSharedPtr<FJsonValue>> Modalities;
		Modalities.Add(MakeShared<FJsonValueString>(TEXT("image")));
		if (!Params.IsValid() || !Params->HasField(TEXT("image_only")) || !Params->GetBoolField(TEXT("image_only")))
		{
			Modalities.Add(MakeShared<FJsonValueString>(TEXT("text")));
		}
		Body->SetArrayField(TEXT("modalities"), Modalities);

		TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), TEXT("user"));
		Message->SetStringField(TEXT("content"), Prompt);
		TArray<TSharedPtr<FJsonValue>> Messages;
		Messages.Add(MakeShared<FJsonValueObject>(Message));
		Body->SetArrayField(TEXT("messages"), Messages);

		TSharedPtr<FJsonObject> ImageConfig = MakeShared<FJsonObject>();
		AddStringIfPresent(ImageConfig, Params, TEXT("aspect_ratio"));
		AddStringIfPresent(ImageConfig, Params, TEXT("image_size"));
		if (ImageConfig->Values.Num() > 0)
		{
			Body->SetObjectField(TEXT("image_config"), ImageConfig);
		}

		HttpPost(TEXT(""), Body,
			[OnComplete = MoveTemp(OnComplete)](const FHttpJsonResult& Result) mutable
			{
				if (!Result.bSuccess)
				{
					OnComplete(FGenerativeJob::MakeFail(FString::Printf(TEXT("OpenRouter image generation failed: %s"), *Result.Error)));
					return;
				}

				OnComplete(ImageResultFromOpenRouterResponse(Result.Json));
			},
			180.0f);
	}

	void CheckStatus(const FString& JobId, const FString& ActionId, FGenerativeJobCallback OnComplete) override
	{
		OnComplete(FGenerativeJob::MakeSuccess(JobId, TEXT("")));
	}

private:
	static TSharedPtr<FJsonObject> BuildTextToImageSchema()
	{
		TMap<FString, TSharedPtr<FJsonObject>> Props;
		Props.Add(TEXT("prompt"), SchemaString(TEXT("Image prompt")));
		Props.Add(TEXT("model"), SchemaString(TEXT("OpenRouter image-capable model id"), {}, TEXT("black-forest-labs/flux.2-flex")));
		Props.Add(TEXT("aspect_ratio"), SchemaString(TEXT("Provider image aspect ratio"), {TEXT("1:1"), TEXT("16:9"), TEXT("9:16"), TEXT("4:3"), TEXT("3:4"), TEXT("3:2"), TEXT("2:3")}));
		Props.Add(TEXT("image_size"), SchemaString(TEXT("Provider image size hint"), {TEXT("0.5K"), TEXT("1K"), TEXT("2K"), TEXT("4K")}, TEXT("1K")));
		Props.Add(TEXT("image_only"), SchemaBool(TEXT("Request image output without text when the selected model supports it"), false));
		return BuildSchema(Props, {TEXT("prompt")});
	}
};

REGISTER_GENERATIVE_PROVIDER(FOpenRouterImageProvider);
