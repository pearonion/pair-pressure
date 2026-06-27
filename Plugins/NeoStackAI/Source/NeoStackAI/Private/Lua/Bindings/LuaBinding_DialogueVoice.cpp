// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"

#include "Sound/DialogueVoice.h"
#include "Sound/DialogueTypes.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* GenderToString(EGrammaticalGender::Type Gender)
{
	switch (Gender)
	{
	case EGrammaticalGender::Neuter:    return "neuter";
	case EGrammaticalGender::Masculine: return "masculine";
	case EGrammaticalGender::Feminine:  return "feminine";
	case EGrammaticalGender::Mixed:     return "mixed";
	default:                            return "neuter";
	}
}

static const char* PluralityToString(EGrammaticalNumber::Type Plurality)
{
	switch (Plurality)
	{
	case EGrammaticalNumber::Singular: return "singular";
	case EGrammaticalNumber::Plural:   return "plural";
	default:                           return "singular";
	}
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> DialogueVoiceDocs = {};

static void BindDialogueVoice(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_dialogue_voice", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UDialogueVoice* Voice = NeoLuaAsset::Resolve<UDialogueVoice>(FPath);
		if (!Voice) return;

		AssetObj["_help_text"] =
			"DialogueVoice enrichment:\n"
			"\n"
			"info() -> {name, gender, plurality, description, localization_guid}\n"
			"\n"
			"Property editing uses the generic asset reflection API:\n"
			"  get(\"PropertyName\")\n"
			"  set(\"PropertyName\", \"Value\")\n"
			"  list_properties(filter?, all?)\n"
			"\n"
			"Use raw engine property names, e.g.:\n"
			"  Gender\n"
			"  Plurality\n";

		// ---- info() ----
		AssetObj.set_function("info", [Voice, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Voice))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Voice->GetName());
			Result["gender"] = GenderToString(Voice->Gender);
			Result["plurality"] = PluralityToString(Voice->Plurality);
			Result["description"] = TCHAR_TO_UTF8(*Voice->GetDesc());
			Result["localization_guid"] = TCHAR_TO_UTF8(*Voice->LocalizationGUID.ToString());

			Session.Log(TEXT("[OK] info() -> DialogueVoice summary"));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(DialogueVoice, DialogueVoiceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindDialogueVoice(Lua, Session);
});
