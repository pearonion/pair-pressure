// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaPropertyTable.h"
#include "Lua/LuaPropertyHelper.h"
#include "Utils/NeoTypeResolver.h"

#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
// UE 5.7 moved PropertyBag into StructUtils/. The old PropertyBag.h at
// Runtime/CoreUObject/Public/PropertyBag.h is a shim that only forwards when
// UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5 is set, which is off for this
// module — so include the authoritative header directly.
//
// UE 5.4 puts PropertyBag in the experimental StructUtils plugin (not on our
// include path) and 5.5 lacks EPropertyBagAlterationResult + the 4-arg
// AddProperty overload. We only compile the bag-binding helpers from 5.6 up;
// older engines get stub implementations of the three public functions that
// return a clear "requires UE 5.6+" error.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "StructUtils/PropertyBag.h"
#include "StructUtils/InstancedStruct.h"
#endif

#include "Math/Vector.h"
#include "Math/Vector2D.h"
#include "Math/Vector4.h"
#include "Math/Quat.h"
#include "Math/Rotator.h"
#include "Math/Color.h"
#include "Math/Box.h"
#include "Math/Box2D.h"
#include "Math/IntPoint.h"
#include "Math/IntVector.h"
#include "Math/Interval.h"
#include "Math/Transform.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Curves/RichCurve.h"
#include "Animation/BoneReference.h"

