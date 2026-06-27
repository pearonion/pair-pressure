// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Materials/MaterialParameterCollection.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

// UMaterialParameterCollection's effective read APIs are exported in UE 5.7, so prefer
// the engine path that understands base collections and per-collection override maps.
static const UMaterialParameterCollection* NSAI_NextMPCInChainConst(const UMaterialParameterCollection* Collection)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return Collection ? Collection->GetBaseParameterCollection() : nullptr;
#else
	(void)Collection;
	return nullptr;
#endif
}

static void NSAI_AppendScalarParameterNames(const UMaterialParameterCollection* MPC, TArray<FName>& OutNames)
{
	if (!MPC) return;
	NSAI_AppendScalarParameterNames(NSAI_NextMPCInChainConst(MPC), OutNames); // base names first, matching engine order
	for (const FCollectionScalarParameter& Param : MPC->ScalarParameters) OutNames.Add(Param.ParameterName);
}

static void NSAI_AppendVectorParameterNames(const UMaterialParameterCollection* MPC, TArray<FName>& OutNames)
{
	if (!MPC) return;
	NSAI_AppendVectorParameterNames(NSAI_NextMPCInChainConst(MPC), OutNames); // base names first, matching engine order
	for (const FCollectionVectorParameter& Param : MPC->VectorParameters) OutNames.Add(Param.ParameterName);
}

static TArray<FName> NSAI_GetScalarParameterNames(const UMaterialParameterCollection* MPC)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return MPC ? MPC->GetScalarParameterNames() : TArray<FName>();
#else
	TArray<FName> Names;
	NSAI_AppendScalarParameterNames(MPC, Names);
	return Names;
#endif
}

static TArray<FName> NSAI_GetVectorParameterNames(const UMaterialParameterCollection* MPC)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return MPC ? MPC->GetVectorParameterNames() : TArray<FName>();
#else
	TArray<FName> Names;
	NSAI_AppendVectorParameterNames(MPC, Names);
	return Names;
#endif
}

static float NSAI_GetScalarParameterDefaultValue(const UMaterialParameterCollection* MPC, FName ParameterName, bool& bParameterFound)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return MPC ? MPC->GetScalarParameterDefaultValue(ParameterName, bParameterFound) : (bParameterFound = false, 0.0f);
#else
	for (const UMaterialParameterCollection* Collection = MPC; Collection; Collection = NSAI_NextMPCInChainConst(Collection))
	{
		for (const FCollectionScalarParameter& Param : Collection->ScalarParameters)
		{
			if (Param.ParameterName == ParameterName)
			{
				bParameterFound = true;
				return Param.DefaultValue;
			}
		}
	}
	bParameterFound = false;
	return 0.0f;
#endif
}

static FLinearColor NSAI_GetVectorParameterDefaultValue(const UMaterialParameterCollection* MPC, FName ParameterName, bool& bParameterFound)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return MPC ? MPC->GetVectorParameterDefaultValue(ParameterName, bParameterFound) : (bParameterFound = false, FLinearColor::Black);
#else
	for (const UMaterialParameterCollection* Collection = MPC; Collection; Collection = NSAI_NextMPCInChainConst(Collection))
	{
		for (const FCollectionVectorParameter& Param : Collection->VectorParameters)
		{
			if (Param.ParameterName == ParameterName)
			{
				bParameterFound = true;
				return Param.DefaultValue;
			}
		}
	}
	bParameterFound = false;
	return FLinearColor::Black;
#endif
}

static double StableLuaFloat(float Value)
{
	return FMath::RoundToDouble(static_cast<double>(Value) * 1000000.0) / 1000000.0;
}

static sol::table ScalarParamToTable(sol::state_view& Lua, const FCollectionScalarParameter& Param)
{
	sol::table E = Lua.create_table();
	E["name"] = TCHAR_TO_UTF8(*Param.ParameterName.ToString());
	E["default_value"] = StableLuaFloat(Param.DefaultValue);
	E["id"] = TCHAR_TO_UTF8(*Param.Id.ToString());
	return E;
}

