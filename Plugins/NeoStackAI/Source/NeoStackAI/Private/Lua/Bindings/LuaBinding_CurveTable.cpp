// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaCurveHelper.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Engine/CurveTable.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "CurveTableEditorUtils.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================
// Curve-key parsing + enum helpers live in Public/Lua/LuaCurveHelper.h.
// Thin aliases for the local call sites that previously used CT_*-prefixed
// variants; they preserve the original naming while centralizing the logic.

static FString CT_InterpModeToString(ERichCurveInterpMode M)         { return NeoLuaCurve::InterpModeToString(M); }
static ERichCurveInterpMode CT_StringToInterpMode(const FString& S)  { return NeoLuaCurve::ParseInterpMode(S); }
static FString CT_TangentModeToString(ERichCurveTangentMode M)       { return NeoLuaCurve::TangentModeToString(M); }
static FString CT_TangentWeightModeToString(ERichCurveTangentWeightMode M) { return NeoLuaCurve::TangentWeightModeToString(M); }
static ERichCurveTangentWeightMode CT_StringToTangentWeightMode(const FString& S) { return NeoLuaCurve::ParseTangentWeightMode(S); }
static FString CT_ExtrapToString(ERichCurveExtrapolation E)          { return NeoLuaCurve::ExtrapolationToString(E); }
static ERichCurveExtrapolation CT_StringToExtrap(const FString& S)   { return NeoLuaCurve::ParseExtrapolation(S, RCCE_None); }

static FString ModeToString(ECurveTableMode M)
{
	switch (M)
	{
		case ECurveTableMode::RichCurves:   return TEXT("RichCurves");
		case ECurveTableMode::SimpleCurves: return TEXT("SimpleCurves");
		default:                            return TEXT("Empty");
	}
}

static FString LogicalModeToString(const UCurveTable* CT)
{
	if (!CT || CT->GetRowMap().Num() == 0)
	{
		return TEXT("Empty");
	}
	return ModeToString(CT->GetCurveTableMode());
}

static bool ParseCurveTableMode(const FString& S, ECurveTableMode& OutMode)
{
	if (S.Equals(TEXT("Rich"), ESearchCase::IgnoreCase) ||
		S.Equals(TEXT("RichCurve"), ESearchCase::IgnoreCase) ||
		S.Equals(TEXT("RichCurves"), ESearchCase::IgnoreCase))
	{
		OutMode = ECurveTableMode::RichCurves;
		return true;
	}
	if (S.Equals(TEXT("Simple"), ESearchCase::IgnoreCase) ||
		S.Equals(TEXT("SimpleCurve"), ESearchCase::IgnoreCase) ||
		S.Equals(TEXT("SimpleCurves"), ESearchCase::IgnoreCase))
	{
		OutMode = ECurveTableMode::SimpleCurves;
		return true;
	}
	if (S.Equals(TEXT("Empty"), ESearchCase::IgnoreCase))
	{
		OutMode = ECurveTableMode::Empty;
		return true;
	}
	return false;
}

static bool IsFactoryDefaultCurveRow(const UCurveTable* CT)
{
	if (!CT || CT->GetRowMap().Num() != 1)
	{
		return false;
	}

	FRealCurve* const* Curve = CT->GetRowMap().Find(FName(TEXT("Curve")));
	return Curve && *Curve && (*Curve)->GetNumKeys() == 0;
}

static bool HasRichOnlyKeyFields(const sol::table& K)
{
	static const char* RichOnlyFields[] = {
		"arrive_tangent", "leave_tangent", "tangent_mode", "tangent",
		"tangent_weight_mode", "weight_mode", "arrive_tangent_weight", "leave_tangent_weight"
	};
	for (const char* Field : RichOnlyFields)
	{
		sol::object Value = K[Field];
		if (Value.valid() && !Value.is<sol::lua_nil_t>())
		{
			return true;
		}
	}

	sol::object InterpObj = K["interp_mode"];
	if (!InterpObj.valid() || InterpObj.is<sol::lua_nil_t>())
	{
		InterpObj = K["interp"];
	}
	if (InterpObj.valid() && InterpObj.is<std::string>())
	{
		return CT_StringToInterpMode(NeoLuaStr::ToFString(InterpObj.as<std::string>())) == RCIM_Cubic;
	}
	return false;
}

static bool ParamsHaveRichOnlyKeyFields(const sol::table& P)
{
	if (HasRichOnlyKeyFields(P))
	{
		return true;
	}

	sol::object KeysObj = P["keys"];
	if (!KeysObj.valid() || !KeysObj.is<sol::table>())
	{
		return false;
	}

	for (auto& KV : KeysObj.as<sol::table>())
	{
		if (KV.second.is<sol::table>() && HasRichOnlyKeyFields(KV.second.as<sol::table>()))
		{
			return true;
		}
	}
	return false;
}

static bool RichCurveCanConvertToSimple(const FRichCurve* Curve)
{
	if (!Curve)
	{
		return true;
	}

	ERichCurveInterpMode CommonInterp = RCIM_None;
	for (const FRichCurveKey& Key : Curve->GetConstRefOfKeys())
	{
		if (Key.InterpMode == RCIM_Cubic ||
			Key.TangentWeightMode != RCTWM_WeightedNone ||
			!FMath::IsNearlyZero(Key.ArriveTangent) ||
			!FMath::IsNearlyZero(Key.LeaveTangent) ||
			Key.TangentMode == RCTM_User ||
			Key.TangentMode == RCTM_Break)
		{
			return false;
		}

		if (CommonInterp == RCIM_None)
		{
			CommonInterp = Key.InterpMode;
		}
		else if (CommonInterp != Key.InterpMode)
		{
			return false;
		}
	}
	return true;
}

static ERichCurveInterpMode GetCommonSimpleInterp(const FRichCurve* Curve, ERichCurveInterpMode Fallback)
{
	if (!Curve)
	{
		return Fallback == RCIM_Cubic ? RCIM_Linear : Fallback;
	}

	ERichCurveInterpMode CommonInterp = RCIM_None;
	for (const FRichCurveKey& Key : Curve->GetConstRefOfKeys())
	{
		if (Key.InterpMode == RCIM_Cubic)
		{
			return Fallback == RCIM_Cubic ? RCIM_Linear : Fallback;
		}
		if (CommonInterp == RCIM_None)
		{
			CommonInterp = Key.InterpMode;
		}
		else if (CommonInterp != Key.InterpMode)
		{
			return Fallback == RCIM_Cubic ? RCIM_Linear : Fallback;
		}
	}
	return CommonInterp == RCIM_None ? (Fallback == RCIM_Cubic ? RCIM_Linear : Fallback) : CommonInterp;
}

static void FinalizeCurveTableMutation(UCurveTable* CT, FCurveTableEditorUtils::ECurveTableChangeInfo ChangeInfo)
{
	if (!CT)
	{
		return;
	}

	FCurveTableEditorUtils::BroadcastPostChange(CT, ChangeInfo);
	CT->PostEditChange();
	CT->MarkPackageDirty();
}

// ============================================================================
// DOCS + REGISTRATION
// ============================================================================

