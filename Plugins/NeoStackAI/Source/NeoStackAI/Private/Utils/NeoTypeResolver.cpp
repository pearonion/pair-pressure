// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Utils/NeoTypeResolver.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace NeoTypeResolver
{

// Fallback finder: iterate loaded objects by short name when TryFindTypeSlow fails.
// TryFindTypeSlow matches against the first object whose *package-qualified* name
// matches, which misses types that weren't registered at that cache point — the
// linear scan catches them.
template<typename T>
static T* FindTypeFallback(const FString& Name)
{
	const FString NameLower = Name.ToLower();
	for (TObjectIterator<T> It; It; ++It)
	{
		if (It->GetName().ToLower() == NameLower)
		{
			return *It;
		}
	}
	return nullptr;
}

// Prefix handling is symmetric: if a C++ type prefix is missing, try adding it;
// if it's present, try stripping it. UE's TryFindTypeSlow indexes under the
// short/authored name (e.g. FDataTableRowHandle is registered as
// "DataTableRowHandle"), so an agent that types the full C++ name
// "FDataTableRowHandle" must also have its prefix stripped to hit the cache.
UClass* FindClassRobust(const FString& ClassName)
{
	UClass* Cls = UClass::TryFindTypeSlow<UClass>(ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("U")))
		Cls = UClass::TryFindTypeSlow<UClass>(TEXT("U") + ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("A")))
		Cls = UClass::TryFindTypeSlow<UClass>(TEXT("A") + ClassName);
	if (!Cls && ClassName.Len() > 1 && (ClassName.StartsWith(TEXT("U")) || ClassName.StartsWith(TEXT("A"))))
		Cls = UClass::TryFindTypeSlow<UClass>(ClassName.RightChop(1));
	if (!Cls) Cls = FindTypeFallback<UClass>(ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("U")))
		Cls = FindTypeFallback<UClass>(TEXT("U") + ClassName);
	if (!Cls && !ClassName.StartsWith(TEXT("A")))
		Cls = FindTypeFallback<UClass>(TEXT("A") + ClassName);
	if (!Cls && ClassName.Len() > 1 && (ClassName.StartsWith(TEXT("U")) || ClassName.StartsWith(TEXT("A"))))
		Cls = FindTypeFallback<UClass>(ClassName.RightChop(1));
	return Cls;
}

UEnum* FindEnumRobust(const FString& EnumName)
{
	UEnum* Enum = UClass::TryFindTypeSlow<UEnum>(EnumName);
	if (!Enum && !EnumName.StartsWith(TEXT("E")))
		Enum = UClass::TryFindTypeSlow<UEnum>(TEXT("E") + EnumName);
	if (!Enum && EnumName.Len() > 1 && EnumName.StartsWith(TEXT("E")))
		Enum = UClass::TryFindTypeSlow<UEnum>(EnumName.RightChop(1));
	if (!Enum) Enum = FindTypeFallback<UEnum>(EnumName);
	if (!Enum && !EnumName.StartsWith(TEXT("E")))
		Enum = FindTypeFallback<UEnum>(TEXT("E") + EnumName);
	if (!Enum && EnumName.Len() > 1 && EnumName.StartsWith(TEXT("E")))
		Enum = FindTypeFallback<UEnum>(EnumName.RightChop(1));
	return Enum;
}

UScriptStruct* FindStructRobust(const FString& StructName)
{
	UScriptStruct* Struct = UClass::TryFindTypeSlow<UScriptStruct>(StructName);
	if (!Struct && !StructName.StartsWith(TEXT("F")))
		Struct = UClass::TryFindTypeSlow<UScriptStruct>(TEXT("F") + StructName);
	if (!Struct && StructName.Len() > 1 && StructName.StartsWith(TEXT("F")))
		Struct = UClass::TryFindTypeSlow<UScriptStruct>(StructName.RightChop(1));
	if (!Struct) Struct = FindTypeFallback<UScriptStruct>(StructName);
	if (!Struct && !StructName.StartsWith(TEXT("F")))
		Struct = FindTypeFallback<UScriptStruct>(TEXT("F") + StructName);
	if (!Struct && StructName.Len() > 1 && StructName.StartsWith(TEXT("F")))
		Struct = FindTypeFallback<UScriptStruct>(StructName.RightChop(1));
	return Struct;
}

