// Copyright 2025-2026 Betide Studio. All Rights Reserved.
//
// Reflection-driven globals: invoke() / subsystem() / class_methods() /
// class_properties() / class_interfaces().
// One binding, replaces hundreds of per-method wrappers by routing through ProcessEvent.
// Mirror of Epic's Python plugin pattern (PyGenUtil::IsScriptExposedFunction +
// FProperty::ImportText/ExportText + UObject::ProcessEvent).

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyTable.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaActorResolver.h"
#include "Lua/LuaStr.h"
#include "Blueprint/BlueprintUtils.h"
#include "Tools/NeoStackToolUtils.h"

#include "Editor.h"
#include "EditorSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Subsystems/EngineSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <sol/sol.hpp>

// ─── Helpers (file-local) ──────────────────────────────────────────────────

namespace
{

// Mirror of Epic's PyGenUtil::IsScriptExposedFunction filter.
bool IsInvokableFunction(const UFunction* Fn)
{
	if (!Fn) return false;
	static const FName MD_NoExport     = "ScriptNoExport";
	static const FName MD_InternalOnly = "BlueprintInternalUseOnly";
	static const FName MD_CustomThunk  = "CustomThunk";
	static const FName MD_NativeBreak  = "NativeBreakFunc";
	static const FName MD_NativeMake   = "NativeMakeFunc";

	return Fn->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent)
		&& !Fn->HasMetaData(MD_NoExport)
		&& !Fn->HasMetaData(MD_InternalOnly)
		&& !Fn->HasMetaData(MD_CustomThunk)
		&& !Fn->HasMetaData(MD_NativeBreak)
		&& !Fn->HasMetaData(MD_NativeMake);
}

// Resolve UClass from short name ("EditorActorSubsystem") or full path
// ("/Script/UnrealEd.EditorActorSubsystem"). Caches nothing — TObjectIterator
// is fast enough at editor speeds, and avoids stale cache after hot-reload.
UClass* ResolveClass(const FString& Name)
{
	if (Name.IsEmpty()) return nullptr;

	// 1. Exact path match
	if (UClass* C = FindObject<UClass>(nullptr, *Name))
	{
		return C;
	}

	// 2. Short name — case-sensitive wins, case-insensitive falls back
	UClass* CaseInsensitiveMatch = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* C = *It;
		if (C->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated)) continue;
		if (C->GetName().Equals(Name, ESearchCase::CaseSensitive)) return C;
		if (!CaseInsensitiveMatch && C->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			CaseInsensitiveMatch = C;
		}
	}
	return CaseInsensitiveMatch;
}

// Build a rich schema entry for one FProperty. Returns the same shape used by
// class_properties() and struct_properties() so agents see consistent metadata
// regardless of whether the parent is a UClass or USTRUCT. DefaultsContainer
// points at memory holding default values (UClass CDO or initialized struct);
// pass nullptr to skip the `default` field. Inner-most property is unwrapped
// from container properties (Array/Set/Map) so enum_values, allowed_class etc.
// describe the element type rather than the container.
sol::table BuildPropertySchema(FProperty* P, const void* DefaultsContainer, sol::state_view L)
{
	sol::table E = L.create_table();
	E["name"]      = TCHAR_TO_UTF8(*P->GetName());
	E["type"]      = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(P));

	FString Category = P->GetMetaData(TEXT("Category"));
	if (Category.IsEmpty()) Category = TEXT("Default");
	E["category"] = TCHAR_TO_UTF8(*Category);
	E["tooltip"]   = TCHAR_TO_UTF8(*P->GetToolTipText().ToString());
	E["editable"]  = P->HasAnyPropertyFlags(CPF_Edit);
	E["read_only"] = P->HasAnyPropertyFlags(CPF_EditConst);

	if (UStruct* Owner = P->GetOwnerStruct())
	{
		E["owner"] = TCHAR_TO_UTF8(*Owner->GetName());
	}

	// Container kind (and unwrap inner property for type-specific metadata)
	FProperty* InnerP = P;
	if (FArrayProperty* AP = CastField<FArrayProperty>(P))
	{
		E["container"]    = "array";
		E["element_type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(AP->Inner));
		InnerP = AP->Inner;
	}
	else if (FSetProperty* SP = CastField<FSetProperty>(P))
	{
		E["container"]    = "set";
		E["element_type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(SP->ElementProp));
		InnerP = SP->ElementProp;
	}
	else if (FMapProperty* MP = CastField<FMapProperty>(P))
	{
		E["container"]  = "map";
		E["key_type"]   = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(MP->KeyProp));
		E["value_type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(MP->ValueProp));
		InnerP = MP->ValueProp;
	}

	// Object/class references — emit class constraint so agents know what to assign.
	if (FClassProperty* CP = CastField<FClassProperty>(InnerP))
	{
		if (CP->MetaClass) E["meta_class"] = TCHAR_TO_UTF8(*CP->MetaClass->GetPathName());
	}
	else if (FSoftClassProperty* SCP = CastField<FSoftClassProperty>(InnerP))
	{
		if (SCP->MetaClass) E["meta_class"] = TCHAR_TO_UTF8(*SCP->MetaClass->GetPathName());
	}
	else if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(InnerP))
	{
		if (OP->PropertyClass) E["allowed_class"] = TCHAR_TO_UTF8(*OP->PropertyClass->GetPathName());
	}

	// Enum values (covers TEnumAsByte and modern enum class properties)
	UEnum* Enum = nullptr;
	if (FByteProperty* BP = CastField<FByteProperty>(InnerP)) Enum = BP->Enum;
	else if (FEnumProperty* EP = CastField<FEnumProperty>(InnerP)) Enum = EP->GetEnum();
	if (Enum)
	{
		E["enum_name"] = TCHAR_TO_UTF8(*Enum->GetName());
		sol::table EV = L.create_table();
		int32 EVIdx = 1;
		const int32 NumEnums = Enum->NumEnums();
		// NumEnums - 1 skips the synthetic _MAX entry the engine appends.
		for (int32 i = 0; i < NumEnums - 1; ++i)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), i)) continue;
			EV[EVIdx++] = TCHAR_TO_UTF8(*Enum->GetNameStringByIndex(i));
		}
		E["enum_values"] = EV;
	}

	// Struct name — agent can drill in via struct_properties(struct_name).
	if (FStructProperty* StP = CastField<FStructProperty>(InnerP))
	{
		if (StP->Struct) E["struct_name"] = TCHAR_TO_UTF8(*StP->Struct->GetName());
	}

	// Numeric / UI metadata
	auto AddMeta = [&E, P](const TCHAR* Key, const char* OutKey)
	{
		FString V = P->GetMetaData(Key);
		if (!V.IsEmpty()) E[OutKey] = TCHAR_TO_UTF8(*V);
	};
	AddMeta(TEXT("ClampMin"),      "clamp_min");
	AddMeta(TEXT("ClampMax"),      "clamp_max");
	AddMeta(TEXT("UIMin"),         "ui_min");
	AddMeta(TEXT("UIMax"),         "ui_max");
	AddMeta(TEXT("EditCondition"), "edit_condition");
	if (P->HasMetaData(TEXT("EditConditionHides"))) E["edit_condition_hides"] = true;

	// Default value — exported via FProperty so it round-trips through ImportText.
	if (DefaultsContainer)
	{
		const void* PropMem = P->ContainerPtrToValuePtr<void>(DefaultsContainer);
		FString DefStr;
		P->ExportTextItem_Direct(DefStr, PropMem, nullptr, nullptr, PPF_None);
		if (!DefStr.IsEmpty()) E["default"] = TCHAR_TO_UTF8(*DefStr);
	}

	return E;
}