namespace NeoLuaProperty
{

// ── Handler registry ────────────────────────────────────────────────────

namespace
{

struct FStructHandlerEntry
{
	FStructWriter Writer;
	FStructReader Reader;
};

FCriticalSection GStructHandlerLock;

TMap<const UScriptStruct*, FStructHandlerEntry>& GetHandlers()
{
	static TMap<const UScriptStruct*, FStructHandlerEntry> Handlers;
	return Handlers;
}

void RegisterBuiltinHandlers();

void EnsureBuiltinsRegistered()
{
	static bool bDone = false;
	if (bDone) return;
	FScopeLock Lock(&GStructHandlerLock);
	if (bDone) return;  // double-check under lock
	RegisterBuiltinHandlers();
	bDone = true;
}

const FStructHandlerEntry* FindHandler(const UScriptStruct* StructType)
{
	if (!StructType) return nullptr;
	EnsureBuiltinsRegistered();
	FScopeLock Lock(&GStructHandlerLock);
	return GetHandlers().Find(StructType);
}

// ── Helpers for numeric coercion ─────────────────────────────────────────

double GetNumber(const sol::table& T, const char* Key, const char* UpperKey, double Default)
{
	if (T[Key].valid()) return T.get<double>(Key);
	if (T[UpperKey].valid()) return T.get<double>(UpperKey);
	return Default;
}

template<typename TVec>
bool WriteVec2(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {x,y}"); return false; }
	sol::table T = Val.as<sol::table>();
	TVec* Out = static_cast<TVec*>(Mem);
	Out->X = static_cast<decltype(Out->X)>(GetNumber(T, "x", "X", 0.0));
	Out->Y = static_cast<decltype(Out->Y)>(GetNumber(T, "y", "Y", 0.0));
	return true;
}

template<typename TVec>
sol::object ReadVec2(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const TVec* V = static_cast<const TVec*>(Mem);
	sol::table T = Lua.create_table();
	T["x"] = static_cast<double>(V->X);
	T["y"] = static_cast<double>(V->Y);
	return sol::make_object(Lua, T);
}

template<typename TVec>
bool WriteVec3(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {x,y,z}"); return false; }
	sol::table T = Val.as<sol::table>();
	TVec* Out = static_cast<TVec*>(Mem);
	Out->X = static_cast<decltype(Out->X)>(GetNumber(T, "x", "X", 0.0));
	Out->Y = static_cast<decltype(Out->Y)>(GetNumber(T, "y", "Y", 0.0));
	Out->Z = static_cast<decltype(Out->Z)>(GetNumber(T, "z", "Z", 0.0));
	return true;
}

template<typename TVec>
sol::object ReadVec3(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const TVec* V = static_cast<const TVec*>(Mem);
	sol::table T = Lua.create_table();
	T["x"] = static_cast<double>(V->X);
	T["y"] = static_cast<double>(V->Y);
	T["z"] = static_cast<double>(V->Z);
	return sol::make_object(Lua, T);
}

template<typename TVec>
bool WriteVec4(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {x,y,z,w}"); return false; }
	sol::table T = Val.as<sol::table>();
	TVec* Out = static_cast<TVec*>(Mem);
	Out->X = static_cast<decltype(Out->X)>(GetNumber(T, "x", "X", 0.0));
	Out->Y = static_cast<decltype(Out->Y)>(GetNumber(T, "y", "Y", 0.0));
	Out->Z = static_cast<decltype(Out->Z)>(GetNumber(T, "z", "Z", 0.0));
	Out->W = static_cast<decltype(Out->W)>(GetNumber(T, "w", "W", 0.0));
	return true;
}

template<typename TVec>
sol::object ReadVec4(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const TVec* V = static_cast<const TVec*>(Mem);
	sol::table T = Lua.create_table();
	T["x"] = static_cast<double>(V->X);
	T["y"] = static_cast<double>(V->Y);
	T["z"] = static_cast<double>(V->Z);
	T["w"] = static_cast<double>(V->W);
	return sol::make_object(Lua, T);
}

template<typename TQuat>
bool WriteQuat(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {x,y,z,w}"); return false; }
	sol::table T = Val.as<sol::table>();
	TQuat* Out = static_cast<TQuat*>(Mem);
	Out->X = static_cast<decltype(Out->X)>(GetNumber(T, "x", "X", 0.0));
	Out->Y = static_cast<decltype(Out->Y)>(GetNumber(T, "y", "Y", 0.0));
	Out->Z = static_cast<decltype(Out->Z)>(GetNumber(T, "z", "Z", 0.0));
	Out->W = static_cast<decltype(Out->W)>(GetNumber(T, "w", "W", 1.0));  // identity
	return true;
}

template<typename TQuat>
sol::object ReadQuat(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const TQuat* Q = static_cast<const TQuat*>(Mem);
	sol::table T = Lua.create_table();
	T["x"] = static_cast<double>(Q->X);
	T["y"] = static_cast<double>(Q->Y);
	T["z"] = static_cast<double>(Q->Z);
	T["w"] = static_cast<double>(Q->W);
	return sol::make_object(Lua, T);
}

bool WriteIntPoint(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {x,y}"); return false; }
	sol::table T = Val.as<sol::table>();
	FIntPoint* Out = static_cast<FIntPoint*>(Mem);
	Out->X = static_cast<int32>(GetNumber(T, "x", "X", 0.0));
	Out->Y = static_cast<int32>(GetNumber(T, "y", "Y", 0.0));
	return true;
}

sol::object ReadIntPoint(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FIntPoint* P = static_cast<const FIntPoint*>(Mem);
	sol::table T = Lua.create_table();
	T["x"] = P->X;
	T["y"] = P->Y;
	return sol::make_object(Lua, T);
}

bool WriteIntVector(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {x,y,z}"); return false; }
	sol::table T = Val.as<sol::table>();
	FIntVector* Out = static_cast<FIntVector*>(Mem);
	Out->X = static_cast<int32>(GetNumber(T, "x", "X", 0.0));
	Out->Y = static_cast<int32>(GetNumber(T, "y", "Y", 0.0));
	Out->Z = static_cast<int32>(GetNumber(T, "z", "Z", 0.0));
	return true;
}

sol::object ReadIntVector(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FIntVector* V = static_cast<const FIntVector*>(Mem);
	sol::table T = Lua.create_table();
	T["x"] = V->X;
	T["y"] = V->Y;
	T["z"] = V->Z;
	return sol::make_object(Lua, T);
}

// FFloatInterval / FInt32Interval — Unreal's "paired float/int range" structs.
// Round-tripped as Lua {min, max}. Partial writes supported: {min=0} leaves Max untouched.
bool WriteFloatInterval(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {min, max}"); return false; }
	sol::table T = Val.as<sol::table>();
	FFloatInterval* Out = static_cast<FFloatInterval*>(Mem);
	sol::optional<double> Min = T.get<sol::optional<double>>("min");
	sol::optional<double> Max = T.get<sol::optional<double>>("max");
	if (!Min) Min = T.get<sol::optional<double>>("Min");
	if (!Max) Max = T.get<sol::optional<double>>("Max");
	if (Min.has_value()) Out->Min = static_cast<float>(Min.value());
	if (Max.has_value()) Out->Max = static_cast<float>(Max.value());
	return true;
}

sol::object ReadFloatInterval(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FFloatInterval* I = static_cast<const FFloatInterval*>(Mem);
	sol::table T = Lua.create_table();
	T["min"] = I->Min;
	T["max"] = I->Max;
	return sol::make_object(Lua, T);
}

bool WriteInt32Interval(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {min, max}"); return false; }
	sol::table T = Val.as<sol::table>();
	FInt32Interval* Out = static_cast<FInt32Interval*>(Mem);
	sol::optional<int> Min = T.get<sol::optional<int>>("min");
	sol::optional<int> Max = T.get<sol::optional<int>>("max");
	if (!Min) Min = T.get<sol::optional<int>>("Min");
	if (!Max) Max = T.get<sol::optional<int>>("Max");
	if (Min.has_value()) Out->Min = Min.value();
	if (Max.has_value()) Out->Max = Max.value();
	return true;
}

sol::object ReadInt32Interval(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FInt32Interval* I = static_cast<const FInt32Interval*>(Mem);
	sol::table T = Lua.create_table();
	T["min"] = I->Min;
	T["max"] = I->Max;
	return sol::make_object(Lua, T);
}

bool WriteRotator(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {pitch,yaw,roll} or {p,y,r}"); return false; }
	sol::table T = Val.as<sol::table>();
	FRotator* Out = static_cast<FRotator*>(Mem);
	// Accept both {pitch,yaw,roll}/{Pitch,Yaw,Roll} (plugin-wide convention) and
	// short {p,y,r}/{P,Y,R} (IKRetargeter convention). Long form takes precedence if both appear.
	Out->Pitch = T["pitch"].valid() ? T.get<double>("pitch") : (T["Pitch"].valid() ? T.get<double>("Pitch") : GetNumber(T, "p", "P", 0.0));
	Out->Yaw   = T["yaw"].valid()   ? T.get<double>("yaw")   : (T["Yaw"].valid()   ? T.get<double>("Yaw")   : GetNumber(T, "y", "Y", 0.0));
	Out->Roll  = T["roll"].valid()  ? T.get<double>("roll")  : (T["Roll"].valid()  ? T.get<double>("Roll")  : GetNumber(T, "r", "R", 0.0));
	return true;
}

sol::object ReadRotator(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FRotator* R = static_cast<const FRotator*>(Mem);
	sol::table T = Lua.create_table();
	T["pitch"] = R->Pitch;
	T["yaw"]   = R->Yaw;
	T["roll"]  = R->Roll;
	return sol::make_object(Lua, T);
}

bool WriteLinearColor(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {r,g,b,a}"); return false; }
	sol::table T = Val.as<sol::table>();
	FLinearColor* Out = static_cast<FLinearColor*>(Mem);
	Out->R = GetNumber(T, "r", "R", 0.0);
	Out->G = GetNumber(T, "g", "G", 0.0);
	Out->B = GetNumber(T, "b", "B", 0.0);
	Out->A = GetNumber(T, "a", "A", 1.0);
	return true;
}

sol::object ReadLinearColor(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FLinearColor* C = static_cast<const FLinearColor*>(Mem);
	sol::table T = Lua.create_table();
	T["r"] = C->R; T["g"] = C->G; T["b"] = C->B; T["a"] = C->A;
	return sol::make_object(Lua, T);
}

bool WriteColor(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {r,g,b,a} (0-255)"); return false; }
	sol::table T = Val.as<sol::table>();
	FColor* Out = static_cast<FColor*>(Mem);
	Out->R = static_cast<uint8>(FMath::Clamp(GetNumber(T, "r", "R", 0.0), 0.0, 255.0));
	Out->G = static_cast<uint8>(FMath::Clamp(GetNumber(T, "g", "G", 0.0), 0.0, 255.0));
	Out->B = static_cast<uint8>(FMath::Clamp(GetNumber(T, "b", "B", 0.0), 0.0, 255.0));
	Out->A = static_cast<uint8>(FMath::Clamp(GetNumber(T, "a", "A", 255.0), 0.0, 255.0));
	return true;
}

sol::object ReadColor(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FColor* C = static_cast<const FColor*>(Mem);
	sol::table T = Lua.create_table();
	T["r"] = static_cast<int32>(C->R);
	T["g"] = static_cast<int32>(C->G);
	T["b"] = static_cast<int32>(C->B);
	T["a"] = static_cast<int32>(C->A);
	return sol::make_object(Lua, T);
}

bool WriteBox(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {min={x,y,z}, max={x,y,z}}"); return false; }
	sol::table T = Val.as<sol::table>();
	FBox* Out = static_cast<FBox*>(Mem);

	sol::optional<sol::table> MinT = T.get<sol::optional<sol::table>>("min");
	if (!MinT) MinT = T.get<sol::optional<sol::table>>("Min");
	sol::optional<sol::table> MaxT = T.get<sol::optional<sol::table>>("max");
	if (!MaxT) MaxT = T.get<sol::optional<sol::table>>("Max");

	if (MinT.has_value() && MaxT.has_value())
	{
		Out->Min = FVector(
			GetNumber(MinT.value(), "x", "X", 0.0),
			GetNumber(MinT.value(), "y", "Y", 0.0),
			GetNumber(MinT.value(), "z", "Z", 0.0));
		Out->Max = FVector(
			GetNumber(MaxT.value(), "x", "X", 0.0),
			GetNumber(MaxT.value(), "y", "Y", 0.0),
			GetNumber(MaxT.value(), "z", "Z", 0.0));
		Out->IsValid = 1;
		return true;
	}

	// Accept flat {min_x, min_y, min_z, max_x, max_y, max_z}
	Out->Min = FVector(T.get_or("min_x", 0.0), T.get_or("min_y", 0.0), T.get_or("min_z", 0.0));
	Out->Max = FVector(T.get_or("max_x", 0.0), T.get_or("max_y", 0.0), T.get_or("max_z", 0.0));
	Out->IsValid = 1;
	return true;
}

sol::object ReadBox(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FBox* B = static_cast<const FBox*>(Mem);
	sol::table T = Lua.create_table();
	sol::table Min = Lua.create_table();
	Min["x"] = B->Min.X; Min["y"] = B->Min.Y; Min["z"] = B->Min.Z;
	sol::table Max = Lua.create_table();
	Max["x"] = B->Max.X; Max["y"] = B->Max.Y; Max["z"] = B->Max.Z;
	T["min"] = Min;
	T["max"] = Max;
	return sol::make_object(Lua, T);
}

bool WriteTransform(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<sol::table>()) { OutError = TEXT("expected table {location, rotation, scale}"); return false; }
	sol::table T = Val.as<sol::table>();
	FTransform* Out = static_cast<FTransform*>(Mem);

	sol::optional<sol::table> Loc = T.get<sol::optional<sol::table>>("location");
	if (!Loc) Loc = T.get<sol::optional<sol::table>>("Location");
	sol::optional<sol::table> Rot = T.get<sol::optional<sol::table>>("rotation");
	if (!Rot) Rot = T.get<sol::optional<sol::table>>("Rotation");
	sol::optional<sol::table> Scale = T.get<sol::optional<sol::table>>("scale");
	if (!Scale) Scale = T.get<sol::optional<sol::table>>("Scale");

