#include "Lua/LuaPropertyHelper.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "BehaviorTree/ValueOrBBKey.h"
#endif
#include "Lua/LuaEnumReflection.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace NeoLuaProperty
{

// Strip the optional class-prefix form some UE editors emit ("Material'/Game/M.M'" → "/Game/M.M").
// StaticLoadObject takes the bare /Path.Path form and rejects the prefixed one.
static FString StripObjectPathClassPrefix(const FString& Input)
{
	int32 OpenQuote = INDEX_NONE;
	if (!Input.FindChar(TEXT('\''), OpenQuote))
	{
		return Input;
	}
	int32 CloseQuote = INDEX_NONE;
	if (!Input.FindLastChar(TEXT('\''), CloseQuote) || CloseQuote <= OpenQuote)
	{
		return Input;
	}
	return Input.Mid(OpenQuote + 1, CloseQuote - OpenQuote - 1);
}

// "fixed_bounds" → "FixedBounds", "max_gpu_particles_spawn_per_frame" → "MaxGpuParticlesSpawnPerFrame"
static FString SnakeToPascalCase(const FString& Input)
{
	FString Result;
	Result.Reserve(Input.Len());
	bool bCapitalizeNext = true;
	for (TCHAR C : Input)
	{
		if (C == TEXT('_'))
		{
			bCapitalizeNext = true;
		}
		else if (bCapitalizeNext)
		{
			Result.AppendChar(FChar::ToUpper(C));
			bCapitalizeNext = false;
		}
		else
		{
			Result.AppendChar(C);
		}
	}
	return Result;
}

FProperty* FindPropertyByName(UStruct* StructType, const FString& PropertyName)
{
	if (!StructType || PropertyName.IsEmpty())
	{
		return nullptr;
	}

	// Collect name variants to try. Order matters: most specific → most permissive.
	TArray<FString, TInlineAllocator<4>> Variants;
	Variants.Add(PropertyName);                            // 1) exact

	// PascalCase (either from snake_case input or already-PascalCase input)
	FString Pascal = PropertyName.Contains(TEXT("_"))
		? SnakeToPascalCase(PropertyName)
		: PropertyName;
	if (!Pascal.Equals(PropertyName))
	{
		Variants.Add(Pascal);                              // 2) snake → Pascal (fixed_bounds → FixedBounds)
	}

	// UE bool convention: b + PascalCase (determinism → bDeterminism, local_space → bLocalSpace)
	if (Pascal.Len() > 0 && FChar::IsUpper(Pascal[0]))
	{
		FString BPrefixed = TEXT("b");
		BPrefixed.Append(Pascal);
		Variants.Add(BPrefixed);                           // 3) Pascal → bPascal
	}

	for (const FString& Variant : Variants)
	{
		if (FProperty* Property = StructType->FindPropertyByName(FName(*Variant)))
		{
			return Property;
		}
	}

	// 4) Redirector-aware lookup for each variant — catches properties renamed via UPROPERTY redirects.
	for (const FString& Variant : Variants)
	{
		if (FProperty* Property = PropertyAccessUtil::FindPropertyByName(FName(*Variant), StructType))
		{
			return Property;
		}
	}

	// 5) Case-insensitive fallback — also checks authored name for display-override cases.
	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		for (const FString& Variant : Variants)
		{
			if (It->GetName().Equals(Variant, ESearchCase::IgnoreCase)
				|| It->GetAuthoredName().Equals(Variant, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
	}

	return nullptr;
}

// Thin forwarder — keeps the short name used below. Delegates to NeoLuaEnum::ParseRuntime so every
// enum lookup in the plugin shares one correct implementation (handles "Red" / "EColor::Red" / authored names).
static bool ResolveEnumValue(UEnum* Enum, const FString& TextValue, int64& OutValue)
{
	return NeoLuaEnum::ParseRuntime(Enum, TextValue, OutValue);
}

// True when Property is declared on OwnerObject's class (or a parent). In that case PropertyAccessUtil's
// Object path is valid — we get CanSet checks, archetype propagation, and Pre/PostEditChange. For nested-struct
// leaves or detached struct buffers (DataTable rows), the property's owner is a UStruct, not a UClass, so
// GetOwnerClass() returns nullptr and we fall back to a raw copy.
static bool CanUsePropertyAccessUtilOnObject(const FProperty* Property, const UObject* OwnerObject)
{
	if (!OwnerObject || !Property)
	{
		return false;
	}
	const UClass* OwnerClass = Property->GetOwnerClass();
	return OwnerClass && OwnerObject->GetClass()->IsChildOf(OwnerClass);
}

// RAII scratch buffer sized for the full property (GetSize = ElementSize * ArrayDim), so engine
// APIs that touch the whole value (CopyCompleteValue, InitializeValue) are safe even on fixed-size
// array properties. AllocateAndInitializeValue/DestroyAndFreeValue is the engine-idiomatic pairing.
struct FScratchValue
{
	FProperty* Property = nullptr;
	void* Data = nullptr;

	explicit FScratchValue(FProperty* InProperty)
		: Property(InProperty)
		, Data(InProperty->AllocateAndInitializeValue())
	{
	}

	~FScratchValue()
	{
		if (Data)
		{
			Property->DestroyAndFreeValue(Data);
			Data = nullptr;
		}
	}

	FScratchValue(const FScratchValue&) = delete;
	FScratchValue& operator=(const FScratchValue&) = delete;
};

// Commit a fully-populated scratch buffer to the destination.
//   - Object path (OwnerObject owns Property): PropertyAccessUtil::SetPropertyValue_Object fires CanSet,
//     propagates to archetype instances inheriting the old value, and emits PreEditChange/PostEditChange.
//     EditorReadOnlyFlags (CPF_EditConst) matches the "Set Editor Property" node — writes to properties
//     marked read-only in editor are rejected rather than silently accepted.
//   - Struct-buffer path (nullptr owner or nested struct leaf): raw CopyCompletePropertyValue, no
//     notifications. Callers that care (LevelDesign, Widget configure) wrap externally with PreEditChange
//     on the top-level property.
static bool CommitScratchToDestination(FProperty* Property, void* DestValuePtr, UObject* OwnerObject, const void* Scratch, FString& OutError)
{
	if (CanUsePropertyAccessUtilOnObject(Property, OwnerObject))
	{
		const EPropertyAccessResultFlags Result = PropertyAccessUtil::SetPropertyValue_Object(
			Property,
			OwnerObject,
			Property,
			Scratch,
			/*InArrayIndex=*/0,
			PropertyAccessUtil::EditorReadOnlyFlags,
			EPropertyAccessChangeNotifyMode::Default);

		if (Result == EPropertyAccessResultFlags::Success)
		{
			return true;
		}

		if (EnumHasAnyFlags(Result, EPropertyAccessResultFlags::ReadOnly))
		{
			OutError = FString::Printf(TEXT("property '%s' is read-only in the editor (CPF_EditConst)"), *Property->GetName());
		}
		else if (EnumHasAnyFlags(Result, EPropertyAccessResultFlags::AccessProtected))
		{
			OutError = FString::Printf(TEXT("property '%s' is not exposed for edit or blueprint access"), *Property->GetName());
		}
		else if (EnumHasAnyFlags(Result, EPropertyAccessResultFlags::CannotEditTemplate))
		{
			OutError = FString::Printf(TEXT("property '%s' cannot be edited on templates"), *Property->GetName());
		}
		else if (EnumHasAnyFlags(Result, EPropertyAccessResultFlags::CannotEditInstance))
		{
			OutError = FString::Printf(TEXT("property '%s' cannot be edited on instances"), *Property->GetName());
		}
		else
		{
			OutError = FString::Printf(TEXT("failed to set property '%s'"), *Property->GetName());
		}
		return false;
	}

	PropertyAccessUtil::CopyCompletePropertyValue(Property, Scratch, Property, DestValuePtr);
	return true;
}

bool ClampNumericPropertyValueFromMetaData(FProperty* Property, void* ValuePtr)
{
	if (!Property || !ValuePtr)
	{
		return false;
	}

	FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
	if (!NumericProperty)
	{
		return false;
	}

	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->GetIntPropertyEnum())
		{
			return false;
		}
	}

	const FString MinString = Property->GetMetaData(TEXT("ClampMin"));
	const FString MaxString = Property->GetMetaData(TEXT("ClampMax"));
	if (MinString.IsEmpty() && MaxString.IsEmpty())
	{
		return false;
	}

	bool bChanged = false;
	if (NumericProperty->IsFloatingPoint())
	{
		double Value = NumericProperty->GetFloatingPointPropertyValue(ValuePtr);
		double ClampedValue = Value;
		if (!MinString.IsEmpty())
		{
			ClampedValue = FMath::Max(FCString::Atod(*MinString), ClampedValue);
		}
		if (!MaxString.IsEmpty())
		{
			ClampedValue = FMath::Min(FCString::Atod(*MaxString), ClampedValue);
		}

		if (ClampedValue != Value)
		{
			NumericProperty->SetFloatingPointPropertyValue(ValuePtr, ClampedValue);
			bChanged = true;
		}
	}
	else if (NumericProperty->IsInteger())
	{
		const bool bUnsigned = CastField<FByteProperty>(Property)
			|| CastField<FUInt16Property>(Property)
			|| CastField<FUInt32Property>(Property)
			|| CastField<FUInt64Property>(Property);
		if (bUnsigned)
		{
			uint64 Value = NumericProperty->GetUnsignedIntPropertyValue(ValuePtr);
			uint64 ClampedValue = Value;
			if (!MinString.IsEmpty())
			{
				ClampedValue = FMath::Max(FCString::Strtoui64(*MinString, nullptr, 10), ClampedValue);
			}
			if (!MaxString.IsEmpty())
			{
				ClampedValue = FMath::Min(FCString::Strtoui64(*MaxString, nullptr, 10), ClampedValue);
			}

			if (ClampedValue != Value)
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, ClampedValue);
				bChanged = true;
			}
		}
		else
		{
			int64 Value = NumericProperty->GetSignedIntPropertyValue(ValuePtr);
			int64 ClampedValue = Value;
			if (!MinString.IsEmpty())
			{
				ClampedValue = FMath::Max(FCString::Atoi64(*MinString), ClampedValue);
			}
			if (!MaxString.IsEmpty())
			{
				ClampedValue = FMath::Min(FCString::Atoi64(*MaxString), ClampedValue);
			}

			if (ClampedValue != Value)
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, ClampedValue);
				bChanged = true;
			}
		}
	}

	return bChanged;
}

