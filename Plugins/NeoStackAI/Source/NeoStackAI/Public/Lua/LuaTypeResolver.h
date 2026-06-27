// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Generic CPP-type-string resolver for Lua-driven binding code.
//
// Replaces hand-rolled per-binding `ResolveCPPType` ladders that hardcode struct
// paths like `{ "Vector" -> "/Script/CoreUObject.Vector" }`. With this helper,
// any UObject-backed type the engine knows about — engine struct (FVector,
// FTransform, FLinearColor, ...), engine class (UStaticMesh, UTexture2D, ...),
// engine enum, user-defined struct/enum at any /Game path, plus all the wrapper
// templates (TArray<>, TSubclassOf<>, TObjectPtr<>, TScriptInterface<>) — is
// resolved via reflection. Adding a new engine struct is auto-supported with
// no binding change.
//
// This is the LCD wrapper that sits below RigVMTypeUtils::ObjectFromCPPType
// without requiring the RigVM module — works for any module that lists
// CoreUObject as a dependency (which is everything).
//
// Usage:
//   FString T = TEXT("FVector");
//   if (UObject* Obj = NeoLuaType::ResolveType(T)) { /* T is now "FVector" canonical */ }
//
//   FString CPP = NeoLuaType::CPPTypeFromObject(Struct); // "FMyStruct"
//
// Modules that already depend on RigVM (NSAI_ControlRig, NSAI_Niagara, etc.)
// may prefer RigVMTypeUtils::ObjectFromCPPType directly when they need its
// extra user-defined-type-with-redirects behaviour. This helper is for the
// other ~20 extensions that don't link RigVM.

#pragma once

#include "CoreMinimal.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

namespace NeoLuaType
{

// ── Constants (mirror RigVMTypeUtils) ────────────────────────────────────────

namespace Detail
{
	inline const TCHAR* TArrayPrefix()           { return TEXT("TArray<"); }
	inline const TCHAR* TSetPrefix()             { return TEXT("TSet<"); }
	inline const TCHAR* TMapPrefix()             { return TEXT("TMap<"); }
	inline const TCHAR* TObjectPtrPrefix()       { return TEXT("TObjectPtr<"); }
	inline const TCHAR* TSubclassOfPrefix()      { return TEXT("TSubclassOf<"); }
	inline const TCHAR* TScriptInterfacePrefix() { return TEXT("TScriptInterface<"); }
	inline const TCHAR* TWeakObjectPtrPrefix()   { return TEXT("TWeakObjectPtr<"); }
	inline const TCHAR* TSoftObjectPtrPrefix()   { return TEXT("TSoftObjectPtr<"); }
	inline const TCHAR* TSoftClassPtrPrefix()    { return TEXT("TSoftClassPtr<"); }
}

// ── Wrapper detection / stripping ────────────────────────────────────────────

inline bool IsArrayType(const FString& T)            { return T.StartsWith(Detail::TArrayPrefix()); }
inline bool IsSetType(const FString& T)              { return T.StartsWith(Detail::TSetPrefix()); }
inline bool IsMapType(const FString& T)              { return T.StartsWith(Detail::TMapPrefix()); }
inline bool IsObjectPtrType(const FString& T)        { return T.StartsWith(Detail::TObjectPtrPrefix()); }
inline bool IsSubclassOfType(const FString& T)       { return T.StartsWith(Detail::TSubclassOfPrefix()); }
inline bool IsScriptInterfaceType(const FString& T)  { return T.StartsWith(Detail::TScriptInterfacePrefix()); }
inline bool IsWeakObjectPtrType(const FString& T)    { return T.StartsWith(Detail::TWeakObjectPtrPrefix()); }
inline bool IsSoftObjectPtrType(const FString& T)    { return T.StartsWith(Detail::TSoftObjectPtrPrefix()); }
inline bool IsSoftClassPtrType(const FString& T)     { return T.StartsWith(Detail::TSoftClassPtrPrefix()); }

inline bool IsWrappedType(const FString& T)
{
	return IsArrayType(T) || IsSetType(T) || IsMapType(T) || IsObjectPtrType(T)
		|| IsSubclassOfType(T) || IsScriptInterfaceType(T)
		|| IsWeakObjectPtrType(T) || IsSoftObjectPtrType(T) || IsSoftClassPtrType(T);
}

// Strip one wrapper level: TArray<FVector> -> FVector. Returns the input if not wrapped.
inline FString StripWrapperOnce(const FString& T)
{
	int32 Open;
	if (T.FindChar(TEXT('<'), Open) && T.EndsWith(TEXT(">")))
	{
		return T.Mid(Open + 1, T.Len() - Open - 2);
	}
	return T;
}

// Strip all wrapper levels: TArray<TObjectPtr<UTexture2D>> -> UTexture2D.
inline FString StripAllWrappers(const FString& T)
{
	FString Cur = T;
	while (IsWrappedType(Cur)) { Cur = StripWrapperOnce(Cur); }
	return Cur;
}

// ── Primitive recognition ───────────────────────────────────────────────────

inline bool IsPrimitiveType(const FString& T)
{
	if (T.IsEmpty()) return false;
	static const TArray<FString> Prims = {
		TEXT("bool"), TEXT("float"), TEXT("double"),
		TEXT("int8"), TEXT("int16"), TEXT("int32"), TEXT("int64"),
		TEXT("uint8"), TEXT("uint16"), TEXT("uint32"), TEXT("uint64"),
		TEXT("FString"), TEXT("FName"), TEXT("FText"),
	};
	return Prims.Contains(T);
}

// Canonicalise common primitive aliases (int -> int32, integer -> int32, string -> FString, etc.).
inline FString NormalisePrimitive(const FString& In)
{
	if (In.Equals(TEXT("int"), ESearchCase::IgnoreCase)
	 || In.Equals(TEXT("integer"), ESearchCase::IgnoreCase)
	 || In.Equals(TEXT("int32"), ESearchCase::IgnoreCase)) return TEXT("int32");
	if (In.Equals(TEXT("bool"), ESearchCase::IgnoreCase))     return TEXT("bool");
	if (In.Equals(TEXT("float"), ESearchCase::IgnoreCase))    return TEXT("float");
	if (In.Equals(TEXT("double"), ESearchCase::IgnoreCase))   return TEXT("double");
	if (In.Equals(TEXT("string"), ESearchCase::IgnoreCase) || In.Equals(TEXT("fstring"), ESearchCase::IgnoreCase))
		return TEXT("FString");
	if (In.Equals(TEXT("name"), ESearchCase::IgnoreCase) || In.Equals(TEXT("fname"), ESearchCase::IgnoreCase))
		return TEXT("FName");
	if (In.Equals(TEXT("text"), ESearchCase::IgnoreCase) || In.Equals(TEXT("ftext"), ESearchCase::IgnoreCase))
		return TEXT("FText");
	return In;
}

// ── Object-from-CPPType resolution ──────────────────────────────────────────

namespace Detail
{
	// FindFirstObject<UField> with NativeFirst preference, falls back to global.
	inline UField* FindFieldByName(const FString& Name)
	{
		if (UField* Found = FindFirstObject<UField>(*Name, EFindFirstObjectOptions::NativeFirst))
			return Found;
		return FindFirstObject<UField>(*Name);
	}

