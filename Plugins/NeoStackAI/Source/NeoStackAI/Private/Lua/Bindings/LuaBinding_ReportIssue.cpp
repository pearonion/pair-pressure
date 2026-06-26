// Copyright 2025-2026 Betide Studio. All Rights Reserved.
//
// report_issue{} — lets agents flag shortcomings (missing bindings, wrong
// results, ambiguous docs, crashes) back to neostack.dev so we can see what
// the agent struggled with. All taxonomy is agent-supplied: agents pick the
// category/severity/tags themselves.
//
// Auth is OPTIONAL — the whole point of report_issue() is to capture
// feedback when something is broken, so we never block on a missing API key.
// If the user has a neostack_* key configured (same key FNeoStackCloudProvider
// uses for chat), we attach it so the report shows up under their account.
// Otherwise it lands as anonymous on the server.
//
// Opt-out is local: if UACPSettings::bDisableAgentIssueReports is true, this
// short-circuits before any HTTP call and returns { accepted=false,
// reason="disabled_locally" } so the agent sees a clean signal and we don't
// leak anything to the network. The toggle lives in the WebUI Settings panel.

#include "Lua/LuaBindingRegistry.h"
#include <sol/sol.hpp>
#include "Lua/LuaStr.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IPluginManager.h"

// ─── Documentation ───

static TArray<FLuaFunctionDoc> ReportIssueDocs = {
	{
		TEXT("report_issue{summary, category?, severity?, tags?, details?}"),
		TEXT("REQUIRED when you cannot accomplish the user's task — missing binding, function returned wrong result, no API exists for the asset type you need, or behavior contradicts the docs. Call this BEFORE telling the user you can't do something so we can fix the gap. DO NOT use for slowness, tedium, retries, or 'this took N iterations' — only concrete capability gaps and incorrect behavior. summary is one line; category/severity/tags are free-form (you tag); details accepts any table for repro/expected/actual."),
		TEXT("{accepted, id?, reason?, error?} — reason=\"disabled_locally\" if user disabled reporting")
	},
};

// ─── Helpers ───

static FString ResolveReportIssueBaseUrl()
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		const FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
		if (!Override.IsEmpty())
		{
			// The chat-provider base URL ends with /v1; strip it so we end up
			// at the API root.
			FString Root = Override;
			Root.RemoveFromEnd(TEXT("/"));
			Root.RemoveFromEnd(TEXT("/v1"));
			Root.RemoveFromEnd(TEXT("/"));
			return Root;
		}
	}
	return TEXT("https://neostack.dev");
}

static FString ResolveReportIssueApiKey()
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->GetChatProviderApiKey(TEXT("neostack"));
	}
	return FString();
}

static FString ResolvePluginVersion()
{
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI")))
	{
		return Plugin->GetDescriptor().VersionName;
	}
	return FString();
}

// Recursive sol::object → TSharedPtr<FJsonValue>. Mirrors what the agent
// passes in `details` — strings, numbers, bools, arrays, objects.
static TSharedPtr<FJsonValue> SolToJson(const sol::object& Obj)
{
	if (!Obj.valid() || Obj.is<sol::lua_nil_t>())
	{
		return MakeShared<FJsonValueNull>();
	}
	if (Obj.is<bool>())
	{
		return MakeShared<FJsonValueBoolean>(Obj.as<bool>());
	}
	if (Obj.is<double>())
	{
		return MakeShared<FJsonValueNumber>(Obj.as<double>());
	}
	if (Obj.is<int>())
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(Obj.as<int>()));
	}
	if (Obj.is<std::string>())
	{
		return MakeShared<FJsonValueString>(NeoLuaStr::ToFString(Obj.as<std::string>()));
	}
	if (Obj.is<sol::table>())
	{
		sol::table T = Obj.as<sol::table>();

		// Detect array vs object: if every key is a positive integer 1..N,
		// it's a Lua sequence → JSON array.
		bool bIsArray = true;
		int32 MaxIdx = 0;
		int32 Count = 0;
		for (const auto& Pair : T)
		{
			++Count;
			if (Pair.first.is<int>())
			{
				const int32 Key = Pair.first.as<int>();
				if (Key < 1) { bIsArray = false; break; }
				MaxIdx = FMath::Max(MaxIdx, Key);
			}
			else
			{
				bIsArray = false;
				break;
			}
		}
		if (bIsArray && Count > 0 && MaxIdx == Count)
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Reserve(Count);
			for (int32 i = 1; i <= MaxIdx; ++i)
			{
				sol::object Item = T[i];
				Arr.Add(SolToJson(Item));
			}
			return MakeShared<FJsonValueArray>(Arr);
		}

		TSharedPtr<FJsonObject> ObjOut = MakeShared<FJsonObject>();
		for (const auto& Pair : T)
		{
			FString Key;
			if (Pair.first.is<std::string>())
			{
				Key = NeoLuaStr::ToFString(Pair.first.as<std::string>());
			}
			else if (Pair.first.is<int>())
			{
				Key = FString::FromInt(Pair.first.as<int>());
			}
			else
			{
				continue;
			}
			ObjOut->SetField(Key, SolToJson(Pair.second));
		}
		return MakeShared<FJsonValueObject>(ObjOut);
	}
	return MakeShared<FJsonValueNull>();
}