static bool ResolveSingleType(const FString& TypeStr, FName& OutCategory, FName& OutSubCategory, TWeakObjectPtr<UObject>& OutSubCategoryObject)
{
	// ---- Primitive types ----
	static const TMap<FString, FName> Primitives = {
		{TEXT("bool"),      UEdGraphSchema_K2::PC_Boolean},
		{TEXT("boolean"),   UEdGraphSchema_K2::PC_Boolean},
		{TEXT("byte"),      UEdGraphSchema_K2::PC_Byte},
		{TEXT("uint8"),     UEdGraphSchema_K2::PC_Byte},
		{TEXT("int"),       UEdGraphSchema_K2::PC_Int},
		{TEXT("integer"),   UEdGraphSchema_K2::PC_Int},
		{TEXT("int32"),     UEdGraphSchema_K2::PC_Int},
		{TEXT("int64"),     UEdGraphSchema_K2::PC_Int64},
		{TEXT("float"),     UEdGraphSchema_K2::PC_Real},
		{TEXT("double"),    UEdGraphSchema_K2::PC_Real},
		{TEXT("real"),      UEdGraphSchema_K2::PC_Real},
		{TEXT("string"),    UEdGraphSchema_K2::PC_String},
		{TEXT("fstring"),   UEdGraphSchema_K2::PC_String},
		{TEXT("name"),      UEdGraphSchema_K2::PC_Name},
		{TEXT("fname"),     UEdGraphSchema_K2::PC_Name},
		{TEXT("text"),      UEdGraphSchema_K2::PC_Text},
		{TEXT("ftext"),     UEdGraphSchema_K2::PC_Text},
		{TEXT("wildcard"),  UEdGraphSchema_K2::PC_Wildcard},
	};

	const FString LowerType = TypeStr.ToLower();

	if (const FName* Category = Primitives.Find(LowerType))
	{
		OutCategory = *Category;
		if (*Category == UEdGraphSchema_K2::PC_Real)
		{
			OutSubCategory = LowerType == TEXT("float")
				? UEdGraphSchema_K2::PC_Float
				: UEdGraphSchema_K2::PC_Double;
		}
		return true;
	}

	// If an explicit prefix was used and the short-name lookup failed, fall back
	// to LoadObject — lets users pass full paths like "/Script/Engine.Actor"
	// without the resolver short-circuiting on the unprefixed asset-path branch
	// further down (which is only reached when no prefix is present).
	auto LoadByPath = [](const FString& Name) -> UObject*
	{
		if (!Name.Contains(TEXT("/")) && !Name.Contains(TEXT("."))) return nullptr;
		return LoadObject<UObject>(nullptr, *Name);
	};

	// ---- Explicit prefix: "class:ClassName" for TSubclassOf / UClass* ----
	if (TypeStr.StartsWith(TEXT("class:"), ESearchCase::IgnoreCase))
	{
		FString ClassName = TypeStr.Mid(6).TrimStartAndEnd();
		UClass* MetaClass = FindClassRobust(ClassName);
		if (!MetaClass) MetaClass = Cast<UClass>(LoadByPath(ClassName));
		if (!MetaClass) return false;

		OutCategory = UEdGraphSchema_K2::PC_Class;
		OutSubCategoryObject = MetaClass;
		return true;
	}

	// ---- Explicit prefix: "softobject:" / "soft_object:" / "soft:" for TSoftObjectPtr ----
	if (TypeStr.StartsWith(TEXT("softobject:"), ESearchCase::IgnoreCase) ||
		TypeStr.StartsWith(TEXT("soft_object:"), ESearchCase::IgnoreCase) ||
		(TypeStr.StartsWith(TEXT("soft:"), ESearchCase::IgnoreCase) && !TypeStr.StartsWith(TEXT("softclass:"), ESearchCase::IgnoreCase)))
	{
		FString ClassName = TypeStr.Mid(TypeStr.Find(TEXT(":")) + 1).TrimStartAndEnd();
		UClass* ObjClass = FindClassRobust(ClassName);
		if (!ObjClass) ObjClass = Cast<UClass>(LoadByPath(ClassName));
		if (!ObjClass) return false;

		OutCategory = UEdGraphSchema_K2::PC_SoftObject;
		OutSubCategoryObject = ObjClass;
		return true;
	}

	// ---- Explicit prefix: "softclass:" / "soft_class:" for TSoftClassPtr ----
	if (TypeStr.StartsWith(TEXT("softclass:"), ESearchCase::IgnoreCase) ||
		TypeStr.StartsWith(TEXT("soft_class:"), ESearchCase::IgnoreCase))
	{
		FString ClassName = TypeStr.Mid(TypeStr.Find(TEXT(":")) + 1).TrimStartAndEnd();
		UClass* MetaClass = FindClassRobust(ClassName);
		if (!MetaClass) MetaClass = Cast<UClass>(LoadByPath(ClassName));
		if (!MetaClass) return false;

		OutCategory = UEdGraphSchema_K2::PC_SoftClass;
		OutSubCategoryObject = MetaClass;
		return true;
	}

	// ---- Explicit prefix: "interface:InterfaceName" ----
	if (TypeStr.StartsWith(TEXT("interface:"), ESearchCase::IgnoreCase))
	{
		FString InterfaceName = TypeStr.Mid(10).TrimStartAndEnd();
		UClass* InterfaceClass = FindClassRobust(InterfaceName);
		if (!InterfaceClass) InterfaceClass = Cast<UClass>(LoadByPath(InterfaceName));
		if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface)) return false;

		OutCategory = UEdGraphSchema_K2::PC_Interface;
		OutSubCategoryObject = InterfaceClass;
		return true;
	}

	// ---- Explicit prefix: "enum:EnumName" ----
	// K2 canonical storage for enum variables is PC_Byte + UEnum (see
	// EdGraphSchema_K2.cpp: PinType.PinCategory = CategoryName == PC_Enum ? PC_Byte : CategoryName).
	// PC_Enum exists only as a UI-picker category label; producing it here
	// creates FEnumProperty pins that refuse to connect to the FByteProperty
	// pins the rest of the engine emits for enum variables.
	if (TypeStr.StartsWith(TEXT("enum:"), ESearchCase::IgnoreCase))
	{
		FString EnumName = TypeStr.Mid(5).TrimStartAndEnd();
		UEnum* Enum = FindEnumRobust(EnumName);
		if (!Enum) Enum = Cast<UEnum>(LoadByPath(EnumName));
		if (!Enum) return false;

		OutCategory = UEdGraphSchema_K2::PC_Byte;
		OutSubCategoryObject = Enum;
		return true;
	}

	// ---- Explicit prefix: "struct:StructName" ----
	if (TypeStr.StartsWith(TEXT("struct:"), ESearchCase::IgnoreCase))
	{
		FString StructName = TypeStr.Mid(7).TrimStartAndEnd();
		UScriptStruct* Struct = FindStructRobust(StructName);
		if (!Struct) Struct = Cast<UScriptStruct>(LoadByPath(StructName));
		if (!Struct) return false;

		OutCategory = UEdGraphSchema_K2::PC_Struct;
		OutSubCategoryObject = Struct;
		return true;
	}

	// ---- Well-known struct aliases (case-insensitive) ----
	// Keeps commonly-used types resolvable before falling into generic reflection.
	static const TMap<FString, FString> StructAliases = {
		// Math
		{TEXT("vector"),            TEXT("FVector")},
		{TEXT("vector2d"),          TEXT("FVector2D")},
		{TEXT("vector4"),           TEXT("FVector4")},
		{TEXT("rotator"),           TEXT("FRotator")},
		{TEXT("transform"),         TEXT("FTransform")},
		{TEXT("quat"),              TEXT("FQuat")},
		{TEXT("quaternion"),        TEXT("FQuat")},
		{TEXT("intpoint"),          TEXT("FIntPoint")},
		{TEXT("intvector"),         TEXT("FIntVector")},
		// Color
		{TEXT("linearcolor"),       TEXT("FLinearColor")},
		{TEXT("color"),             TEXT("FColor")},
		// Gameplay
		{TEXT("gameplaytag"),       TEXT("FGameplayTag")},
		{TEXT("gameplay_tag"),      TEXT("FGameplayTag")},
		{TEXT("tag"),               TEXT("FGameplayTag")},
		{TEXT("gameplaytagcontainer"), TEXT("FGameplayTagContainer")},
		{TEXT("gameplay_tag_container"), TEXT("FGameplayTagContainer")},
		{TEXT("tagcontainer"),      TEXT("FGameplayTagContainer")},
		{TEXT("tag_container"),     TEXT("FGameplayTagContainer")},
		// DateTime / Timespan
		{TEXT("datetime"),          TEXT("FDateTime")},
		{TEXT("timespan"),          TEXT("FTimespan")},
		// Common types
		{TEXT("guid"),              TEXT("FGuid")},
		{TEXT("softobjectpath"),    TEXT("FSoftObjectPath")},
		{TEXT("softclasspath"),     TEXT("FSoftClassPath")},
		{TEXT("primaryassetid"),    TEXT("FPrimaryAssetId")},
		{TEXT("primaryassettype"),  TEXT("FPrimaryAssetType")},
		// Input
		{TEXT("key"),               TEXT("FKey")},
		{TEXT("inputactionvalue"),  TEXT("FInputActionValue")},
		// Hit / overlap
		{TEXT("hitresult"),         TEXT("FHitResult")},
		// Data Table
		{TEXT("datatable"),         TEXT("FDataTableRowHandle")},
		{TEXT("datatablerowhandle"),TEXT("FDataTableRowHandle")},
	};

	if (const FString* CanonicalName = StructAliases.Find(LowerType))
	{
		if (UScriptStruct* Struct = FindStructRobust(*CanonicalName))
		{
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubCategoryObject = Struct;
			return true;
		}
	}

	// ---- Try as struct via reflection (with auto F-prefix) ----
	if (UScriptStruct* Struct = FindStructRobust(TypeStr))
	{
		OutCategory = UEdGraphSchema_K2::PC_Struct;
		OutSubCategoryObject = Struct;
		return true;
	}

	// ---- Try as class/object via reflection (with auto A/U prefix) ----
	if (UClass* ObjClass = FindClassRobust(TypeStr))
	{
		OutCategory = ObjClass->HasAnyClassFlags(CLASS_Interface)
			? UEdGraphSchema_K2::PC_Interface
			: UEdGraphSchema_K2::PC_Object;
		OutSubCategoryObject = ObjClass;
		return true;
	}

	// ---- Try as enum via reflection (with auto E-prefix) ----
	// PC_Byte + UEnum (canonical storage — see PC_Enum note on the explicit prefix above).
	if (UEnum* Enum = FindEnumRobust(TypeStr))
	{
		OutCategory = UEdGraphSchema_K2::PC_Byte;
		OutSubCategoryObject = Enum;
		return true;
	}

	// ---- Try loading by asset path (for user-defined structs/enums/BPs) ----
	if (TypeStr.Contains(TEXT("/")) || TypeStr.Contains(TEXT(".")))
	{
		UObject* Loaded = LoadObject<UObject>(nullptr, *TypeStr);
		if (UScriptStruct* Struct = Cast<UScriptStruct>(Loaded))
		{
			OutCategory = UEdGraphSchema_K2::PC_Struct;
			OutSubCategoryObject = Struct;
			return true;
		}
		if (UEnum* Enum = Cast<UEnum>(Loaded))
		{
			OutCategory = UEdGraphSchema_K2::PC_Byte;
			OutSubCategoryObject = Enum;
			return true;
		}
		if (UClass* Cls = Cast<UClass>(Loaded))
		{
			OutCategory = UEdGraphSchema_K2::PC_Object;
			OutSubCategoryObject = Cls;
			return true;
		}
		if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
		{
			if (BP->GeneratedClass)
			{
				OutCategory = UEdGraphSchema_K2::PC_Object;
				OutSubCategoryObject = BP->GeneratedClass;
				return true;
			}
		}
	}

	return false;
}