// Import a string representation into the scratch buffer.
// PropertyAccessUtil::ImportDefaultPropertyValue handles FVector/FRotator/FColor/FLinearColor edge cases
// (these export in a non-standard "X=1,Y=2,Z=3" form) and delegates everything else to ImportText_Direct,
// which handles enum names, object paths (including SubObject paths), and nested struct literals.
static bool ImportStringToScratch(FProperty* Property, const FString& Value, void* Scratch, FString& OutError)
{
	if (PropertyAccessUtil::ImportDefaultPropertyValue(Property, Scratch, Value))
	{
		return true;
	}

	// Lua typically passes lowercase "true"/"false" for bool imports, but FBoolProperty::ImportText wants
	// capitalized "True"/"False". Retry with normalized casing before giving up.
	FString Transformed = Value;
	if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
	{
		Transformed = TEXT("True");
	}
	else if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
	{
		Transformed = TEXT("False");
	}

	if (!Transformed.Equals(Value)
		&& PropertyAccessUtil::ImportDefaultPropertyValue(Property, Scratch, Transformed))
	{
		return true;
	}

	// Special edge cases for structs. Writing the raw string into the wrapper fails; writing it into the inner property succeeds.
	if (FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		FProperty* StructProperty = nullptr;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		if (Struct->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct()))
		{
			//NOTE: All 11 derived types expose a "DefaultValue" property
			StructProperty = Struct->Struct->FindPropertyByName(TEXT("DefaultValue"));
		}
#endif
		if (Struct->Struct == FBlackboardKeySelector::StaticStruct())
		{
			StructProperty = Struct->Struct->FindPropertyByName(TEXT("SelectedKeyName"));
		}

		if (StructProperty)
		{
			void* InnerPtr = StructProperty->ContainerPtrToValuePtr<void>(Scratch);
			if (PropertyAccessUtil::ImportDefaultPropertyValue(StructProperty, InnerPtr, Value))
			{
				return true;
			}
			if (!Transformed.Equals(Value)
				&& PropertyAccessUtil::ImportDefaultPropertyValue(StructProperty, InnerPtr, Transformed))
			{
				return true;
			}
		}
	}

	OutError = FString::Printf(TEXT("failed to parse value \"%s\" for type %s"), *Value, *Property->GetCPPType());
	return false;
}