	// Try LoadObject<T> on a /Script/... or /Game/... path.
	template <typename T>
	T* TryLoadByPath(const FString& Path)
	{
		if (!Path.Contains(TEXT(".")) && !Path.StartsWith(TEXT("/"))) return nullptr;
		return LoadObject<T>(nullptr, *Path);
	}
}

// Produce a canonical CPP type string from a UScriptStruct/UClass/UEnum.
// bAsClass=true emits TSubclassOf<UFoo> for UClass; bAsClass=false emits TObjectPtr<UFoo>.
// For interfaces (UClass IsChildOf UInterface), emits TScriptInterface<IFoo>.
inline FString CPPTypeFromObject(const UObject* InObj, bool bAsClass = false)
{
	if (!InObj) return FString();
	if (const UClass* Class = Cast<UClass>(InObj))
	{
		if (bAsClass)
			return FString::Printf(TEXT("TSubclassOf<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
		if (Class->IsChildOf(UInterface::StaticClass()))
			return FString::Printf(TEXT("TScriptInterface<I%s>"), *Class->GetName());
		return FString::Printf(TEXT("TObjectPtr<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
	}
	if (const UScriptStruct* Struct = Cast<UScriptStruct>(InObj))
	{
		return FString::Printf(TEXT("F%s"), *Struct->GetName());
	}
	if (const UEnum* Enum = Cast<UEnum>(InObj))
	{
		return Enum->GetName();
	}
	return FString();
}

// CPP type from an FProperty (handles all wrapper templates via the engine itself).
inline FString CPPTypeFromProperty(const FProperty* Property)
{
	if (!Property) return FString();
	FString ExtendedType;
	FString CPPType = Property->GetCPPType(&ExtendedType);
	CPPType += ExtendedType;
	// Strip stray spaces inside templates: "TArray< FVector >" -> "TArray<FVector>".
	CPPType.ReplaceInline(TEXT("< "), TEXT("<"));
	CPPType.ReplaceInline(TEXT(" >"), TEXT(">"));
	return CPPType;
}

// Resolve a CPP type string to its backing UObject (UScriptStruct, UClass, or UEnum).
// Mutates InOutCPPType to the canonical form. Returns nullptr for primitives and
// unresolvable types (caller should check IsPrimitiveType to disambiguate).
inline UObject* ResolveType(FString& InOutCPPType)
{
	if (InOutCPPType.IsEmpty()) return nullptr;

	// Normalise primitives first (int -> int32, string -> FString, etc.).
	const FString Normalised = NormalisePrimitive(InOutCPPType);
	if (IsPrimitiveType(Normalised))
	{
		InOutCPPType = Normalised;
		return nullptr;
	}

	// Strip outer wrappers. For TArray<FVector>, we resolve FVector and then re-wrap.
	const bool bArray = IsArrayType(InOutCPPType);
	const bool bClass = IsSubclassOfType(InOutCPPType) || IsSoftClassPtrType(InOutCPPType);
	const FString Inner = bArray ? StripWrapperOnce(InOutCPPType) : InOutCPPType;

	// Recurse for arrays so user can pass "TArray<FVector>" or "TArray<TObjectPtr<UTexture2D>>".
	if (bArray)
	{
		FString InnerCanonical = Inner;
		UObject* InnerObj = ResolveType(InnerCanonical);
		if (InnerObj || IsPrimitiveType(InnerCanonical))
		{
			InOutCPPType = FString::Printf(TEXT("TArray<%s>"), *InnerCanonical);
			return InnerObj;
		}
		return nullptr;
	}

	// For object-pointer wrappers, strip down to the bare class name.
	FString Bare = Inner;
	while (IsWrappedType(Bare)) { Bare = StripWrapperOnce(Bare); }

	// Path-based lookup (e.g. "/Script/CoreUObject.Vector", "/Game/MyStruct.MyStruct").
	if (Bare.Contains(TEXT(".")) || Bare.StartsWith(TEXT("/")))
	{
		if (UScriptStruct* S = Detail::TryLoadByPath<UScriptStruct>(Bare))
		{
			InOutCPPType = FString::Printf(TEXT("F%s"), *S->GetName());
			return S;
		}
		if (UClass* C = Detail::TryLoadByPath<UClass>(Bare))
		{
			InOutCPPType = CPPTypeFromObject(C, bClass);
			return C;
		}
		if (UEnum* E = Detail::TryLoadByPath<UEnum>(Bare))
		{
			InOutCPPType = E->GetName();
			return E;
		}
		return nullptr;
	}

	// Strip F/U/A prefix only for fallback name search; leave the canonical form alone.
	FString StrippedName = Bare;
	if (StrippedName.Len() > 1
	 && (StrippedName[0] == TEXT('F') || StrippedName[0] == TEXT('U') || StrippedName[0] == TEXT('A'))
	 && FChar::IsUpper(StrippedName[1]))
	{
		StrippedName = StrippedName.Mid(1);
	}

	// Try original first (UE4 idiomatic short names work directly via FindFirstObject).
	if (UField* F = Detail::FindFieldByName(Bare))
	{
		if (UScriptStruct* S = Cast<UScriptStruct>(F))
		{
			InOutCPPType = FString::Printf(TEXT("F%s"), *S->GetName());
			return S;
		}
		if (UClass* C = Cast<UClass>(F))
		{
			InOutCPPType = CPPTypeFromObject(C, bClass);
			return C;
		}
		if (UEnum* E = Cast<UEnum>(F))
		{
			InOutCPPType = E->GetName();
			return E;
		}
	}

	// Fallback to stripped form.
	if (UField* F = Detail::FindFieldByName(StrippedName))
	{
		if (UScriptStruct* S = Cast<UScriptStruct>(F))
		{
			InOutCPPType = FString::Printf(TEXT("F%s"), *S->GetName());
			return S;
		}
		if (UClass* C = Cast<UClass>(F))
		{
			InOutCPPType = CPPTypeFromObject(C, bClass);
			return C;
		}
		if (UEnum* E = Cast<UEnum>(F))
		{
			InOutCPPType = E->GetName();
			return E;
		}
	}

	return nullptr;
}

// Convenience: const overload that doesn't mutate. Returns canonical form via OutCanonical.
inline UObject* ResolveType(const FString& InCPPType, FString& OutCanonical)
{
	OutCanonical = InCPPType;
	return ResolveType(OutCanonical);
}

} // namespace NeoLuaType
