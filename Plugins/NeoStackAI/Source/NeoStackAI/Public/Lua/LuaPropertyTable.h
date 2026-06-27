// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Table-based property apply/read helpers backed by FProperty reflection.
// Handles primitives, enums by authored name, object refs by asset path,
// arrays (Lua sequences), maps (Lua dicts), nested structs (Lua sub-tables),
// and domain-registered struct types (FVector, FLinearColor, FNiagaraBool, ...).
//
// This is the plugin-wide LCD: any binding that exposes `configure(id, {properties={...}})`,
// `add(type, {properties={...}})`, or `get(...)` should use these helpers instead of
// hardcoded per-type dispatch chains.
//
// Usage:
//   FString Error;
//   TArray<FString> Warnings;
//   bool bOk = NeoLuaProperty::ApplyTable(Renderer, LuaProps, Error, &Warnings);
//
// Domain extensions (NSAI_Niagara, NSAI_ControlRig, etc.) register custom struct handlers
// for their exotic types via RegisterStructHandler() at module startup.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

class FProperty;
class UScriptStruct;
struct FRichCurve;
// Engine's property-bag type from StructUtils/PropertyBag.h — forward-declared
// at global scope so the AddPropertyBag*/SetPropertyBagValue/ReadPropertyBagValue
// signatures below bind to ::FInstancedPropertyBag, not a namespaced shadow.
struct FInstancedPropertyBag;
struct FPropertyBagPropertyDesc;