static bool DoesOwnScalarParameter(const UMaterialParameterCollection* MPC, FName ParameterName)
{
	if (!MPC) return false;
	return MPC->ScalarParameters.ContainsByPredicate([ParameterName](const FCollectionScalarParameter& Param)
	{
		return Param.ParameterName == ParameterName;
	});
}

static bool DoesOwnVectorParameter(const UMaterialParameterCollection* MPC, FName ParameterName)
{
	if (!MPC) return false;
	return MPC->VectorParameters.ContainsByPredicate([ParameterName](const FCollectionVectorParameter& Param)
	{
		return Param.ParameterName == ParameterName;
	});
}

static bool DoesParameterNameExist(const UMaterialParameterCollection* MPC, FName ParameterName)
{
	if (!MPC) return false;
	return MPC->GetScalarParameterByName(ParameterName) != nullptr
		|| MPC->GetVectorParameterByName(ParameterName) != nullptr;
}

static sol::table ScalarParamToTable(sol::state_view& Lua, const UMaterialParameterCollection* MPC, FName ParameterName)
{
	sol::table E = Lua.create_table();
	E["name"] = TCHAR_TO_UTF8(*ParameterName.ToString());

	bool bFound = false;
	const float DefaultValue = NSAI_GetScalarParameterDefaultValue(MPC, ParameterName, bFound);
	E["default_value"] = StableLuaFloat(DefaultValue);
	E["owned"] = DoesOwnScalarParameter(MPC, ParameterName);

	if (const FCollectionScalarParameter* Param = MPC->GetScalarParameterByName(ParameterName))
	{
		E["id"] = TCHAR_TO_UTF8(*Param->Id.ToString());
	}
	E["found"] = bFound;
	return E;
}

static sol::table VectorParamToTable(sol::state_view& Lua, const FCollectionVectorParameter& Param)
{
	sol::table E = Lua.create_table();
	E["name"] = TCHAR_TO_UTF8(*Param.ParameterName.ToString());
	sol::table Color = Lua.create_table();
	Color["r"] = StableLuaFloat(Param.DefaultValue.R);
	Color["g"] = StableLuaFloat(Param.DefaultValue.G);
	Color["b"] = StableLuaFloat(Param.DefaultValue.B);
	Color["a"] = StableLuaFloat(Param.DefaultValue.A);
	E["default_value"] = Color;
	E["id"] = TCHAR_TO_UTF8(*Param.Id.ToString());
	return E;
}

static sol::table VectorParamToTable(sol::state_view& Lua, const UMaterialParameterCollection* MPC, FName ParameterName)
{
	sol::table E = Lua.create_table();
	E["name"] = TCHAR_TO_UTF8(*ParameterName.ToString());

	bool bFound = false;
	const FLinearColor DefaultValue = NSAI_GetVectorParameterDefaultValue(MPC, ParameterName, bFound);
	sol::table Color = Lua.create_table();
	Color["r"] = StableLuaFloat(DefaultValue.R);
	Color["g"] = StableLuaFloat(DefaultValue.G);
	Color["b"] = StableLuaFloat(DefaultValue.B);
	Color["a"] = StableLuaFloat(DefaultValue.A);
	E["default_value"] = Color;
	E["owned"] = DoesOwnVectorParameter(MPC, ParameterName);

	if (const FCollectionVectorParameter* Param = MPC->GetVectorParameterByName(ParameterName))
	{
		E["id"] = TCHAR_TO_UTF8(*Param->Id.ToString());
	}
	E["found"] = bFound;
	return E;
}