static TArray<FLuaFunctionDoc> CurveTableDocs = {};

static void BindCurveTable(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_curve_table", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UCurveTable* CT = NeoLuaAsset::Resolve<UCurveTable>(FPath);
		if (!CT) return;

		TWeakObjectPtr<UCurveTable> WeakCT(CT);

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  row — a named curve row in the table\n"
			"  key — a single key within a curve row\n"
			"\n"
			"info():\n"
			"  Returns {mode, row_count, rows=[{name, key_count, default_value,\n"
			"    pre_extrap, post_extrap}]}.\n"
			"\n"
			"list(\"rows\"):\n"
			"  Returns array of {name, key_count, time_range={min,max}, value_range={min,max},\n"
			"    default_value, pre_extrap, post_extrap}.\n"
			"\n"
			"list(\"keys\", {row=\"RowName\"}):\n"
			"  Returns array of key data for a specific row.\n"
			"  RichCurve: {index, time, value, interp_mode, tangent_mode,\n"
			"    arrive_tangent, leave_tangent, tangent_weight_mode,\n"
			"    arrive_tangent_weight, leave_tangent_weight}\n"
			"  SimpleCurve: {index, time, value, interp_mode}\n"
			"\n"
			"add(\"row\", {name=\"NewRow\", mode=\"rich\"|\"simple\", keys=[{time=0, value=1}, ...],\n"
			"    default_value=0, pre_extrap=\"Constant\", post_extrap=\"Constant\"}):\n"
			"  Add a new curve row. Keys and curve properties are optional.\n"
			"  For RichCurves: keys can include interp_mode, arrive_tangent, leave_tangent,\n"
			"    tangent_weight_mode, arrive_tangent_weight, leave_tangent_weight.\n"
			"\n"
			"add(\"key\", {row=\"RowName\", time=1.0, value=2.0, interp_mode=\"Cubic\"}):\n"
			"  Add a single key to an existing row. For SimpleCurves, interp_mode applies to the row.\n"
			"\n"
			"remove(\"row\", {name=\"RowName\"}):\n"
			"  Remove an entire row.\n"
			"\n"
			"remove(\"all_rows\"):\n"
			"  Remove all rows (empty the table).\n"
			"\n"
			"remove(\"key\", {row=\"RowName\", index=N}) or remove(\"key\", {row=\"RowName\", time=T}):\n"
			"  Remove a single key by index (1-based) or by time.\n"
			"\n"
			"configure(\"row\", {name=\"RowName\", keys=[{time=.., value=..}, ...],\n"
			"    default_value=0, pre_extrap=\"Constant\", post_extrap=\"Constant\"}):\n"
			"  Replace ALL keys in a row with the provided array. Curve properties optional.\n"
			"\n"
			"configure(\"rename_row\", {name=\"OldName\", new_name=\"NewName\"}):\n"
			"  Rename a curve row.\n"
			"\n"
			"configure(\"mode\", {mode=\"RichCurves\"|\"SimpleCurves\", force=false}):\n"
			"  Convert the table between UE's rich and simple curve storage modes.\n"
			"  Rich->Simple rejects cubic/tangent/weighted data unless force=true.\n"
			"\n"
			"configure(\"import_csv\", {data=\"---,0,1,2\\nRow,0,1,2\", interp_mode=\"Linear\"}):\n"
			"  Replace entire table from CSV string.\n"
			"\n"
			"configure(\"import_json\", {data=\"[{...}]\", interp_mode=\"Linear\"}):\n"
			"  Replace entire table from JSON string.\n"
			"\n"
			"evaluate({row=\"RowName\", time=1.5}):\n"
			"  Evaluate a curve at the given time. Returns {row, time, value}.\n"
			"\n"
			"list(\"csv\"):\n"
			"  Returns the entire curve table as a CSV string.\n"
			"\n"
			"list(\"json\"):\n"
			"  Returns the entire curve table as a JSON string.\n"
			"\n"
			"Extrapolation modes: Cycle, CycleWithOffset, Oscillate, Linear, Constant, None\n"
			"Tangent weight modes: None, Arrive, Leave, Both\n";

		// ==================================================================
		// info()
		// ==================================================================
		AssetObj.set_function("info", [WeakCT, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UCurveTable* CT = WeakCT.Get();
			if (!CT) { Session.Log(TEXT("[FAIL] CurveTable no longer valid")); return sol::lua_nil; }

			const TMap<FName, FRealCurve*>& RowMap = CT->GetRowMap();

			sol::table Result = Lua.create_table();
			Result["mode"] = TCHAR_TO_UTF8(*LogicalModeToString(CT));
			Result["row_count"] = RowMap.Num();
			Result["is_factory_default"] = IsFactoryDefaultCurveRow(CT);

			sol::table Rows = Lua.create_table();
			int Idx = 1;
			for (const auto& Pair : RowMap)
			{
				sol::table Row = Lua.create_table();
				Row["name"] = TCHAR_TO_UTF8(*Pair.Key.ToString());
				Row["is_factory_default"] = Pair.Key == FName(TEXT("Curve")) && IsFactoryDefaultCurveRow(CT);
				if (Pair.Value)
				{
					Row["key_count"] = Pair.Value->GetNumKeys();
					Row["default_value"] = Pair.Value->DefaultValue;
					Row["pre_extrap"] = TCHAR_TO_UTF8(*CT_ExtrapToString(Pair.Value->PreInfinityExtrap));
					Row["post_extrap"] = TCHAR_TO_UTF8(*CT_ExtrapToString(Pair.Value->PostInfinityExtrap));
				}
				else
				{
					Row["key_count"] = 0;
				}
				Rows[Idx++] = Row;
			}
			Result["rows"] = Rows;

			Session.Log(TEXT("[OK] info()"));
			return Result;
		});

		// ==================================================================
		// list("rows") / list("keys", {row=..}) / list("csv") / list("json")
		// ==================================================================
		AssetObj.set_function("list", [WeakCT, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UCurveTable* CT = WeakCT.Get();
			if (!CT) { Session.Log(TEXT("[FAIL] CurveTable no longer valid")); return sol::lua_nil; }

			FString FType = NeoLuaStr::ToFString(Type);

			// --- list("rows") ---
			if (FType.Equals(TEXT("rows"), ESearchCase::IgnoreCase))
			{
				const TMap<FName, FRealCurve*>& RowMap = CT->GetRowMap();
				sol::table Result = Lua.create_table();
				int Idx = 1;
				for (const auto& Pair : RowMap)
				{
					FRealCurve* Curve = Pair.Value;
					if (!Curve) continue;

					sol::table Row = Lua.create_table();
					Row["name"] = TCHAR_TO_UTF8(*Pair.Key.ToString());
					Row["key_count"] = Curve->GetNumKeys();
					Row["is_factory_default"] = Pair.Key == FName(TEXT("Curve")) && IsFactoryDefaultCurveRow(CT);

					float MinTime = 0.f, MaxTime = 0.f;
					Curve->GetTimeRange(MinTime, MaxTime);
					sol::table TimeRange = Lua.create_table();
					TimeRange["min"] = MinTime;
					TimeRange["max"] = MaxTime;
					Row["time_range"] = TimeRange;

					float MinVal = 0.f, MaxVal = 0.f;
					Curve->GetValueRange(MinVal, MaxVal);
					sol::table ValueRange = Lua.create_table();
					ValueRange["min"] = MinVal;
					ValueRange["max"] = MaxVal;
					Row["value_range"] = ValueRange;

					Row["default_value"] = Curve->DefaultValue;
					Row["pre_extrap"] = TCHAR_TO_UTF8(*CT_ExtrapToString(Curve->PreInfinityExtrap));
					Row["post_extrap"] = TCHAR_TO_UTF8(*CT_ExtrapToString(Curve->PostInfinityExtrap));

					Result[Idx++] = Row;
				}
				Session.Log(TEXT("[OK] list(\"rows\")"));
				return Result;
			}

			// --- list("keys", {row=..}) ---
			if (FType.Equals(TEXT("keys"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"keys\") requires {row=\"RowName\"}"));
					return sol::lua_nil;
				}
				sol::optional<std::string> RowNameOpt = Params.value().get<sol::optional<std::string>>("row");
				if (!RowNameOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"keys\") requires {row=\"RowName\"}"));
					return sol::lua_nil;
				}
				FName RowName = FName(NeoLuaStr::ToFStringOpt(RowNameOpt));
				ECurveTableMode Mode = CT->GetCurveTableMode();

				sol::table Result = Lua.create_table();

				if (Mode == ECurveTableMode::RichCurves)
				{
					FRichCurve* Curve = CT->FindRichCurve(RowName, TEXT("LuaBinding"), false);
					if (!Curve)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
						return sol::lua_nil;
					}
					const TArray<FRichCurveKey>& Keys = Curve->GetConstRefOfKeys();
					for (int32 i = 0; i < Keys.Num(); ++i)
					{
						const FRichCurveKey& K = Keys[i];
						sol::table KeyT = Lua.create_table();
						KeyT["index"] = i + 1;
						KeyT["time"] = K.Time;
						KeyT["value"] = K.Value;
						KeyT["interp_mode"] = TCHAR_TO_UTF8(*CT_InterpModeToString(K.InterpMode));
						KeyT["tangent_mode"] = TCHAR_TO_UTF8(*CT_TangentModeToString(K.TangentMode));
						KeyT["arrive_tangent"] = K.ArriveTangent;
						KeyT["leave_tangent"] = K.LeaveTangent;
						KeyT["tangent_weight_mode"] = TCHAR_TO_UTF8(*CT_TangentWeightModeToString(K.TangentWeightMode));
						KeyT["arrive_tangent_weight"] = K.ArriveTangentWeight;
						KeyT["leave_tangent_weight"] = K.LeaveTangentWeight;
						Result[i + 1] = KeyT;
					}
				}
				else if (Mode == ECurveTableMode::SimpleCurves)
				{
					FSimpleCurve* Curve = CT->FindSimpleCurve(RowName, TEXT("LuaBinding"), false);
					if (!Curve)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
						return sol::lua_nil;
					}
					const TArray<FSimpleCurveKey>& Keys = Curve->GetConstRefOfKeys();
					for (int32 i = 0; i < Keys.Num(); ++i)
					{
						const FSimpleCurveKey& K = Keys[i];
						sol::table KeyT = Lua.create_table();
						KeyT["index"] = i + 1;
						KeyT["time"] = K.Time;
						KeyT["value"] = K.Value;
						KeyT["interp_mode"] = TCHAR_TO_UTF8(*CT_InterpModeToString(Curve->GetKeyInterpMode()));
						Result[i + 1] = KeyT;
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] CurveTable is Empty — no keys to list"));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"keys\", row=\"%s\")"), *RowName.ToString()));
				return Result;
			}

			// --- list("csv") ---
			if (FType.Equals(TEXT("csv"), ESearchCase::IgnoreCase))
			{
				FString CSV = CT->GetTableAsCSV();
				Session.Log(TEXT("[OK] list(\"csv\")"));
				return sol::make_object(Lua, TCHAR_TO_UTF8(*CSV));
			}

			// --- list("json") ---
			if (FType.Equals(TEXT("json"), ESearchCase::IgnoreCase))
			{
				FString JSON = CT->GetTableAsJSON();
				Session.Log(TEXT("[OK] list(\"json\")"));
				return sol::make_object(Lua, TCHAR_TO_UTF8(*JSON));
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") — unknown type. Use \"rows\", \"keys\", \"csv\", or \"json\""), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// add("row", {..}) / add("key", {..})
		// ==================================================================
		AssetObj.set_function("add", [WeakCT, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UCurveTable* CT = WeakCT.Get();
			if (!CT) { Session.Log(TEXT("[FAIL] CurveTable no longer valid")); return sol::lua_nil; }

			FString FType = NeoLuaStr::ToFString(Type);

			// --- add("row", {name=.., keys=[..]}) ---
			if (FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"row\") requires {name=\"RowName\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> NameOpt = P.get<sol::optional<std::string>>("name");
				if (!NameOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"row\") requires {name=\"RowName\"}"));
					return sol::lua_nil;
				}
				FName RowName = FName(NeoLuaStr::ToFStringOpt(NameOpt));

				// Check for duplicate
				if (CT->FindCurveUnchecked(RowName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' already exists"), *RowName.ToString()));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Add CurveTable Row")));
				CT->Modify();

				ECurveTableMode Mode = CT->GetCurveTableMode();
				if (CT->GetRowMap().Num() == 0 && Mode != ECurveTableMode::Empty)
				{
					CT->EmptyTable();
					Mode = ECurveTableMode::Empty;
				}

				ECurveTableMode RequestedMode = Mode;
				sol::optional<std::string> ModeOpt = P.get<sol::optional<std::string>>("mode");
				if (!ModeOpt.has_value())
				{
					ModeOpt = P.get<sol::optional<std::string>>("curve_type");
				}
				if (ModeOpt.has_value())
				{
					if (!ParseCurveTableMode(NeoLuaStr::ToFStringOpt(ModeOpt), RequestedMode) || RequestedMode == ECurveTableMode::Empty)
					{
						Session.Log(TEXT("[FAIL] add(\"row\") mode must be RichCurves or SimpleCurves"));
						return sol::lua_nil;
					}
					if (Mode != ECurveTableMode::Empty && RequestedMode != Mode)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"row\") requested %s but table is %s. Use remove(\"all_rows\") first or configure(\"mode\", {mode=\"%s\", force=true}) to convert."),
							*ModeToString(RequestedMode), *ModeToString(Mode), *ModeToString(RequestedMode)));
						return sol::lua_nil;
					}
				}

				if ((Mode == ECurveTableMode::SimpleCurves || RequestedMode == ECurveTableMode::SimpleCurves) && ParamsHaveRichOnlyKeyFields(P))
				{
					Session.Log(TEXT("[FAIL] add(\"row\") simple curves cannot store cubic/tangent/tangent-weight key fields. Use mode=\"rich\" or remove those fields."));
					return sol::lua_nil;
				}

				// Helper to apply curve properties (default_value, pre/post extrap)
				auto ApplyCurveProperties = [&P](FRealCurve* Curve)
				{
					sol::optional<float> DefValOpt = P.get<sol::optional<float>>("default_value");
					if (DefValOpt.has_value())
					{
						Curve->SetDefaultValue(DefValOpt.value());
					}
					sol::optional<std::string> PreExtrapOpt = P.get<sol::optional<std::string>>("pre_extrap");
					if (PreExtrapOpt.has_value())
					{
						Curve->PreInfinityExtrap = CT_StringToExtrap(NeoLuaStr::ToFStringOpt(PreExtrapOpt));
					}
					sol::optional<std::string> PostExtrapOpt = P.get<sol::optional<std::string>>("post_extrap");
					if (PostExtrapOpt.has_value())
					{
						Curve->PostInfinityExtrap = CT_StringToExtrap(NeoLuaStr::ToFStringOpt(PostExtrapOpt));
					}
				};

				if (Mode == ECurveTableMode::Empty && RequestedMode == ECurveTableMode::SimpleCurves)
				{
					FSimpleCurve& NewCurve = CT->AddSimpleCurve(RowName);

					sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
					if (InterpOpt.has_value())
					{
						ERichCurveInterpMode IM = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
						if (IM == RCIM_Cubic)
						{
							Session.Log(TEXT("[FAIL] SimpleCurves cannot use Cubic interpolation"));
							return sol::lua_nil;
						}
						NewCurve.SetKeyInterpMode(IM);
					}

					sol::optional<sol::table> KeysOpt = P.get<sol::optional<sol::table>>("keys");
					if (KeysOpt.has_value())
					{
						TArray<FSimpleCurveKey> NewKeys;
						for (auto& KV : KeysOpt.value())
						{
							if (!KV.second.is<sol::table>()) continue;
							sol::table K = KV.second.as<sol::table>();
							float Time = 0.f, Value = 0.f;
							if (NeoLuaCurve::ReadKeyTimeValue(K, Time, Value, false))
							{
								NewKeys.Add(FSimpleCurveKey(Time, Value));
							}
						}
						NewKeys.Sort([](const FSimpleCurveKey& A, const FSimpleCurveKey& B) { return A.Time < B.Time; });
						NewCurve.SetKeys(NewKeys);
					}

					ApplyCurveProperties(&NewCurve);
				}
				else if (Mode == ECurveTableMode::SimpleCurves)
				{
					sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
					ERichCurveInterpMode InterpMode = RCIM_Linear;
					if (InterpOpt.has_value())
					{
						InterpMode = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
						if (InterpMode == RCIM_Cubic)
						{
							Session.Log(TEXT("[FAIL] add(\"row\") simple curves cannot use Cubic interpolation. Use mode=\"rich\" or remove interp_mode."));
							return sol::lua_nil;
						}
					}

					FSimpleCurve& NewCurve = CT->AddSimpleCurve(RowName);
					if (InterpOpt.has_value())
					{
						NewCurve.SetKeyInterpMode(InterpMode);
					}

					// Populate keys if provided
					sol::optional<sol::table> KeysOpt = P.get<sol::optional<sol::table>>("keys");
					if (KeysOpt.has_value())
					{
						sol::table KeysArr = KeysOpt.value();
						for (auto& KV : KeysArr)
						{
							if (!KV.second.is<sol::table>()) continue;
							sol::table K = KV.second.as<sol::table>();
							float Time = K.get_or("time", 0.f);
							float Value = K.get_or("value", 0.f);
							NewCurve.AddKey(Time, Value);
						}
					}

					ApplyCurveProperties(&NewCurve);
				}
				else
				{
					// RichCurves or Empty (defaults to Rich)
					FRichCurve& NewCurve = CT->AddRichCurve(RowName);

					// Populate keys if provided
					sol::optional<sol::table> KeysOpt = P.get<sol::optional<sol::table>>("keys");
					if (KeysOpt.has_value())
					{
						sol::table KeysArr = KeysOpt.value();
						for (auto& KV : KeysArr)
						{
							if (!KV.second.is<sol::table>()) continue;
							sol::table K = KV.second.as<sol::table>();
							float Time = K.get_or("time", 0.f);
							float Value = K.get_or("value", 0.f);
							FKeyHandle Handle = NewCurve.AddKey(Time, Value);

							sol::optional<std::string> InterpOpt = K.get<sol::optional<std::string>>("interp_mode");
							if (InterpOpt.has_value())
							{
								NewCurve.SetKeyInterpMode(Handle, CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt)));
							}

							sol::optional<float> ArriveOpt = K.get<sol::optional<float>>("arrive_tangent");
							sol::optional<float> LeaveOpt = K.get<sol::optional<float>>("leave_tangent");
							if (ArriveOpt.has_value() || LeaveOpt.has_value())
							{
								FRichCurveKey& RKey = NewCurve.GetKey(Handle);
								if (ArriveOpt.has_value()) RKey.ArriveTangent = ArriveOpt.value();
								if (LeaveOpt.has_value()) RKey.LeaveTangent = LeaveOpt.value();
								RKey.TangentMode = RCTM_User;
							}

							// Tangent weight support
							sol::optional<std::string> WeightModeOpt = K.get<sol::optional<std::string>>("tangent_weight_mode");
							if (WeightModeOpt.has_value())
							{
								NewCurve.SetKeyTangentWeightMode(Handle, CT_StringToTangentWeightMode(NeoLuaStr::ToFStringOpt(WeightModeOpt)), false);
								FRichCurveKey& RKey = NewCurve.GetKey(Handle);
								sol::optional<float> ArrWOpt = K.get<sol::optional<float>>("arrive_tangent_weight");
								sol::optional<float> LvWOpt = K.get<sol::optional<float>>("leave_tangent_weight");
								if (ArrWOpt.has_value()) RKey.ArriveTangentWeight = ArrWOpt.value();
								if (LvWOpt.has_value()) RKey.LeaveTangentWeight = LvWOpt.value();
							}
						}
					}

					ApplyCurveProperties(&NewCurve);
				}

				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				CT->PostEditChange();
				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"row\", name=\"%s\")"), *RowName.ToString()));

				sol::table Result = Lua.create_table();
				Result["name"] = TCHAR_TO_UTF8(*RowName.ToString());
				Result["mode"] = TCHAR_TO_UTF8(*ModeToString(CT->GetCurveTableMode()));
				return Result;
			}

			// --- add("key", {row=.., time=.., value=..}) ---
			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"key\") requires {row=\"RowName\", time=.., value=..}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> RowOpt = P.get<sol::optional<std::string>>("row");
				if (!RowOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"key\") requires row=\"RowName\""));
					return sol::lua_nil;
				}
				FName RowName = FName(NeoLuaStr::ToFStringOpt(RowOpt));
				float Time = P.get_or("time", 0.f);
				float Value = P.get_or("value", 0.f);

				ECurveTableMode Mode = CT->GetCurveTableMode();

				FScopedTransaction Txn(FText::FromString(TEXT("Add CurveTable Key")));
				CT->Modify();

				if (Mode == ECurveTableMode::RichCurves)
				{
					FRichCurve* Curve = CT->FindRichCurve(RowName, TEXT("LuaBinding"), false);
					if (!Curve)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
						return sol::lua_nil;
					}
					FKeyHandle Handle = Curve->AddKey(Time, Value);

					sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
					if (InterpOpt.has_value())
					{
						Curve->SetKeyInterpMode(Handle, CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt)));
					}

					sol::optional<float> ArriveOpt = P.get<sol::optional<float>>("arrive_tangent");
					sol::optional<float> LeaveOpt = P.get<sol::optional<float>>("leave_tangent");
					if (ArriveOpt.has_value() || LeaveOpt.has_value())
					{
						FRichCurveKey& RKey = Curve->GetKey(Handle);
						if (ArriveOpt.has_value()) RKey.ArriveTangent = ArriveOpt.value();
						if (LeaveOpt.has_value()) RKey.LeaveTangent = LeaveOpt.value();
						RKey.TangentMode = RCTM_User;
					}
					else
					{
						// Auto-compute tangents if interp is Cubic and user didn't set explicit tangents
						FRichCurveKey& RKey = Curve->GetKey(Handle);
						if (RKey.InterpMode == RCIM_Cubic && RKey.TangentMode == RCTM_Auto)
						{
							Curve->AutoSetTangents();
						}
					}

					// Tangent weight support
					sol::optional<std::string> WeightModeOpt = P.get<sol::optional<std::string>>("tangent_weight_mode");
					if (WeightModeOpt.has_value())
					{
						Curve->SetKeyTangentWeightMode(Handle, CT_StringToTangentWeightMode(NeoLuaStr::ToFStringOpt(WeightModeOpt)), false);
						FRichCurveKey& RKey = Curve->GetKey(Handle);
						sol::optional<float> ArrWOpt = P.get<sol::optional<float>>("arrive_tangent_weight");
						sol::optional<float> LvWOpt = P.get<sol::optional<float>>("leave_tangent_weight");
						if (ArrWOpt.has_value()) RKey.ArriveTangentWeight = ArrWOpt.value();
						if (LvWOpt.has_value()) RKey.LeaveTangentWeight = LvWOpt.value();
					}
				}
				else if (Mode == ECurveTableMode::SimpleCurves)
				{
					FSimpleCurve* Curve = CT->FindSimpleCurve(RowName, TEXT("LuaBinding"), false);
					if (!Curve)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
						return sol::lua_nil;
					}
					if (HasRichOnlyKeyFields(P))
					{
						Session.Log(TEXT("[FAIL] add(\"key\") simple curves cannot store cubic/tangent/tangent-weight key fields. Convert to RichCurves first."));
						return sol::lua_nil;
					}
					sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
					ERichCurveInterpMode InterpMode = RCIM_Linear;
					if (InterpOpt.has_value())
					{
						InterpMode = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
						if (InterpMode == RCIM_Cubic)
						{
							Session.Log(TEXT("[FAIL] add(\"key\") simple curves cannot use Cubic interpolation. Convert to RichCurves first."));
							return sol::lua_nil;
						}
					}
					if (InterpOpt.has_value())
					{
						Curve->SetKeyInterpMode(InterpMode);
					}
					Curve->AddKey(Time, Value);
				}
				else
				{
					Session.Log(TEXT("[FAIL] CurveTable is Empty — add a row first"));
					return sol::lua_nil;
				}

				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowData);
				CT->PostEditChange();
				CT->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"key\", row=\"%s\", time=%.4f)"), *RowName.ToString(), Time));

				sol::table Result = Lua.create_table();
				Result["row"] = TCHAR_TO_UTF8(*RowName.ToString());
				Result["time"] = Time;
				Result["value"] = Value;
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") — unknown type. Use \"row\" or \"key\""), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// remove("row", {name=..}) / remove("key", {..}) / remove("all_rows")
		// ==================================================================
		AssetObj.set_function("remove", [WeakCT, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UCurveTable* CT = WeakCT.Get();
			if (!CT) { Session.Log(TEXT("[FAIL] CurveTable no longer valid")); return sol::lua_nil; }

			FString FType = NeoLuaStr::ToFString(Type);

			// --- remove("all_rows") ---
			if (FType.Equals(TEXT("all_rows"), ESearchCase::IgnoreCase))
			{
				FScopedTransaction Txn(FText::FromString(TEXT("Empty CurveTable")));
				CT->Modify();
				int32 Count = CT->GetRowMap().Num();
				CT->EmptyTable();
				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				CT->PostEditChange();
				CT->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"all_rows\") — removed %d rows"), Count));
				sol::table Result = Lua.create_table();
				Result["removed_count"] = Count;
				return Result;
			}

			// --- remove("row", {name=..}) ---
			if (FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"row\") requires {name=\"RowName\"}"));
					return sol::lua_nil;
				}
				sol::optional<std::string> NameOpt = Params.value().get<sol::optional<std::string>>("name");
				if (!NameOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"row\") requires {name=\"RowName\"}"));
					return sol::lua_nil;
				}
				FName RowName = FName(NeoLuaStr::ToFStringOpt(NameOpt));

				if (!CT->FindCurveUnchecked(RowName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Remove CurveTable Row")));
				CT->Modify();
				CT->RemoveRow(RowName);
				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				CT->PostEditChange();
				CT->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"row\", name=\"%s\")"), *RowName.ToString()));
				sol::table Result = Lua.create_table();
				Result["removed"] = TCHAR_TO_UTF8(*RowName.ToString());
				return Result;
			}

			// --- remove("key", {row=.., index=.. or time=..}) ---
			if (FType.Equals(TEXT("key"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"key\") requires {row=\"RowName\", index=N} or {row=\"RowName\", time=T}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> RowOpt = P.get<sol::optional<std::string>>("row");
				if (!RowOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"key\") requires row=\"RowName\""));
					return sol::lua_nil;
				}
				FName RowName = FName(NeoLuaStr::ToFStringOpt(RowOpt));

				FRealCurve* BaseCurve = CT->FindCurve(RowName, TEXT("LuaBinding"), false);
				if (!BaseCurve)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
					return sol::lua_nil;
				}

				// Find the key handle to delete — by index (1-based) or by time
				FKeyHandle HandleToDelete = FKeyHandle::Invalid();
				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				sol::optional<float> TimeOpt = P.get<sol::optional<float>>("time");

				if (IndexOpt.has_value())
				{
					int32 Idx = IndexOpt.value() - 1; // 1-based to 0-based
					if (Idx < 0 || Idx >= BaseCurve->GetNumKeys())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] Key index %d out of range (1-%d)"), IndexOpt.value(), BaseCurve->GetNumKeys()));
						return sol::lua_nil;
					}
					// Iterate handles to find the one at this index
					auto HandleIt = BaseCurve->GetKeyHandleIterator();
					int32 Cur = 0;
					for (; HandleIt; ++HandleIt, ++Cur)
					{
						if (Cur == Idx)
						{
							HandleToDelete = *HandleIt;
							break;
						}
					}
				}
				else if (TimeOpt.has_value())
				{
					HandleToDelete = BaseCurve->FindKey(TimeOpt.value());
					if (HandleToDelete == FKeyHandle::Invalid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] No key found at time %.4f"), TimeOpt.value()));
						return sol::lua_nil;
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"key\") requires index=N or time=T"));
					return sol::lua_nil;
				}

				if (HandleToDelete == FKeyHandle::Invalid())
				{
					Session.Log(TEXT("[FAIL] Could not resolve key handle"));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Remove CurveTable Key")));
				CT->Modify();
				BaseCurve->DeleteKey(HandleToDelete);
				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowData);
				CT->PostEditChange();
				CT->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"key\", row=\"%s\")"), *RowName.ToString()));
				sol::table Result = Lua.create_table();
				Result["row"] = TCHAR_TO_UTF8(*RowName.ToString());
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") — unknown type. Use \"row\", \"key\", or \"all_rows\""), *FType));
			return sol::lua_nil;
		});

		// ==================================================================
		// configure("row", {..}) / configure("rename_row", {..})
		// configure("import_csv", {..}) / configure("import_json", {..})
		// ==================================================================
		AssetObj.set_function("configure", [WeakCT, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UCurveTable* CT = WeakCT.Get();
			if (!CT) { Session.Log(TEXT("[FAIL] CurveTable no longer valid")); return sol::lua_nil; }

			FString FType = NeoLuaStr::ToFString(Type);

			// --- configure("mode", {mode="RichCurves"|"SimpleCurves", force=false}) ---
			if (FType.Equals(TEXT("mode"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"mode\") requires {mode=\"RichCurves\"|\"SimpleCurves\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> ModeOpt = P.get<sol::optional<std::string>>("mode");
				if (!ModeOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"mode\") requires mode=\"RichCurves\" or mode=\"SimpleCurves\""));
					return sol::lua_nil;
				}

				ECurveTableMode TargetMode = ECurveTableMode::Empty;
				if (!ParseCurveTableMode(NeoLuaStr::ToFStringOpt(ModeOpt), TargetMode) || TargetMode == ECurveTableMode::Empty)
				{
					Session.Log(TEXT("[FAIL] configure(\"mode\") mode must be RichCurves or SimpleCurves"));
					return sol::lua_nil;
				}

				const ECurveTableMode CurrentMode = CT->GetCurveTableMode();
				if (CurrentMode == TargetMode)
				{
					sol::table Result = Lua.create_table();
					Result["mode"] = TCHAR_TO_UTF8(*ModeToString(CurrentMode));
					Result["row_count"] = CT->GetRowMap().Num();
					Result["converted"] = false;
					Session.Log(FString::Printf(TEXT("[OK] configure(\"mode\") -> %s (already set)"), *ModeToString(CurrentMode)));
					return Result;
				}

				const bool bForce = P.get_or("force", false);
				ERichCurveInterpMode FallbackInterp = RCIM_Linear;
				sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
				if (InterpOpt.has_value())
				{
					FallbackInterp = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Configure CurveTable Mode")));
				CT->Modify();

				if (TargetMode == ECurveTableMode::RichCurves)
				{
					struct FSimpleRowSnapshot
					{
						FName Name;
						TArray<FSimpleCurveKey> Keys;
						ERichCurveInterpMode Interp = RCIM_Linear;
						float DefaultValue = MAX_flt;
						ERichCurveExtrapolation Pre = RCCE_Constant;
						ERichCurveExtrapolation Post = RCCE_Constant;
					};

					TArray<FSimpleRowSnapshot> Rows;
					if (CurrentMode == ECurveTableMode::SimpleCurves)
					{
						for (const TPair<FName, FRealCurve*>& Pair : CT->GetRowMap())
						{
							FSimpleCurve* Source = static_cast<FSimpleCurve*>(Pair.Value);
							if (!Source) continue;
							FSimpleRowSnapshot Snapshot;
							Snapshot.Name = Pair.Key;
							Snapshot.Keys = Source->GetConstRefOfKeys();
							Snapshot.Interp = Source->GetKeyInterpMode();
							Snapshot.DefaultValue = Source->DefaultValue;
							Snapshot.Pre = Source->PreInfinityExtrap;
							Snapshot.Post = Source->PostInfinityExtrap;
							Rows.Add(MoveTemp(Snapshot));
						}
					}

					CT->EmptyTable();
					for (const FSimpleRowSnapshot& Snapshot : Rows)
					{
						FRichCurve& Dest = CT->AddRichCurve(Snapshot.Name);
						Dest.SetDefaultValue(Snapshot.DefaultValue);
						Dest.PreInfinityExtrap = Snapshot.Pre;
						Dest.PostInfinityExtrap = Snapshot.Post;
						for (const FSimpleCurveKey& Key : Snapshot.Keys)
						{
							FKeyHandle Handle = Dest.AddKey(Key.Time, Key.Value);
							Dest.SetKeyInterpMode(Handle, Snapshot.Interp);
						}
						Dest.AutoSetTangents();
					}
				}
				else
				{
					struct FRichRowSnapshot
					{
						FName Name;
						TArray<FRichCurveKey> Keys;
						ERichCurveInterpMode Interp = RCIM_Linear;
						float DefaultValue = MAX_flt;
						ERichCurveExtrapolation Pre = RCCE_Constant;
						ERichCurveExtrapolation Post = RCCE_Constant;
					};

					TArray<FRichRowSnapshot> Rows;
					if (CurrentMode == ECurveTableMode::RichCurves)
					{
						for (const TPair<FName, FRealCurve*>& Pair : CT->GetRowMap())
						{
							FRichCurve* Source = static_cast<FRichCurve*>(Pair.Value);
							if (!Source) continue;
							if (!bForce && !RichCurveCanConvertToSimple(Source))
							{
								Session.Log(FString::Printf(TEXT("[FAIL] configure(\"mode\") row '%s' has cubic/tangent/weighted data; pass force=true to convert lossy to SimpleCurves"),
									*Pair.Key.ToString()));
								return sol::lua_nil;
							}

							FRichRowSnapshot Snapshot;
							Snapshot.Name = Pair.Key;
							Snapshot.Keys = Source->GetConstRefOfKeys();
							Snapshot.Interp = GetCommonSimpleInterp(Source, FallbackInterp);
							Snapshot.DefaultValue = Source->DefaultValue;
							Snapshot.Pre = Source->PreInfinityExtrap;
							Snapshot.Post = Source->PostInfinityExtrap;
							Rows.Add(MoveTemp(Snapshot));
						}
					}

					CT->EmptyTable();
					for (const FRichRowSnapshot& Snapshot : Rows)
					{
						FSimpleCurve& Dest = CT->AddSimpleCurve(Snapshot.Name);
						Dest.SetDefaultValue(Snapshot.DefaultValue);
						Dest.PreInfinityExtrap = Snapshot.Pre;
						Dest.PostInfinityExtrap = Snapshot.Post;
						Dest.SetKeyInterpMode(Snapshot.Interp == RCIM_Cubic ? RCIM_Linear : Snapshot.Interp);
						TArray<FSimpleCurveKey> SimpleKeys;
						for (const FRichCurveKey& Key : Snapshot.Keys)
						{
							SimpleKeys.Add(FSimpleCurveKey(Key.Time, Key.Value));
						}
						SimpleKeys.Sort([](const FSimpleCurveKey& A, const FSimpleCurveKey& B) { return A.Time < B.Time; });
						Dest.SetKeys(SimpleKeys);
					}
				}

				FinalizeCurveTableMutation(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				sol::table Result = Lua.create_table();
				Result["mode"] = TCHAR_TO_UTF8(*ModeToString(CT->GetCurveTableMode()));
				Result["row_count"] = CT->GetRowMap().Num();
				Result["converted"] = true;
				Session.Log(FString::Printf(TEXT("[OK] configure(\"mode\") -> %s"), *ModeToString(CT->GetCurveTableMode())));
				return Result;
			}

			// --- configure("rename_row", {name=.., new_name=..}) ---
			if (FType.Equals(TEXT("rename_row"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"rename_row\") requires {name=\"OldName\", new_name=\"NewName\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> NameOpt = P.get<sol::optional<std::string>>("name");
				sol::optional<std::string> NewNameOpt = P.get<sol::optional<std::string>>("new_name");
				if (!NameOpt.has_value() || !NewNameOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"rename_row\") requires name and new_name"));
					return sol::lua_nil;
				}

				FName OldName = FName(NeoLuaStr::ToFStringOpt(NameOpt));
				FName NewName = FName(NeoLuaStr::ToFStringOpt(NewNameOpt));

				if (!CT->FindCurveUnchecked(OldName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *OldName.ToString()));
					return sol::lua_nil;
				}
				if (CT->FindCurveUnchecked(NewName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' already exists"), *NewName.ToString()));
					return sol::lua_nil;
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Rename CurveTable Row")));
				CT->Modify();
				CT->RenameRow(OldName, NewName);
				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				CT->PostEditChange();
				CT->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"rename_row\", \"%s\" -> \"%s\")"), *OldName.ToString(), *NewName.ToString()));
				sol::table Result = Lua.create_table();
				Result["old_name"] = TCHAR_TO_UTF8(*OldName.ToString());
				Result["new_name"] = TCHAR_TO_UTF8(*NewName.ToString());
				return Result;
			}

			// --- configure("import_csv", {data=.., interp_mode=..}) ---
			if (FType.Equals(TEXT("import_csv"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"import_csv\") requires {data=\"CSV string\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> DataOpt = P.get<sol::optional<std::string>>("data");
				if (!DataOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"import_csv\") requires data=\"CSV string\""));
					return sol::lua_nil;
				}

				ERichCurveInterpMode InterpMode = RCIM_Linear;
				sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
				if (InterpOpt.has_value())
				{
					InterpMode = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Import CurveTable CSV")));
				CT->Modify();
				CT->EmptyTable();
				FString CSVData = NeoLuaStr::ToFStringOpt(DataOpt);
				TArray<FString> Problems = CT->CreateTableFromCSVString(CSVData, InterpMode);
				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				CT->PostEditChange();
				CT->MarkPackageDirty();

				if (Problems.Num() > 0)
				{
					FString ProblemStr = FString::Join(Problems, TEXT("; "));
					Session.Log(FString::Printf(TEXT("[WARN] import_csv completed with issues: %s"), *ProblemStr));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[OK] configure(\"import_csv\") — %d rows imported"), CT->GetRowMap().Num()));
				}

				sol::table Result = Lua.create_table();
				Result["row_count"] = CT->GetRowMap().Num();
				if (Problems.Num() > 0)
				{
					sol::table ProbArr = Lua.create_table();
					for (int32 i = 0; i < Problems.Num(); ++i)
					{
						ProbArr[i + 1] = TCHAR_TO_UTF8(*Problems[i]);
					}
					Result["problems"] = ProbArr;
				}
				return Result;
			}

			// --- configure("import_json", {data=.., interp_mode=..}) ---
			if (FType.Equals(TEXT("import_json"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"import_json\") requires {data=\"JSON string\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				sol::optional<std::string> DataOpt = P.get<sol::optional<std::string>>("data");
				if (!DataOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"import_json\") requires data=\"JSON string\""));
					return sol::lua_nil;
				}

				ERichCurveInterpMode InterpMode = RCIM_Linear;
				sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
				if (InterpOpt.has_value())
				{
					InterpMode = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
				}

				FScopedTransaction Txn(FText::FromString(TEXT("Import CurveTable JSON")));
				CT->Modify();
				CT->EmptyTable();
				FString JSONData = NeoLuaStr::ToFStringOpt(DataOpt);
				TArray<FString> Problems = CT->CreateTableFromJSONString(JSONData, InterpMode);
				FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowList);
				CT->PostEditChange();
				CT->MarkPackageDirty();

				if (Problems.Num() > 0)
				{
					FString ProblemStr = FString::Join(Problems, TEXT("; "));
					Session.Log(FString::Printf(TEXT("[WARN] import_json completed with issues: %s"), *ProblemStr));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[OK] configure(\"import_json\") — %d rows imported"), CT->GetRowMap().Num()));
				}

				sol::table Result = Lua.create_table();
				Result["row_count"] = CT->GetRowMap().Num();
				if (Problems.Num() > 0)
				{
					sol::table ProbArr = Lua.create_table();
					for (int32 i = 0; i < Problems.Num(); ++i)
					{
						ProbArr[i + 1] = TCHAR_TO_UTF8(*Problems[i]);
					}
					Result["problems"] = ProbArr;
				}
				return Result;
			}

			// --- configure("row", {name=.., keys=[..], default_value=.., pre_extrap=.., post_extrap=..}) ---
			if (!FType.Equals(TEXT("row"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") — unknown type. Use \"row\", \"rename_row\", \"import_csv\", or \"import_json\""), *FType));
				return sol::lua_nil;
			}
			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] configure(\"row\") requires {name=\"RowName\", ...}"));
				return sol::lua_nil;
			}
			sol::table P = Params.value();
			sol::optional<std::string> NameOpt = P.get<sol::optional<std::string>>("name");
			if (!NameOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] configure(\"row\") requires name=\"RowName\""));
				return sol::lua_nil;
			}
			FName RowName = FName(NeoLuaStr::ToFStringOpt(NameOpt));

			ECurveTableMode Mode = CT->GetCurveTableMode();

			if (Mode == ECurveTableMode::Empty)
			{
				Session.Log(TEXT("[FAIL] CurveTable is Empty — add a row first"));
				return sol::lua_nil;
			}

			FScopedTransaction Txn(FText::FromString(TEXT("Configure CurveTable Row")));
			CT->Modify();

			// Helper to apply curve properties
			auto ApplyCurveProperties = [&P](FRealCurve* Curve)
			{
				sol::optional<float> DefValOpt = P.get<sol::optional<float>>("default_value");
				if (DefValOpt.has_value())
				{
					Curve->SetDefaultValue(DefValOpt.value());
				}
				sol::optional<std::string> PreExtrapOpt = P.get<sol::optional<std::string>>("pre_extrap");
				if (PreExtrapOpt.has_value())
				{
					Curve->PreInfinityExtrap = CT_StringToExtrap(NeoLuaStr::ToFStringOpt(PreExtrapOpt));
				}
				sol::optional<std::string> PostExtrapOpt = P.get<sol::optional<std::string>>("post_extrap");
				if (PostExtrapOpt.has_value())
				{
					Curve->PostInfinityExtrap = CT_StringToExtrap(NeoLuaStr::ToFStringOpt(PostExtrapOpt));
				}
			};

			if (Mode == ECurveTableMode::RichCurves)
			{
				FRichCurve* Curve = CT->FindRichCurve(RowName, TEXT("LuaBinding"), false);
				if (!Curve)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
					return sol::lua_nil;
				}

				// Replace keys if provided
				sol::optional<sol::table> KeysOpt = P.get<sol::optional<sol::table>>("keys");
				if (KeysOpt.has_value())
				{
					TArray<FRichCurveKey> NewKeys;
					sol::table KeysArr = KeysOpt.value();
					for (auto& KV : KeysArr)
					{
						if (!KV.second.is<sol::table>()) continue;
						sol::table K = KV.second.as<sol::table>();
						float Time = K.get_or("time", 0.f);
						float Value = K.get_or("value", 0.f);

						FRichCurveKey NewKey(Time, Value);

						sol::optional<std::string> InterpOpt = K.get<sol::optional<std::string>>("interp_mode");
						if (InterpOpt.has_value())
						{
							NewKey.InterpMode = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
						}

						sol::optional<float> ArriveOpt = K.get<sol::optional<float>>("arrive_tangent");
						sol::optional<float> LeaveOpt = K.get<sol::optional<float>>("leave_tangent");
						if (ArriveOpt.has_value()) NewKey.ArriveTangent = ArriveOpt.value();
						if (LeaveOpt.has_value()) NewKey.LeaveTangent = LeaveOpt.value();
						if (ArriveOpt.has_value() || LeaveOpt.has_value())
						{
							NewKey.TangentMode = RCTM_User;
						}

						// Tangent weight support
						sol::optional<std::string> WeightModeOpt = K.get<sol::optional<std::string>>("tangent_weight_mode");
						if (WeightModeOpt.has_value())
						{
							NewKey.TangentWeightMode = CT_StringToTangentWeightMode(NeoLuaStr::ToFStringOpt(WeightModeOpt));
							sol::optional<float> ArrWOpt = K.get<sol::optional<float>>("arrive_tangent_weight");
							sol::optional<float> LvWOpt = K.get<sol::optional<float>>("leave_tangent_weight");
							if (ArrWOpt.has_value()) NewKey.ArriveTangentWeight = ArrWOpt.value();
							if (LvWOpt.has_value()) NewKey.LeaveTangentWeight = LvWOpt.value();
						}

						NewKeys.Add(NewKey);
					}

					// Sort by time before SetKeys (it expects sorted input)
					NewKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });
					Curve->SetKeys(NewKeys);
				}

				ApplyCurveProperties(Curve);
			}
			else if (Mode == ECurveTableMode::SimpleCurves)
			{
				if (ParamsHaveRichOnlyKeyFields(P))
				{
					Session.Log(TEXT("[FAIL] configure(\"row\") simple curves cannot store cubic/tangent/tangent-weight key fields. Convert to RichCurves first."));
					return sol::lua_nil;
				}
				FSimpleCurve* Curve = CT->FindSimpleCurve(RowName, TEXT("LuaBinding"), false);
				if (!Curve)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
					return sol::lua_nil;
				}

				// Replace keys if provided
				sol::optional<sol::table> KeysOpt = P.get<sol::optional<sol::table>>("keys");
				if (KeysOpt.has_value())
				{
					TArray<FSimpleCurveKey> NewKeys;
					sol::table KeysArr = KeysOpt.value();
					for (auto& KV : KeysArr)
					{
						if (!KV.second.is<sol::table>()) continue;
						sol::table K = KV.second.as<sol::table>();
						float Time = K.get_or("time", 0.f);
						float Value = K.get_or("value", 0.f);
						NewKeys.Add(FSimpleCurveKey(Time, Value));
					}

					NewKeys.Sort([](const FSimpleCurveKey& A, const FSimpleCurveKey& B) { return A.Time < B.Time; });
					Curve->SetKeys(NewKeys);
				}

				// Set interp mode if provided
				sol::optional<std::string> InterpOpt = P.get<sol::optional<std::string>>("interp_mode");
				if (InterpOpt.has_value())
				{
					ERichCurveInterpMode IM = CT_StringToInterpMode(NeoLuaStr::ToFStringOpt(InterpOpt));
					if (IM == RCIM_Cubic)
					{
						Session.Log(TEXT("[WARN] SimpleCurves cannot use Cubic interpolation — skipped"));
					}
					else
					{
						Curve->SetKeyInterpMode(IM);
					}
				}

				ApplyCurveProperties(Curve);
			}

			FCurveTableEditorUtils::BroadcastPostChange(CT, FCurveTableEditorUtils::ECurveTableChangeInfo::RowData);
			CT->PostEditChange();
			CT->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] configure(\"row\", name=\"%s\")"), *RowName.ToString()));
			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*RowName.ToString());
			return Result;
		});

		// ==================================================================
		// evaluate({row=.., time=..})
		// ==================================================================
		AssetObj.set_function("evaluate", [WeakCT, &Session](sol::table /*self*/,
			sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UCurveTable* CT = WeakCT.Get();
			if (!CT) { Session.Log(TEXT("[FAIL] CurveTable no longer valid")); return sol::lua_nil; }

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] evaluate() requires {row=\"RowName\", time=T}"));
				return sol::lua_nil;
			}
			sol::table P = Params.value();
			sol::optional<std::string> RowOpt = P.get<sol::optional<std::string>>("row");
			if (!RowOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] evaluate() requires row=\"RowName\""));
				return sol::lua_nil;
			}
			FName RowName = FName(NeoLuaStr::ToFStringOpt(RowOpt));
			float Time = P.get_or("time", 0.f);

			FRealCurve* Curve = CT->FindCurve(RowName, TEXT("LuaBinding"), false);
			if (!Curve)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] Row '%s' not found"), *RowName.ToString()));
				return sol::lua_nil;
			}

			float Value = Curve->Eval(Time, 0.f);

			Session.Log(FString::Printf(TEXT("[OK] evaluate(row=\"%s\", time=%.4f) = %.4f"), *RowName.ToString(), Time, Value));
			sol::table Result = Lua.create_table();
			Result["row"] = TCHAR_TO_UTF8(*RowName.ToString());
			Result["time"] = Time;
			Result["value"] = Value;
			return Result;
		});

	}); // end _enrich_curve_table
}

REGISTER_LUA_BINDING(CurveTable, CurveTableDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindCurveTable(Lua, Session);
}); // end REGISTER_LUA_BINDING