sol::table BuildFunctionSchema(UFunction* Fn, sol::state_view L)
{
	sol::table Entry = L.create_table();
	Entry["name"]     = TCHAR_TO_UTF8(*Fn->GetName());
	Entry["category"] = TCHAR_TO_UTF8(*Fn->GetMetaData(TEXT("Category")));
	Entry["tooltip"]  = TCHAR_TO_UTF8(*Fn->GetToolTipText().ToString());
	if (UClass* OwnerClass = Fn->GetOwnerClass())
	{
		Entry["owner"] = TCHAR_TO_UTF8(*OwnerClass->GetName());
	}

	sol::table ParamArr = L.create_table();
	int32 PIdx = 1;
	for (TFieldIterator<FProperty> P(Fn); P; ++P)
	{
		FProperty* Pr = *P;
		if (!Pr->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (Pr->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

		sol::table PT = L.create_table();
		PT["name"]   = TCHAR_TO_UTF8(*Pr->GetName());
		PT["type"]   = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Pr));
		PT["is_out"] = Pr->HasAnyPropertyFlags(CPF_OutParm)
		               && !Pr->HasAnyPropertyFlags(CPF_ReferenceParm);

		const FString DefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *Pr->GetName());
		if (Fn->HasMetaData(*DefaultKey))
		{
			PT["default"] = TCHAR_TO_UTF8(*Fn->GetMetaData(*DefaultKey));
		}
		ParamArr[PIdx++] = PT;
	}
	Entry["params"] = ParamArr;

	if (FProperty* Ret = Fn->GetReturnProperty())
	{
		Entry["returns"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Ret));
	}
	return Entry;
}

// Resolve a UScriptStruct by short name ("NiagaraTypeDefinition") or full path.
UScriptStruct* ResolveStruct(const FString& Name)
{
	if (Name.IsEmpty()) return nullptr;
	if (UScriptStruct* S = FindObject<UScriptStruct>(nullptr, *Name)) return S;
	UScriptStruct* CaseInsensitive = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* S = *It;
		if (S->GetName().Equals(Name, ESearchCase::CaseSensitive)) return S;
		if (!CaseInsensitive && S->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			CaseInsensitive = S;
		}
	}
	return CaseInsensitive;
}

