#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"
#include "Lua/LuaBindingRegistry.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Unified curve-key parsing for all Lua bindings. Replaces drifted per-binding
// parsers in LuaBinding_Curve, LuaBinding_CurveTable, LuaBinding_AnimSequence,
// LuaBinding_Niagara, and LuaBinding_Blueprint (timelines).
//
// Accepted key formats:
//   Named-field (preferred):
//     {time=0.0, value=1.0, interp_mode="Cubic", tangent_mode="Auto",
//      arrive_tangent=0, leave_tangent=0, tangent_weight_mode="None",
//      arrive_tangent_weight=0, leave_tangent_weight=0}
//   Aliases: "interp" == "interp_mode", "tangent" == "tangent_mode",
//            "weight_mode" == "tangent_weight_mode"
//   Enum strings are case-insensitive ("linear"/"Linear" both work).
//   Positional (timeline-legacy, only when bAllowPositional=true):
//     {0.0, 1.0}   -- [1]=time, [2]=value, no tangent/interp info
namespace NeoLuaCurve
{

// -- Enum parsing (case-insensitive, lowercase aliases accepted) ---------------

inline ERichCurveInterpMode ParseInterpMode(const FString& S, ERichCurveInterpMode Default = RCIM_Linear)
{
	if (S.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))   return RCIM_Linear;
	if (S.Equals(TEXT("Constant"), ESearchCase::IgnoreCase)) return RCIM_Constant;
	if (S.Equals(TEXT("Cubic"), ESearchCase::IgnoreCase))    return RCIM_Cubic;
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))     return RCIM_None;
	return Default;
}

inline ERichCurveTangentMode ParseTangentMode(const FString& S, ERichCurveTangentMode Default = RCTM_Auto)
{
	if (S.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))       return RCTM_Auto;
	if (S.Equals(TEXT("User"), ESearchCase::IgnoreCase))       return RCTM_User;
	if (S.Equals(TEXT("Break"), ESearchCase::IgnoreCase))      return RCTM_Break;
	if (S.Equals(TEXT("SmartAuto"), ESearchCase::IgnoreCase) ||
	    S.Equals(TEXT("Smart_Auto"), ESearchCase::IgnoreCase) ||
	    S.Equals(TEXT("Smart Auto"), ESearchCase::IgnoreCase)) return RCTM_SmartAuto;
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))       return RCTM_None;
	return Default;
}

inline ERichCurveTangentWeightMode ParseTangentWeightMode(const FString& S, ERichCurveTangentWeightMode Default = RCTWM_WeightedNone)
{
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))   return RCTWM_WeightedNone;
	if (S.Equals(TEXT("Arrive"), ESearchCase::IgnoreCase)) return RCTWM_WeightedArrive;
	if (S.Equals(TEXT("Leave"), ESearchCase::IgnoreCase))  return RCTWM_WeightedLeave;
	if (S.Equals(TEXT("Both"), ESearchCase::IgnoreCase))   return RCTWM_WeightedBoth;
	return Default;
}

inline ERichCurveExtrapolation ParseExtrapolation(const FString& S, ERichCurveExtrapolation Default = RCCE_Constant)
{
	if (S.Equals(TEXT("Cycle"), ESearchCase::IgnoreCase))           return RCCE_Cycle;
	if (S.Equals(TEXT("CycleWithOffset"), ESearchCase::IgnoreCase) ||
	    S.Equals(TEXT("Cycle_With_Offset"), ESearchCase::IgnoreCase)) return RCCE_CycleWithOffset;
	if (S.Equals(TEXT("Oscillate"), ESearchCase::IgnoreCase))       return RCCE_Oscillate;
	if (S.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))          return RCCE_Linear;
	if (S.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))        return RCCE_Constant;
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))            return RCCE_None;
	return Default;
}

// -- String serialization (for read paths) -------------------------------------

inline const TCHAR* InterpModeToString(ERichCurveInterpMode M)
{
	switch (M)
	{
	case RCIM_Linear:   return TEXT("Linear");
	case RCIM_Constant: return TEXT("Constant");
	case RCIM_Cubic:    return TEXT("Cubic");
	default:            return TEXT("None");
	}
}

