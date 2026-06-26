// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Unified `generate()` Lua binding — routes to any registered generative provider
// (Meshy / Tripo / ElevenLabs / fal.ai). Async via UE::NeoStack::Lua::AwaitAsync:
// the script yields while waiting for HTTP responses and ticker delays, so the
// editor stays interactive throughout long generations and downloads.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaCoroutineAwait.h"
#include "Lua/NeoLuaState.h"
#include "Lua/LuaStr.h"
#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "Tools/AssetImportUtils.h"
#include "NSAIAnalytics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/Base64.h"
#include "HAL/FileManager.h"
#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "RunOnMainThread.h"
#include "Sound/SoundWave.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
// sol/sol.hpp transitively includes lua.h with appropriate extern "C" guards.

// ── Docs ─────────────────────────────────────────────────────────────

static TArray<FLuaFunctionDoc> GenerateDocs = {
	{ TEXT("generate(opts)"),
	  TEXT("Universal generative AI function. Routes to any registered provider (OpenAI, OpenRouter, Meshy, Tripo, ElevenLabs, etc.). "
	       "opts.provider = provider id (e.g. 'openai', 'openrouter', 'meshy'). "
	       "opts.action = what to do (e.g. 'text_to_image', 'text_to_3d', 'rig', 'retexture', 'tts'). "
	       "Action 'check' polls a job until done. Action 'import' downloads/imports the result into UE5; image imports accept "
	       "background={mode='chroma_key', color='#00ff00', tolerance=40, softness=40, spill_cleanup=80, despill_edge_px=2, choke_px=0} "
	       "and texture={preset='ui_icon|pixel_art|texture|normal_map|decal'}. "
	       "Action 'discover' lists available providers and actions (no provider required). "
	       "Action 'cancel' cancels a running job. Action 'balance' checks credit balance. "
	       "Action 'get_result' fetches the final result of a completed job. "
	       "All other fields are action-specific params passed to the provider."),
	  TEXT("table {job_id, status, progress, result_url, thumbnail_url, extra_urls, image_urls, error, balance, ...}") },
};

// ── Param-conversion helpers (sync — no async needed) ─────────────────

namespace
{
	constexpr int32 MaxTableDepth = 32;

	bool NormalizeImportDestinationPath(FString& InOutPath, FString& OutError)
	{
		InOutPath.TrimStartAndEndInline();
		while (InOutPath.Len() > 1 && InOutPath.EndsWith(TEXT("/")))
		{
			InOutPath.LeftChopInline(1);
		}

		const FString ProbePackageName = InOutPath / TEXT("__NeoStackImportValidation__");
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(ProbePackageName, /*bIncludeReadOnlyRoots=*/false, &Reason))
		{
			OutError = Reason.ToString();
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> TableToJson(const sol::table& T, int32 Depth = 0)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (Depth > MaxTableDepth) return Json;

		for (const auto& Pair : T)
		{
			if (!Pair.first.is<std::string>()) continue;
			const FString Key = NeoLuaStr::ToFString(Pair.first.as<std::string>());

			if (Pair.second.is<std::string>())
			{
				Json->SetStringField(Key, NeoLuaStr::ToFString(Pair.second.as<std::string>()));
			}
			else if (Pair.second.is<bool>())
			{
				Json->SetBoolField(Key, Pair.second.as<bool>());
			}
			else if (Pair.second.is<double>())
			{
				Json->SetNumberField(Key, Pair.second.as<double>());
			}
			else if (Pair.second.is<int>())
			{
				Json->SetNumberField(Key, Pair.second.as<int>());
			}
			else if (Pair.second.is<sol::table>())
			{
				sol::table Sub = Pair.second.as<sol::table>();
				bool bIsArray = true;
				int32 ExpectedIdx = 1;
				for (const auto& SubPair : Sub)
				{
					if (!SubPair.first.is<int>() || SubPair.first.as<int>() != ExpectedIdx)
					{
						bIsArray = false;
						break;
					}
					ExpectedIdx++;
				}

				if (bIsArray)
				{
					TArray<TSharedPtr<FJsonValue>> Arr;
					for (const auto& SubPair : Sub)
					{
						if (SubPair.second.is<std::string>())
							Arr.Add(MakeShared<FJsonValueString>(NeoLuaStr::ToFString(SubPair.second.as<std::string>())));
						else if (SubPair.second.is<bool>())
							Arr.Add(MakeShared<FJsonValueBoolean>(SubPair.second.as<bool>()));
						else if (SubPair.second.is<double>())
							Arr.Add(MakeShared<FJsonValueNumber>(SubPair.second.as<double>()));
						else if (SubPair.second.is<int>())
							Arr.Add(MakeShared<FJsonValueNumber>(static_cast<double>(SubPair.second.as<int>())));
						else if (SubPair.second.is<sol::table>())
							Arr.Add(MakeShared<FJsonValueObject>(TableToJson(SubPair.second.as<sol::table>(), Depth + 1)));
					}
					Json->SetArrayField(Key, Arr);
				}
				else
				{
					Json->SetObjectField(Key, TableToJson(Sub, Depth + 1));
				}
			}
		}
		return Json;
	}

	FString JsonToString(const TSharedPtr<FJsonObject>& Json)
	{
		if (!Json.IsValid()) return TEXT("");
		FString Out;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
		return Out;
	}

	FString ExtractGenerationModelId(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid()) return FString();

