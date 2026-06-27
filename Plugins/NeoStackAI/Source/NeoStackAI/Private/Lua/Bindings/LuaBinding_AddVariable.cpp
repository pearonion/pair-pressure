#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Lua/LuaStr.h"
#include "Blueprint/BlueprintUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static TArray<FParamDesc> ParseParamsTable(const sol::table& ParamsTable)
{
	TArray<FParamDesc> Params;
	for (auto& Pair : ParamsTable)
	{
		if (Pair.second.is<sol::table>())
		{
			sol::table P = Pair.second.as<sol::table>();
			FParamDesc Desc;
			Desc.Name = NeoLuaStr::ToFString(P.get_or<std::string>("name", ""));
			Desc.Type = NeoLuaStr::ToFString(P.get_or<std::string>("type", ""));
			if (!Desc.Name.IsEmpty() && !Desc.Type.IsEmpty())
			{
				Params.Add(MoveTemp(Desc));
			}
		}
	}
	return Params;
}

static void ApplyVariableOptions(UBlueprint* BP, const FString& VarName, const sol::table& Options, FLuaSessionData& Session)
{
	auto ApplyKey = [&](const char* Key, const char* PropKey)
	{
		sol::object Val = Options[Key];
		if (Val.valid() && Val.get_type() != sol::type::lua_nil)
		{
			FString StrVal = NeoLuaPin::SolToString(Val);
			if (NeoBlueprint::SetVariableProperty(BP, VarName, FString(PropKey), StrVal))
			{
				Session.Log(FString::Printf(TEXT("[OK] set %s.%s = %s"), *VarName, UTF8_TO_TCHAR(Key), *StrVal));
			}
		}
	};

	ApplyKey("category", "category");
	ApplyKey("tooltip", "tooltip");
	ApplyKey("replicated", "replicated");
	ApplyKey("rep_notify", "rep_notify");
	ApplyKey("edit_anywhere", "edit_anywhere");
	ApplyKey("edit_defaults_only", "edit_defaults_only");
	ApplyKey("edit_instance_only", "edit_instance_only");
	ApplyKey("blueprint_read_only", "blueprint_read_only");
	ApplyKey("save_game", "save_game");
	ApplyKey("transient", "transient");
	ApplyKey("expose_on_spawn", "expose_on_spawn");

	// Interp flag (expose to Sequencer/Cinematics)
	FName FVarName(*VarName);
	sol::object InterpObj = Options["interp"];
	if (InterpObj.valid() && InterpObj.get_type() != sol::type::lua_nil)
	{
		FBlueprintEditorUtils::SetInterpFlag(BP, FVarName, InterpObj.as<bool>());
		Session.Log(FString::Printf(TEXT("[OK] set %s.interp = %s"), *VarName, InterpObj.as<bool>() ? TEXT("true") : TEXT("false")));
	}

	// Advanced Display flag
	sol::object AdvancedObj = Options["advanced_display"];
	if (AdvancedObj.valid() && AdvancedObj.get_type() != sol::type::lua_nil)
	{
		FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(BP, FVarName, AdvancedObj.as<bool>());
		Session.Log(FString::Printf(TEXT("[OK] set %s.advanced_display = %s"), *VarName, AdvancedObj.as<bool>() ? TEXT("true") : TEXT("false")));
	}

	// Deprecated flag
	sol::object DeprecatedObj = Options["deprecated"];
	if (DeprecatedObj.valid() && DeprecatedObj.get_type() != sol::type::lua_nil)
	{
		FBlueprintEditorUtils::SetVariableDeprecatedFlag(BP, FVarName, DeprecatedObj.as<bool>());
		Session.Log(FString::Printf(TEXT("[OK] set %s.deprecated = %s"), *VarName, DeprecatedObj.as<bool>() ? TEXT("true") : TEXT("false")));
	}

	// Arbitrary metadata
	sol::optional<sol::table> MetaData = Options.get<sol::optional<sol::table>>("metadata");
	if (MetaData.has_value())
	{
		for (auto& KV : MetaData.value())
		{
			if (!KV.first.is<std::string>()) continue;
			FString MetaKey = NeoLuaStr::ToFString(KV.first.as<std::string>());
			if (KV.second.is<std::string>())
			{
				FString MetaValue = NeoLuaStr::ToFString(KV.second.as<std::string>());
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, FVarName, nullptr, FName(*MetaKey), MetaValue);
				Session.Log(FString::Printf(TEXT("[OK] set %s.metadata.%s = \"%s\""), *VarName, *MetaKey, *MetaValue));
			}
			else if (KV.second.is<sol::lua_nil_t>())
			{
				FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(BP, FVarName, nullptr, FName(*MetaKey));
				Session.Log(FString::Printf(TEXT("[OK] set %s.metadata.%s -> removed"), *VarName, *MetaKey));
			}
		}
	}
}