static FLinearColor LuaTableToLinearColor(const sol::table& Color, FLinearColor Result = FLinearColor::Black)
{
	Result.R = static_cast<float>(Color.get_or("r", static_cast<double>(Result.R)));
	Result.G = static_cast<float>(Color.get_or("g", static_cast<double>(Result.G)));
	Result.B = static_cast<float>(Color.get_or("b", static_cast<double>(Result.B)));
	Result.A = static_cast<float>(Color.get_or("a", static_cast<double>(Result.A)));
	return Result;
}

// UMaterialParameterCollection::GetBaseParameterCollection (parent-collection chain)
// was added in UE 5.7. On older engines the chain collapses to just the head MPC.
static UMaterialParameterCollection* NSAI_NextMPCInChain(UMaterialParameterCollection* Collection)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return Collection ? Collection->GetBaseParameterCollection() : nullptr;
#else
	(void)Collection;
	return nullptr;
#endif
}

static FCollectionScalarParameter* FindScalarParameterInCollectionChain(UMaterialParameterCollection* MPC, FName ParameterName, UMaterialParameterCollection*& OutOwner)
{
	OutOwner = nullptr;
	for (UMaterialParameterCollection* Collection = MPC; Collection; Collection = NSAI_NextMPCInChain(Collection))
	{
		for (FCollectionScalarParameter& Param : Collection->ScalarParameters)
		{
			if (Param.ParameterName == ParameterName)
			{
				OutOwner = Collection;
				return &Param;
			}
		}
	}
	return nullptr;
}

static FCollectionVectorParameter* FindVectorParameterInCollectionChain(UMaterialParameterCollection* MPC, FName ParameterName, UMaterialParameterCollection*& OutOwner)
{
	OutOwner = nullptr;
	for (UMaterialParameterCollection* Collection = MPC; Collection; Collection = NSAI_NextMPCInChain(Collection))
	{
		for (FCollectionVectorParameter& Param : Collection->VectorParameters)
		{
			if (Param.ParameterName == ParameterName)
			{
				OutOwner = Collection;
				return &Param;
			}
		}
	}
	return nullptr;
}

template<typename MapType>
static MapType* GetMPCOverrideMap(UMaterialParameterCollection* MPC, const TCHAR* PropertyName)
{
	if (!MPC) return nullptr;
	FMapProperty* MapProp = FindFProperty<FMapProperty>(UMaterialParameterCollection::StaticClass(), FName(PropertyName));
	if (!MapProp) return nullptr;
	return reinterpret_cast<MapType*>(MapProp->ContainerPtrToValuePtr<void>(MPC));
}

static bool ApplyScalarDefaultValue(UMaterialParameterCollection* MPC, FName ParameterName, float Value)
{
	UMaterialParameterCollection* Owner = nullptr;
	FCollectionScalarParameter* Param = FindScalarParameterInCollectionChain(MPC, ParameterName, Owner);
	if (!Param) return false;
	if (Owner == MPC)
	{
		Param->DefaultValue = Value;
		return true;
	}

	TMap<FGuid, float>* Overrides = GetMPCOverrideMap<TMap<FGuid, float>>(MPC, TEXT("ScalarParameterBaseOverrides"));
	if (!Overrides) return false;
	Overrides->FindOrAdd(Param->Id) = Value;
	return true;
}

static bool ApplyVectorDefaultValue(UMaterialParameterCollection* MPC, FName ParameterName, const FLinearColor& Value)
{
	UMaterialParameterCollection* Owner = nullptr;
	FCollectionVectorParameter* Param = FindVectorParameterInCollectionChain(MPC, ParameterName, Owner);
	if (!Param) return false;
	if (Owner == MPC)
	{
		Param->DefaultValue = Value;
		return true;
	}

	TMap<FGuid, FLinearColor>* Overrides = GetMPCOverrideMap<TMap<FGuid, FLinearColor>>(MPC, TEXT("VectorParameterBaseOverrides"));
	if (!Overrides) return false;
	Overrides->FindOrAdd(Param->Id) = Value;
	return true;
}