	if (Loc.has_value())
	{
		Out->SetLocation(FVector(
			GetNumber(Loc.value(), "x", "X", 0.0),
			GetNumber(Loc.value(), "y", "Y", 0.0),
			GetNumber(Loc.value(), "z", "Z", 0.0)));
	}
	if (Rot.has_value())
	{
		sol::table R = Rot.value();
		// Accept either rotator (pitch/yaw/roll) or quat (x/y/z/w)
		if (R["w"].valid() || R["W"].valid())
		{
			Out->SetRotation(FQuat(
				GetNumber(R, "x", "X", 0.0),
				GetNumber(R, "y", "Y", 0.0),
				GetNumber(R, "z", "Z", 0.0),
				GetNumber(R, "w", "W", 1.0)));
		}
		else
		{
			Out->SetRotation(FRotator(
				GetNumber(R, "pitch", "Pitch", 0.0),
				GetNumber(R, "yaw", "Yaw", 0.0),
				GetNumber(R, "roll", "Roll", 0.0)).Quaternion());
		}
	}
	if (Scale.has_value())
	{
		Out->SetScale3D(FVector(
			GetNumber(Scale.value(), "x", "X", 1.0),
			GetNumber(Scale.value(), "y", "Y", 1.0),
			GetNumber(Scale.value(), "z", "Z", 1.0)));
	}
	return true;
}

sol::object ReadTransform(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FTransform* X = static_cast<const FTransform*>(Mem);
	sol::table T = Lua.create_table();
	FVector Loc = X->GetLocation();
	FRotator Rot = X->Rotator();
	FVector Scale = X->GetScale3D();

	sol::table LocT = Lua.create_table();
	LocT["x"] = Loc.X; LocT["y"] = Loc.Y; LocT["z"] = Loc.Z;
	sol::table RotT = Lua.create_table();
	RotT["pitch"] = Rot.Pitch; RotT["yaw"] = Rot.Yaw; RotT["roll"] = Rot.Roll;
	sol::table ScaleT = Lua.create_table();
	ScaleT["x"] = Scale.X; ScaleT["y"] = Scale.Y; ScaleT["z"] = Scale.Z;

	T["location"] = LocT;
	T["rotation"] = RotT;
	T["scale"] = ScaleT;
	return sol::make_object(Lua, T);
}

bool WriteSoftObjectPath(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<std::string>())
	{
		OutError = TEXT("expected string asset path");
		return false;
	}
	FString Path = UTF8_TO_TCHAR(Val.as<std::string>().c_str());
	FSoftObjectPath* Out = static_cast<FSoftObjectPath*>(Mem);
	*Out = FSoftObjectPath(Path);
	return true;
}

sol::object ReadSoftObjectPath(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FSoftObjectPath* P = static_cast<const FSoftObjectPath*>(Mem);
	return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*P->ToString())));
}

bool WriteGuid(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<std::string>())
	{
		OutError = TEXT("expected string guid");
		return false;
	}
	FString Str = UTF8_TO_TCHAR(Val.as<std::string>().c_str());
	FGuid* Out = static_cast<FGuid*>(Mem);
	if (!FGuid::Parse(Str, *Out))
	{
		OutError = FString::Printf(TEXT("could not parse guid '%s'"), *Str);
		return false;
	}
	return true;
}

sol::object ReadGuid(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FGuid* G = static_cast<const FGuid*>(Mem);
	return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*G->ToString())));
}

bool WriteDateTime(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	if (!Val.is<std::string>()) { OutError = TEXT("expected ISO-8601 string"); return false; }
	FString Str = UTF8_TO_TCHAR(Val.as<std::string>().c_str());
	FDateTime* Out = static_cast<FDateTime*>(Mem);
	if (!FDateTime::ParseIso8601(*Str, *Out))
	{
		OutError = FString::Printf(TEXT("could not parse ISO-8601 datetime '%s'"), *Str);
		return false;
	}
	return true;
}

sol::object ReadDateTime(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FDateTime* D = static_cast<const FDateTime*>(Mem);
	return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*D->ToIso8601())));
}

// FBoneReference — used by IK retargeter (Pelvis/Root bones), AnimGraph, SkeletalControl,
// PhysicsAsset, and anywhere the UI lets users pick a bone. Accept either a raw string
// ("pelvis") or a table {bone="pelvis"}/{name="pelvis"}. Cached bone indices are invalidated
// on write so the next Initialize() re-resolves against the owning skeleton.
bool WriteBoneReference(UScriptStruct*, void* Mem, const sol::object& Val, FString& OutError)
{
	FBoneReference* Out = static_cast<FBoneReference*>(Mem);
	FName NewBoneName = NAME_None;

	if (Val.is<std::string>())
	{
		const std::string S = Val.as<std::string>();
		NewBoneName = S.empty() ? NAME_None : FName(UTF8_TO_TCHAR(S.c_str()));
	}
	else if (Val.is<sol::table>())
	{
		sol::table T = Val.as<sol::table>();
		std::string S;
		if (T["bone"].valid())      S = T.get<std::string>("bone");
		else if (T["name"].valid()) S = T.get<std::string>("name");
		else if (T["Bone"].valid()) S = T.get<std::string>("Bone");
		else if (T["BoneName"].valid()) S = T.get<std::string>("BoneName");
		else
		{
			OutError = TEXT("expected {bone=\"...\"} or {name=\"...\"} for FBoneReference");
			return false;
		}
		NewBoneName = S.empty() ? NAME_None : FName(UTF8_TO_TCHAR(S.c_str()));
	}
	else
	{
		OutError = TEXT("expected string bone name or table {bone=\"...\"}");
		return false;
	}

	Out->BoneName = NewBoneName;
	Out->InvalidateCachedBoneIndex();
	return true;
}

sol::object ReadBoneReference(UScriptStruct*, const void* Mem, sol::state_view Lua)
{
	const FBoneReference* B = static_cast<const FBoneReference*>(Mem);
	return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*B->BoneName.ToString())));
}

// Some core math structs (FBox, FTransform, FIntVector) don't have a TBaseStructure<T>
// specialization in UE 5.7 (they're LWC template types — TBox<double>, TTransform<double>,
// TIntVector3<int32>). Their reflection descriptors must be resolved at runtime by path.
UScriptStruct* FindCoreStructByPath(const TCHAR* Path)
{
	return NeoTypeResolver::FindStructRobust(FString(Path));
}