static TArray<FLuaFunctionDoc> AddVariableDocs = {
	{ TEXT("add_variable(blueprint_path, name, type, options?)"), TEXT("Add a member variable to a Blueprint — type can be int, float, string, vector, etc. Arrays: use 'int[]' or {container='array'}. Sets: 'set:type'. Maps: 'map:key:value'. Also supports event dispatchers, flags, metadata"), TEXT("true or nil") },
	{ TEXT("change_variable_type(blueprint_path, name, new_type)"), TEXT("Change the type of an existing member variable (or local with scope option)"), TEXT("true or nil") },
	{ TEXT("move_variable(blueprint_path, name, target_name, position?)"), TEXT("Reorder a variable — position='before' (default) or 'after' relative to target_name"), TEXT("true or nil") },
	{ TEXT("is_variable_used(blueprint_path, name)"), TEXT("Check if a variable is referenced by any node in the blueprint"), TEXT("true or false") },
	{ TEXT("remove_local_variable(blueprint_path, function_name, variable_name)"), TEXT("Remove a local variable from a function graph"), TEXT("true or nil") },
	{ TEXT("rename_local_variable(blueprint_path, function_name, old_name, new_name)"), TEXT("Rename a local variable in a function graph"), TEXT("true or nil") },
};

REGISTER_LUA_BINDING(AddVariable, AddVariableDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("add_variable", [&Session](const std::string& BPPath, const std::string& Name,
		const std::string& Type, sol::optional<sol::object> OptionsOrDefault, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FBPPath = NeoLuaStr::ToFString(BPPath);
		FString VarName = NeoLuaStr::ToFString(Name);
		FString FType = NeoLuaStr::ToFString(Type);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FBPPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_variable -> blueprint \"%s\" not found"), *FBPPath));
			return sol::lua_nil;
		}

		// Check for duplicate_from option (duplicate an existing variable)
		if (OptionsOrDefault.has_value() && OptionsOrDefault->is<sol::table>())
		{
			sol::table OptsTbl = OptionsOrDefault->as<sol::table>();
			std::string DupFrom = OptsTbl.get_or<std::string>("duplicate_from", "");
			if (!DupFrom.empty())
			{
				FString SourceVarName = NeoLuaStr::ToFString(DupFrom);
				FName DupNewName = FBlueprintEditorUtils::DuplicateVariable(Info.Blueprint, nullptr, FName(*SourceVarName));
				if (DupNewName != NAME_None)
				{
					// Rename to requested name if different
					if (!VarName.Equals(DupNewName.ToString(), ESearchCase::IgnoreCase))
					{
						NeoBlueprint::RenameVariable(Info.Blueprint, DupNewName.ToString(), VarName);
						DupNewName = FName(*VarName);
					}
					Session.Log(FString::Printf(TEXT("[OK] add_variable(\"%s\") -> duplicated from \"%s\""), *VarName, *SourceVarName));
					return sol::make_object(S, std::string(TCHAR_TO_UTF8(*DupNewName.ToString())));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_variable(\"%s\") -> failed to duplicate from \"%s\" (source not found?)"), *VarName, *SourceVarName));
					return sol::lua_nil;
				}
			}
		}

		// Check if this is an event dispatcher
		FString TypeLower = FType.ToLower();
		if (TypeLower == TEXT("mcdelegate") || TypeLower == TEXT("delegate") || TypeLower == TEXT("event_dispatcher") || TypeLower == TEXT("dispatcher"))
		{
			TArray<FParamDesc> Params;
			if (OptionsOrDefault.has_value() && OptionsOrDefault->is<sol::table>())
			{
				sol::table Opts = OptionsOrDefault->as<sol::table>();
				sol::object ParamsObj = Opts["params"];
				if (ParamsObj.valid() && ParamsObj.is<sol::table>())
				{
					Params = ParseParamsTable(ParamsObj.as<sol::table>());
				}
			}

			UEdGraph* SigGraph = NeoBlueprint::AddEventDispatcher(Info.Blueprint, VarName, Params);
			if (!SigGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_variable(\"%s\", \"mcdelegate\") -> failed to create event dispatcher"), *VarName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_variable(\"%s\", \"mcdelegate\") -> event dispatcher created with %d params"),
				*VarName, Params.Num()));

			// Apply non-param options (category, tooltip, etc.)
			if (OptionsOrDefault.has_value() && OptionsOrDefault->is<sol::table>())
			{
				ApplyVariableOptions(Info.Blueprint, VarName, OptionsOrDefault->as<sol::table>(), Session);
			}

			return sol::make_object(S, true);
		}

		// Regular variable
		FString FDefault;
		sol::table OptionsTable;
		bool bHasOptions = false;

		if (OptionsOrDefault.has_value())
		{
			if (OptionsOrDefault->is<sol::table>())
			{
				OptionsTable = OptionsOrDefault->as<sol::table>();
				bHasOptions = true;
				// Extract default from options table
				sol::object DefObj = OptionsTable["default"];
				if (DefObj.valid() && DefObj.get_type() != sol::type::lua_nil)
				{
					FDefault = NeoLuaPin::SolToString(DefObj);
				}
			}
			else
			{
				// Plain default value (string, number, bool)
				FDefault = NeoLuaPin::SolToString(OptionsOrDefault.value());
			}
		}

		// Check for scope option → local variable
		if (bHasOptions)
		{
			std::string ScopeStr = OptionsTable.get_or<std::string>("scope", "");
			if (!ScopeStr.empty())
			{
				FString FScope = NeoLuaStr::ToFString(ScopeStr);
				if (NeoBlueprint::AddLocalVariable(Info.Blueprint, FScope, VarName, FType, FDefault))
				{
					Session.Log(FString::Printf(TEXT("[OK] add_variable(\"%s\", \"%s\") -> local in \"%s\""),
						*VarName, *FType, *FScope));
					return sol::make_object(S, true);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_variable(\"%s\", scope=\"%s\") -> failed (function not found or bad type)"),
						*VarName, *FScope));
					return sol::lua_nil;
				}
			}
		}

		// Handle container option: {container="array"} or {container="set"} or {container="map", value_type="..."}
		if (bHasOptions)
		{
			std::string ContainerStr = OptionsTable.get_or<std::string>("container", "");
			if (!ContainerStr.empty())
			{
				FString Container = NeoLuaStr::ToFString(ContainerStr);
				if (Container.Equals(TEXT("array"), ESearchCase::IgnoreCase))
				{
					FType = FString::Printf(TEXT("array:%s"), *FType);
				}
				else if (Container.Equals(TEXT("set"), ESearchCase::IgnoreCase))
				{
					FType = FString::Printf(TEXT("set:%s"), *FType);
				}
				else if (Container.Equals(TEXT("map"), ESearchCase::IgnoreCase))
				{
					std::string ValTypeStr = OptionsTable.get_or<std::string>("value_type", "string");
					FString ValType = NeoLuaStr::ToFString(ValTypeStr);
					FType = FString::Printf(TEXT("map:%s:%s"), *FType, *ValType);
				}
			}
		}

		// Check for duplicate variable name
		if (FBlueprintEditorUtils::FindNewVariableIndex(Info.Blueprint, FName(*VarName)) != INDEX_NONE)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_variable(\"%s\") -> variable already exists"), *VarName));
			return sol::lua_nil;
		}

		// For container types, don't pass default to AddVariable (it's ignored by the BP compiler)
		bool bIsContainer = FType.Contains(TEXT(":")) || FType.EndsWith(TEXT("[]"));
		FString AddDefault = bIsContainer ? FString() : FDefault;

		if (!NeoBlueprint::AddVariable(Info.Blueprint, VarName, FType, AddDefault))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_variable(\"%s\", \"%s\") -> unknown type. Use: bool, int, float, string, name, text, vector, rotator, transform, color, byte, int64, or a class/struct name. Arrays: 'int[]' or 'array:int'. Sets: 'set:type'. Maps: 'map:key:value'."),
				*VarName, *FType));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] add_variable(\"%s\", \"%s\") -> added to \"%s\""),
			*VarName, *FType, *FBPPath));

		// For container types with a default value, compile the BP first then set on the CDO
		if (bIsContainer && !FDefault.IsEmpty())
		{
			// MarkBlueprintAsStructurallyModified only does skeleton regen — need full compile
			// so that GeneratedClass has the property on its CDO
			FKismetEditorUtilities::CompileBlueprint(Info.Blueprint);
			if (Info.Blueprint->GeneratedClass)
			{
				UObject* CDO = Info.Blueprint->GeneratedClass->GetDefaultObject();
				if (CDO)
				{
					FString Error;
					if (NeoBlueprint::SetObjectProperty(CDO, VarName, FDefault, Error))
					{
						Session.Log(FString::Printf(TEXT("[OK] set default for \"%s\" = \"%s\""), *VarName, *FDefault));
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] add_variable(\"%s\") -> default value could not be set: %s"), *VarName, *Error));
					}
				}
			}
		}

		// Apply options if provided
		if (bHasOptions)
		{
			ApplyVariableOptions(Info.Blueprint, VarName, OptionsTable, Session);
		}

		return sol::make_object(S, true);
	});

	Lua.set_function("change_variable_type", [&Session](const std::string& BPPath, const std::string& Name,
		const std::string& NewType, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
	{
		FString FBPPath = NeoLuaStr::ToFString(BPPath);
		FString VarName = NeoLuaStr::ToFString(Name);
		FString FNewType = NeoLuaStr::ToFString(NewType);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FBPPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] change_variable_type -> blueprint \"%s\" not found"), *FBPPath));
			return sol::lua_nil;
		}

		// Check for scope option → local variable type change
		if (Options.has_value())
		{
			std::string ScopeStr = Options->get_or<std::string>("scope", "");
			if (!ScopeStr.empty())
			{
				FString FScope = NeoLuaStr::ToFString(ScopeStr);
				FKismetEditorUtilities::CompileBlueprint(Info.Blueprint);
				if (NeoBlueprint::ChangeLocalVariableType(Info.Blueprint, FScope, VarName, FNewType))
				{
					Session.Log(FString::Printf(TEXT("[OK] change_variable_type(\"%s\") -> local in \"%s\" changed to \"%s\""),
						*VarName, *FScope, *FNewType));
					return sol::make_object(S, true);
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] change_variable_type(\"%s\", scope=\"%s\") -> failed (variable not found or bad type)"),
						*VarName, *FScope));
					return sol::lua_nil;
				}
			}
		}

		if (NeoBlueprint::ChangeVariableType(Info.Blueprint, VarName, FNewType))
		{
			Session.Log(FString::Printf(TEXT("[OK] change_variable_type(\"%s\") -> changed to \"%s\""), *VarName, *FNewType));
			return sol::make_object(S, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] change_variable_type(\"%s\", \"%s\") -> variable not found or bad type"), *VarName, *FNewType));
			return sol::lua_nil;
		}
	});

	Lua.set_function("move_variable", [&Session](const std::string& BPPath, const std::string& Name,
		const std::string& TargetName, sol::optional<std::string> Position, sol::this_state S) -> sol::object
	{
		FString FBPPath = NeoLuaStr::ToFString(BPPath);
		FString VarName = NeoLuaStr::ToFString(Name);
		FString TargetVarName = NeoLuaStr::ToFString(TargetName);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FBPPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] move_variable -> blueprint \"%s\" not found"), *FBPPath));
			return sol::lua_nil;
		}

		bool bAfter = false;
		if (Position.has_value())
		{
			FString Pos = NeoLuaStr::ToFStringOpt(Position);
			bAfter = Pos.ToLower() == TEXT("after");
		}

		bool bSuccess = bAfter
			? NeoBlueprint::MoveVariableAfter(Info.Blueprint, VarName, TargetVarName)
			: NeoBlueprint::MoveVariableBefore(Info.Blueprint, VarName, TargetVarName);

		if (bSuccess)
		{
			Session.Log(FString::Printf(TEXT("[OK] move_variable(\"%s\") -> %s \"%s\""),
				*VarName, bAfter ? TEXT("after") : TEXT("before"), *TargetVarName));
			return sol::make_object(S, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] move_variable(\"%s\") -> failed (variable not found?)"), *VarName));
			return sol::lua_nil;
		}
	});

	Lua.set_function("is_variable_used", [&Session](const std::string& BPPath, const std::string& Name,
		sol::this_state S) -> sol::object
	{
		FString FBPPath = NeoLuaStr::ToFString(BPPath);
		FString VarName = NeoLuaStr::ToFString(Name);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FBPPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] is_variable_used -> blueprint \"%s\" not found"), *FBPPath));
			return sol::lua_nil;
		}

		bool bUsed = NeoBlueprint::IsVariableUsed(Info.Blueprint, VarName);
		Session.Log(FString::Printf(TEXT("[OK] is_variable_used(\"%s\") -> %s"), *VarName, bUsed ? TEXT("true") : TEXT("false")));
		return sol::make_object(S, bUsed);
	});

	Lua.set_function("remove_local_variable", [&Session](const std::string& BPPath, const std::string& FuncName,
		const std::string& VarNameStr, sol::this_state S) -> sol::object
	{
		FString FBPPath = NeoLuaStr::ToFString(BPPath);
		FString FFuncName = NeoLuaStr::ToFString(FuncName);
		FString VarName = NeoLuaStr::ToFString(VarNameStr);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FBPPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_local_variable -> blueprint \"%s\" not found"), *FBPPath));
			return sol::lua_nil;
		}

		if (NeoBlueprint::RemoveLocalVariable(Info.Blueprint, FFuncName, VarName))
		{
			Session.Log(FString::Printf(TEXT("[OK] remove_local_variable(\"%s\") -> removed from \"%s\""), *VarName, *FFuncName));
			return sol::make_object(S, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] remove_local_variable(\"%s\") -> failed (function or variable not found)"), *VarName));
			return sol::lua_nil;
		}
	});

	Lua.set_function("rename_local_variable", [&Session](const std::string& BPPath, const std::string& FuncName,
		const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
	{
		FString FBPPath = NeoLuaStr::ToFString(BPPath);
		FString FFuncName = NeoLuaStr::ToFString(FuncName);
		FString FOldName = NeoLuaStr::ToFString(OldName);
		FString FNewName = NeoLuaStr::ToFString(NewName);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FBPPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] rename_local_variable -> blueprint \"%s\" not found"), *FBPPath));
			return sol::lua_nil;
		}

		if (NeoBlueprint::RenameLocalVariable(Info.Blueprint, FFuncName, FOldName, FNewName))
		{
			Session.Log(FString::Printf(TEXT("[OK] rename_local_variable(\"%s\" -> \"%s\") in \"%s\""), *FOldName, *FNewName, *FFuncName));
			return sol::make_object(S, true);
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] rename_local_variable(\"%s\") -> failed (function or variable not found)"), *FOldName));
			return sol::lua_nil;
		}
	});
});