inline const TCHAR* TangentModeToString(ERichCurveTangentMode M)
{
	switch (M)
	{
	case RCTM_Auto:      return TEXT("Auto");
	case RCTM_User:      return TEXT("User");
	case RCTM_Break:     return TEXT("Break");
	case RCTM_SmartAuto: return TEXT("SmartAuto");
	default:             return TEXT("None");
	}
}

inline const TCHAR* TangentWeightModeToString(ERichCurveTangentWeightMode M)
{
	switch (M)
	{
	case RCTWM_WeightedNone:   return TEXT("None");
	case RCTWM_WeightedArrive: return TEXT("Arrive");
	case RCTWM_WeightedLeave:  return TEXT("Leave");
	case RCTWM_WeightedBoth:   return TEXT("Both");
	default:                   return TEXT("None");
	}
}

inline const TCHAR* ExtrapolationToString(ERichCurveExtrapolation E)
{
	switch (E)
	{
	case RCCE_Cycle:           return TEXT("Cycle");
	case RCCE_CycleWithOffset: return TEXT("CycleWithOffset");
	case RCCE_Oscillate:       return TEXT("Oscillate");
	case RCCE_Linear:          return TEXT("Linear");
	case RCCE_Constant:        return TEXT("Constant");
	default:                   return TEXT("None");
	}
}

// -- Lua-table field readers ---------------------------------------------------

// Read time+value from a key table. Returns true if both were found via any
// accepted form. On success, OutTime/OutValue are populated.
//
// bAllowPositional: when true, {0.5, 1.0} is parsed as {time=0.5, value=1.0}
// for backward compatibility with existing timeline scripts.
inline bool ReadKeyTimeValue(const sol::table& K, float& OutTime, float& OutValue, bool bAllowPositional)
{
	bool bGotTime = false;
	bool bGotValue = false;

	sol::object TimeObj = K["time"];
	if (TimeObj.valid() && TimeObj.is<double>())
	{
		OutTime = static_cast<float>(TimeObj.as<double>());
		bGotTime = true;
	}

	sol::object ValueObj = K["value"];
	if (ValueObj.valid() && ValueObj.is<double>())
	{
		OutValue = static_cast<float>(ValueObj.as<double>());
		bGotValue = true;
	}

	if (bAllowPositional)
	{
		if (!bGotTime)
		{
			sol::object P1 = K[1];
			if (P1.valid() && P1.is<double>())
			{
				OutTime = static_cast<float>(P1.as<double>());
				bGotTime = true;
			}
		}
		if (!bGotValue)
		{
			sol::object P2 = K[2];
			if (P2.valid() && P2.is<double>())
			{
				OutValue = static_cast<float>(P2.as<double>());
				bGotValue = true;
			}
		}
	}

	return bGotTime && bGotValue;
}

// -- Core parser: Lua key table -> FRichCurveKey ------------------------------