void RegisterBuiltinHandlers()
{
	TMap<const UScriptStruct*, FStructHandlerEntry>& Handlers = GetHandlers();

	auto Add = [&](UScriptStruct* S, FStructWriter W, FStructReader R)
	{
		if (S) Handlers.Add(S, { MoveTemp(W), MoveTemp(R) });
	};

	// Vectors (LWC-aware): FVector is FVector3d in 5.0+
	Add(TBaseStructure<FVector>::Get(),       &WriteVec3<FVector>,    &ReadVec3<FVector>);
	Add(TVariantStructure<FVector3f>::Get(),  &WriteVec3<FVector3f>,  &ReadVec3<FVector3f>);
	Add(TVariantStructure<FVector3d>::Get(),  &WriteVec3<FVector3d>,  &ReadVec3<FVector3d>);

	Add(TBaseStructure<FVector2D>::Get(),     &WriteVec2<FVector2D>,  &ReadVec2<FVector2D>);
	Add(TVariantStructure<FVector2f>::Get(),  &WriteVec2<FVector2f>,  &ReadVec2<FVector2f>);

	Add(TBaseStructure<FVector4>::Get(),      &WriteVec4<FVector4>,   &ReadVec4<FVector4>);
	Add(TVariantStructure<FVector4f>::Get(),  &WriteVec4<FVector4f>,  &ReadVec4<FVector4f>);

	Add(TBaseStructure<FQuat>::Get(),         &WriteQuat<FQuat>,      &ReadQuat<FQuat>);
	Add(TVariantStructure<FQuat4f>::Get(),    &WriteQuat<FQuat4f>,    &ReadQuat<FQuat4f>);

	Add(TBaseStructure<FIntPoint>::Get(),     &WriteIntPoint,         &ReadIntPoint);
	Add(FindCoreStructByPath(TEXT("/Script/CoreUObject.IntVector")), &WriteIntVector, &ReadIntVector);

	// Intervals — {min,max} pairs. Common on FoliageType scale/height/cull ranges, audio
	// RtRoll, Niagara spawn counts, etc. Without these, ApplyTable falls back to generic
	// struct walker which then requires nested {Min=..,Max=..} PascalCase to match.
	Add(TBaseStructure<FFloatInterval>::Get(), &WriteFloatInterval,   &ReadFloatInterval);
	Add(TBaseStructure<FInt32Interval>::Get(), &WriteInt32Interval,   &ReadInt32Interval);

	Add(TBaseStructure<FRotator>::Get(),      &WriteRotator,          &ReadRotator);

	Add(TBaseStructure<FLinearColor>::Get(),  &WriteLinearColor,      &ReadLinearColor);
	Add(TBaseStructure<FColor>::Get(),        &WriteColor,            &ReadColor);

	// FBox / FTransform: no TBaseStructure specialization for LWC — resolve by path.
	// (If the struct name changes, these silently become null and the generic fallback walker
	//  handles them via ApplyTableToStruct, which still works via our FVector/FQuat handlers.)
	Add(FindCoreStructByPath(TEXT("/Script/CoreUObject.Box")),       &WriteBox,       &ReadBox);
	Add(FindCoreStructByPath(TEXT("/Script/CoreUObject.Transform")), &WriteTransform, &ReadTransform);

	Add(TBaseStructure<FSoftObjectPath>::Get(), &WriteSoftObjectPath, &ReadSoftObjectPath);
	Add(TBaseStructure<FGuid>::Get(),         &WriteGuid,             &ReadGuid);

	// FBoneReference has no TBaseStructure<T> specialization — resolve by path.
	Add(FindCoreStructByPath(TEXT("/Script/Engine.BoneReference")), &WriteBoneReference, &ReadBoneReference);
	Add(TBaseStructure<FDateTime>::Get(),     &WriteDateTime,         &ReadDateTime);
}

} // namespace

// ── Public registry API ─────────────────────────────────────────────────

void RegisterStructHandler(UScriptStruct* StructType, FStructWriter Writer, FStructReader Reader)
{
	if (!StructType) return;
	EnsureBuiltinsRegistered();
	FScopeLock Lock(&GStructHandlerLock);
	GetHandlers().Add(StructType, { MoveTemp(Writer), MoveTemp(Reader) });
}

void UnregisterStructHandler(UScriptStruct* StructType)
{
	if (!StructType) return;
	FScopeLock Lock(&GStructHandlerLock);
	GetHandlers().Remove(StructType);
}

bool HasStructHandler(const UScriptStruct* StructType)
{
	if (!StructType) return false;
	EnsureBuiltinsRegistered();
	FScopeLock Lock(&GStructHandlerLock);
	return GetHandlers().Contains(StructType);
}

// ── Apply: Lua → UObject / struct ────────────────────────────────────────

bool ApplyTable(UObject* Target, const sol::table& Props, FString& OutError, TArray<FString>* OutWarnings)
{
	if (!Target) { OutError = TEXT("null target object"); return false; }
	Target->Modify();
	const bool bApplied = ApplyTableToStruct(Target->GetClass(), Target, Target, Props, OutError, OutWarnings);
	if (bApplied)
	{
		Target->PostEditChange();
	}
	return bApplied;
}

bool ApplyTableToStruct(const UStruct* StructDef, void* StructMem, UObject* OwnerForPostEdit,
	const sol::table& Props, FString& OutError, TArray<FString>* OutWarnings)
{
	if (!StructDef || !StructMem)
	{
		OutError = TEXT("null struct def or memory");
		return false;
	}
	EnsureBuiltinsRegistered();

	bool bAnyApplied = false;
	for (auto& KV : Props)
	{
		if (!KV.first.is<std::string>()) continue;
		FString Key = UTF8_TO_TCHAR(KV.first.as<std::string>().c_str());

		FProperty* Prop = FindPropertyByName(const_cast<UStruct*>(StructDef), Key);
		if (!Prop)
		{
			if (OutWarnings) OutWarnings->Add(FString::Printf(TEXT("unknown property '%s'"), *Key));
			continue;
		}
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated | CPF_EditConst))
		{
			if (OutWarnings) OutWarnings->Add(FString::Printf(TEXT("non-editable '%s'"), *Key));
			continue;
		}

		FString FieldError;
		void* PropContainer = Prop->ContainerPtrToValuePtr<void>(StructMem);
		if (!ApplySolValueToProperty(Prop, PropContainer, OwnerForPostEdit, KV.second, FieldError))
		{
			if (OutWarnings) OutWarnings->Add(FString::Printf(TEXT("'%s': %s"), *Key, *FieldError));
		}
		else
		{
			bAnyApplied = true;
		}
	}
	return bAnyApplied;
}