// Discover a singleton subsystem by class + scope. "editor" / "engine" supported here;
// "game" / "world" need a live PIE world and aren't typically what agents want, so we
// skip them for now.
UObject* GetSubsystemForScope(UClass* Class, const FString& Scope)
{
	if (!Class) return nullptr;

	if (Scope.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
	{
		if (GEditor && Class->IsChildOf(UEditorSubsystem::StaticClass()))
		{
			return GEditor->GetEditorSubsystemBase((TSubclassOf<UEditorSubsystem>)Class);
		}
	}
	else if (Scope.Equals(TEXT("engine"), ESearchCase::IgnoreCase))
	{
		if (GEngine && Class->IsChildOf(UEngineSubsystem::StaticClass()))
		{
			return GEngine->GetEngineSubsystemBase((TSubclassOf<UEngineSubsystem>)Class);
		}
	}
	return nullptr;
}

// Resolve the (Receiver, DispatchClass) pair for an `invoke()` target.
//   • string starting with "/Script/" → class path → CDO + class (static UFunctions)
//   • other string                    → asset path → loaded UObject + its class
//   • table with __subsystem_scope    → re-fetched subsystem singleton
//   • table with "path"               → re-loaded asset
//   • table with "actor_label"        → level actor (case-insensitive name OR label)
//   • table with "actor_label"+"component" → UActorComponent on that actor
struct FInvokeTarget
{
	UObject* Receiver = nullptr;
	UClass*  DispatchClass = nullptr;
	FString  DescribeForLog;
};

// Component lookup by GetName or GetReadableName (case-insensitive). Mirrors the
// static helper in LuaBinding_LevelDesign.cpp — intentionally duplicated rather
// than promoted: it's three lines, and promoting forces a shared-header rebuild
// across every binding.
UActorComponent* FindComponentByName(AActor* Actor, const FString& CompName)
{
	if (!Actor || CompName.IsEmpty()) return nullptr;
	TArray<UActorComponent*> AllComps;
	Actor->GetComponents(AllComps);
	for (UActorComponent* Comp : AllComps)
	{
		if (!Comp) continue;
		if (Comp->GetName().Equals(CompName, ESearchCase::IgnoreCase)
			|| Comp->GetReadableName().Equals(CompName, ESearchCase::IgnoreCase))
		{
			return Comp;
		}
	}
	return nullptr;
}

// Thin alias kept for readability; delegates to NeoLuaAsset::ResolveWithRegistry,
// which is the single source of truth for "Lua-path → UObject" with AR fallback.
inline UObject* LoadAssetByPath(const FString& Path)
{
	return NeoLuaAsset::ResolveWithRegistry(Path);
}

UObject* ResolveObjectArg(const sol::table& T, const FObjectPropertyBase* ObjectProperty, FString& OutError)
{
	if (!ObjectProperty || !ObjectProperty->PropertyClass)
	{
		OutError = TEXT("object parameter has no PropertyClass");
		return nullptr;
	}

	sol::optional<std::string> PathOpt = T["path"];
	if (PathOpt.has_value())
	{
		const FString ObjectPath = NeoLuaAsset::ToObjectPath(NeoLuaStr::ToFString(PathOpt.value()));
		UObject* Obj = LoadAssetByPath(ObjectPath);
		if (!Obj)
		{
			OutError = FString::Printf(TEXT("could not resolve object path '%s'"), *ObjectPath);
			return nullptr;
		}
		if (!Obj->IsA(ObjectProperty->PropertyClass))
		{
			OutError = FString::Printf(TEXT("'%s' resolved to %s but parameter expects %s"),
				*ObjectPath, *Obj->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());
			return nullptr;
		}
		return Obj;
	}

	sol::optional<std::string> LabelOpt = T["actor_label"];
	if (LabelOpt.has_value())
	{
		const FString FLabel = NeoLuaStr::ToFStringOpt(LabelOpt);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		AActor* Actor = World ? NeoLuaActor::FindByNameOrLabel(World, FLabel) : nullptr;
		if (!Actor)
		{
			OutError = FString::Printf(TEXT("could not resolve actor_label '%s'"), *FLabel);
			return nullptr;
		}

		UObject* Obj = Actor;
		sol::optional<std::string> CompOpt = T["component"];
		if (CompOpt.has_value())
		{
			const FString FCompName = NeoLuaStr::ToFStringOpt(CompOpt);
			Obj = FindComponentByName(Actor, FCompName);
			if (!Obj)
			{
				OutError = FString::Printf(TEXT("could not resolve component '%s' on actor_label '%s'"), *FCompName, *FLabel);
				return nullptr;
			}
		}

		if (!Obj->IsA(ObjectProperty->PropertyClass))
		{
			OutError = FString::Printf(TEXT("actor handle resolved to %s but parameter expects %s"),
				*Obj->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());
			return nullptr;
		}
		return Obj;
	}

	OutError = TEXT("object table requires 'path' or 'actor_label'");
	return nullptr;
}

FInvokeTarget ResolveTarget(const sol::object& Target)
{
	FInvokeTarget Out;

	if (Target.is<std::string>())
	{
		const FString S = NeoLuaStr::ToFString(Target.as<std::string>());

		// Class path → CDO (covers BlueprintFunctionLibrary statics)
		if (UClass* C = FindObject<UClass>(nullptr, *S))
		{
			Out.DispatchClass = C;
			Out.Receiver = C->GetDefaultObject();
			Out.DescribeForLog = C->GetName();
			return Out;
		}

		// Asset path → loaded object
		if (UObject* Obj = LoadAssetByPath(S))
		{
			Out.Receiver = Obj;
			Out.DispatchClass = Obj->GetClass();
			Out.DescribeForLog = FString::Printf(TEXT("%s(%s)"), *Obj->GetClass()->GetName(), *S);
			return Out;
		}

		Out.DescribeForLog = S;
		return Out;
	}

	if (Target.is<sol::table>())
	{
		sol::table T = Target.as<sol::table>();

		// Subsystem handle: { class = "/Script/...", __subsystem_scope = "editor" }
		sol::optional<std::string> ScopeOpt = T["__subsystem_scope"];
		sol::optional<std::string> ClassOpt = T["class"];
		if (ScopeOpt.has_value() && ClassOpt.has_value())
		{
			const FString FClass = NeoLuaStr::ToFStringOpt(ClassOpt);
			const FString FScope = NeoLuaStr::ToFStringOpt(ScopeOpt);
			if (UClass* C = ResolveClass(FClass))
			{
				if (UObject* Sub = GetSubsystemForScope(C, FScope))
				{
					Out.Receiver = Sub;
					Out.DispatchClass = Sub->GetClass();
					Out.DescribeForLog = FString::Printf(TEXT("subsystem(%s)"), *C->GetName());
					return Out;
				}
			}
			Out.DescribeForLog = FString::Printf(TEXT("subsystem(%s)"), *FClass);
			return Out;
		}

		// Asset handle: { path = "/Game/...", ... } (open_asset shape)
		sol::optional<std::string> PathOpt = T["path"];
		if (PathOpt.has_value())
		{
			const FString FPath = NeoLuaStr::ToFStringOpt(PathOpt);
			if (UObject* Obj = LoadAssetByPath(FPath))
			{
				Out.Receiver = Obj;
				Out.DispatchClass = Obj->GetClass();
				Out.DescribeForLog = FString::Printf(TEXT("%s(%s)"), *Obj->GetClass()->GetName(), *FPath);
				return Out;
			}
			Out.DescribeForLog = FPath;
			return Out;
		}

		// Actor handle: { actor_label = "MyActor" } — reach UFUNCTIONs on an
		// in-level actor that has no /Game/ asset path of its own.
		// Optional `component = "Name"` narrows the target to a UActorComponent —
		// lets agents reach component-level UFUNCTIONs (UNiagaraComponent::SetAsset,
		// UAudioComponent::Play, USkeletalMeshComponent::PlayAnimation, …) that
		// aren't reachable through the actor CDO.
		sol::optional<std::string> LabelOpt = T["actor_label"];
		if (LabelOpt.has_value())
		{
			const FString FLabel = NeoLuaStr::ToFStringOpt(LabelOpt);
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World)
			{
				if (AActor* Actor = NeoLuaActor::FindByNameOrLabel(World, FLabel))
				{
					sol::optional<std::string> CompOpt = T["component"];
					if (CompOpt.has_value())
					{
						const FString FCompName = NeoLuaStr::ToFStringOpt(CompOpt);
						if (UActorComponent* Comp = FindComponentByName(Actor, FCompName))
						{
							Out.Receiver = Comp;
							Out.DispatchClass = Comp->GetClass();
							Out.DescribeForLog = FString::Printf(TEXT("%s(%s.%s)"), *Comp->GetClass()->GetName(), *FLabel, *FCompName);
							return Out;
						}
						Out.DescribeForLog = FString::Printf(TEXT("component=%s on actor=%s (not found)"), *FCompName, *FLabel);
						return Out;
					}
					Out.Receiver = Actor;
					Out.DispatchClass = Actor->GetClass();
					Out.DescribeForLog = FString::Printf(TEXT("%s(label=%s)"), *Actor->GetClass()->GetName(), *FLabel);
					return Out;
				}
			}
			Out.DescribeForLog = FString::Printf(TEXT("actor_label=%s"), *FLabel);
			return Out;
		}
	}

	return Out;
}