namespace NeoLuaProperty
{

// ── Struct handler registry ─────────────────────────────────────────────
//
// Writer: Lua value → struct bytes. Returns true on success, false with error on failure.
using FStructWriter = TFunction<bool(UScriptStruct* StructDef,
                                      void* StructMem,
                                      const sol::object& Value,
                                      FString& OutError)>;

// Reader: struct bytes → Lua value. Returns sol::lua_nil on failure.
using FStructReader = TFunction<sol::object(UScriptStruct* StructDef,
                                             const void* StructMem,
                                             sol::state_view Lua)>;

NEOSTACKAI_API void RegisterStructHandler(UScriptStruct* StructType,
                                           FStructWriter Writer,
                                           FStructReader Reader);

NEOSTACKAI_API void UnregisterStructHandler(UScriptStruct* StructType);

NEOSTACKAI_API bool HasStructHandler(const UScriptStruct* StructType);

// ── Apply: Lua → UObject / struct ────────────────────────────────────────

// Apply a Lua table to a UObject. Calls Modify()/PostEditChange() and returns true
// if at least one property applied successfully. Unknown/non-editable properties
// are reported in OutWarnings (if non-null) rather than failing the whole call.
NEOSTACKAI_API bool ApplyTable(UObject* Target,
                                const sol::table& Props,
                                FString& OutError,
                                TArray<FString>* OutWarnings = nullptr);

// Apply a Lua table to a USTRUCT (recursive entry for nested struct properties).
// OwnerForPostEdit is used for object-path resolution and Modify() calls on the
// containing object; it may be null for detached struct memory.
NEOSTACKAI_API bool ApplyTableToStruct(const UStruct* StructDef,
                                        void* StructMem,
                                        UObject* OwnerForPostEdit,
                                        const sol::table& Props,
                                        FString& OutError,
                                        TArray<FString>* OutWarnings = nullptr);

// Apply a single sol::object to a specific FProperty.
// Dispatches on property type (primitive/enum/object/array/struct) and value type.
NEOSTACKAI_API bool ApplySolValueToProperty(FProperty* Property,
                                             void* Container,
                                             UObject* OwnerObject,
                                             const sol::object& Value,
                                             FString& OutError);

// Apply a Lua value to raw struct memory of known type. Tries a registered handler
// first (e.g. FVector3f → {x,y,z}), falls back to ApplyTableToStruct if Value is a
// table, or ImportText_Direct if Value is a string. Caller owns Memory and is
// responsible for prior InitializeStruct / zero-init.
NEOSTACKAI_API bool ApplyValueToStructMemory(UScriptStruct* Struct,
                                              void* Memory,
                                              const sol::object& Value,
                                              UObject* OwnerForPostEdit,
                                              FString& OutError);

// ── Read: UObject / struct → Lua ─────────────────────────────────────────

// Read all editable properties of a UObject into a Lua table.
// SkipFlags defaults to CPF_Deprecated | CPF_Transient (matches FJsonObjectConverter).
NEOSTACKAI_API sol::table ReadAsTable(UObject* Source,
                                       sol::state_view Lua,
                                       int64 SkipFlags = 0);

// Read a single FProperty value as a sol::object.
NEOSTACKAI_API sol::object ReadPropertyAsSol(FProperty* Property,
                                              const void* Container,
                                              sol::state_view Lua);

// Read a USTRUCT to a table (mirror of ApplyTableToStruct).
NEOSTACKAI_API sol::table ReadStructAsTable(const UStruct* StructDef,
                                             const void* StructMem,
                                             sol::state_view Lua,
                                             int64 SkipFlags = 0);

// Read raw struct memory to a Lua value. Tries a registered handler first
// (e.g. FVector3f → {x,y,z}), falls back to ReadStructAsTable.
NEOSTACKAI_API sol::object ReadStructMemoryAsSol(UScriptStruct* Struct,
                                                  const void* Memory,
                                                  sol::state_view Lua);

// ── Curve populators ─────────────────────────────────────────────────────
//
// Generic FRichCurve populator used by Niagara curve DIs, UCurveFloat-style
// assets, BlendSpace curve samples, etc. Each Lua key entry is a table where:
//   - "time" sets the key time (defaults to 0.0)
//   - ValueExtractor pulls the key's float value from the entry
// The curve is Reset() first to match replace-semantics. Optional "interp"
// string per key is honored: "linear" | "constant" | "cubic" | "auto".
NEOSTACKAI_API bool ApplyKeysToCurve(FRichCurve& OutCurve,
                                      const sol::table& Keys,
                                      TFunctionRef<float(const sol::table&)> ValueExtractor,
                                      FString& OutError);

// Read an FRichCurve back into a Lua array of {time, value} key entries.
NEOSTACKAI_API sol::table ReadCurveAsKeys(const FRichCurve& Curve, sol::state_view Lua);

// ── Property bag helpers ─────────────────────────────────────────────────
//
// FInstancedPropertyBag is used by StateTree (global + per-state parameters),
// MassEntity config, SmartObjects, and any editor system that exposes a
// dynamically-typed parameter surface. These helpers cover the full engine
// type matrix (scalar, enum, struct, object/class refs, soft refs, arrays/sets)
// via FPropertyBagPropertyDesc so call-sites don't need a hand-rolled switch.
//
// TypeSpec form (Lua):
//   "Bool" | "Int32" | "Float" | "Double" | "Name" | "String" | "Text" | ...
//   { type="Enum",   enum="/Script/MyModule.EMyEnum" }
//   { type="Struct", struct="/Script/CoreUObject.Vector" | "Vector" }
//   { type="Object", class="/Script/Engine.Texture2D" | "Texture2D" }
//   { type="SoftObject", class="Texture2D" }
//   { type="Class", class="Actor" }
//   { type="SoftClass", class="Actor" }
//   Append `container="Array"` or `container="Set"` on any of the above
//   to produce a container property.

NEOSTACKAI_API bool AddPropertyBagProperty(FInstancedPropertyBag& Bag,
                                            FName PropName,
                                            const sol::object& TypeSpec,
                                            FString& OutError);

// Resolve a Lua type spec to an engine property-bag descriptor without mutating
// a bag. Useful for APIs such as UPCGGraph::AddUserParameters that own their
// own notification path.
NEOSTACKAI_API bool MakePropertyBagPropertyDesc(FName PropName,
                                                 const sol::object& TypeSpec,
                                                 FPropertyBagPropertyDesc& OutDesc,
                                                 FString& OutError);

// Set a value on an existing property bag entry from any Lua value.
// Falls back to engine's SetValueSerializedString when no typed setter fits.
NEOSTACKAI_API bool SetPropertyBagValue(FInstancedPropertyBag& Bag,
                                         FName PropName,
                                         const sol::object& Value,
                                         FString& OutError);

// Read a single property bag entry as a sol value (scalar / struct table /
// object path / enum name).
NEOSTACKAI_API sol::object ReadPropertyBagValue(const FInstancedPropertyBag& Bag,
                                                 FName PropName,
                                                 sol::state_view Lua);

} // namespace NeoLuaProperty