bool ApplySolValueToProperty(FProperty* Prop, void* Container, UObject* Owner, const sol::object& Val, FString& OutError)
{
	if (!Prop)      { OutError = TEXT("null property"); return false; }
	if (!Container) { OutError = TEXT("null container"); return false; }
	if (!Val.valid() || Val.is<sol::lua_nil_t>())
	{
		OutError = TEXT("value is nil");
		return false;
	}
	EnsureBuiltinsRegistered();

	// ── Struct property: handler → nested table → T3D string fallback ──
	if (FStructProperty* SP = CastField<FStructProperty>(Prop))
	{
		if (const FStructHandlerEntry* Handler = FindHandler(SP->Struct))
		{
			return Handler->Writer(SP->Struct, Container, Val, OutError);
		}

		if (Val.is<sol::table>())
		{
			TArray<FString> Warnings;
			const bool bOk = ApplyTableToStruct(SP->Struct, Container, Owner, Val.as<sol::table>(), OutError, &Warnings);
			if (!bOk && Warnings.Num() > 0 && OutError.IsEmpty())
			{
				OutError = FString::Join(Warnings, TEXT("; "));
			}
			return bOk;
		}

		if (Val.is<std::string>())
		{
			FString Str = UTF8_TO_TCHAR(Val.as<std::string>().c_str());
			if (Prop->ImportText_Direct(*Str, Container, Owner, PPF_None))
			{
				return true;
			}
			OutError = FString::Printf(TEXT("could not parse '%s' as %s"), *Str, *SP->Struct->GetName());
			return false;
		}

		OutError = FString::Printf(TEXT("unsupported Lua type for struct '%s'"), *SP->Struct->GetName());
		return false;
	}

	// ── Array property ──
	if (FArrayProperty* AP = CastField<FArrayProperty>(Prop))
	{
		if (!Val.is<sol::table>())
		{
			OutError = TEXT("array property requires a Lua sequence");
			return false;
		}
		sol::table T = Val.as<sol::table>();
		FScriptArrayHelper Helper(AP, Container);
		Helper.EmptyValues();
		for (int32 i = 1; T[i].valid(); ++i)
		{
			const int32 NewIdx = Helper.AddValue();
			void* ElemPtr = Helper.GetRawPtr(NewIdx);
			FString ElemError;
			sol::object ElemVal = T[i];
			if (!ApplySolValueToProperty(AP->Inner, ElemPtr, nullptr, ElemVal, ElemError))
			{
				OutError = FString::Printf(TEXT("array element %d: %s"), i, *ElemError);
				return false;
			}
		}
		return true;
	}

	// ── Map property ──
	if (FMapProperty* MP = CastField<FMapProperty>(Prop))
	{
		if (!Val.is<sol::table>())
		{
			OutError = TEXT("map property requires a Lua table");
			return false;
		}
		sol::table T = Val.as<sol::table>();
		FScriptMapHelper Helper(MP, Container);
		Helper.EmptyValues();

		for (auto& Pair : T)
		{
			if (!Pair.first.valid() || Pair.first.is<sol::lua_nil_t>() || Pair.first.is<sol::table>())
			{
				OutError = TEXT("map property requires scalar keys");
				return false;
			}

			FString KeyStr;
			if (Pair.first.is<std::string>())
			{
				KeyStr = UTF8_TO_TCHAR(Pair.first.as<std::string>().c_str());
			}
			else if (Pair.first.is<double>())
			{
				KeyStr = FString::SanitizeFloat(Pair.first.as<double>());
			}
			else if (Pair.first.is<bool>())
			{
				KeyStr = Pair.first.as<bool>() ? TEXT("true") : TEXT("false");
			}
			else
			{
				OutError = TEXT("map property requires scalar string/number/bool keys");
				return false;
			}

			const int32 NewIdx = Helper.AddDefaultValue_Invalid_NeedsRehash();

			FString KeyFieldError;
			if (!ApplySolValueToProperty(MP->KeyProp, Helper.GetKeyPtr(NewIdx), nullptr, Pair.first, KeyFieldError))
			{
				OutError = FString::Printf(TEXT("map key '%s': %s"), *KeyStr, *KeyFieldError);
				return false;
			}

			FString ValueError;
			if (!ApplySolValueToProperty(MP->ValueProp, Helper.GetValuePtr(NewIdx), nullptr, Pair.second, ValueError))
			{
				OutError = FString::Printf(TEXT("map value for '%s': %s"), *KeyStr, *ValueError);
				return false;
			}
		}
		Helper.Rehash();
		return true;
	}

	if (FSetProperty* SP = CastField<FSetProperty>(Prop))
	{
		if (!Val.is<sol::table>())
		{
			OutError = TEXT("set property requires a Lua sequence");
			return false;
		}

		sol::table T = Val.as<sol::table>();
		FScriptSetHelper Helper(SP, Container);
		Helper.EmptyElements();

		for (int32 i = 1; T[i].valid(); ++i)
		{
			const int32 NewIdx = Helper.AddDefaultValue_Invalid_NeedsRehash();
			void* ElemPtr = Helper.GetElementPtr(NewIdx);
			FString ElemError;
			sol::object ElemVal = T[i];
			if (!ApplySolValueToProperty(SP->ElementProp, ElemPtr, nullptr, ElemVal, ElemError))
			{
				OutError = FString::Printf(TEXT("set element %d: %s"), i, *ElemError);
				Helper.Rehash();
				return false;
			}
		}

		Helper.Rehash();
		return true;
	}

	// ── Primitives via boolean ──
	if (Val.is<bool>())
	{
		if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		{
			BP->SetPropertyValue(Container, Val.as<bool>());
			return true;
		}
		// Treat boolean as 1/0 for numeric props (edge case)
		if (FNumericProperty* NP = CastField<FNumericProperty>(Prop))
		{
			const double Dbl = Val.as<bool>() ? 1.0 : 0.0;
			if (NP->IsFloatingPoint()) NP->SetFloatingPointPropertyValue(Container, Dbl);
			else NP->SetIntPropertyValue(Container, static_cast<int64>(Dbl));
			ClampNumericPropertyValueFromMetaData(Prop, Container);
			return true;
		}
	}

	// ── Primitives via number ──
	if (Val.is<double>())
	{
		const double Num = Val.as<double>();
		// Enums also accept integer underlying values (preserves back-compat with Lua
		// scripts that passed the numeric ENiagara* enum index).
		if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
		{
			EP->GetUnderlyingProperty()->SetIntPropertyValue(Container, static_cast<int64>(Num));
			return true;
		}
		if (FByteProperty* ByteP = CastField<FByteProperty>(Prop))
		{
			ByteP->SetIntPropertyValue(Container, static_cast<int64>(Num));
			return true;
		}
		if (FNumericProperty* NP = CastField<FNumericProperty>(Prop))
		{
			if (NP->IsFloatingPoint()) NP->SetFloatingPointPropertyValue(Container, Num);
			else NP->SetIntPropertyValue(Container, static_cast<int64>(Num));
			ClampNumericPropertyValueFromMetaData(Prop, Container);
			return true;
		}
		if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		{
			BP->SetPropertyValue(Container, Num != 0.0);
			return true;
		}
	}

	// ── Strings: delegate to existing robust setter for enums, objects, T3D fallback ──
	if (Val.is<std::string>())
	{
		const FString StringValue = UTF8_TO_TCHAR(Val.as<std::string>().c_str());
		if (FStrProperty* StrP = CastField<FStrProperty>(Prop))
		{
			StrP->SetPropertyValue(Container, StringValue);
			return true;
		}
		if (FNameProperty* NameP = CastField<FNameProperty>(Prop))
		{
			NameP->SetPropertyValue(Container, FName(*StringValue));
			return true;
		}
		if (FTextProperty* TextP = CastField<FTextProperty>(Prop))
		{
			TextP->SetPropertyValue(Container, FText::FromString(StringValue));
			return true;
		}

		FPropertyValueInput Input;
		Input.bIsString = true;
		Input.StringValue = StringValue;
		FString Err;
		if (SetPropertyValue(Prop, Container, Owner, Input, Err))
		{
			return true;
		}
		OutError = Err;
		return false;
	}

	OutError = FString::Printf(TEXT("unsupported Lua value type for property '%s' (%s)"),
		*Prop->GetName(), *Prop->GetCPPType());
	return false;
}

bool ApplyValueToStructMemory(UScriptStruct* Struct, void* Memory, const sol::object& Value,
	UObject* OwnerForPostEdit, FString& OutError)
{
	if (!Struct || !Memory)
	{
		OutError = TEXT("null struct def or memory");
		return false;
	}
	if (!Value.valid() || Value.is<sol::lua_nil_t>())
	{
		OutError = TEXT("value is nil");
		return false;
	}
	EnsureBuiltinsRegistered();

	if (const FStructHandlerEntry* Handler = FindHandler(Struct))
	{
		if (Handler->Writer(Struct, Memory, Value, OutError))
		{
			return true;
		}
		if (!Value.is<std::string>())
		{
			return false;
		}
	}

	if (Value.is<sol::table>())
	{
		TArray<FString> Warnings;
		const bool bOk = ApplyTableToStruct(Struct, Memory, OwnerForPostEdit,
			Value.as<sol::table>(), OutError, &Warnings);
		if (!bOk && OutError.IsEmpty() && Warnings.Num() > 0)
		{
			OutError = FString::Join(Warnings, TEXT("; "));
		}
		return bOk;
	}

	if (Value.is<std::string>())
	{
		FString Str = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
		// Use the struct's own T3D text import if supported
		UScriptStruct::ICppStructOps* Ops = Struct->GetCppStructOps();
		if (Ops && Ops->HasImportTextItem())
		{
			const TCHAR* Buf = *Str;
			if (Ops->ImportTextItem(Buf, Memory, PPF_None, OwnerForPostEdit, GWarn))
			{
				return true;
			}
		}
		if (Struct->ImportText(*Str, Memory, OwnerForPostEdit, PPF_None, GWarn, Struct->GetName()) != nullptr)
		{
			return true;
		}
		OutError = FString::Printf(TEXT("could not parse '%s' as struct '%s'"), *Str, *Struct->GetName());
		return false;
	}

	OutError = FString::Printf(TEXT("unsupported Lua type for struct '%s'"), *Struct->GetName());
	return false;
}