		FString Model;
		if (Params->TryGetStringField(TEXT("model"), Model) && !Model.IsEmpty()) return Model;
		if (Params->TryGetStringField(TEXT("model_id"), Model) && !Model.IsEmpty()) return Model;
		if (Params->TryGetStringField(TEXT("modelId"), Model) && !Model.IsEmpty()) return Model;
		if (Params->TryGetStringField(TEXT("ai_model"), Model) && !Model.IsEmpty()) return Model;
		if (Params->TryGetStringField(TEXT("model_version"), Model) && !Model.IsEmpty()) return Model;
		return FString();
	}

	FString InferGenerationAssetType(const FString& Action, const FString& Format = FString())
	{
		if (Action.Contains(TEXT("image"), ESearchCase::IgnoreCase)) return TEXT("image");
		if (Action.Contains(TEXT("tts"), ESearchCase::IgnoreCase) || Action.Contains(TEXT("audio"), ESearchCase::IgnoreCase)) return TEXT("audio");
		if (Action.Contains(TEXT("3d"), ESearchCase::IgnoreCase)
			|| Action.Contains(TEXT("model"), ESearchCase::IgnoreCase)
			|| Action.Contains(TEXT("mesh"), ESearchCase::IgnoreCase)
			|| Action.Contains(TEXT("rig"), ESearchCase::IgnoreCase)
			|| Action.Contains(TEXT("texture"), ESearchCase::IgnoreCase))
		{
			return TEXT("model");
		}

		const FString ExtLower = Format.ToLower();
		if (ExtLower == TEXT("png") || ExtLower == TEXT("jpg") || ExtLower == TEXT("jpeg") || ExtLower == TEXT("webp")) return TEXT("image");
		if (ExtLower == TEXT("wav") || ExtLower == TEXT("ogg") || ExtLower == TEXT("mp3")) return TEXT("audio");
		if (ExtLower == TEXT("glb") || ExtLower == TEXT("gltf") || ExtLower == TEXT("fbx") || ExtLower == TEXT("obj")) return TEXT("model");
		return TEXT("generated_asset");
	}

	const TCHAR* GenerationStatusForAnalytics(EGenerativeJobStatus Status)
	{
		switch (Status)
		{
		case EGenerativeJobStatus::Pending: return TEXT("pending");
		case EGenerativeJobStatus::Running: return TEXT("running");
		case EGenerativeJobStatus::Succeeded: return TEXT("succeeded");
		case EGenerativeJobStatus::Failed: return TEXT("failed");
		case EGenerativeJobStatus::Cancelled: return TEXT("cancelled");
		default: return TEXT("unknown");
		}
	}

	void RecordGenerativeAssetOperation(
		const FString& Operation,
		const FString& ProviderId,
		const FString& ModelId,
		const FString& AssetType,
		const FString& Status,
		bool bSuccess,
		const FString& ErrorMessage = FString())
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("operation"), Operation);
		Props->SetStringField(TEXT("tool_kind"), TEXT("generation"));
		Props->SetStringField(TEXT("asset_type"), AssetType.IsEmpty() ? TEXT("generated_asset") : AssetType);
		Props->SetStringField(TEXT("status"), Status);
		Props->SetBoolField(TEXT("success"), bSuccess);
		if (!ProviderId.IsEmpty())
		{
			Props->SetStringField(TEXT("provider"), ProviderId);
		}
		if (!ModelId.IsEmpty())
		{
			Props->SetStringField(TEXT("model"), ModelId);
		}
		if (!ErrorMessage.IsEmpty())
		{
			Props->SetStringField(TEXT("error"), FNSAIAnalytics::SanitizeErrorForAnalytics(ErrorMessage));
		}
		FNSAIAnalytics::Get().RecordEvent(TEXT("asset_operation"), Props);
	}

	// ── Job → sol::table ──
	// Builds a sol::table from an FGenerativeJob. Called on the game thread (no
	// async); used both inside continuations (when the coroutine is mid-resume)
	// and from sync paths.
	sol::table JobToTable(sol::state_view Lua, const FGenerativeJob& Job)
	{
		sol::table T = Lua.create_table();
		if (!Job.ProviderId.IsEmpty()) T["provider"] = TCHAR_TO_UTF8(*Job.ProviderId);
		if (!Job.ActionId.IsEmpty())   T["action"]   = TCHAR_TO_UTF8(*Job.ActionId);
		if (!Job.JobId.IsEmpty())      T["job_id"]   = TCHAR_TO_UTF8(*Job.JobId);

		switch (Job.Status)
		{
		case EGenerativeJobStatus::Pending:   T["status"] = "PENDING"; break;
		case EGenerativeJobStatus::Running:   T["status"] = "IN_PROGRESS"; break;
		case EGenerativeJobStatus::Succeeded: T["status"] = "SUCCEEDED"; break;
		case EGenerativeJobStatus::Failed:    T["status"] = "FAILED"; break;
		case EGenerativeJobStatus::Cancelled: T["status"] = "CANCELLED"; break;
		}

		T["progress"] = Job.Progress;

		if (!Job.ResultUrl.IsEmpty())    T["result_url"]    = TCHAR_TO_UTF8(*Job.ResultUrl);
		if (!Job.ThumbnailUrl.IsEmpty()) T["thumbnail_url"] = TCHAR_TO_UTF8(*Job.ThumbnailUrl);
		if (!Job.ErrorMessage.IsEmpty()) T["error"]         = TCHAR_TO_UTF8(*Job.ErrorMessage);

		if (Job.ExtraUrls.Num() > 0)
		{
			sol::table Extra = Lua.create_table();
			for (const auto& Pair : Job.ExtraUrls)
			{
				Extra[TCHAR_TO_UTF8(*Pair.Key)] = TCHAR_TO_UTF8(*Pair.Value);
			}
			T["extra_urls"] = Extra;
		}

		if (Job.ImageUrls.Num() > 0)
		{
			sol::table Imgs = Lua.create_table();
			for (int32 i = 0; i < Job.ImageUrls.Num(); i++)
			{
				Imgs[i + 1] = TCHAR_TO_UTF8(*Job.ImageUrls[i]);
			}
			T["image_urls"] = Imgs;
		}

		if (Job.RawResponse.IsValid() && Job.RawResponse->HasField(TEXT("balance")))
		{
			T["balance"] = Job.RawResponse->GetIntegerField(TEXT("balance"));
		}

		return T;
	}

	// ── Safe Session access for async lambdas ─────────────────────────
	//
	// Async callbacks (HTTP completions, ticker fires, provider continuations)
	// can outlive the FNeoLuaState that owns the Session — most commonly when
	// the parent tool is cancelled while a generate() call is mid-flight. To
	// avoid use-after-free, we capture TWeakPtr<FNeoLuaState> instead of a raw
	// FLuaSessionData*, pin it in each callback, and skip the operation if the
	// runner is gone.
	void SessionLog(const TWeakPtr<FNeoLuaState>& WeakRunner, const FString& Msg)
	{
		if (TSharedPtr<FNeoLuaState> Runner = WeakRunner.Pin())
		{
			if (FLuaSessionData* S = Runner->GetSession())
			{
				S->Log(Msg);
			}
		}
	}

	void SessionAddImage(const TWeakPtr<FNeoLuaState>& WeakRunner,
		const FString& Base64, const FString& MimeType)
	{
		if (TSharedPtr<FNeoLuaState> Runner = WeakRunner.Pin())
		{
			if (FLuaSessionData* S = Runner->GetSession())
			{
				S->AddImage(Base64, MimeType, 0, 0);
			}
		}
	}

	// Helper: download a thumbnail URL and add to session images via the safe
	// pin-and-access path. Used by multiple action handlers.
	void DownloadThumbnailToSession(const TWeakPtr<FNeoLuaState>& WeakRunner, const FString& ThumbnailUrl,
		TFunction<void()>&& Then)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(ThumbnailUrl);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(30.0f);
		Request->OnProcessRequestComplete().BindLambda(
			[WeakRunner, ThumbnailUrl, Then = MoveTemp(Then)]
			(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bSucceeded) mutable
			{
				UE::NeoStack::RunOnMainThreadDeferred(
					[WeakRunner, ThumbnailUrl, Resp, bSucceeded, Then = MoveTemp(Then)]() mutable
					{
						if (bSucceeded && Resp.IsValid()
							&& Resp->GetResponseCode() >= 200 && Resp->GetResponseCode() < 300)
						{
							const TArray<uint8>& Content = Resp->GetContent();
							const FString Base64 = FBase64::Encode(Content.GetData(), Content.Num());

							FString MimeType = TEXT("image/png");
							if (ThumbnailUrl.Contains(TEXT(".jpg")) || ThumbnailUrl.Contains(TEXT(".jpeg")))
								MimeType = TEXT("image/jpeg");
							else if (ThumbnailUrl.Contains(TEXT(".webp")))
								MimeType = TEXT("image/webp");

							SessionAddImage(WeakRunner, Base64, MimeType);
						}
						Then();
					});
			});
		Request->ProcessRequest();
	}

	// Keep this state type at namespace scope. VS 2022 14.38 ICEs in UE 5.6
	// when constructing an equivalent lambda-local struct through MakeShared.
	struct FGenerateCheckPollState
	{
		TSharedPtr<IGenerativeProvider> Provider;
		FString ProviderId;
		FString JobId;
		FString OrigAction;
		bool bWait = true;
		int32 Timeout = 300;
		int32 PollInterval = 10;
		double StartTime = 0.0;
		TWeakPtr<FNeoLuaState> WeakRunner;
		UE::NeoStack::Lua::FAwaitContinuation Continuation;

		// Self-referencing poll function. Calling it issues one CheckStatus.
		TFunction<void()> Poll;
	};

	// Strip query params and fragment identifiers from a URL extension
	FString CleanExtensionFromUrl(const FString& Url, const FString& FallbackFormat)
	{
		FString Extension = FPaths::GetExtension(Url);
		int32 Pos = INDEX_NONE;
		if (Extension.FindChar(TEXT('?'), Pos))
			Extension = Extension.Left(Pos);
		if (Extension.FindChar(TEXT('#'), Pos))
			Extension = Extension.Left(Pos);
		if (Extension.IsEmpty())
			Extension = FallbackFormat;
		return Extension;
	}

	FColor ParseGeneratedImageColor(const FString& ColorString, const FColor& Fallback)
	{
		FString Clean = ColorString.TrimStartAndEnd();
		if (Clean.IsEmpty())
		{
			return Fallback;
		}
		if (Clean.StartsWith(TEXT("#")))
		{
			Clean.RightChopInline(1);
		}
		if (Clean.Equals(TEXT("green"), ESearchCase::IgnoreCase))
		{
			return FColor::Green;
		}
		if (Clean.Equals(TEXT("blue"), ESearchCase::IgnoreCase))
		{
			return FColor::Blue;
		}
		if (Clean.Equals(TEXT("white"), ESearchCase::IgnoreCase))
		{
			return FColor::White;
		}
		if (Clean.Equals(TEXT("black"), ESearchCase::IgnoreCase))
		{
			return FColor::Black;
		}
		return Clean.Len() == 6 || Clean.Len() == 8 ? FColor::FromHex(Clean) : Fallback;
	}

	FGeneratedTextureImportOptions ParseTextureImportOptions(sol::table Opts)
	{
		FGeneratedTextureImportOptions Options;

		if (sol::optional<sol::table> TextureOpt = Opts.get<sol::optional<sol::table>>("texture"))
		{
			Options.Preset = NeoLuaStr::ToFString(TextureOpt->get_or<std::string>("preset", ""));
		}
		else
		{
			Options.Preset = NeoLuaStr::ToFString(Opts.get_or<std::string>("texture_preset", ""));
		}

		if (sol::optional<sol::table> BackgroundOpt = Opts.get<sol::optional<sol::table>>("background"))
		{
			const FString Mode = NeoLuaStr::ToFString(BackgroundOpt->get_or<std::string>("mode", ""));
			const FString Color = NeoLuaStr::ToFString(BackgroundOpt->get_or<std::string>("color", "#00ff00"));
			Options.Background.bEnableChromaKey =
				Mode.Equals(TEXT("chroma_key"), ESearchCase::IgnoreCase)
				|| Mode.Equals(TEXT("green_screen"), ESearchCase::IgnoreCase)
				|| Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase)
				|| (!Color.IsEmpty() && Mode.IsEmpty());
			Options.Background.ChromaKeyColor = ParseGeneratedImageColor(Color, FColor::Green);
			Options.Background.Tolerance = FMath::Clamp(BackgroundOpt->get_or("tolerance", 40), 0, 441);
			Options.Background.bForcePng = BackgroundOpt->get_or("force_png", true);
			Options.Background.Softness = FMath::Clamp(BackgroundOpt->get_or("softness", 40), 0, 441);
			Options.Background.SpillCleanup = FMath::Clamp(BackgroundOpt->get_or("spill_cleanup", 80), 0, 441);
			Options.Background.DespillEdgePixels = FMath::Clamp(BackgroundOpt->get_or("despill_edge_px", 2), 0, 16);
			Options.Background.bFloodFromEdges = BackgroundOpt->get_or("flood_from_edges", true);
			Options.Background.FloodAlphaThreshold = FMath::Clamp(BackgroundOpt->get_or("flood_alpha_threshold", 64), 0, 255);
			Options.Background.ChokePixels = FMath::Clamp(BackgroundOpt->get_or("choke_px", 0), 0, 8);
		}

		const sol::object RemoveBackgroundObj = Opts.get<sol::object>("remove_background");
		if (RemoveBackgroundObj.is<bool>() && RemoveBackgroundObj.as<bool>())
		{
			Options.Background.bEnableChromaKey = true;
			Options.Background.ChromaKeyColor = FColor::Green;
		}
		else if (RemoveBackgroundObj.is<std::string>())
		{
			const FString RemoveBackground = NeoLuaStr::ToFString(RemoveBackgroundObj.as<std::string>());
			Options.Background.bEnableChromaKey = true;
			Options.Background.ChromaKeyColor = ParseGeneratedImageColor(RemoveBackground, FColor::Green);
		}

		return Options;
	}

	// Build a "FAILED" result table on the coroutine stack and return 1 (one value pushed).
	int PushFailTable(lua_State* L, const FString& ErrorMsg)
	{
		sol::state_view Lua(L);
		sol::table T = Lua.create_table();
		T["status"] = "FAILED";
		T["error"] = TCHAR_TO_UTF8(*ErrorMsg);
		sol::stack::push(L, T);
		return 1;
	}
}