// Populate scratch from a typed FPropertyValueInput. Numbers/bools write directly into the scratch
// buffer (preserves precision — no stringify round-trip for numerics). Strings fall through to
// ImportDefaultPropertyValue, which handles enum/object-path/struct-literal imports uniformly.
static bool MaterializePropertyInput(FProperty* Property, const FPropertyValueInput& Input, void* Scratch, FString& OutError)
{
	if (Input.bIsNumber)
	{
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(Scratch, Input.NumberValue);
				return true;
			}
			if (NumericProperty->IsInteger())
			{
				NumericProperty->SetIntPropertyValue(Scratch, static_cast<int64>(Input.NumberValue));
				return true;
			}
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(Scratch, Input.NumberValue != 0.0);
			return true;
		}
	}

	if (Input.bIsBool)
	{
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(Scratch, Input.BoolValue);
			return true;
		}
	}

	if (Input.bIsString)
	{
		// Fast path for enum properties — accept short names ("Red"), full names ("EColor::Red"),
		// and authored display names. ImportText would reject some of these forms.
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			int64 EnumValue = INDEX_NONE;
			if (ResolveEnumValue(EnumProperty->GetEnum(), Input.StringValue, EnumValue))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Scratch, EnumValue);
				return true;
			}

			OutError = FString::Printf(TEXT("invalid enum value '%s' for '%s'"), *Input.StringValue, *Property->GetName());
			return false;
		}

		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (UEnum* Enum = ByteProperty->GetIntPropertyEnum())
			{
				int64 EnumValue = INDEX_NONE;
				if (ResolveEnumValue(Enum, Input.StringValue, EnumValue))
				{
					ByteProperty->SetIntPropertyValue(Scratch, EnumValue);
					return true;
				}

				OutError = FString::Printf(TEXT("invalid enum value '%s' for '%s'"), *Input.StringValue, *Property->GetName());
				return false;
			}
		}

		// Soft refs: store a path; the engine resolves lazily, so we never need to
		// load the asset just to assign it. Must come before the FObjectPropertyBase
		// branch since FSoftObjectProperty derives from it.
		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (Input.StringValue.IsEmpty())
			{
				SoftObjectProperty->SetPropertyValue(Scratch, FSoftObjectPtr());
				return true;
			}
			SoftObjectProperty->SetPropertyValue(Scratch, FSoftObjectPtr(FSoftObjectPath(StripObjectPathClassPrefix(Input.StringValue))));
			return true;
		}

		// Hard refs (FObjectProperty / FWeakObjectProperty / FLazyObjectProperty /
		// FInterfaceProperty): the underlying ImportDefaultPropertyValue path calls
		// FObjectProperty::ImportText with a null Parent, which delegates to FindObject
		// — so unloaded assets silently resolve to null. StaticLoadObject loads on
		// demand. Repro before this fix: configure a Niagara mesh renderer's
		// OverrideMaterials[i].ExplicitMat with a path to an engine material that
		// isn't already in memory; the property reads back as null with no error.
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (Input.StringValue.IsEmpty())
			{
				ObjectProperty->SetObjectPropertyValue(Scratch, nullptr);
				return true;
			}

			UClass* PropertyClass = ObjectProperty->PropertyClass;
			if (!PropertyClass)
			{
				OutError = FString::Printf(TEXT("object property '%s' has no PropertyClass"), *Property->GetName());
				return false;
			}

			const FString PathString = StripObjectPathClassPrefix(Input.StringValue);
			UObject* Loaded = StaticLoadObject(PropertyClass, /*Outer*/nullptr, *PathString, nullptr, LOAD_NoWarn | LOAD_Quiet);

			if (!Loaded)
			{
				// Fall back to ImportText for forms StaticLoadObject can't take
				// (sub-object refs, in-memory transient objects, etc.). Preserves
				// any pre-existing working paths.
				return ImportStringToScratch(Property, Input.StringValue, Scratch, OutError);
			}

			if (!Loaded->IsA(PropertyClass))
			{
				OutError = FString::Printf(TEXT("'%s' resolved to %s but property '%s' expects %s"),
					*PathString, *Loaded->GetClass()->GetName(),
					*Property->GetName(), *PropertyClass->GetName());
				return false;
			}

			ObjectProperty->SetObjectPropertyValue(Scratch, Loaded);
			return true;
		}

		return ImportStringToScratch(Property, Input.StringValue, Scratch, OutError);
	}

	OutError = FString::Printf(TEXT("unsupported value for property '%s' (%s)"), *Property->GetName(), *Property->GetCPPType());
	return false;
}