// ─── Binding ───

static void BindReportIssue(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("report_issue",
		[&Session](sol::table Opts, sol::this_state S) -> sol::object
	{
		sol::state_view L(S);
		sol::table Result = L.create_table();

		// ---- Validate summary ----
		const FString Summary = NeoLuaStr::ToFString(Opts.get_or<std::string>("summary", ""));
		if (Summary.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] report_issue -> 'summary' is required"));
			Result["accepted"] = false;
			Result["error"] = "summary is required";
			return Result;
		}

		// ---- Local opt-out (toggle in WebUI Settings panel) ----
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			if (Settings->bDisableAgentIssueReports)
			{
				Session.Log(TEXT("[OK] report_issue -> skipped (disabled in settings)"));
				Result["accepted"] = false;
				Result["reason"] = "disabled_locally";
				return Result;
			}
		}

		// ---- Optional auth ----
		// API key is optional. Server accepts anonymous reports.
		const FString ApiKey = ResolveReportIssueApiKey();

		// ---- Build JSON body ----
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("summary"), Summary);

		const FString Category = NeoLuaStr::ToFString(Opts.get_or<std::string>("category", ""));
		if (!Category.IsEmpty()) Body->SetStringField(TEXT("category"), Category);

		const FString Severity = NeoLuaStr::ToFString(Opts.get_or<std::string>("severity", ""));
		if (!Severity.IsEmpty()) Body->SetStringField(TEXT("severity"), Severity);

		// tags + details: pass through as-is (agents are smart, let them tag)
		sol::object TagsObj = Opts["tags"];
		if (TagsObj.valid() && !TagsObj.is<sol::lua_nil_t>())
		{
			Body->SetField(TEXT("tags"), SolToJson(TagsObj));
		}
		sol::object DetailsObj = Opts["details"];
		if (DetailsObj.valid() && !DetailsObj.is<sol::lua_nil_t>())
		{
			Body->SetField(TEXT("details"), SolToJson(DetailsObj));
		}

		// Auto-attached environment context — useful for correlating reports
		// against builds without forcing the agent to know about it.
		Body->SetStringField(TEXT("pluginVersion"), ResolvePluginVersion());
		Body->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
		Body->SetStringField(TEXT("os"), FString(FPlatformProperties::PlatformName()));

		FString BodyStr;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, Writer);

		// ---- Fire HTTP ----
		const FString Url = ResolveReportIssueBaseUrl() + TEXT("/api/agent-reports");
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		if (!ApiKey.IsEmpty())
		{
			// Attach the key only when one is configured — server treats the
			// request as anonymous otherwise.
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		}
		Request->SetContentAsString(BodyStr);
		Request->SetTimeout(10.0f);
		Request->ProcessRequestUntilComplete();

		// ---- Interpret response ----
		const EHttpRequestStatus::Type Status = Request->GetStatus();
		if (Status != EHttpRequestStatus::Succeeded)
		{
			const FString FailureReason = LexToString(Request->GetFailureReason());
			Session.Log(FString::Printf(TEXT("[FAIL] report_issue -> request did not complete (status=%d, reason=%s)"),
				static_cast<int32>(Status), *FailureReason));
			Result["accepted"] = false;
			Result["error"] = NeoLuaStr::ToStdString(FString::Printf(TEXT("request failed (%s)"), *FailureReason));
			return Result;
		}

		const FHttpResponsePtr Response = Request->GetResponse();
		const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
		const FString ResponseBody = Response.IsValid() ? Response->GetContentAsString() : FString();

		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
		FJsonSerializer::Deserialize(Reader, Parsed);

		if (Code >= 200 && Code < 300 && Parsed.IsValid())
		{
			const bool bAccepted = Parsed->GetBoolField(TEXT("accepted"));
			Result["accepted"] = bAccepted;

			FString Id;
			if (Parsed->TryGetStringField(TEXT("id"), Id))
			{
				Result["id"] = NeoLuaStr::ToStdString(Id);
			}
			FString Reason;
			if (Parsed->TryGetStringField(TEXT("reason"), Reason))
			{
				Result["reason"] = NeoLuaStr::ToStdString(Reason);
			}

			if (bAccepted)
			{
				Session.Log(FString::Printf(TEXT("[OK] report_issue -> %s"), *Id));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[OK] report_issue -> not accepted (%s)"), *Reason));
			}
			return Result;
		}

		// Error path — try to surface the server's error message
		FString ErrorMessage;
		if (Parsed.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
			if (Parsed->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && (*ErrorObj).IsValid())
			{
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMessage);
			}
		}
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = FString::Printf(TEXT("HTTP %d"), Code);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] report_issue -> %s"), *ErrorMessage));
		Result["accepted"] = false;
		Result["error"] = NeoLuaStr::ToStdString(ErrorMessage);
		return Result;
	});
}

REGISTER_LUA_BINDING(ReportIssue, ReportIssueDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindReportIssue(Lua, Session);
});
