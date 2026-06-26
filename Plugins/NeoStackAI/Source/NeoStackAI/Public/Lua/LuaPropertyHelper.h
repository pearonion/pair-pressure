#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FProperty;
class UObject;
class UStruct;

namespace NeoLuaProperty
{

struct NEOSTACKAI_API FPropertyValueInput
{
	double NumberValue = 0.0;
	FString StringValue;
	bool BoolValue = false;
	bool bIsNumber = false;
	bool bIsString = false;
	bool bIsBool = false;
};

NEOSTACKAI_API FProperty* FindPropertyByName(UStruct* StructType, const FString& PropertyName);

NEOSTACKAI_API bool SetPropertyValue(
	FProperty* Property,
	void* Container,
	UObject* OwnerObject,
	const FPropertyValueInput& Input,
	FString& OutError);

NEOSTACKAI_API bool SetPropertyValueFromStringSafe(
	FProperty* Property,
	void* Container,
	UObject* OwnerObject,
	const FString& Value,
	FString& OutError);

NEOSTACKAI_API bool SetPropertyValueFromStringSafe(
	FProperty* Property,
	void* Container,
	UObject* OwnerObject,
	const FString& Value,
	TFunctionRef<void()> PreCommit,
	FString& OutError);

NEOSTACKAI_API bool ClampNumericPropertyValueFromMetaData(
	FProperty* Property,
	void* ValuePtr);

NEOSTACKAI_API bool PatchPropertyValueFromString(
	FProperty* Property,
	UObject* OwnerObject,
	const FString& Value,
	bool bEnableInlineEditConditionToggle,
	FString& OutError);

NEOSTACKAI_API bool SetNamedPropertyValue(
	UObject* Object,
	const FString& PropertyName,
	const FPropertyValueInput& Input,
	FString& OutError);

NEOSTACKAI_API bool SetNamedPropertyValue(
	void* Container,
	UStruct* StructType,
	UObject* OwnerObject,
	const FString& PropertyName,
	const FPropertyValueInput& Input,
	FString& OutError);

// Like SetNamedPropertyValue, but brackets the write with Modify() + PreEditChange(Property)
// + PostEditChangeProperty(FPropertyChangedEvent(Property, EPropertyChangeType::ValueSet)).
// Use when the engine class keys behavior off PropertyChangedEvent.Property — e.g. UHLODLayer
// recreates HLODBuilderSettings only when the changed property is LayerType or HLODBuilderClass
// (HLODLayer.cpp:175). Bare PostEditChange() supplies a null Property and skips those branches.
NEOSTACKAI_API bool SetNamedPropertyValueWithEditChange(
	UObject* Object,
	const FString& PropertyName,
	const FPropertyValueInput& Input,
	FString& OutError);

}