// Walk a UFunction's parameter properties and apply Lua args. Args may be
// positional (sequence indices) or named (string keys matching parameter names);
// named takes precedence on collision.
//
// Pure-out params (CPF_OutParm without CPF_ReferenceParm) are skipped — they get
// filled by ProcessEvent and read back by BuildResult.
bool MarshalArgs(const UFunction* Fn,
                  uint8* ParamsBuffer,
                  UObject* OwnerForPostEdit,
                  const sol::table& Args,
                  FString& OutError)
{
	int32 PositionalIdx = 1;
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		FProperty* P = *It;
		if (!P->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (P->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

		const bool bPureOut = P->HasAnyPropertyFlags(CPF_OutParm)
			&& !P->HasAnyPropertyFlags(CPF_ReferenceParm);
		if (bPureOut) continue;

		// Named arg first, then positional. Lua table keys are UTF-8 strings;
		// do not use FString directly or the lookup misses valid {ParamName=...} args.
		const std::string ParamKey = TCHAR_TO_UTF8(*P->GetName());
		sol::object Val = Args[ParamKey];
		if (!Val.valid() || Val.is<sol::lua_nil_t>())
		{
			Val = Args[PositionalIdx];
			++PositionalIdx;
		}

		if (!Val.valid() || Val.is<sol::lua_nil_t>())
		{
			// Honor C++ default if the function declared one (UHT writes "CPP_Default_<Param>"
			// metadata for parameters with default values).
			const FString DefaultKey = FString::Printf(TEXT("CPP_Default_%s"), *P->GetName());
			if (Fn->HasMetaData(*DefaultKey))
			{
				const FString DefaultVal = Fn->GetMetaData(*DefaultKey);
				P->ImportText_Direct(*DefaultVal,
					P->ContainerPtrToValuePtr<void>(ParamsBuffer),
					OwnerForPostEdit, PPF_None);
			}
			// Else: leave zero-init from FStructOnScope.
			continue;
		}

		// Object-handle ergonomics: if the param wants a UObject, accept the same
		// Lua handles invoke() targets use: asset handles (`path`) and live level
		// actor/component handles (`actor_label`, optional `component`).
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(P); ObjectProperty && Val.is<sol::table>())
		{
			FString ResolveError;
			UObject* Obj = ResolveObjectArg(Val.as<sol::table>(), ObjectProperty, ResolveError);
			if (!Obj)
			{
				OutError = FString::Printf(TEXT("arg \"%s\": %s"), *P->GetName(), *ResolveError);
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(ParamsBuffer), Obj);
			continue;
		}

		void* ParamValuePtr = P->ContainerPtrToValuePtr<void>(ParamsBuffer);
		FString PartialError;
		const bool bOk = NeoLuaProperty::ApplySolValueToProperty(
			P, ParamValuePtr, nullptr, Val, PartialError);
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("arg \"%s\": %s"), *P->GetName(), *PartialError);
			return false;
		}
	}
	return true;
}