bool SetPropertyValue(FProperty* Property, void* Container, UObject* OwnerObject, const FPropertyValueInput& Input, FString& OutError)
{
	if (!Property)
	{
		OutError = TEXT("null property");
		return false;
	}

	if (!Container)
	{
		OutError = TEXT("null property container");
		return false;
	}

	FScratchValue Scratch(Property);

	// Seed scratch with the current value so struct imports that only touch a subset of fields
	// (e.g. "{Size=24}" on FSlateFontInfo) don't zero out the untouched fields.
	Property->CopyCompleteValue(Scratch.Data, Container);

	if (!MaterializePropertyInput(Property, Input, Scratch.Data, OutError))
	{
		return false;
	}

	ClampNumericPropertyValueFromMetaData(Property, Scratch.Data);

	return CommitScratchToDestination(Property, Container, OwnerObject, Scratch.Data, OutError);
}

bool SetPropertyValueFromStringSafe(FProperty* Property, void* Container, UObject* OwnerObject, const FString& Value, FString& OutError)
{
	return SetPropertyValueFromStringSafe(Property, Container, OwnerObject, Value, []() {}, OutError);
}

bool SetPropertyValueFromStringSafe(
	FProperty* Property,
	void* Container,
	UObject* OwnerObject,
	const FString& Value,
	TFunctionRef<void()> PreCommit,
	FString& OutError)
{
	if (!Property)
	{
		OutError = TEXT("null property");
		return false;
	}

	if (!Container)
	{
		OutError = TEXT("null property container");
		return false;
	}

	// Scratch starts as a copy of the current destination value, so a failed parse leaves the real
	// destination untouched (replaces the old backup-then-restore-on-failure pattern — if we never write
	// to Container before we know the parse succeeded, there's nothing to restore).
	FScratchValue Scratch(Property);
	Property->CopyCompleteValue(Scratch.Data, Container);

	if (!ImportStringToScratch(Property, Value, Scratch.Data, OutError))
	{
		return false;
	}

	ClampNumericPropertyValueFromMetaData(Property, Scratch.Data);

	PreCommit();
	return CommitScratchToDestination(Property, Container, OwnerObject, Scratch.Data, OutError);
}