// Parse a single Lua key table into an FRichCurveKey. Returns true on success,
// false + session warning if time/value couldn't be resolved.
//
// Recognized fields (all optional except time+value):
//   time, value, interp_mode|interp, tangent_mode|tangent,
//   tangent_weight_mode|weight_mode,
//   arrive_tangent, leave_tangent, arrive_tangent_weight, leave_tangent_weight
inline bool ParseKey(const sol::table& K, FRichCurveKey& OutKey, bool bAllowPositional = false)
{
	float Time = 0.f, Value = 0.f;
	if (!ReadKeyTimeValue(K, Time, Value, bAllowPositional))
	{
		return false;
	}

	OutKey = FRichCurveKey(Time, Value);

	auto ReadStr = [&K](const char* A, const char* B) -> FString
	{
		sol::object O = K[A];
		if (O.valid() && O.is<std::string>()) return UTF8_TO_TCHAR(O.as<std::string>().c_str());
		if (B)
		{
			O = K[B];
			if (O.valid() && O.is<std::string>()) return UTF8_TO_TCHAR(O.as<std::string>().c_str());
		}
		return FString();
	};

	const FString InterpStr = ReadStr("interp_mode", "interp");
	if (!InterpStr.IsEmpty()) OutKey.InterpMode = ParseInterpMode(InterpStr, OutKey.InterpMode);

	const FString TangentStr = ReadStr("tangent_mode", "tangent");
	if (!TangentStr.IsEmpty()) OutKey.TangentMode = ParseTangentMode(TangentStr, OutKey.TangentMode);

	const FString WeightStr = ReadStr("tangent_weight_mode", "weight_mode");
	if (!WeightStr.IsEmpty()) OutKey.TangentWeightMode = ParseTangentWeightMode(WeightStr, OutKey.TangentWeightMode);

	auto ReadFloat = [&K](const char* Name, float& Out) -> bool
	{
		sol::object O = K[Name];
		if (O.valid() && O.is<double>()) { Out = static_cast<float>(O.as<double>()); return true; }
		return false;
	};

	bool bExplicitTangent = false;
	if (ReadFloat("arrive_tangent", OutKey.ArriveTangent)) bExplicitTangent = true;
	if (ReadFloat("leave_tangent", OutKey.LeaveTangent))   bExplicitTangent = true;
	ReadFloat("arrive_tangent_weight", OutKey.ArriveTangentWeight);
	ReadFloat("leave_tangent_weight", OutKey.LeaveTangentWeight);

	// If caller passed explicit tangent values and didn't override tangent_mode,
	// switch to User so the values aren't overwritten by AutoSetTangents().
	if (bExplicitTangent && TangentStr.IsEmpty())
	{
		OutKey.TangentMode = RCTM_User;
	}

	return true;
}

// Parse a Lua array of key tables into a sorted TArray<FRichCurveKey>.
// Returns the number of keys successfully parsed. Logs a warning for each
// malformed entry via Session.
inline int32 ParseKeysToArray(const sol::table& KeysArr, TArray<FRichCurveKey>& OutKeys,
	FLuaSessionData& Session, const FString& Context, bool bAllowPositional = false)
{
	int32 SkippedCount = 0;
	int32 Index = 0;
	for (auto& Pair : KeysArr)
	{
		++Index;
		if (!Pair.second.is<sol::table>())
		{
			Session.Log(FString::Printf(TEXT("[WARN] %s key #%d is not a table, skipped"), *Context, Index));
			++SkippedCount;
			continue;
		}
		FRichCurveKey NewKey;
		if (!ParseKey(Pair.second.as<sol::table>(), NewKey, bAllowPositional))
		{
			Session.Log(FString::Printf(TEXT("[WARN] %s key #%d has no usable time/value%s, skipped"),
				*Context, Index, bAllowPositional ? TEXT(" (accepts {time=, value=} or {time, value})") : TEXT(" (needs {time=, value=})")));
			++SkippedCount;
			continue;
		}
		OutKeys.Add(NewKey);
	}

	OutKeys.Sort([](const FRichCurveKey& A, const FRichCurveKey& B) { return A.Time < B.Time; });
	return OutKeys.Num();
}

// -- Post-add property application (for handle-based flows) -------------------