// ── Read: UObject / struct → Lua ─────────────────────────────────────────

sol::table ReadAsTable(UObject* Source, sol::state_view Lua, int64 SkipFlags)
{
	if (!Source) return Lua.create_table();
	return ReadStructAsTable(Source->GetClass(), Source, Lua, SkipFlags);
}

sol::table ReadStructAsTable(const UStruct* StructDef, const void* StructMem, sol::state_view Lua, int64 SkipFlags)
{
	sol::table Out = Lua.create_table();
	if (!StructDef || !StructMem) return Out;
	EnsureBuiltinsRegistered();

	if (SkipFlags == 0) SkipFlags = CPF_Deprecated | CPF_Transient;

	for (TFieldIterator<FProperty> It(StructDef); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop || Prop->HasAnyPropertyFlags(SkipFlags)) continue;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		const void* ValPtr = Prop->ContainerPtrToValuePtr<void>(StructMem);
		sol::object Val = ReadPropertyAsSol(Prop, ValPtr, Lua);
		if (Val.valid() && !Val.is<sol::lua_nil_t>())
		{
			// Keep the authored name as the key (matches what writers accept via FindPropertyByName)
			Out[TCHAR_TO_UTF8(*Prop->GetAuthoredName())] = Val;
		}
	}
	return Out;
}

sol::object ReadStructMemoryAsSol(UScriptStruct* Struct, const void* Memory, sol::state_view Lua)
{
	if (!Struct || !Memory) return sol::make_object(Lua, sol::lua_nil);
	EnsureBuiltinsRegistered();

	if (const FStructHandlerEntry* Handler = FindHandler(Struct))
	{
		return Handler->Reader(Struct, Memory, Lua);
	}
	return sol::make_object(Lua, ReadStructAsTable(Struct, Memory, Lua));
}

sol::object ReadPropertyAsSol(FProperty* Prop, const void* Container, sol::state_view Lua)
{
	if (!Prop || !Container) return sol::make_object(Lua, sol::lua_nil);
	EnsureBuiltinsRegistered();

	if (FStructProperty* SP = CastField<FStructProperty>(Prop))
	{
		if (const FStructHandlerEntry* Handler = FindHandler(SP->Struct))
		{
			return Handler->Reader(SP->Struct, Container, Lua);
		}
		return sol::make_object(Lua, ReadStructAsTable(SP->Struct, Container, Lua));
	}

	if (FArrayProperty* AP = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(AP, Container);
		sol::table T = Lua.create_table();
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			T[i + 1] = ReadPropertyAsSol(AP->Inner, Helper.GetRawPtr(i), Lua);
		}
		return sol::make_object(Lua, T);
	}

	if (FMapProperty* MP = CastField<FMapProperty>(Prop))
	{
		FScriptMapHelper Helper(MP, Container);
		sol::table T = Lua.create_table();
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			FString KeyStr;
			MP->KeyProp->ExportTextItem_Direct(KeyStr, Helper.GetKeyPtr(It), nullptr, nullptr, PPF_None);
			sol::object ValObj = ReadPropertyAsSol(MP->ValueProp, Helper.GetValuePtr(It), Lua);
			T[TCHAR_TO_UTF8(*KeyStr)] = ValObj;
		}
		return sol::make_object(Lua, T);
	}

	if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
	{
		return sol::make_object(Lua, BP->GetPropertyValue(Container));
	}

	if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
	{
		const int64 Value = EP->GetUnderlyingProperty()->GetSignedIntPropertyValue(Container);
		const FString Name = EP->GetEnum()->GetAuthoredNameStringByValue(Value);
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Name)));
	}

	if (FByteProperty* ByteP = CastField<FByteProperty>(Prop))
	{
		if (UEnum* Enum = ByteP->GetIntPropertyEnum())
		{
			const int64 Value = ByteP->GetSignedIntPropertyValue(Container);
			const FString Name = Enum->GetAuthoredNameStringByValue(Value);
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Name)));
		}
		return sol::make_object(Lua, static_cast<double>(ByteP->GetPropertyValue(Container)));
	}

	if (FNumericProperty* NP = CastField<FNumericProperty>(Prop))
	{
		if (NP->IsFloatingPoint())
		{
			return sol::make_object(Lua, NP->GetFloatingPointPropertyValue(Container));
		}
		return sol::make_object(Lua, static_cast<double>(NP->GetSignedIntPropertyValue(Container)));
	}

	if (FStrProperty* StrP = CastField<FStrProperty>(Prop))
	{
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*StrP->GetPropertyValue(Container))));
	}
	if (FNameProperty* NameP = CastField<FNameProperty>(Prop))
	{
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*NameP->GetPropertyValue(Container).ToString())));
	}
	if (FTextProperty* TextP = CastField<FTextProperty>(Prop))
	{
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*TextP->GetPropertyValue(Container).ToString())));
	}

	if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* Obj = OP->GetObjectPropertyValue(Container);
		if (!Obj) return sol::make_object(Lua, std::string(""));
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Obj->GetPathName())));
	}
	if (FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Prop))
	{
		const FSoftObjectPath Path = SOP->GetPropertyValue(Container).ToSoftObjectPath();
		return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Path.ToString())));
	}

	// Fallback: ExportText
	FString Out;
	Prop->ExportTextItem_Direct(Out, Container, nullptr, nullptr, PPF_None);
	return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Out)));
}

// ── Curve populators ─────────────────────────────────────────────────────

namespace
{

ERichCurveInterpMode ParseInterpMode(const FString& S)
{
	const FString Lower = S.ToLower();
	if (Lower == TEXT("linear"))   return RCIM_Linear;
	if (Lower == TEXT("constant")) return RCIM_Constant;
	if (Lower == TEXT("cubic"))    return RCIM_Cubic;
	return RCIM_Cubic;  // default — matches FRichCurve::AddKey default behavior
}

const TCHAR* InterpModeToString(ERichCurveInterpMode Mode)
{
	switch (Mode)
	{
	case RCIM_Linear:   return TEXT("linear");
	case RCIM_Constant: return TEXT("constant");
	case RCIM_Cubic:    return TEXT("cubic");
	default:            return TEXT("auto");
	}
}

} // namespace

bool ApplyKeysToCurve(FRichCurve& OutCurve, const sol::table& Keys,
	TFunctionRef<float(const sol::table&)> ValueExtractor, FString& OutError)
{
	OutCurve.Reset();

	int32 Index = 0;
	for (auto& KV : Keys)
	{
		++Index;
		if (!KV.second.is<sol::table>())
		{
			OutError = FString::Printf(TEXT("curve key %d: expected table with 'time' and value fields"), Index);
			return false;
		}
		sol::table K = KV.second.as<sol::table>();
		const float Time  = static_cast<float>(K.get_or("time", 0.0));
		const float Value = ValueExtractor(K);

		const FKeyHandle Handle = OutCurve.AddKey(Time, Value);

		// Optional per-key interp mode
		sol::optional<std::string> InterpOpt = K.get<sol::optional<std::string>>("interp");
		if (InterpOpt.has_value())
		{
			OutCurve.SetKeyInterpMode(Handle, ParseInterpMode(UTF8_TO_TCHAR(InterpOpt.value().c_str())));
		}
	}

	OutCurve.AutoSetTangents();
	return true;
}

sol::table ReadCurveAsKeys(const FRichCurve& Curve, sol::state_view Lua)
{
	sol::table Out = Lua.create_table();
	int32 Idx = 1;
	for (auto It = Curve.GetKeyIterator(); It; ++It)
	{
		const FRichCurveKey& K = *It;
		sol::table E = Lua.create_table();
		E["time"]   = static_cast<double>(K.Time);
		E["value"]  = static_cast<double>(K.Value);
		E["interp"] = std::string(TCHAR_TO_UTF8(InterpModeToString(K.InterpMode)));
		Out[Idx++] = E;
	}
	return Out;
}