bool ResolveTypeString(const FString& VarType, FEdGraphPinType& OutPinType)
{
	FString Trimmed = VarType.TrimStartAndEnd();

	// Sugar: Struct<T>, Class<T>, SoftObject<T>, SoftClass<T>, Enum<T>, Interface<T>.
	// Rewrites to the canonical "prefix:Inner" form and re-enters the resolver.
	// Lets users spell the intent the same way C++/Blueprint type-pickers do,
	// and correctly routes full asset paths through the LoadByPath fallback
	// in ResolveSingleType.
	auto TryTypeSugar = [&](const TCHAR* Keyword, int32 KeywordLen, const TCHAR* Prefix) -> TOptional<bool>
	{
		if (Trimmed.Len() <= KeywordLen + 1) return {};
		if (!Trimmed.StartsWith(Keyword, ESearchCase::IgnoreCase)) return {};
		if (Trimmed[KeywordLen] != TEXT('<') || !Trimmed.EndsWith(TEXT(">"))) return {};
		FString Inner = Trimmed.Mid(KeywordLen + 1, Trimmed.Len() - KeywordLen - 2).TrimStartAndEnd();
		if (Inner.IsEmpty()) return false;
		return ResolveTypeString(FString(Prefix) + Inner, OutPinType);
	};
	if (TOptional<bool> R = TryTypeSugar(TEXT("Struct"),     6, TEXT("struct:")))     return *R;
	if (TOptional<bool> R = TryTypeSugar(TEXT("Class"),      5, TEXT("class:")))      return *R;
	if (TOptional<bool> R = TryTypeSugar(TEXT("SoftObject"), 10, TEXT("softobject:"))) return *R;
	if (TOptional<bool> R = TryTypeSugar(TEXT("SoftClass"),  9, TEXT("softclass:")))  return *R;
	if (TOptional<bool> R = TryTypeSugar(TEXT("Enum"),       4, TEXT("enum:")))       return *R;
	if (TOptional<bool> R = TryTypeSugar(TEXT("Interface"),  9, TEXT("interface:"))) return *R;

	// Template-style container syntax: Array<T>, Set<T>, Map<K,V>
	if (Trimmed.StartsWith(TEXT("Array<"), ESearchCase::IgnoreCase) && Trimmed.EndsWith(TEXT(">")))
	{
		FString Inner = Trimmed.Mid(6, Trimmed.Len() - 7).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("Set<"), ESearchCase::IgnoreCase) && Trimmed.EndsWith(TEXT(">")))
	{
		FString Inner = Trimmed.Mid(4, Trimmed.Len() - 5).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Set;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("Map<"), ESearchCase::IgnoreCase) && Trimmed.EndsWith(TEXT(">")))
	{
		FString Inner = Trimmed.Mid(4, Trimmed.Len() - 5).TrimStartAndEnd();
		int32 CommaIdx;
		if (!Inner.FindChar(TEXT(','), CommaIdx)) return false;

		FString KeyStr = Inner.Left(CommaIdx).TrimStartAndEnd();
		FString ValStr = Inner.Mid(CommaIdx + 1).TrimStartAndEnd();

		FName KeyCat, KeySub;
		TWeakObjectPtr<UObject> KeyObj;
		if (!ResolveSingleType(KeyStr, KeyCat, KeySub, KeyObj)) return false;

		FName ValCat, ValSub;
		TWeakObjectPtr<UObject> ValObj;
		if (!ResolveSingleType(ValStr, ValCat, ValSub, ValObj)) return false;

		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinCategory = KeyCat;
		OutPinType.PinSubCategory = KeySub;
		OutPinType.PinSubCategoryObject = KeyObj;
		OutPinType.PinValueType.TerminalCategory = ValCat;
		OutPinType.PinValueType.TerminalSubCategory = ValSub;
		OutPinType.PinValueType.TerminalSubCategoryObject = ValObj;
		return true;
	}

	// Shorthand suffix: T[]
	if (Trimmed.EndsWith(TEXT("[]")))
	{
		FString Inner = Trimmed.LeftChop(2).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}

	// Colon-prefix containers: array:T, set:T, map:K:V
	if (Trimmed.StartsWith(TEXT("array:"), ESearchCase::IgnoreCase))
	{
		FString Inner = Trimmed.Mid(6).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("set:"), ESearchCase::IgnoreCase))
	{
		FString Inner = Trimmed.Mid(4).TrimStartAndEnd();
		FEdGraphPinType InnerType;
		if (!ResolveTypeString(Inner, InnerType)) return false;
		OutPinType = InnerType;
		OutPinType.ContainerType = EPinContainerType::Set;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("map:"), ESearchCase::IgnoreCase))
	{
		FString Rest = Trimmed.Mid(4).TrimStartAndEnd();
		int32 ColonIdx = INDEX_NONE;
		Rest.FindChar(TEXT(':'), ColonIdx);
		if (ColonIdx == INDEX_NONE) return false;

		FString KeyStr = Rest.Left(ColonIdx).TrimStartAndEnd();
		FString ValStr = Rest.Mid(ColonIdx + 1).TrimStartAndEnd();
		if (KeyStr.IsEmpty() || ValStr.IsEmpty()) return false;

		FName KeyCat, KeySub;
		TWeakObjectPtr<UObject> KeyObj;
		if (!ResolveSingleType(KeyStr, KeyCat, KeySub, KeyObj)) return false;

		FName ValCat, ValSub;
		TWeakObjectPtr<UObject> ValObj;
		if (!ResolveSingleType(ValStr, ValCat, ValSub, ValObj)) return false;

		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinCategory = KeyCat;
		OutPinType.PinSubCategory = KeySub;
		OutPinType.PinSubCategoryObject = KeyObj;
		OutPinType.PinValueType.TerminalCategory = ValCat;
		OutPinType.PinValueType.TerminalSubCategory = ValSub;
		OutPinType.PinValueType.TerminalSubCategoryObject = ValObj;
		return true;
	}

	FName Cat, Sub;
	TWeakObjectPtr<UObject> SubObj;
	if (!ResolveSingleType(Trimmed, Cat, Sub, SubObj)) return false;

	OutPinType.PinCategory = Cat;
	OutPinType.PinSubCategory = Sub;
	OutPinType.PinSubCategoryObject = SubObj;
	return true;
}

} // namespace NeoTypeResolver