// Apply interp/tangent/weight properties from a params table to an existing key
// on an FRichCurve, via the engine's setter APIs (which handle auto-tangent
// refresh correctly). Use this after AddKey/UpdateOrAddKey when you want to
// reapply properties parsed from Lua.
inline void ApplyKeyProperties(FRichCurve* Curve, FKeyHandle Handle, const sol::table& P)
{
	if (!Curve || !Curve->IsKeyHandleValid(Handle)) return;

	auto ReadStr = [&P](const char* A, const char* B) -> FString
	{
		sol::object O = P[A];
		if (O.valid() && O.is<std::string>()) return UTF8_TO_TCHAR(O.as<std::string>().c_str());
		if (B)
		{
			O = P[B];
			if (O.valid() && O.is<std::string>()) return UTF8_TO_TCHAR(O.as<std::string>().c_str());
		}
		return FString();
	};

	const FString InterpStr = ReadStr("interp_mode", "interp");
	if (!InterpStr.IsEmpty())
	{
		Curve->SetKeyInterpMode(Handle, ParseInterpMode(InterpStr), false);
	}

	const FString TangentStr = ReadStr("tangent_mode", "tangent");
	if (!TangentStr.IsEmpty())
	{
		Curve->SetKeyTangentMode(Handle, ParseTangentMode(TangentStr), false);
	}

	const FString WeightStr = ReadStr("tangent_weight_mode", "weight_mode");
	if (!WeightStr.IsEmpty())
	{
		Curve->SetKeyTangentWeightMode(Handle, ParseTangentWeightMode(WeightStr), false);
	}

	auto ReadFloat = [&P](const char* Name) -> TOptional<float>
	{
		sol::object O = P[Name];
		if (O.valid() && O.is<double>()) return static_cast<float>(O.as<double>());
		return TOptional<float>();
	};

	TOptional<float> Arrive = ReadFloat("arrive_tangent");
	TOptional<float> Leave = ReadFloat("leave_tangent");
	TOptional<float> ArriveW = ReadFloat("arrive_tangent_weight");
	TOptional<float> LeaveW = ReadFloat("leave_tangent_weight");

	if (Arrive.IsSet() || Leave.IsSet() || ArriveW.IsSet() || LeaveW.IsSet())
	{
		FRichCurveKey& Key = Curve->GetKey(Handle);
		if (Arrive.IsSet())  Key.ArriveTangent = Arrive.GetValue();
		if (Leave.IsSet())   Key.LeaveTangent = Leave.GetValue();
		if (ArriveW.IsSet()) Key.ArriveTangentWeight = ArriveW.GetValue();
		if (LeaveW.IsSet())  Key.LeaveTangentWeight = LeaveW.GetValue();
	}

	// For Cubic + Auto/SmartAuto, recompute tangents so auto-curves stay smooth.
	const ERichCurveInterpMode FinalInterp = Curve->GetKeyInterpMode(Handle);
	const ERichCurveTangentMode FinalTangent = Curve->GetKeyTangentMode(Handle);
	if (FinalInterp == RCIM_Cubic && (FinalTangent == RCTM_Auto || FinalTangent == RCTM_SmartAuto))
	{
		Curve->AutoSetTangents();
	}
}

// -- Multi-channel key readers (for color and vector curves) -------------------

// Read time + per-channel float values from a key table. Supports:
//   {time=0.5, r=1, g=0, b=0, a=1}               -- color form
//   {time=0.5, color={r=1, g=0, b=0, a=1}}       -- nested color form
//   {time=0.5, x=1, y=0, z=0}                    -- vector form
//   {time=0.5, vector={x=1, y=0, z=0}}           -- nested vector form
//
// ChannelNames is an array of field names in the order they map to OutValues.
// Defaults let unspecified channels retain the supplied DefaultValues.
inline bool ReadMultiChannelKey(const sol::table& K, float& OutTime,
	TArrayView<const TCHAR*> ChannelNames, float* OutValues, TArrayView<const float> DefaultValues,
	const TCHAR* NestedTableName = nullptr)
{
	sol::object TimeObj = K["time"];
	if (!TimeObj.valid() || !TimeObj.is<double>()) return false;
	OutTime = static_cast<float>(TimeObj.as<double>());

	const sol::table* Source = &K;
	sol::table NestedBuf;
	if (NestedTableName)
	{
		sol::object NestedObj = K[TCHAR_TO_UTF8(NestedTableName)];
		if (NestedObj.valid() && NestedObj.is<sol::table>())
		{
			NestedBuf = NestedObj.as<sol::table>();
			Source = &NestedBuf;
		}
	}

	for (int32 i = 0; i < ChannelNames.Num(); ++i)
	{
		const float Default = (i < DefaultValues.Num()) ? DefaultValues[i] : 0.f;
		sol::object ChObj = (*Source)[TCHAR_TO_UTF8(ChannelNames[i])];
		if (ChObj.valid() && ChObj.is<double>())
		{
			OutValues[i] = static_cast<float>(ChObj.as<double>());
		}
		else
		{
			OutValues[i] = Default;
		}
	}
	return true;
}

} // namespace NeoLuaCurve