bool PatchPropertyValueFromString(FProperty* Property, UObject* OwnerObject, const FString& Value, bool bEnableInlineEditConditionToggle, FString& OutError)
{
	if (!Property)
	{
		OutError = TEXT("null property");
		return false;
	}

	if (!OwnerObject)
	{
		OutError = TEXT("null object");
		return false;
	}

	FScratchValue Scratch(Property);

	// Seed from the live value. Direct read (no getter) matches prior behavior; a native getter with
	// side effects would surprise callers doing partial imports.
	if (const void* CurrentValue = Property->ContainerPtrToValuePtr<void>(OwnerObject))
	{
		Property->CopyCompleteValue(Scratch.Data, CurrentValue);
	}

	if (!ImportStringToScratch(Property, Value, Scratch.Data, OutError))
	{
		return false;
	}

	ClampNumericPropertyValueFromMetaData(Property, Scratch.Data);

	if (Property->HasSetter())
	{
		// Respect native UPROPERTY(Setter=...) — the setter owns invariants that a direct memcpy would bypass.
		// PropertyAccessUtil's Object path would skip the setter, so we keep this branch separate.
		Property->CallSetter(OwnerObject, Scratch.Data);
	}
	else
	{
		if (!CommitScratchToDestination(Property, Property->ContainerPtrToValuePtr<void>(OwnerObject), OwnerObject, Scratch.Data, OutError))
		{
			return false;
		}
	}

	if (bEnableInlineEditConditionToggle)
	{
		const FString EditCondition = Property->GetMetaData(TEXT("editcondition"));
		if (!EditCondition.IsEmpty())
		{
			if (FBoolProperty* ToggleProperty = CastField<FBoolProperty>(OwnerObject->GetClass()->FindPropertyByName(*EditCondition)))
			{
				if (ToggleProperty->HasMetaData(TEXT("InlineEditConditionToggle")))
				{
					ToggleProperty->SetPropertyValue_InContainer(OwnerObject, true);
				}
			}
		}
	}

	return true;
}