// Build the Lua return value from a post-ProcessEvent params buffer.
//   • void function          → true
//   • single return, no outs → that value
//   • return + outs / outs   → { ret = X, outparam_name = Y, ... }
sol::object BuildResult(const UFunction* Fn,
                         uint8* ParamsBuffer,
                         sol::state_view Lua)
{
	FProperty* ReturnProp = Fn->GetReturnProperty();

	TArray<FProperty*> OutProps;
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		FProperty* P = *It;
		if (!P->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (P->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
		// Treat ref-out params as outs too — they often carry mutated values.
		if (P->HasAnyPropertyFlags(CPF_OutParm)) OutProps.Add(P);
	}

	if (!ReturnProp && OutProps.Num() == 0)
	{
		return sol::make_object(Lua, true);
	}

	if (OutProps.Num() == 0 && ReturnProp)
	{
		return NeoLuaProperty::ReadPropertyAsSol(ReturnProp, ReturnProp->ContainerPtrToValuePtr<void>(ParamsBuffer), Lua);
	}

	sol::table Result = Lua.create_table();
	if (ReturnProp)
	{
		Result["ret"] = NeoLuaProperty::ReadPropertyAsSol(ReturnProp, ReturnProp->ContainerPtrToValuePtr<void>(ParamsBuffer), Lua);
	}
	for (FProperty* P : OutProps)
	{
		Result[TCHAR_TO_UTF8(*P->GetName())] =
			NeoLuaProperty::ReadPropertyAsSol(P, P->ContainerPtrToValuePtr<void>(ParamsBuffer), Lua);
	}
	return Result;
}

// Resolve a UClass from any "target-or-class" arg form (string short/full path,
// asset handle table, subsystem handle table). Used by class_methods/class_properties.
UClass* ResolveClassFromTarget(const sol::object& TargetOrClass)
{
	if (TargetOrClass.is<std::string>())
	{
		const FString S = NeoLuaStr::ToFString(TargetOrClass.as<std::string>());
		if (UClass* C = ResolveClass(S)) return C;
		// Maybe an asset path — load it and use its class
		if (UObject* Obj = LoadAssetByPath(S)) return Obj->GetClass();
		return nullptr;
	}

	FInvokeTarget T = ResolveTarget(TargetOrClass);
	return T.DispatchClass;
}

} // namespace

// ─── Docs ─────────────────────────────────────────────────────────────────

static TArray<FLuaFunctionDoc> ReflectionDocs = {
	{ TEXT("invoke(target, fn_name, args?)"),
	  TEXT("Call any UFUNCTION(BlueprintCallable or BlueprintEvent) by name. Target: asset path, class path, asset handle, subsystem handle, or {actor_label=\"...\"}. Args is positional or named table"),
	  TEXT("ret value | {ret=, outparam=,...} | true | nil") },
	{ TEXT("subsystem(class_name, scope?)"),
	  TEXT("Get a subsystem handle. scope: editor (default) | engine. Returns a handle table for use with invoke()"),
	  TEXT("subsystem handle or nil") },
	{ TEXT("class_methods(target_or_class, opts?)"),
	  TEXT("List BlueprintCallable or BlueprintEvent methods on a class. opts: {include_inherited=true, filter=''}"),
	  TEXT("table[]") },
	{ TEXT("class_properties(target_or_class, opts?)"),
	  TEXT("List editor-editable or BlueprintVisible properties on a class. opts: {include_inherited=true, filter='', editable_only=true}. Each entry: {name, type, category, tooltip, editable, read_only, owner, container?, element_type?/key_type+value_type?, allowed_class?, meta_class?, struct_name?, enum_name?, enum_values?, clamp_min?, clamp_max?, ui_min?, ui_max?, edit_condition?, edit_condition_hides?, default?}"),
	  TEXT("table[]") },
	{ TEXT("class_interfaces(target_or_class, opts?)"),
	  TEXT("List implemented interfaces separately from class_methods/class_properties. opts: {include_inherited=true, filter='', editable_only=true}. Each entry: {name, class, path, owner, implemented_by_k2, pointer_offset, method_count, methods, property_count, properties}"),
	  TEXT("table[]") },
	{ TEXT("struct_properties(struct_name, opts?)"),
	  TEXT("Same shape as class_properties() but for USTRUCTs. opts: {editable_only=false, filter=''}. Defaults reflect FStructOnScope-initialized memory."),
	  TEXT("table[]") },
	{ TEXT("list_subclasses(parent_class, opts?)"),
	  TEXT("List concrete subclasses of a class. opts: {exclude_abstract=true, native_only=false, include_self=false, filter=''}. Each entry: {name, path, abstract, native, super}"),
	  TEXT("table[]") },
};