// Wraps PreEditChange + Modify + mutation + PostEditChangeProperty + MarkPackageDirty
static void MPCPreEdit(UMaterialParameterCollection* MPC)
{
	MPC->PreEditChange(nullptr);
	MPC->Modify();
}

static void MPCPostEdit(UMaterialParameterCollection* MPC, EPropertyChangeType::Type ChangeType)
{
	FPropertyChangedEvent Event(nullptr, ChangeType);
	MPC->PostEditChangeProperty(Event);
	MPC->MarkPackageDirty();
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> MaterialParamCollectionDocs = {};

static void BindMaterialParamCollection(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_material_param_collection", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UMaterialParameterCollection* MPC = NeoLuaAsset::Resolve<UMaterialParameterCollection>(FPath);
		if (!MPC) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"MaterialParameterCollection enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  scalar_param_count, vector_param_count, owned_scalar_param_count, owned_vector_param_count, state_id, base_collection\n"
			"\n"
			"list(type):\n"
			"  list() or list(\"all\") — effective {scalars=[...], vectors=[...]} including base collections and overrides\n"
			"  list(\"scalars\") — effective array of {name, default_value, id, owned}\n"
			"  list(\"vectors\") — effective array of {name, default_value={r,g,b,a}, id, owned}\n"
			"  list(\"owned_scalars\") / list(\"owned_vectors\") — local array entries only\n"
			"\n"
			"add(type, params):\n"
			"  add(\"scalar\", {name=\"MyParam\", default_value=0.5})\n"
			"  add(\"vector\", {name=\"MyColor\", default_value={r=1,g=0,b=0,a=1}})\n"
			"\n"
			"remove(type, name):\n"
			"  remove(\"scalar\", \"ParamName\")\n"
			"  remove(\"vector\", \"ParamName\")\n"
			"\n"
			"configure(type, name, params):\n"
			"  configure(\"scalar\", \"ParamName\", {default_value=0.8}) — edits local defaults or derived overrides\n"
			"  configure(\"vector\", \"ParamName\", {default_value={r=1,g=1,b=1,a=1}}) — edits local defaults or derived overrides\n"
			"  use configure_at(\"ScalarParameters\"|\"VectorParameters\", name, {ParameterName=\"Renamed\"}) for owned-array renames only\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [MPC, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			TArray<FName> ScalarNames = NSAI_GetScalarParameterNames(MPC);
			TArray<FName> VectorNames = NSAI_GetVectorParameterNames(MPC);

			sol::table Result = Lua.create_table();
			Result["scalar_param_count"] = ScalarNames.Num();
			Result["vector_param_count"] = VectorNames.Num();
			Result["owned_scalar_param_count"] = MPC->ScalarParameters.Num();
			Result["owned_vector_param_count"] = MPC->VectorParameters.Num();
			Result["state_id"] = TCHAR_TO_UTF8(*MPC->StateId.ToString());

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			UMaterialParameterCollection* Base = MPC->GetBaseParameterCollection();
			if (Base)
			{
				Result["base_collection"] = TCHAR_TO_UTF8(*Base->GetPathName());
			}
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> MPC, %d scalars, %d vectors"),
				ScalarNames.Num(), VectorNames.Num()));
			return Result;
		});

		// ================================================================
		// list(type?)
		// ================================================================
		AssetObj.set_function("list", [MPC, &Session](sol::table /*self*/,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFStringOpt(TypeOpt, TEXT("all"));

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();

				sol::table Scalars = Lua.create_table();
				TArray<FName> ScalarNames = NSAI_GetScalarParameterNames(MPC);
				for (int32 i = 0; i < ScalarNames.Num(); i++)
				{
					Scalars[i + 1] = ScalarParamToTable(Lua, MPC, ScalarNames[i]);
				}
				Result["scalars"] = Scalars;

				sol::table Vectors = Lua.create_table();
				TArray<FName> VectorNames = NSAI_GetVectorParameterNames(MPC);
				for (int32 i = 0; i < VectorNames.Num(); i++)
				{
					Vectors[i + 1] = VectorParamToTable(Lua, MPC, VectorNames[i]);
				}
				Result["vectors"] = Vectors;

				Session.Log(FString::Printf(TEXT("[OK] list(\"all\") -> %d scalars, %d vectors"),
					ScalarNames.Num(), VectorNames.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("scalars"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				TArray<FName> ScalarNames = NSAI_GetScalarParameterNames(MPC);
				for (int32 i = 0; i < ScalarNames.Num(); i++)
				{
					Result[i + 1] = ScalarParamToTable(Lua, MPC, ScalarNames[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"scalars\") -> %d"), ScalarNames.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("vectors"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				TArray<FName> VectorNames = NSAI_GetVectorParameterNames(MPC);
				for (int32 i = 0; i < VectorNames.Num(); i++)
				{
					Result[i + 1] = VectorParamToTable(Lua, MPC, VectorNames[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"vectors\") -> %d"), VectorNames.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("owned_scalars"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("owned_scalar"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < MPC->ScalarParameters.Num(); i++)
				{
					Result[i + 1] = ScalarParamToTable(Lua, MPC->ScalarParameters[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"owned_scalars\") -> %d"), MPC->ScalarParameters.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("owned_vectors"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("owned_vector"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < MPC->VectorParameters.Num(); i++)
				{
					Result[i + 1] = VectorParamToTable(Lua, MPC->VectorParameters[i]);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"owned_vectors\") -> %d"), MPC->VectorParameters.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: all, scalars, vectors, owned_scalars, owned_vectors"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [MPC, &Session](sol::table /*self*/,
			std::string TypeStr, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = NeoLuaStr::ToFString(TypeStr);
			std::string NameStr = Params.get_or<std::string>("name", "");
			if (NameStr.empty())
			{
				Session.Log(TEXT("[FAIL] add -> 'name' is required"));
				return sol::lua_nil;
			}
			FName ParamName = FName(NeoLuaStr::ToFString(NameStr));

			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				if (DoesParameterNameExist(MPC, ParamName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"scalar\") -> '%s' already exists in this collection"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				FCollectionScalarParameter NewParam;
				NewParam.ParameterName = ParamName;
				NewParam.DefaultValue = static_cast<float>(Params.get_or("default_value", 0.0));

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Add Scalar Parameter")));
				MPCPreEdit(MPC);
				MPC->ScalarParameters.Add(NewParam);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayAdd);

				Session.Log(FString::Printf(TEXT("[OK] add(\"scalar\", \"%s\") -> default=%.4f"),
					*ParamName.ToString(), NewParam.DefaultValue + 0.0));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				if (DoesParameterNameExist(MPC, ParamName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"vector\") -> '%s' already exists in this collection"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				FCollectionVectorParameter NewParam;
				NewParam.ParameterName = ParamName;

				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("default_value");
				if (ColorOpt.has_value())
				{
					sol::table Color = ColorOpt.value();
					NewParam.DefaultValue.R = static_cast<float>(Color.get_or("r", 0.0));
					NewParam.DefaultValue.G = static_cast<float>(Color.get_or("g", 0.0));
					NewParam.DefaultValue.B = static_cast<float>(Color.get_or("b", 0.0));
					NewParam.DefaultValue.A = static_cast<float>(Color.get_or("a", 1.0));
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Add Vector Parameter")));
				MPCPreEdit(MPC);
				MPC->VectorParameters.Add(NewParam);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayAdd);

				Session.Log(FString::Printf(TEXT("[OK] add(\"vector\", \"%s\") -> default=(%.2f, %.2f, %.2f, %.2f)"),
					*ParamName.ToString(),
					NewParam.DefaultValue.R + 0.0, NewParam.DefaultValue.G + 0.0,
					NewParam.DefaultValue.B + 0.0, NewParam.DefaultValue.A + 0.0));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: scalar, vector"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, name)
		// ================================================================
		AssetObj.set_function("remove", [MPC, &Session](sol::table /*self*/,
			std::string TypeStr, std::string NameStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = NeoLuaStr::ToFString(TypeStr);
			FName ParamName = FName(NeoLuaStr::ToFString(NameStr));

			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				int32 Idx = MPC->ScalarParameters.IndexOfByPredicate([&ParamName](const FCollectionScalarParameter& P) { return P.ParameterName == ParamName; });
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"scalar\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Remove Scalar Parameter")));
				MPCPreEdit(MPC);
				MPC->ScalarParameters.RemoveAt(Idx);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayRemove);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"scalar\", \"%s\")"), *ParamName.ToString()));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				int32 Idx = MPC->VectorParameters.IndexOfByPredicate([&ParamName](const FCollectionVectorParameter& P) { return P.ParameterName == ParamName; });
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"vector\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Remove Vector Parameter")));
				MPCPreEdit(MPC);
				MPC->VectorParameters.RemoveAt(Idx);
				MPCPostEdit(MPC, EPropertyChangeType::ArrayRemove);

				Session.Log(FString::Printf(TEXT("[OK] remove(\"vector\", \"%s\")"), *ParamName.ToString()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: scalar, vector"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, name, params)
		// ================================================================
		AssetObj.set_function("configure", [MPC, &Session](sol::table /*self*/,
			std::string TypeStr, std::string NameStr, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(MPC))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FType = NeoLuaStr::ToFString(TypeStr);
			FName ParamName = FName(NeoLuaStr::ToFString(NameStr));

			if (FType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				sol::object ValueObj = Params["default_value"];
				if (!ValueObj.valid() || ValueObj == sol::lua_nil)
				{
					Session.Log(TEXT("[FAIL] configure(\"scalar\") -> 'default_value' is required"));
					return sol::lua_nil;
				}
				if (MPC->GetScalarParameterByName(ParamName) == nullptr)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"scalar\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				const float NewValue = static_cast<float>(ValueObj.as<double>());
				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Set Scalar Default")));
				MPCPreEdit(MPC);
				const bool bOk = ApplyScalarDefaultValue(MPC, ParamName, NewValue);
				MPCPostEdit(MPC, EPropertyChangeType::ValueSet);
				if (!bOk)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"scalar\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] configure(\"scalar\", \"%s\") -> default=%.4f"),
					*ParamName.ToString(), NewValue + 0.0));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("default_value");
				if (!ColorOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"vector\") -> 'default_value' table is required"));
					return sol::lua_nil;
				}
				if (MPC->GetVectorParameterByName(ParamName) == nullptr)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"vector\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				bool bFound = false;
				const FLinearColor CurrentValue = NSAI_GetVectorParameterDefaultValue(MPC, ParamName, bFound);
				const FLinearColor NewValue = LuaTableToLinearColor(ColorOpt.value(), bFound ? CurrentValue : FLinearColor::Black);
				const FScopedTransaction Transaction(FText::FromString(TEXT("MPC: Set Vector Default")));
				MPCPreEdit(MPC);
				const bool bOk = ApplyVectorDefaultValue(MPC, ParamName, NewValue);
				MPCPostEdit(MPC, EPropertyChangeType::ValueSet);
				if (!bOk)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"vector\", \"%s\") -> not found"), *ParamName.ToString()));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] configure(\"vector\", \"%s\") -> default=(%.2f, %.2f, %.2f, %.2f)"),
					*ParamName.ToString(), NewValue.R + 0.0, NewValue.G + 0.0, NewValue.B + 0.0, NewValue.A + 0.0));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: scalar, vector"), *FType));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(MaterialParamCollection, MaterialParamCollectionDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindMaterialParamCollection(Lua, Session);
});