bool SetNamedPropertyValue(UObject* Object, const FString& PropertyName, const FPropertyValueInput& Input, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("null object");
		return false;
	}

	FProperty* Property = FindPropertyByName(Object->GetClass(), PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("property '%s' not found"), *PropertyName);
		return false;
	}

	return SetPropertyValue(Property, Property->ContainerPtrToValuePtr<void>(Object), Object, Input, OutError);
}

bool SetNamedPropertyValue(void* Container, UStruct* StructType, UObject* OwnerObject, const FString& PropertyName, const FPropertyValueInput& Input, FString& OutError)
{
	FProperty* Property = FindPropertyByName(StructType, PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("property '%s' not found"), *PropertyName);
		return false;
	}

	return SetPropertyValue(Property, Property->ContainerPtrToValuePtr<void>(Container), OwnerObject, Input, OutError);
}

bool SetNamedPropertyValueWithEditChange(UObject* Object, const FString& PropertyName, const FPropertyValueInput& Input, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("null object");
		return false;
	}

	FProperty* Property = FindPropertyByName(Object->GetClass(), PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("property '%s' not found"), *PropertyName);
		return false;
	}

	Object->Modify();
	Object->PreEditChange(Property);

	const bool bOk = SetPropertyValue(Property, Property->ContainerPtrToValuePtr<void>(Object), Object, Input, OutError);

	// Always close the bracket — engine listeners observe both success and failure transitions.
	FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(ChangeEvent);

	return bOk;
}

}