// ─── Registration ─────────────────────────────────────────────────────────

REGISTER_LUA_BINDING(Reflection, ReflectionDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// ── invoke ─────────────────────────────────────────────────────────
	Lua.set_function("invoke",
		[&Session](sol::object Target, const std::string& FnName,
		           sol::optional<sol::table> Args, sol::this_state S) -> sol::object
		{
			sol::state_view L(S);
			const FString FFnName = NeoLuaStr::ToFString(FnName);

			FInvokeTarget T = ResolveTarget(Target);
			if (!T.Receiver || !T.DispatchClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] invoke -> could not resolve target for \"%s\" (%s)"),
					*FFnName, *T.DescribeForLog));
				return sol::lua_nil;
			}

			UFunction* Fn = T.DispatchClass->FindFunctionByName(*FFnName);
			if (!Fn)
			{
				// Case-insensitive fallback before giving up
				for (TFieldIterator<UFunction> It(T.DispatchClass); It; ++It)
				{
					if (It->GetName().Equals(FFnName, ESearchCase::IgnoreCase))
					{
						Fn = *It;
						break;
					}
				}
			}
			if (!Fn)
			{
				const FString Suggest = NeoBlueprint::FuzzyMatchFunction(T.DispatchClass, FFnName);
				Session.Log(FString::Printf(TEXT("[FAIL] invoke(%s.%s) -> %s"),
					*T.DispatchClass->GetName(), *FFnName, *Suggest));
				return sol::lua_nil;
			}
			if (!IsInvokableFunction(Fn))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] invoke(%s.%s) -> not BlueprintCallable/BlueprintEvent or explicitly hidden"),
					*T.DispatchClass->GetName(), *FFnName));
				return sol::lua_nil;
			}

			// Allocate a properly-constructed parameter buffer.
			FStructOnScope ParamScope(Fn);
			uint8* ParamsBuffer = ParamScope.GetStructMemory();

			sol::table ArgTable = Args.value_or(L.create_table());
			FString MarshalErr;
			if (!MarshalArgs(Fn, ParamsBuffer, T.Receiver, ArgTable, MarshalErr))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] invoke(%s.%s) -> %s"),
					*T.DispatchClass->GetName(), *FFnName, *MarshalErr));
				return sol::lua_nil;
			}

			// Editor-script guard — same protection the Python plugin uses.
			FEditorScriptExecutionGuard Guard;
			T.Receiver->ProcessEvent(Fn, ParamsBuffer);

			sol::object Out = BuildResult(Fn, ParamsBuffer, L);
			Session.Log(FString::Printf(TEXT("[OK] invoke(%s.%s)"),
				*T.DispatchClass->GetName(), *FFnName));
			return Out;
		});

	// ── subsystem ─────────────────────────────────────────────────────
	Lua.set_function("subsystem",
		[&Session](const std::string& Name, sol::optional<std::string> ScopeOpt,
		           sol::this_state S) -> sol::object
		{
			sol::state_view L(S);
			const FString FName_ = NeoLuaStr::ToFString(Name);
			const FString FScope = NeoLuaStr::ToFStringOpt(ScopeOpt, TEXT("editor"));

			UClass* Class = ResolveClass(FName_);
			if (!Class)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] subsystem(\"%s\") -> class not found"), *FName_));
				return sol::lua_nil;
			}

			UObject* Sub = GetSubsystemForScope(Class, FScope);
			if (!Sub)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] subsystem(\"%s\", \"%s\") -> not available in scope"),
					*FName_, *FScope));
				return sol::lua_nil;
			}

			// Return a handle table — invoke() re-fetches the singleton each call,
			// so the handle stays valid across hot-reload / module reinit.
			sol::table Handle = L.create_table();
			Handle["class"] = TCHAR_TO_UTF8(*Class->GetPathName());
			Handle["__subsystem_scope"] = TCHAR_TO_UTF8(*FScope);
			Handle["name"] = TCHAR_TO_UTF8(*Class->GetName());
			Session.Log(FString::Printf(TEXT("[OK] subsystem(%s, %s)"), *Class->GetName(), *FScope));
			return Handle;
		});

	// ── class_methods ─────────────────────────────────────────────────
	Lua.set_function("class_methods",
		[&Session](sol::object TargetOrClass, sol::optional<sol::table> Opts,
		           sol::this_state S) -> sol::object
		{
			sol::state_view L(S);

			UClass* Class = ResolveClassFromTarget(TargetOrClass);
			if (!Class)
			{
				Session.Log(TEXT("[FAIL] class_methods -> could not resolve class"));
				return sol::lua_nil;
			}

			bool bIncludeInherited = true;
			FString Filter;
			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (O["include_inherited"].valid())
				{
					bIncludeInherited = O["include_inherited"].get_or(true);
				}
				if (O["filter"].valid())
				{
					Filter = NeoLuaStr::ToFString(O["filter"].get<std::string>());
				}
			}

			const EFieldIteratorFlags::SuperClassFlags SuperFlag =
				bIncludeInherited ? EFieldIteratorFlags::IncludeSuper
				                  : EFieldIteratorFlags::ExcludeSuper;

			sol::table Result = L.create_table();
			int32 Idx = 1;

			for (TFieldIterator<UFunction> It(Class, SuperFlag); It; ++It)
			{
				UFunction* Fn = *It;
				if (!IsInvokableFunction(Fn)) continue;
				if (!Filter.IsEmpty() && !Fn->GetName().Contains(Filter, ESearchCase::IgnoreCase)) continue;

				Result[Idx++] = BuildFunctionSchema(Fn, L);
			}

			Session.Log(FString::Printf(TEXT("[OK] class_methods(%s) -> %d entries"),
				*Class->GetName(), Idx - 1));
			return Result;
		});

	// ── class_properties ──────────────────────────────────────────────
	Lua.set_function("class_properties",
		[&Session](sol::object TargetOrClass, sol::optional<sol::table> Opts,
		           sol::this_state S) -> sol::object
		{
			sol::state_view L(S);

			UClass* Class = ResolveClassFromTarget(TargetOrClass);
			if (!Class)
			{
				Session.Log(TEXT("[FAIL] class_properties -> could not resolve class"));
				return sol::lua_nil;
			}

			bool bIncludeInherited = true;
			bool bEditableOnly = true;
			FString Filter;
			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (O["include_inherited"].valid())
				{
					bIncludeInherited = O["include_inherited"].get_or(true);
				}
				if (O["editable_only"].valid())
				{
					bEditableOnly = O["editable_only"].get_or(true);
				}
				if (O["filter"].valid())
				{
					Filter = NeoLuaStr::ToFString(O["filter"].get<std::string>());
				}
			}

			const EFieldIteratorFlags::SuperClassFlags SuperFlag =
				bIncludeInherited ? EFieldIteratorFlags::IncludeSuper
				                  : EFieldIteratorFlags::ExcludeSuper;

			// CDO supplies default values for every property the schema enumerates.
			const UObject* CDO = Class->GetDefaultObject(false);

			sol::table Result = L.create_table();
			int32 Idx = 1;

			for (TFieldIterator<FProperty> It(Class, SuperFlag); It; ++It)
			{
				FProperty* P = *It;
				if (P->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
				if (bEditableOnly && !P->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
				if (!Filter.IsEmpty() && !P->GetName().Contains(Filter, ESearchCase::IgnoreCase)) continue;

				Result[Idx++] = BuildPropertySchema(P, CDO, L);
			}

			Session.Log(FString::Printf(TEXT("[OK] class_properties(%s) -> %d entries"),
				*Class->GetName(), Idx - 1));
			return Result;
		});

	// ── class_interfaces ──────────────────────────────────────────────
	Lua.set_function("class_interfaces",
		[&Session](sol::object TargetOrClass, sol::optional<sol::table> Opts,
		           sol::this_state S) -> sol::object
		{
			sol::state_view L(S);

			UClass* Class = ResolveClassFromTarget(TargetOrClass);
			if (!Class)
			{
				Session.Log(TEXT("[FAIL] class_interfaces -> could not resolve class"));
				return sol::lua_nil;
			}

			bool bIncludeInherited = true;
			bool bEditableOnly = true;
			FString Filter;
			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (O["include_inherited"].valid())
				{
					bIncludeInherited = O["include_inherited"].get_or(true);
				}
				if (O["editable_only"].valid())
				{
					bEditableOnly = O["editable_only"].get_or(true);
				}
				if (O["filter"].valid())
				{
					Filter = NeoLuaStr::ToFString(O["filter"].get<std::string>());
				}
			}

			sol::table Result = L.create_table();
			int32 Idx = 1;
			TSet<UClass*> SeenInterfaces;

			for (UClass* Current = Class; Current; Current = bIncludeInherited ? Current->GetSuperClass() : nullptr)
			{
				for (const FImplementedInterface& Impl : Current->Interfaces)
				{
					UClass* InterfaceClass = Impl.Class.Get();
					if (!InterfaceClass || SeenInterfaces.Contains(InterfaceClass)) continue;
					SeenInterfaces.Add(InterfaceClass);

					const bool bInterfaceMatchesFilter = Filter.IsEmpty()
						|| InterfaceClass->GetName().Contains(Filter, ESearchCase::IgnoreCase);

					sol::table MethodArr = L.create_table();
					int32 MethodIdx = 1;
					for (TFieldIterator<UFunction> It(InterfaceClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
					{
						UFunction* Fn = *It;
						if (!IsInvokableFunction(Fn)) continue;
						if (!bInterfaceMatchesFilter && !Fn->GetName().Contains(Filter, ESearchCase::IgnoreCase)) continue;
						MethodArr[MethodIdx++] = BuildFunctionSchema(Fn, L);
					}

					sol::table PropertyArr = L.create_table();
					int32 PropertyIdx = 1;
					for (TFieldIterator<FProperty> It(InterfaceClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
					{
						FProperty* P = *It;
						if (P->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
						if (bEditableOnly && !P->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
						if (!bInterfaceMatchesFilter && !P->GetName().Contains(Filter, ESearchCase::IgnoreCase)) continue;
						PropertyArr[PropertyIdx++] = BuildPropertySchema(P, nullptr, L);
					}

					if (!Filter.IsEmpty() && !bInterfaceMatchesFilter && MethodIdx == 1 && PropertyIdx == 1)
					{
						continue;
					}

					sol::table Entry = L.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*InterfaceClass->GetName());
					Entry["class"] = TCHAR_TO_UTF8(*InterfaceClass->GetName());
					Entry["path"] = TCHAR_TO_UTF8(*InterfaceClass->GetPathName());
					Entry["owner"] = TCHAR_TO_UTF8(*Current->GetName());
					Entry["implemented_by_k2"] = Impl.bImplementedByK2;
					Entry["pointer_offset"] = Impl.PointerOffset;
					Entry["method_count"] = MethodIdx - 1;
					Entry["methods"] = MethodArr;
					Entry["property_count"] = PropertyIdx - 1;
					Entry["properties"] = PropertyArr;
					Result[Idx++] = Entry;
				}

				if (!bIncludeInherited)
				{
					break;
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] class_interfaces(%s) -> %d entries"),
				*Class->GetName(), Idx - 1));
			return Result;
		});

	// ── struct_properties ─────────────────────────────────────────────
	// USTRUCT counterpart to class_properties. Defaults come from a
	// freshly-initialized struct memory blob (FStructOnScope), so the
	// reported defaults match what `InitializeStruct` produces.
	Lua.set_function("struct_properties",
		[&Session](const std::string& StructName, sol::optional<sol::table> Opts,
		           sol::this_state S) -> sol::object
		{
			sol::state_view L(S);
			const FString FStructName = NeoLuaStr::ToFString(StructName);
			UScriptStruct* Struct = ResolveStruct(FStructName);
			if (!Struct)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] struct_properties(\"%s\") -> struct not found"), *FStructName));
				return sol::lua_nil;
			}

			bool bEditableOnly = false; // USTRUCTs often don't tag CPF_Edit; default to all
			FString Filter;
			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (O["editable_only"].valid()) bEditableOnly = O["editable_only"].get_or(false);
				if (O["filter"].valid()) Filter = NeoLuaStr::ToFString(O["filter"].get<std::string>());
			}

			// Initialize struct memory so ExportTextItem_Direct can read defaults.
			FStructOnScope Scope(Struct);
			const void* DefaultsMem = Scope.GetStructMemory();

			sol::table Result = L.create_table();
			int32 Idx = 1;

			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				FProperty* P = *It;
				if (P->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
				if (bEditableOnly && !P->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
				if (!Filter.IsEmpty() && !P->GetName().Contains(Filter, ESearchCase::IgnoreCase)) continue;

				Result[Idx++] = BuildPropertySchema(P, DefaultsMem, L);
			}

			Session.Log(FString::Printf(TEXT("[OK] struct_properties(%s) -> %d entries"),
				*Struct->GetName(), Idx - 1));
			return Result;
		});

	// ── list_subclasses ───────────────────────────────────────────────
	// "Which classes derive from X?" — used to discover renderer types,
	// data interface types, material expressions, etc., before calling
	// class_properties on a specific subclass.
	Lua.set_function("list_subclasses",
		[&Session](const std::string& ParentName, sol::optional<sol::table> Opts,
		           sol::this_state S) -> sol::object
		{
			sol::state_view L(S);
			const FString FParentName = NeoLuaStr::ToFString(ParentName);
			UClass* Parent = ResolveClass(FParentName);
			if (!Parent)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list_subclasses(\"%s\") -> class not found"), *FParentName));
				return sol::lua_nil;
			}

			bool bExcludeAbstract = true;
			bool bNativeOnly      = false;
			bool bIncludeSelf     = false;
			FString Filter;
			if (Opts.has_value())
			{
				sol::table O = Opts.value();
				if (O["exclude_abstract"].valid()) bExcludeAbstract = O["exclude_abstract"].get_or(true);
				if (O["native_only"].valid())      bNativeOnly      = O["native_only"].get_or(false);
				if (O["include_self"].valid())     bIncludeSelf     = O["include_self"].get_or(false);
				if (O["filter"].valid())           Filter           = NeoLuaStr::ToFString(O["filter"].get<std::string>());
			}

			sol::table Result = L.create_table();
			int32 Idx = 1;

			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* C = *It;
				if (!C || !C->IsChildOf(Parent)) continue;
				if (!bIncludeSelf && C == Parent) continue;
				if (C->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated)) continue;
				if (bExcludeAbstract && C->HasAnyClassFlags(CLASS_Abstract)) continue;
				if (bNativeOnly && !C->HasAnyClassFlags(CLASS_Native)) continue;
				if (!Filter.IsEmpty() && !C->GetName().Contains(Filter, ESearchCase::IgnoreCase)) continue;

				sol::table Entry = L.create_table();
				Entry["name"]     = TCHAR_TO_UTF8(*C->GetName());
				Entry["path"]     = TCHAR_TO_UTF8(*C->GetPathName());
				Entry["abstract"] = C->HasAnyClassFlags(CLASS_Abstract);
				Entry["native"]   = C->HasAnyClassFlags(CLASS_Native);
				if (UClass* Super = C->GetSuperClass())
				{
					Entry["super"] = TCHAR_TO_UTF8(*Super->GetName());
				}
				Result[Idx++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_subclasses(%s) -> %d entries"),
				*Parent->GetName(), Idx - 1));
			return Result;
		});
});