// ── Per-action async handlers ────────────────────────────────────────
//
// Each handler is called by the binding's dispatcher. It either:
//   - returns synchronously (push 1 table, return 1) — for sync actions like discover
//   - calls UE::NeoStack::Lua::AwaitAsync (long-jumps) — for async actions
//
// AwaitAsync's continuation pushes the result table onto the resumed coroutine's
// stack and returns the count.

namespace
{
	int HandleDiscover(lua_State* L, sol::table Opts, FLuaSessionData& Session)
	{
		sol::state_view Lua(L);
		const FString OutputFilter = NeoLuaStr::ToFString(Opts.get_or<std::string>("output", ""));
		auto AllActions = FGenerativeProviderRegistry::Get().GetAllActions(OutputFilter);

		sol::table Result = Lua.create_table();
		Result["status"] = "SUCCEEDED";

		sol::table ActionsList = Lua.create_table();
		int32 Idx = 1;
		for (const auto& Pair : AllActions)
		{
			sol::table Entry = Lua.create_table();
			Entry["provider"] = TCHAR_TO_UTF8(*Pair.Key);
			Entry["action"] = TCHAR_TO_UTF8(*Pair.Value.ActionId);
			Entry["description"] = TCHAR_TO_UTF8(*Pair.Value.Description);
			Entry["synchronous"] = Pair.Value.bIsSynchronous;

			sol::table Inputs = Lua.create_table();
			for (int32 i = 0; i < Pair.Value.InputHints.Num(); i++)
				Inputs[i + 1] = TCHAR_TO_UTF8(*Pair.Value.InputHints[i]);
			Entry["inputs"] = Inputs;

			sol::table Outputs = Lua.create_table();
			for (int32 i = 0; i < Pair.Value.OutputHints.Num(); i++)
				Outputs[i + 1] = TCHAR_TO_UTF8(*Pair.Value.OutputHints[i]);
			Entry["outputs"] = Outputs;

			if (!Pair.Value.CreditCost.IsEmpty())
				Entry["cost"] = TCHAR_TO_UTF8(*Pair.Value.CreditCost);

			if (Pair.Value.ParamsSchema.IsValid())
				Entry["params_schema"] = TCHAR_TO_UTF8(*JsonToString(Pair.Value.ParamsSchema));

			ActionsList[Idx++] = Entry;
		}
		Result["actions"] = ActionsList;

		sol::table ProvidersList = Lua.create_table();
		Idx = 1;
		for (const auto& Prov : FGenerativeProviderRegistry::Get().GetAll())
		{
			sol::table Entry = Lua.create_table();
			Entry["id"] = TCHAR_TO_UTF8(*Prov->GetId());
			Entry["name"] = TCHAR_TO_UTF8(*Prov->GetDisplayName());
			Entry["action_count"] = Prov->GetActions().Num();
			const FString Website = Prov->GetWebsite();
			if (!Website.IsEmpty())
				Entry["website"] = TCHAR_TO_UTF8(*Website);
			ProvidersList[Idx++] = Entry;
		}
		Result["providers"] = ProvidersList;

		Session.Log(FString::Printf(TEXT("[OK] discover -> %d actions across %d providers"),
			AllActions.Num(), FGenerativeProviderRegistry::Get().Num()));

		sol::stack::push(L, Result);
		return 1;
	}

