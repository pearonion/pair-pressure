// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "ACPSettings.h"
#include "Tools/AssetImportUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
FString JsonString(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, const FString& Default = FString())
{
	FString Value;
	return Json.IsValid() && Json->TryGetStringField(Key, Value) ? Value : Default;
}

int32 JsonInt(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, int32 Default)
{
	int32 Value = Default;
	return Json.IsValid() && Json->TryGetNumberField(Key, Value) ? Value : Default;
}

void SetIfPresent(const TSharedPtr<FJsonObject>& Out, const TSharedPtr<FJsonObject>& In, const TCHAR* Key)
{
	if (!In.IsValid() || !In->HasField(Key))
	{
		return;
	}

	const TSharedPtr<FJsonValue> Value = In->TryGetField(Key);
	if (Value.IsValid())
	{
		Out->SetField(Key, Value);
	}
}

FGenerativeJob ImageResultFromOpenAIResponse(const TSharedPtr<FJsonObject>& Json)
{
	if (!Json.IsValid())
	{
		return FGenerativeJob::MakeFail(TEXT("OpenAI image generation returned an empty response"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
	if (!Json->TryGetArrayField(TEXT("data"), Data) || !Data || Data->Num() == 0)
	{
		return FGenerativeJob::MakeFail(TEXT("OpenAI image generation returned no images"));
	}

	FGenerativeJob Job;
	Job.ProviderId = TEXT("openai");
	Job.ActionId = TEXT("text_to_image");
	Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Job.Status = EGenerativeJobStatus::Succeeded;
	Job.Progress = 100;
	Job.RawResponse = Json;

	for (const TSharedPtr<FJsonValue>& EntryValue : *Data)
	{
		const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			continue;
		}

		const FString Url = JsonString(Entry, TEXT("url"));
		if (!Url.IsEmpty())
		{
			Job.ImageUrls.Add(Url);
			if (Job.ResultUrl.IsEmpty())
			{
				Job.ResultUrl = Url;
			}
			continue;
		}

		const FString B64 = JsonString(Entry, TEXT("b64_json"));
		if (!B64.IsEmpty())
		{
			FString SaveError;
			const FString TempFile = AssetImportUtils::SaveBase64ToTempFile(B64, TEXT("png"), SaveError);
			if (TempFile.IsEmpty())
			{
				return FGenerativeJob::MakeFail(FString::Printf(TEXT("OpenAI image decode failed: %s"), *SaveError));
			}

			Job.ImageUrls.Add(TempFile);
			if (Job.ResultUrl.IsEmpty())
			{
				Job.ResultUrl = TempFile;
			}
		}
	}

	if (Job.ResultUrl.IsEmpty())
	{
		return FGenerativeJob::MakeFail(TEXT("OpenAI image response did not contain url or b64_json data"));
	}

	return Job;
}
}

class FOpenAIImageProvider : public FGenerativeProviderBase
{
public:
	FString GetId() const override { return TEXT("openai"); }
	FString GetDisplayName() const override { return TEXT("OpenAI Images"); }
	FString GetWebsite() const override { return TEXT("https://platform.openai.com"); }
	FString GetDirectBaseUrl() const override { return TEXT("https://api.openai.com/v1"); }
	FString GetApiKeySettingName() const override { return TEXT("OpenAIApiKey"); }

	FString GetAuthToken() const override
	{
		const UACPSettings* Settings = UACPSettings::Get();
		return Settings ? Settings->OpenAIApiKey.TrimStartAndEnd() : FString();
	}

	FString GetBaseUrl() const override
	{
		return GetDirectBaseUrl();
	}

	TArray<FProviderActionDescriptor> GetActions() const override
	{
		return {
			{TEXT("text_to_image"),
				TEXT("Generate a raster image from a prompt using OpenAI GPT Image models. "
					"Returns a PNG temp file path that generate({action='import'}) can import as Texture2D."),
				{TEXT("text")}, {TEXT("image")}, BuildTextToImageSchema(), TEXT("varies"), true},
		};
	}

	void Submit(const FString& ActionId,
		const TSharedPtr<FJsonObject>& Params,
		FGenerativeJobCallback OnComplete) override
	{
		if (ActionId != TEXT("text_to_image"))
		{
			OnComplete(FGenerativeJob::MakeFail(FString::Printf(TEXT("Unknown OpenAI image action: %s"), *ActionId)));
			return;
		}

		const FString Prompt = JsonString(Params, TEXT("prompt"), JsonString(Params, TEXT("text")));
		if (Prompt.IsEmpty())
		{
			OnComplete(FGenerativeJob::MakeFail(TEXT("OpenAI text_to_image requires 'prompt'")));
			return;
		}

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), JsonString(Params, TEXT("model"), TEXT("gpt-image-2")));
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetNumberField(TEXT("n"), FMath::Clamp(JsonInt(Params, TEXT("n"), 1), 1, 10));

		SetIfPresent(Body, Params, TEXT("size"));
		SetIfPresent(Body, Params, TEXT("quality"));
		SetIfPresent(Body, Params, TEXT("background"));
		SetIfPresent(Body, Params, TEXT("output_format"));
		SetIfPresent(Body, Params, TEXT("output_compression"));
		SetIfPresent(Body, Params, TEXT("moderation"));

		if (!Body->HasField(TEXT("output_format")))
		{
			Body->SetStringField(TEXT("output_format"), TEXT("png"));
		}

		HttpPost(TEXT("/images/generations"), Body,
			[OnComplete = MoveTemp(OnComplete)](const FHttpJsonResult& Result) mutable
			{
				if (!Result.bSuccess)
				{
					OnComplete(FGenerativeJob::MakeFail(FString::Printf(TEXT("OpenAI image generation failed: %s"), *Result.Error)));
					return;
				}

				OnComplete(ImageResultFromOpenAIResponse(Result.Json));
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
		Props.Add(TEXT("model"), SchemaString(TEXT("OpenAI image model"), {TEXT("gpt-image-2"), TEXT("gpt-image-1.5"), TEXT("gpt-image-1"), TEXT("gpt-image-1-mini")}, TEXT("gpt-image-2")));
		Props.Add(TEXT("size"), SchemaString(TEXT("Image size. Use auto unless a fixed size is required."), {TEXT("auto"), TEXT("1024x1024"), TEXT("1024x1536"), TEXT("1536x1024")}, TEXT("auto")));
		Props.Add(TEXT("quality"), SchemaString(TEXT("Output quality"), {TEXT("auto"), TEXT("low"), TEXT("medium"), TEXT("high")}, TEXT("auto")));
		Props.Add(TEXT("background"), SchemaString(TEXT("Background handling"), {TEXT("auto"), TEXT("transparent"), TEXT("opaque")}, TEXT("auto")));
		Props.Add(TEXT("output_format"), SchemaString(TEXT("Output format"), {TEXT("png"), TEXT("jpeg"), TEXT("webp")}, TEXT("png")));
		Props.Add(TEXT("output_compression"), SchemaInt(TEXT("Compression for jpeg/webp output"), 0, 100, 0));
		Props.Add(TEXT("n"), SchemaInt(TEXT("Number of images"), 1, 10, 1));
		return BuildSchema(Props, {TEXT("prompt")});
	}
};

REGISTER_GENERATIVE_PROVIDER(FOpenAIImageProvider);