// ── Property bag helpers ────────────────────────────────────────────────
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
namespace
{

EPropertyBagPropertyType ParseBagScalarType(const FString& S)
{
	if (S.Equals(TEXT("Bool"),    ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Bool;
	if (S.Equals(TEXT("Byte"),    ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Byte;
	if (S.Equals(TEXT("Int32"),   ESearchCase::IgnoreCase) || S.Equals(TEXT("Int"), ESearchCase::IgnoreCase) || S.Equals(TEXT("Integer"), ESearchCase::IgnoreCase))
		return EPropertyBagPropertyType::Int32;
	if (S.Equals(TEXT("Int64"),   ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Int64;
	if (S.Equals(TEXT("UInt32"),  ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::UInt32;
	if (S.Equals(TEXT("UInt64"),  ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::UInt64;
	if (S.Equals(TEXT("Float"),   ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Float;
	if (S.Equals(TEXT("Double"),  ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Double;
	if (S.Equals(TEXT("Name"),    ESearchCase::IgnoreCase) || S.Equals(TEXT("FName"), ESearchCase::IgnoreCase))
		return EPropertyBagPropertyType::Name;
	if (S.Equals(TEXT("String"),  ESearchCase::IgnoreCase) || S.Equals(TEXT("FString"), ESearchCase::IgnoreCase))
		return EPropertyBagPropertyType::String;
	if (S.Equals(TEXT("Text"),    ESearchCase::IgnoreCase) || S.Equals(TEXT("FText"), ESearchCase::IgnoreCase))
		return EPropertyBagPropertyType::Text;
	if (S.Equals(TEXT("Enum"),       ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Enum;
	if (S.Equals(TEXT("Struct"),     ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Struct;
	if (S.Equals(TEXT("Object"),     ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Object;
	if (S.Equals(TEXT("SoftObject"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::SoftObject;
	if (S.Equals(TEXT("Class"),      ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Class;
	if (S.Equals(TEXT("SoftClass"),  ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::SoftClass;
	return EPropertyBagPropertyType::None;
}

EPropertyBagContainerType ParseBagContainer(const FString& S)
{
	if (S.Equals(TEXT("Array"), ESearchCase::IgnoreCase)) return EPropertyBagContainerType::Array;
	if (S.Equals(TEXT("Set"),   ESearchCase::IgnoreCase)) return EPropertyBagContainerType::Set;
	return EPropertyBagContainerType::None;
}

UObject* ResolveTypeObject(EPropertyBagPropertyType Type, const FString& Ref)
{
	if (Ref.IsEmpty()) return nullptr;
	if (Type == EPropertyBagPropertyType::Enum)
	{
		if (UEnum* E = FindObject<UEnum>(nullptr, *Ref)) return E;
		for (TObjectIterator<UEnum> It; It; ++It)
			if (It->GetName().Equals(Ref, ESearchCase::IgnoreCase)) return *It;
		return nullptr;
	}
	if (Type == EPropertyBagPropertyType::Struct)
	{
		if (UScriptStruct* S = FindObject<UScriptStruct>(nullptr, *Ref)) return S;
		FString Candidate = Ref.StartsWith(TEXT("F")) ? Ref : FString::Printf(TEXT("F%s"), *Ref);
		for (TObjectIterator<UScriptStruct> It; It; ++It)
			if (It->GetName().Equals(Ref, ESearchCase::IgnoreCase) || It->GetName().Equals(Candidate, ESearchCase::IgnoreCase)) return *It;
		return nullptr;
	}
	// Object / SoftObject / Class / SoftClass all want a UClass*
	if (UClass* C = FindObject<UClass>(nullptr, *Ref)) return C;
	FString Candidate = Ref.StartsWith(TEXT("U")) || Ref.StartsWith(TEXT("A")) ? Ref : FString::Printf(TEXT("U%s"), *Ref);
	for (TObjectIterator<UClass> It; It; ++It)
		if (It->GetName().Equals(Ref, ESearchCase::IgnoreCase) || It->GetName().Equals(Candidate, ESearchCase::IgnoreCase)) return *It;
	return nullptr;
}

// Resolve TypeSpec → (type, container, typeObject)
bool ResolveTypeSpec(const sol::object& Spec,
                     EPropertyBagPropertyType& OutType,
                     EPropertyBagContainerType& OutContainer,
                     UObject*& OutTypeObject,
                     FString& OutError)
{
	OutContainer = EPropertyBagContainerType::None;
	OutTypeObject = nullptr;

	if (Spec.is<std::string>())
	{
		OutType = ParseBagScalarType(UTF8_TO_TCHAR(Spec.as<std::string>().c_str()));
		if (OutType == EPropertyBagPropertyType::None)
		{
			OutError = FString::Printf(TEXT("unknown type '%s'"), UTF8_TO_TCHAR(Spec.as<std::string>().c_str()));
			return false;
		}
		return true;
	}
	if (!Spec.is<sol::table>())
	{
		OutError = TEXT("type spec must be a string or a table");
		return false;
	}

	sol::table T = Spec.as<sol::table>();
	FString TypeStr  = UTF8_TO_TCHAR(T.get_or<std::string>("type", "").c_str());
	FString ContStr  = UTF8_TO_TCHAR(T.get_or<std::string>("container", "").c_str());
	OutType = ParseBagScalarType(TypeStr);
	if (OutType == EPropertyBagPropertyType::None)
	{
		OutError = FString::Printf(TEXT("unknown type '%s'"), *TypeStr);
		return false;
	}
	if (!ContStr.IsEmpty())
	{
		OutContainer = ParseBagContainer(ContStr);
		if (OutContainer == EPropertyBagContainerType::None && !ContStr.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("unknown container '%s' (use Array or Set)"), *ContStr);
			return false;
		}
	}

	FString Ref;
	if (OutType == EPropertyBagPropertyType::Enum)
		Ref = UTF8_TO_TCHAR(T.get_or<std::string>("enum", "").c_str());
	else if (OutType == EPropertyBagPropertyType::Struct)
		Ref = UTF8_TO_TCHAR(T.get_or<std::string>("struct", "").c_str());
	else if (OutType == EPropertyBagPropertyType::Object || OutType == EPropertyBagPropertyType::SoftObject
	      || OutType == EPropertyBagPropertyType::Class  || OutType == EPropertyBagPropertyType::SoftClass)
		Ref = UTF8_TO_TCHAR(T.get_or<std::string>("class", "").c_str());

	if (!Ref.IsEmpty())
	{
		OutTypeObject = ResolveTypeObject(OutType, Ref);
		if (!OutTypeObject)
		{
			OutError = FString::Printf(TEXT("could not resolve %s '%s'"),
				*UEnum::GetValueAsString(OutType), *Ref);
			return false;
		}
	}
	else if (OutType == EPropertyBagPropertyType::Enum || OutType == EPropertyBagPropertyType::Struct
	      || OutType == EPropertyBagPropertyType::Object || OutType == EPropertyBagPropertyType::SoftObject
	      || OutType == EPropertyBagPropertyType::Class  || OutType == EPropertyBagPropertyType::SoftClass)
	{
		OutError = FString::Printf(TEXT("%s type requires a reference (enum=/struct=/class=)"),
			*UEnum::GetValueAsString(OutType));
		return false;
	}
	return true;
}

} // namespace

bool AddPropertyBagProperty(FInstancedPropertyBag& Bag, FName PropName,
                             const sol::object& TypeSpec, FString& OutError)
{
	FPropertyBagPropertyDesc Desc;
	if (!MakePropertyBagPropertyDesc(PropName, TypeSpec, Desc, OutError))
		return false;

	const EPropertyBagAlterationResult Result = Bag.AddProperties({ Desc }, /*bOverwrite=*/ false);
	if (Result == EPropertyBagAlterationResult::Success) return true;

	switch (Result)
	{
	case EPropertyBagAlterationResult::PropertyNameEmpty:            OutError = TEXT("property name is empty"); break;
	case EPropertyBagAlterationResult::PropertyNameInvalidCharacters: OutError = TEXT("property name has invalid characters"); break;
	case EPropertyBagAlterationResult::TargetPropertyAlreadyExists:  OutError = TEXT("property already exists"); break;
	default:                                                          OutError = TEXT("internal error adding property"); break;
	}
	return false;
}

bool MakePropertyBagPropertyDesc(FName PropName, const sol::object& TypeSpec,
                                  FPropertyBagPropertyDesc& OutDesc, FString& OutError)
{
	EPropertyBagPropertyType Type;
	EPropertyBagContainerType Container;
	UObject* TypeObject = nullptr;
	if (!ResolveTypeSpec(TypeSpec, Type, Container, TypeObject, OutError))
		return false;

	if (Container == EPropertyBagContainerType::None)
	{
		OutDesc = FPropertyBagPropertyDesc(PropName, Type, TypeObject);
	}
	else
	{
		OutDesc = FPropertyBagPropertyDesc(PropName, Container, Type, TypeObject);
	}
	return true;
}

bool SetPropertyBagValue(FInstancedPropertyBag& Bag, FName PropName,
                          const sol::object& Value, FString& OutError)
{
	const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
	if (!BagStruct) { OutError = TEXT("property bag has no struct"); return false; }
	const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(PropName);
	if (!Desc) { OutError = FString::Printf(TEXT("property '%s' not found"), *PropName.ToString()); return false; }

	// Container case: iterate Lua table entries and write via FPropertyBagArrayRef.
	if (Desc->ContainerTypes.Num() > 0)
	{
		if (!Value.is<sol::table>())
		{
			OutError = TEXT("container property requires a Lua table");
			return false;
		}
		TValueOrError<FPropertyBagArrayRef, EPropertyBagResult> ArrRes = Bag.GetMutableArrayRef(PropName);
		if (ArrRes.HasError())
		{
			OutError = TEXT("could not get array ref");
			return false;
		}
		// For simplicity we use SetValueSerializedString per-entry after rebuilding via AddProperty semantics:
		// exporting structure values through FProperty is more reliable than the narrow typed setters here.
		// For now we support scalar arrays; callers needing complex nested containers can call the typed setters directly.
		sol::table Arr = Value.as<sol::table>();
		FString Joined;
		bool bFirst = true;
		Joined.Append(TEXT("("));
		for (auto& KV : Arr)
		{
			if (!bFirst) Joined.Append(TEXT(","));
			bFirst = false;
			const sol::object& Item = KV.second;
			if (Item.is<std::string>())      Joined.Append(FString::Printf(TEXT("\"%s\""), UTF8_TO_TCHAR(Item.as<std::string>().c_str())));
			else if (Item.is<bool>())        Joined.Append(Item.as<bool>() ? TEXT("true") : TEXT("false"));
			else if (Item.is<double>())      Joined.Append(FString::SanitizeFloat(Item.as<double>()));
			else                              Joined.Append(TEXT("()"));
		}
		Joined.Append(TEXT(")"));
		const EPropertyBagResult R = Bag.SetValueSerializedString(PropName, Joined);
		if (R == EPropertyBagResult::Success) return true;
		OutError = FString::Printf(TEXT("could not set container value (serialized form: %s)"), *Joined);
		return false;
	}

	// Scalar / struct / object / enum — convert sol → FString and use SetValueSerializedString
	// for uniform handling. The engine helper dispatches per-property-type internally.
	FString TextValue;
	if (Value.is<std::string>())
	{
		TextValue = UTF8_TO_TCHAR(Value.as<std::string>().c_str());
	}
	else if (Value.is<bool>())
	{
		TextValue = Value.as<bool>() ? TEXT("true") : TEXT("false");
	}
	else if (Value.is<double>())
	{
		double D = Value.as<double>();
		if (FMath::IsNearlyEqual(D, FMath::RoundToDouble(D)))
			TextValue = FString::Printf(TEXT("%lld"), static_cast<int64>(D));
		else
			TextValue = FString::SanitizeFloat(D);
	}
	else if (Value.is<sol::table>())
	{
		// For struct values, try domain handler first; otherwise fall back to textual form if the
		// struct has its own exporter. Rather than reinvent, require callers with complex struct
		// literals to use the typed setter directly.
		if (Desc->ValueType == EPropertyBagPropertyType::Struct && Desc->ValueTypeObject)
		{
			UScriptStruct* Struct = const_cast<UScriptStruct*>(Cast<UScriptStruct>(Desc->ValueTypeObject.Get()));
			if (!Struct) { OutError = TEXT("struct descriptor missing UScriptStruct"); return false; }

			FInstancedStruct Staging;
			Staging.InitializeAs(Struct);
			FString ApplyErr;
			if (!ApplyValueToStructMemory(Struct, Staging.GetMutableMemory(), Value, nullptr, ApplyErr))
			{
				OutError = FString::Printf(TEXT("could not write struct: %s"), *ApplyErr);
				return false;
			}
			const EPropertyBagResult R = Bag.SetValueStruct(PropName, FConstStructView(Struct, Staging.GetMemory()));
			if (R == EPropertyBagResult::Success) return true;
			OutError = TEXT("SetValueStruct failed");
			return false;
		}
		OutError = TEXT("table value only supported for Struct-typed parameters");
		return false;
	}
	else if (Value == sol::lua_nil)
	{
		TextValue = FString();
	}
	else
	{
		OutError = TEXT("unsupported Lua value type");
		return false;
	}

	const EPropertyBagResult R = Bag.SetValueSerializedString(PropName, TextValue);
	if (R == EPropertyBagResult::Success) return true;
	OutError = FString::Printf(TEXT("could not set '%s' from '%s'"), *PropName.ToString(), *TextValue);
	return false;
}

sol::object ReadPropertyBagValue(const FInstancedPropertyBag& Bag, FName PropName, sol::state_view Lua)
{
	const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
	if (!BagStruct) return sol::lua_nil;
	const uint8* Memory = Bag.GetValue().GetMemory();
	if (!Memory) return sol::lua_nil;

	const FProperty* Prop = BagStruct->FindPropertyByName(PropName);
	if (!Prop) return sol::lua_nil;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Memory);
	return ReadPropertyAsSol(const_cast<FProperty*>(Prop), ValuePtr, Lua);
}

#else // ENGINE_MINOR_VERSION < 6

// Pre-5.6: PropertyBag header path differs (5.4) and EPropertyBagAlterationResult
// / 4-arg AddProperty don't exist (5.5). The public functions still link
// (the header forward-declares FInstancedPropertyBag) but return a clear error
// so callers in extensions can report "requires UE 5.6+" instead of crashing.
bool AddPropertyBagProperty(FInstancedPropertyBag& /*Bag*/, FName /*PropName*/,
                             const sol::object& /*TypeSpec*/, FString& OutError)
{
	OutError = TEXT("PropertyBag bindings require UE 5.6+");
	return false;
}

bool MakePropertyBagPropertyDesc(FName /*PropName*/, const sol::object& /*TypeSpec*/,
                                  FPropertyBagPropertyDesc& /*OutDesc*/, FString& OutError)
{
	OutError = TEXT("PropertyBag bindings require UE 5.6+");
	return false;
}

bool SetPropertyBagValue(FInstancedPropertyBag& /*Bag*/, FName /*PropName*/,
                          const sol::object& /*Value*/, FString& OutError)
{
	OutError = TEXT("PropertyBag bindings require UE 5.6+");
	return false;
}

sol::object ReadPropertyBagValue(const FInstancedPropertyBag& /*Bag*/, FName /*PropName*/, sol::state_view /*Lua*/)
{
	return sol::lua_nil;
}

#endif // ENGINE_MINOR_VERSION >= 6

} // namespace NeoLuaProperty