	int HandleBalance(lua_State* L, TSharedPtr<IGenerativeProvider> Provider, const FString& ProviderId, FLuaSessionData& /*Session*/)
	{
		// Capture the FNeoLuaState weakly — the Session pointer derived from this
		// is only valid while the runner is alive; pinning before each access
		// makes us crash-safe if the parent tool is cancelled mid-flight.
		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);
		const FString CapturedProviderId = ProviderId;
		return UE::NeoStack::Lua::AwaitAsync(L,
			[Provider, CapturedProviderId, WeakRunner](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				Provider->GetBalance(
					[CapturedProviderId, WeakRunner, Continuation = MoveTemp(Continuation)]
					(int32 Balance, const FString& Error) mutable
					{
						// Three outcomes per FBalanceCallback contract:
						//   Balance >= 0, Error empty → success
						//   Balance == -1, Error empty → provider doesn't support balance
						//   Balance == -1, Error non-empty → call failed; Error explains
						const bool bSuccess = (Balance >= 0);
						const bool bUnsupported = (Balance < 0 && Error.IsEmpty());
						const FString FinalError = bSuccess ? FString()
							: (bUnsupported
								? FString::Printf(TEXT("Provider '%s' does not support balance checking"), *CapturedProviderId)
								: Error);

						if (bSuccess)
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[OK] generate balance -> %s: %d"),
								*CapturedProviderId, Balance));
						}
						else
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate balance (%s): %s"),
								*CapturedProviderId, *FinalError));
						}

						Continuation([Balance, bSuccess, FinalError](lua_State* CoroL) -> int
						{
							sol::state_view Lua(CoroL);
							sol::table Result = Lua.create_table();
							if (bSuccess)
							{
								Result["status"] = "SUCCEEDED";
								Result["balance"] = Balance;
							}
							else
							{
								Result["status"] = "FAILED";
								Result["error"] = TCHAR_TO_UTF8(*FinalError);
							}
							sol::stack::push(CoroL, Result);
							return 1;
						});
					});
			});
	}

	int HandleCancel(lua_State* L, TSharedPtr<IGenerativeProvider> Provider, sol::table Opts, FLuaSessionData& Session)
	{
		const FString JobId = NeoLuaStr::ToFString(Opts.get_or<std::string>("job_id", ""));
		if (JobId.IsEmpty())
		{
			Session.Log(TEXT("[ERROR] generate cancel: job_id is required"));
			return PushFailTable(L, TEXT("job_id is required for cancel"));
		}

		const FString CapturedJobId = JobId;
		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);
		return UE::NeoStack::Lua::AwaitAsync(L,
			[Provider, CapturedJobId, WeakRunner](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				Provider->CancelJob(CapturedJobId,
					[CapturedJobId, WeakRunner, Continuation = MoveTemp(Continuation)](bool bSuccess) mutable
					{
						if (bSuccess)
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[OK] generate cancel -> %s"), *CapturedJobId));
						}
						else
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate cancel: failed for job %s"), *CapturedJobId));
						}

						Continuation([bSuccess, CapturedJobId](lua_State* CoroL) -> int
						{
							sol::state_view Lua(CoroL);
							sol::table Result = Lua.create_table();
							if (bSuccess)
							{
								Result["status"] = "SUCCEEDED";
								Result["job_id"] = TCHAR_TO_UTF8(*CapturedJobId);
							}
							else
							{
								Result["status"] = "FAILED";
								Result["error"] = "Cancel failed or not supported by this provider";
								Result["job_id"] = TCHAR_TO_UTF8(*CapturedJobId);
							}
							sol::stack::push(CoroL, Result);
							return 1;
						});
					});
			});
	}

	int HandleGetResult(lua_State* L, TSharedPtr<IGenerativeProvider> Provider, const FString& ProviderId, sol::table Opts, FLuaSessionData& Session)
	{
		const FString JobId = NeoLuaStr::ToFString(Opts.get_or<std::string>("job_id", ""));
		if (JobId.IsEmpty())
		{
			Session.Log(TEXT("[ERROR] generate get_result: job_id is required"));
			return PushFailTable(L, TEXT("job_id is required for get_result"));
		}

		const FString OrigAction = NeoLuaStr::ToFString(Opts.get_or<std::string>("original_action", ""));
		const FString CapturedProviderId = ProviderId;
		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);

		// Two-phase: GetResult, then download thumbnail if applicable.
		return UE::NeoStack::Lua::AwaitAsync(L,
			[Provider, CapturedProviderId, JobId, OrigAction, WeakRunner](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				Provider->GetResult(JobId, OrigAction,
					[CapturedProviderId, JobId, WeakRunner, Continuation = MoveTemp(Continuation)](const FGenerativeJob& JobIn) mutable
					{
						FGenerativeJob Job = JobIn;
						Job.ProviderId = CapturedProviderId;

						SessionLog(WeakRunner, FString::Printf(TEXT("[OK] generate get_result -> %s %s"),
							*JobId, Job.IsSuccess() ? TEXT("SUCCEEDED") : TEXT("not ready")));

						auto FinishWithJob = [Job, Continuation = MoveTemp(Continuation)]() mutable
						{
							Continuation([Job](lua_State* CoroL) -> int
							{
								sol::state_view Lua(CoroL);
								sol::table T = JobToTable(Lua, Job);
								sol::stack::push(CoroL, T);
								return 1;
							});
						};

						if (Job.IsSuccess() && !Job.ThumbnailUrl.IsEmpty())
						{
							DownloadThumbnailToSession(WeakRunner, Job.ThumbnailUrl, MoveTemp(FinishWithJob));
						}
						else
						{
							FinishWithJob();
						}
					});
			});
	}

	// `check` action: polls Provider->CheckStatus until the job reaches a terminal
	// state, the wait flag is false, or the timeout elapses. Each poll yields the
	// coroutine so the editor stays interactive throughout.
	//
	// Implementation: stores the poll loop's state on a TSharedRef so a self-
	// referencing `Poll` TFunction can call itself for each iteration. The shared
	// state is kept alive by whichever continuation captures it (HTTP request
	// completion or FTSTicker delegate); when the coroutine resumes (Continuation
	// fires), the chain unwinds.
	int HandleCheck(lua_State* L, TSharedPtr<IGenerativeProvider> Provider, const FString& ProviderId, sol::table Opts, FLuaSessionData& Session)
	{
		const FString JobId = NeoLuaStr::ToFString(Opts.get_or<std::string>("job_id", ""));
		if (JobId.IsEmpty())
		{
			Session.Log(TEXT("[ERROR] generate check: job_id is required"));
			return PushFailTable(L, TEXT("job_id is required for check"));
		}

		const FString OrigAction = NeoLuaStr::ToFString(Opts.get_or<std::string>("original_action", ""));
		const bool bWait = Opts.get_or("wait", true);
		const int32 Timeout = Opts.get_or("timeout", 300);
		const int32 PollInterval = FMath::Clamp(Opts.get_or("poll_interval", 10), 3, 60);
		const FString CapturedProviderId = ProviderId;
		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);
		const double StartTime = FPlatformTime::Seconds();

		return UE::NeoStack::Lua::AwaitAsync(L,
			[Provider, CapturedProviderId, JobId, OrigAction, bWait, Timeout, PollInterval, WeakRunner, StartTime]
			(UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				TSharedRef<FGenerateCheckPollState> State = MakeShared<FGenerateCheckPollState>();
				State->Provider     = Provider;
				State->ProviderId   = CapturedProviderId;
				State->JobId        = JobId;
				State->OrigAction   = OrigAction;
				State->bWait        = bWait;
				State->Timeout      = Timeout;
				State->PollInterval = PollInterval;
				State->StartTime    = StartTime;
				State->WeakRunner   = WeakRunner;
				State->Continuation = MoveTemp(Continuation);

				// Resume the coroutine with the final job result, optionally after
				// downloading the thumbnail to the session image cache.
				auto Resolve = [State](FGenerativeJob Job)
				{
					SessionLog(State->WeakRunner, FString::Printf(TEXT("[OK] generate check -> %s %d%%"),
						*Job.JobId, Job.Progress));

					auto Finish = [State, Job]()
					{
						State->Continuation([Job](lua_State* CoroL) -> int
						{
							sol::state_view Lua(CoroL);
							sol::table T = JobToTable(Lua, Job);
							sol::stack::push(CoroL, T);
							return 1;
						});
					};

					if (Job.IsSuccess() && !Job.ThumbnailUrl.IsEmpty())
					{
						DownloadThumbnailToSession(State->WeakRunner, Job.ThumbnailUrl, MoveTemp(Finish));
					}
					else
					{
						Finish();
					}
				};

				// One poll: CheckStatus, then either resolve or schedule another iteration.
				State->Poll = [State, Resolve = MoveTemp(Resolve)]()
				{
					State->Provider->CheckStatus(State->JobId, State->OrigAction,
						[State, Resolve](const FGenerativeJob& JobIn)
						{
							FGenerativeJob Job = JobIn;
							Job.ProviderId = State->ProviderId;

							const bool bDone = Job.IsTerminal()
								|| !State->bWait
								|| (FPlatformTime::Seconds() - State->StartTime > State->Timeout);

							if (bDone)
							{
								Resolve(Job);
								return;
							}

							// Not done — yield for PollInterval seconds, then iterate.
							FTSTicker::GetCoreTicker().AddTicker(
								FTickerDelegate::CreateLambda(
									[State](float /*DeltaTime*/) -> bool
									{
										State->Poll();  // recurse via stored TFunction
										return false;   // one-shot
									}),
								(float)State->PollInterval);
						});
				};

				// Kick off the first poll.
				State->Poll();
			});
	}

	int HandleImport(lua_State* L, TSharedPtr<IGenerativeProvider> Provider, const FString& ProviderId, sol::table Opts, FLuaSessionData& Session)
	{
		const FString JobId = NeoLuaStr::ToFString(Opts.get_or<std::string>("job_id", ""));
		const FString ResultUrl = NeoLuaStr::ToFString(Opts.get_or<std::string>("result_url", ""));
		FString AssetPath = NeoLuaStr::ToFString(Opts.get_or<std::string>("asset_path", "/Game/Generated"));
		const FString AssetName = NeoLuaStr::ToFString(Opts.get_or<std::string>("asset_name", ""));
		const FString OrigAction = NeoLuaStr::ToFString(Opts.get_or<std::string>("original_action", ""));
		const FString Format = NeoLuaStr::ToFString(Opts.get_or<std::string>("format", "glb"));
		const FGeneratedTextureImportOptions TextureImportOptions = ParseTextureImportOptions(Opts);
		const FString ImportAssetType = InferGenerationAssetType(OrigAction.IsEmpty() ? TEXT("import") : OrigAction, Format);

		FString AssetPathError;
		if (!NormalizeImportDestinationPath(AssetPath, AssetPathError))
		{
			Session.Log(FString::Printf(TEXT("[ERROR] generate import: invalid asset_path '%s': %s"), *AssetPath, *AssetPathError));
			return PushFailTable(L, FString::Printf(TEXT("Invalid asset_path '%s': %s"), *AssetPath, *AssetPathError));
		}

		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);

		// Step 1: get the download URL. Either provided, or fetched via GetResult.
		// Step 2: download the file.
		// Step 3: import it into UE5 (sync — happens in the resume).
		return UE::NeoStack::Lua::AwaitAsync(L,
			[Provider, ProviderId, JobId, ResultUrl, OrigAction, Format, AssetPath, AssetName, TextureImportOptions, WeakRunner, ImportAssetType]
			(UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				auto DoDownload = [Provider, ProviderId, JobId, ResultUrl, OrigAction, Format, AssetPath, AssetName, TextureImportOptions, WeakRunner, ImportAssetType,
					Continuation = MoveTemp(Continuation)]
					(const FString& DownloadUrl) mutable
				{
					if (DownloadUrl.IsEmpty())
					{
						SessionLog(WeakRunner, TEXT("[ERROR] generate import: no download URL"));
						Continuation([](lua_State* CoroL) -> int
						{
							return PushFailTable(CoroL, TEXT("No download URL available. Provide result_url or job_id."));
						});
						return;
					}

					const FString Extension = CleanExtensionFromUrl(DownloadUrl, Format);
					const FString TempDir = FPaths::ProjectSavedDir() / TEXT("Temp");
					const FString TempFile = TempDir / FString::Printf(TEXT("gen_%s.%s"),
						*FGuid::NewGuid().ToString(), *Extension);
					IFileManager::Get().MakeDirectory(*TempDir, true);

					FString LocalSourceFile = DownloadUrl;
					if (DownloadUrl.StartsWith(TEXT("data:")))
					{
						FString DecodeError;
						LocalSourceFile = AssetImportUtils::SaveBase64ToTempFile(DownloadUrl, Extension, DecodeError);
						if (LocalSourceFile.IsEmpty())
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate import: base64 decode failed: %s"), *DecodeError));
							Continuation([DecodeError](lua_State* CoroL) -> int
							{
								return PushFailTable(CoroL, FString::Printf(TEXT("Failed to decode image data: %s"), *DecodeError));
							});
							return;
						}
					}
					if (LocalSourceFile.StartsWith(TEXT("file://")))
					{
						LocalSourceFile.RightChopInline(7);
					}
					if (FPaths::FileExists(LocalSourceFile))
					{
						UE::NeoStack::RunOnMainThreadDeferred(
							[LocalSourceFile, AssetPath, AssetName, Extension, TextureImportOptions, WeakRunner,
							 ProviderId, ImportAssetType, Continuation = MoveTemp(Continuation)]() mutable
							{
								FString FinalName = AssetName;
								if (FinalName.IsEmpty())
								{
									FinalName = FString::Printf(TEXT("Generated_%s"), *FGuid::NewGuid().ToString().Left(8));
								}

								FString ImportedPath;
								FString ImportError;
								const FString ExtLower = Extension.ToLower();

								if (ExtLower == TEXT("glb") || ExtLower == TEXT("gltf") || ExtLower == TEXT("fbx") || ExtLower == TEXT("obj"))
								{
									UStaticMesh* Mesh = AssetImportUtils::ImportStaticMesh(LocalSourceFile, AssetPath, FinalName, ImportError);
									ImportedPath = Mesh ? Mesh->GetPathName() : FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *LocalSourceFile);
								}
								else if (ExtLower == TEXT("png") || ExtLower == TEXT("jpg") || ExtLower == TEXT("jpeg") || ExtLower == TEXT("webp"))
								{
									UTexture2D* Tex = AssetImportUtils::ImportTexture(LocalSourceFile, AssetPath, FinalName, TextureImportOptions, ImportError);
									ImportedPath = Tex ? Tex->GetPathName() : FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *LocalSourceFile);
								}
								else if (ExtLower == TEXT("wav") || ExtLower == TEXT("ogg"))
								{
									USoundWave* Sound = AssetImportUtils::ImportAudio(LocalSourceFile, AssetPath, FinalName, ImportError);
									ImportedPath = Sound ? Sound->GetPathName() : FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *LocalSourceFile);
								}
								else
								{
									ImportedPath = FString::Printf(TEXT("Unknown format '%s'. File saved to: %s"), *Extension, *LocalSourceFile);
								}

								const bool bImportOk = ImportedPath.StartsWith(TEXT("/")) && !ImportedPath.Contains(TEXT("ERROR"));
								RecordGenerativeAssetOperation(TEXT("import"), ProviderId, FString(), ImportAssetType, bImportOk ? TEXT("succeeded") : TEXT("failed"), bImportOk, ImportError);
								SessionLog(WeakRunner, FString::Printf(TEXT("[%s] generate import -> %s"),
									bImportOk ? TEXT("OK") : TEXT("ERROR"), *ImportedPath));

								Continuation([bImportOk, ImportedPath, ImportError](lua_State* CoroL) -> int
								{
									sol::state_view Lua(CoroL);
									sol::table Result = Lua.create_table();
									Result["status"] = bImportOk ? "SUCCEEDED" : "FAILED";
									Result["asset_path"] = TCHAR_TO_UTF8(*ImportedPath);
									if (!bImportOk && !ImportError.IsEmpty())
										Result["error"] = TCHAR_TO_UTF8(*ImportError);
									sol::stack::push(CoroL, Result);
									return 1;
								});
							});
						return;
					}

					// Async download via OnProcessRequestComplete
					TSharedRef<IHttpRequest, ESPMode::ThreadSafe> DlReq = FHttpModule::Get().CreateRequest();
					DlReq->SetURL(DownloadUrl);
					DlReq->SetVerb(TEXT("GET"));
					DlReq->SetTimeout(300.0f);

					DlReq->OnProcessRequestComplete().BindLambda(
						[TempFile, AssetPath, AssetName, Extension, TextureImportOptions, WeakRunner, ProviderId, ImportAssetType, Continuation = MoveTemp(Continuation)]
						(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bSucceeded) mutable
						{
							UE::NeoStack::RunOnMainThreadDeferred(
								[TempFile, AssetPath, AssetName, Extension, TextureImportOptions, WeakRunner, Resp, bSucceeded, ProviderId, ImportAssetType,
								 Continuation = MoveTemp(Continuation)]() mutable
								{
									if (!bSucceeded || !Resp.IsValid()
										|| Resp->GetResponseCode() < 200 || Resp->GetResponseCode() >= 300)
									{
										const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
										SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate import: download failed (HTTP %d)"), Code));
										Continuation([Code](lua_State* CoroL) -> int
										{
											return PushFailTable(CoroL, FString::Printf(TEXT("Failed to download result file (HTTP %d)"), Code));
										});
										return;
									}

									if (!FFileHelper::SaveArrayToFile(Resp->GetContent(), *TempFile))
									{
										SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate import: save failed: %s"), *TempFile));
										Continuation([TempFile](lua_State* CoroL) -> int
										{
											return PushFailTable(CoroL, FString::Printf(TEXT("Failed to save downloaded file to %s"), *TempFile));
										});
										return;
									}

									// Import based on file extension. This is sync (UE asset import API has no
									// async surface for these). It's bounded — typically <1s per asset — so
									// the brief freeze is acceptable.
									FString FinalName = AssetName;
									if (FinalName.IsEmpty())
									{
										FinalName = FString::Printf(TEXT("Generated_%s"), *FGuid::NewGuid().ToString().Left(8));
									}

									FString ImportedPath;
									FString ImportError;
									const FString ExtLower = Extension.ToLower();

									if (ExtLower == TEXT("glb") || ExtLower == TEXT("gltf") || ExtLower == TEXT("fbx") || ExtLower == TEXT("obj"))
									{
										UStaticMesh* Mesh = AssetImportUtils::ImportStaticMesh(TempFile, AssetPath, FinalName, ImportError);
										if (Mesh)
											ImportedPath = Mesh->GetPathName();
										else
											ImportedPath = FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *TempFile);
									}
									else if (ExtLower == TEXT("png") || ExtLower == TEXT("jpg") || ExtLower == TEXT("jpeg") || ExtLower == TEXT("webp"))
									{
										UTexture2D* Tex = AssetImportUtils::ImportTexture(TempFile, AssetPath, FinalName, TextureImportOptions, ImportError);
										if (Tex)
											ImportedPath = Tex->GetPathName();
										else
											ImportedPath = FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *TempFile);
									}
									else if (ExtLower == TEXT("wav") || ExtLower == TEXT("ogg"))
									{
										USoundWave* Sound = AssetImportUtils::ImportAudio(TempFile, AssetPath, FinalName, ImportError);
										if (Sound)
											ImportedPath = Sound->GetPathName();
										else
											ImportedPath = FString::Printf(TEXT("ERROR: %s. File saved to: %s"), *ImportError, *TempFile);
									}
									else if (ExtLower == TEXT("mp3"))
									{
										ImportedPath = FString::Printf(TEXT("MP3 format not directly supported by UE5. Convert to WAV first. File saved to: %s"), *TempFile);
									}
									else
									{
										ImportedPath = FString::Printf(TEXT("Unknown format '%s'. File saved to: %s"), *Extension, *TempFile);
									}

									// Cleanup temp file only if import succeeded
									if (ImportedPath.StartsWith(TEXT("/")) && !ImportedPath.StartsWith(TEXT("ERROR")) && !ImportedPath.StartsWith(TEXT("MP3")) && !ImportedPath.StartsWith(TEXT("Unknown")))
									{
										IFileManager::Get().Delete(*TempFile);
									}

									const bool bImportOk = ImportedPath.StartsWith(TEXT("/")) && !ImportedPath.Contains(TEXT("ERROR"));
									RecordGenerativeAssetOperation(TEXT("import"), ProviderId, FString(), ImportAssetType, bImportOk ? TEXT("succeeded") : TEXT("failed"), bImportOk, ImportError);
									SessionLog(WeakRunner, FString::Printf(TEXT("[%s] generate import -> %s"),
										bImportOk ? TEXT("OK") : TEXT("ERROR"), *ImportedPath));

									Continuation([bImportOk, ImportedPath, ImportError](lua_State* CoroL) -> int
									{
										sol::state_view Lua(CoroL);
										sol::table Result = Lua.create_table();
										Result["status"] = bImportOk ? "SUCCEEDED" : "FAILED";
										Result["asset_path"] = TCHAR_TO_UTF8(*ImportedPath);
										if (!bImportOk && !ImportError.IsEmpty())
											Result["error"] = TCHAR_TO_UTF8(*ImportError);
										sol::stack::push(CoroL, Result);
										return 1;
									});
								});
						});
					DlReq->ProcessRequest();
				};

				if (!ResultUrl.IsEmpty())
				{
					// Direct URL — no GetResult needed.
					DoDownload(ResultUrl);
				}
				else if (!JobId.IsEmpty())
				{
					Provider->GetResult(JobId, OrigAction,
						[Format, DoDownload = MoveTemp(DoDownload), WeakRunner](const FGenerativeJob& Job) mutable
						{
							if (!Job.IsSuccess())
							{
								SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate import: job not ready: %s"), *Job.ErrorMessage));
								DoDownload(FString());  // empty triggers fail path
								return;
							}
							// Try requested format first, then primary URL
							FString DownloadUrl;
							if (Job.ExtraUrls.Contains(Format))
								DownloadUrl = Job.ExtraUrls[Format];
							else
								DownloadUrl = Job.ResultUrl;
							DoDownload(DownloadUrl);
						});
				}
				else
				{
					DoDownload(FString());
				}
			});
	}

	int HandleSubmit(lua_State* L, TSharedPtr<IGenerativeProvider> Provider, const FString& ProviderId, const FString& Action, sol::table Opts, FLuaSessionData& /*Session*/)
	{
		// Convert remaining opts to JSON params (skip provider/action meta-keys)
		auto Params = TableToJson(Opts);
		Params->RemoveField(TEXT("provider"));
		Params->RemoveField(TEXT("action"));

		const FString CapturedProviderId = ProviderId;
		const FString CapturedAction = Action;
		const FString CapturedModelId = ExtractGenerationModelId(Params);
		const FString CapturedAssetType = InferGenerationAssetType(Action);
		TWeakPtr<FNeoLuaState> WeakRunner = FNeoLuaState::FindForState(L);

		return UE::NeoStack::Lua::AwaitAsync(L,
			[Provider, CapturedProviderId, CapturedAction, CapturedModelId, CapturedAssetType, Params, WeakRunner](UE::NeoStack::Lua::FAwaitContinuation Continuation)
			{
				Provider->Submit(CapturedAction, Params,
					[CapturedProviderId, CapturedAction, CapturedModelId, CapturedAssetType, WeakRunner, Continuation = MoveTemp(Continuation)]
					(const FGenerativeJob& JobIn) mutable
					{
						FGenerativeJob Job = JobIn;
						Job.ProviderId = CapturedProviderId;
						Job.ActionId = CapturedAction;
						RecordGenerativeAssetOperation(
							CapturedAction,
							CapturedProviderId,
							CapturedModelId,
							CapturedAssetType,
							GenerationStatusForAnalytics(Job.Status),
							Job.IsSuccess(),
							Job.ErrorMessage);

						if (Job.IsSuccess())
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[OK] generate %s/%s -> SUCCEEDED"),
								*CapturedProviderId, *CapturedAction));
						}
						else if (Job.Status == EGenerativeJobStatus::Failed)
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[ERROR] generate %s/%s -> %s"),
								*CapturedProviderId, *CapturedAction, *Job.ErrorMessage));
						}
						else
						{
							SessionLog(WeakRunner, FString::Printf(TEXT("[OK] generate %s/%s -> job_id=%s status=%s"),
								*CapturedProviderId, *CapturedAction, *Job.JobId,
								Job.Status == EGenerativeJobStatus::Pending ? TEXT("PENDING") : TEXT("IN_PROGRESS")));
						}

						auto Finish = [Job, Continuation = MoveTemp(Continuation)]() mutable
						{
							Continuation([Job](lua_State* CoroL) -> int
							{
								sol::state_view Lua(CoroL);
								sol::table T = JobToTable(Lua, Job);
								sol::stack::push(CoroL, T);
								return 1;
							});
						};

						if (Job.IsSuccess() && !Job.ThumbnailUrl.IsEmpty())
						{
							DownloadThumbnailToSession(WeakRunner, Job.ThumbnailUrl, MoveTemp(Finish));
						}
						else
						{
							Finish();
						}
					});
			});
	}
}

