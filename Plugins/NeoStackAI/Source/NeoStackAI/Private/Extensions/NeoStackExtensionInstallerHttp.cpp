// Copyright 2026 Betide Studio. All Rights Reserved.
//
// HTTP-backed steps for FNeoStackExtensionInstaller:
//   • Step_ResolveDownload — GET /api/external/download to get a signed R2 URL
//   • Step_DownloadFile    — GET the signed URL, stream to disk with progress
//
// UE's FHttpModule callbacks run on game thread; no marshaling needed.

#include "Extensions/NeoStackExtensionInstallerInternal.h"

#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

namespace
{
	// Tiny helper: extract a friendly error message from a JSON {error:{message,code}} body.
	FString TryExtractJsonError(const FString& Body)
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return FString();
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Root->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && ErrorObj->IsValid())
		{
			FString Msg;
			(*ErrorObj)->TryGetStringField(TEXT("message"), Msg);
			return Msg;
		}
		return FString();
	}
}

namespace NeoStackExtensionInstallerInternal
{
	// ── Step_ResolveDownload ────────────────────────────────────────
	// Calls GET /api/external/download with the op's slug + engine + platform
	// + variant + channel, receives a short-lived signed R2 URL + sha256 +
	// filename + size, stores it on the op, then advances.

	void Step_ResolveDownload(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		const FString ApiKey = GetApiKey();
		if (ApiKey.IsEmpty())
		{
			FailOp(Op, TEXT("No NeoStack API key. Paste one in Settings > Chat & Agents > Chat Providers > NeoStack Cloud."));
			return;
		}

		const FString BaseUrl = GetApiBaseUrl();
		const FString Url = FString::Printf(
			TEXT("%s/api/external/download?slug=%s&engine=%s&platform=%s&variant=%s&channel=%s"),
			*BaseUrl,
			*FGenericPlatformHttp::UrlEncode(Op->Slug),
			*FGenericPlatformHttp::UrlEncode(Op->EngineVersion),
			*FGenericPlatformHttp::UrlEncode(Op->Platform),
			*FGenericPlatformHttp::UrlEncode(Op->RequestedVariant),
			*FGenericPlatformHttp::UrlEncode(Op->RequestedChannel));

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));

		Request->OnProcessRequestComplete().BindLambda(
			[Op](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				if (!Op.IsValid()) return;

				if (!bConnected || !Response.IsValid())
				{
					FailOp(Op, TEXT("Download metadata request failed (network)."));
					return;
				}

				const int32 Code = Response->GetResponseCode();
				const FString Body = Response->GetContentAsString();

				if (Code != 200)
				{
					const FString Msg = TryExtractJsonError(Body);
					FailOp(Op, Msg.IsEmpty()
						? FString::Printf(TEXT("Download metadata HTTP %d"), Code)
						: Msg);
					return;
				}

				TSharedPtr<FJsonObject> Root;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
				if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
				{
					FailOp(Op, TEXT("Download metadata response was not valid JSON."));
					return;
				}

				Root->TryGetStringField(TEXT("url"), Op->ResolvedUrl);
				Root->TryGetStringField(TEXT("fileName"), Op->ResolvedFileName);
				Root->TryGetStringField(TEXT("checksum"), Op->ResolvedSha256);
				Root->TryGetStringField(TEXT("version"), Op->ResolvedVersion);
				double SizeDouble = 0;
				if (Root->TryGetNumberField(TEXT("fileSize"), SizeDouble))
				{
					Op->ResolvedFileSize = static_cast<int64>(SizeDouble);
					Op->BytesTotal = Op->ResolvedFileSize;
				}

				if (Op->ResolvedUrl.IsEmpty())
				{
					FailOp(Op, TEXT("Download metadata did not include a URL."));
					return;
				}

				AdvanceOp(Op);
			});

		if (!Request->ProcessRequest())
		{
			FailOp(Op, TEXT("Could not start the download metadata request."));
		}
	}

	// ── Step_DownloadFile ───────────────────────────────────────────
	// Stream the zip bytes from the signed R2 URL to a temp file in the cache
	// root. UE's IHttpRequest accumulates the response in memory, which is
	// fine for typical extension zips (≤ ~20 MB). For core plugins >100 MB,
	// the existing plugin-update flow uses a different streaming approach;
	// extensions stay small so in-memory is acceptable.
	//
	// Progress events update Op->BytesDone so the UI can draw a progress bar.

	void Step_DownloadFile(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		Op->TempArchivePath = BuildTempArchivePath(Op->Slug);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Op->ResolvedUrl);
		Request->SetVerb(TEXT("GET"));

		Request->OnRequestProgress64().BindLambda(
			[Op](FHttpRequestPtr, uint64 BytesSent, uint64 BytesReceived)
			{
				if (!Op.IsValid()) return;
				Op->BytesDone = static_cast<int64>(BytesReceived);
			});

		Request->OnProcessRequestComplete().BindLambda(
			[Op](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				if (!Op.IsValid()) return;

				if (!bConnected || !Response.IsValid())
				{
					FailOp(Op, TEXT("Zip download failed (network)."));
					return;
				}

				const int32 Code = Response->GetResponseCode();
				if (Code < 200 || Code >= 300)
				{
					FailOp(Op, FString::Printf(TEXT("Zip download HTTP %d"), Code));
					return;
				}

				// Ensure parent dir exists, write bytes.
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Op->TempArchivePath));

				const TArray<uint8>& Bytes = Response->GetContent();
				if (!FFileHelper::SaveArrayToFile(Bytes, *Op->TempArchivePath))
				{
					FailOp(Op, FString::Printf(TEXT("Could not write %s"), *Op->TempArchivePath));
					return;
				}

				// Final byte count (in case the progress callback missed the last chunk).
				Op->BytesDone = Bytes.Num();
				if (Op->BytesTotal <= 0)
				{
					Op->BytesTotal = Bytes.Num();
				}

				AdvanceOp(Op);
			});

		if (!Request->ProcessRequest())
		{
			FailOp(Op, TEXT("Could not start the zip download."));
		}
	}
}