// ── Binding ──────────────────────────────────────────────────────────
//
// Registered as a raw Lua C function (via lua_pushcclosure + upvalue) rather
// than sol2's set_function wrapping. Reasons:
//   1. Sol2's wrapping pushes a copy of the lambda's return value onto the Lua
//      stack — but we manually push our result table via sol::stack::push
//      inside the action handlers, so sol2's auto-push would create a stray
//      duplicate value.
//   2. Sol2's wrapping creates C++ objects (sol::table, etc.) on the C stack
//      whose destructors would be skipped if AwaitAsync long-jumps via lua_yield
//      from a deeper frame.
//
// The raw C function approach gives us direct control over both the return-count
// (we return the int we push'd ourselves) and the stack frame (no surprise C++
// destructors at yield time).

namespace
{
	// Raw Lua C function for `generate`. Session pointer carried via upvalue 1.
	int GenerateLuaCFunc(lua_State* L)
	{
		FLuaSessionData* SessionPtr = static_cast<FLuaSessionData*>(
			lua_touserdata(L, lua_upvalueindex(1)));
		check(SessionPtr);
		FLuaSessionData& Sess = *SessionPtr;

		// Args: stack position 1 should be the opts table.
		if (lua_gettop(L) < 1 || !lua_istable(L, 1))
		{
			Sess.Log(TEXT("[ERROR] generate: expected table argument"));
			return PushFailTable(L, TEXT("generate(opts): opts must be a table"));
		}

		sol::table Opts(L, 1);

		const FString Provider = NeoLuaStr::ToFString(Opts.get_or<std::string>("provider", ""));
		const FString Action   = NeoLuaStr::ToFString(Opts.get_or<std::string>("action", ""));

		// discover — no provider needed, sync only
		if (Action.Equals(TEXT("discover"), ESearchCase::IgnoreCase))
		{
			return HandleDiscover(L, Opts, Sess);
		}

		// Resolve provider
		if (Provider.IsEmpty())
		{
			Sess.Log(TEXT("[ERROR] generate: 'provider' is required"));
			return PushFailTable(L, TEXT("provider is required"));
		}

		TSharedPtr<IGenerativeProvider> ProviderPtr = FGenerativeProviderRegistry::Get().Find(Provider);
		if (!ProviderPtr.IsValid())
		{
			Sess.Log(FString::Printf(TEXT("[ERROR] generate: unknown provider '%s'"), *Provider));
			return PushFailTable(L, FString::Printf(TEXT("Unknown provider: %s"), *Provider));
		}

		// Action dispatch — each handler either pushes 1 result and returns 1,
		// or calls AwaitAsync which long-jumps via lua_yield (this function never
		// returns from those branches; the coroutine's continuation pushes the
		// final result on resume).
		if (Action.Equals(TEXT("balance"), ESearchCase::IgnoreCase))
			return HandleBalance(L, ProviderPtr, Provider, Sess);

		if (Action.Equals(TEXT("cancel"), ESearchCase::IgnoreCase))
			return HandleCancel(L, ProviderPtr, Opts, Sess);

		if (Action.Equals(TEXT("get_result"), ESearchCase::IgnoreCase))
			return HandleGetResult(L, ProviderPtr, Provider, Opts, Sess);

		if (Action.Equals(TEXT("check"), ESearchCase::IgnoreCase))
			return HandleCheck(L, ProviderPtr, Provider, Opts, Sess);

		if (Action.Equals(TEXT("import"), ESearchCase::IgnoreCase))
			return HandleImport(L, ProviderPtr, Provider, Opts, Sess);

		if (Action.IsEmpty())
		{
			Sess.Log(TEXT("[ERROR] generate: 'action' is required"));
			return PushFailTable(L, TEXT("action is required"));
		}

		if (!ProviderPtr->SupportsAction(Action))
		{
			Sess.Log(FString::Printf(TEXT("[ERROR] generate: provider '%s' does not support action '%s'"),
				*Provider, *Action));
			return PushFailTable(L, FString::Printf(TEXT("Provider '%s' does not support action '%s'"),
				*Provider, *Action));
		}

		// Default: submit a new job
		return HandleSubmit(L, ProviderPtr, Provider, Action, Opts, Sess);
	}
}

REGISTER_LUA_BINDING(Generate, GenerateDocs,
[](sol::state& Lua, FLuaSessionData& Session)
{
	lua_State* L = Lua.lua_state();

	// Register `generate` as a C closure with the session pointer as upvalue 1.
	// (Lua doesn't move the binding's heap address as long as Session stays alive,
	// which is guaranteed by FNeoLuaState owning Session for the script's lifetime.)
	lua_pushlightuserdata(L, &Session);
	lua_pushcclosure(L, &GenerateLuaCFunc, /*nupvalues*/ 1);
	lua_setglobal(L, "generate");
});
