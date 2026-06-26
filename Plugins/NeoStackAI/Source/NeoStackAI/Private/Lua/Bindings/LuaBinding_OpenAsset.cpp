// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetCapabilityRegistry.h"
#include "Lua/LuaEditorActions.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaPropertyTable.h"
#include "Blueprint/BlueprintUtils.h"
#include "Tools/NeoStackToolUtils.h"
#include "Utils/NeoTypeResolver.h"

#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Lua/LuaSubsystem.h"
#include "Misc/Paths.h"
#include "IMaterialEditor.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Toolkits/ToolkitManager.h"
#include "Toolkits/IToolkit.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTNode.h"
#include "LevelSequence.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Engine/UserDefinedEnum.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "Internationalization/StringTable.h"
#include "Engine/CurveTable.h"
#include "Curves/CurveBase.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundMix.h"
#include "VT/RuntimeVirtualTexture.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Engine/AssetUserData.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "FoliageType_Actor.h"
#include "LandscapeGrassType.h"
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
// PoseSearch headers removed — binding lives in NSAI_PoseSearch extension module.
// Type detection uses class name strings (no link dependency needed).

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers — property find, get, set, with all the ConfigureAssetTool smarts
// ============================================================================

namespace LuaAssetHelper
{

// Is a Play In Editor session running right now?
//
// Mutating a UMaterial / UMaterialInstance UPROPERTY while PIE is rendering
// kicks PostEditChangeProperty -> CacheResourceShadersForRendering, which
// races the rendering thread that's currently sampling the material. The
// async shader compiler then dereferences the swapped-out FMaterialResource
// (vtable poisoned to 0xFFFFFFFFFFFFFFFF) and the editor hard-crashes off
// the game thread — past the SEH net in NeoLuaState.cpp. Refuse the edit up
// front; the agent must stop PIE first.
static bool IsPIESession()
{
	return GEditor && GEditor->IsPlaySessionInProgress();
}

static bool RefuseUnsafeMaterialMutation(UObject* Asset, const TCHAR* Verb, FLuaSessionData& Session, const FString& Target = FString())
{
	if (!Asset || !Asset->IsA<UMaterialInterface>() || !IsPIESession())
	{
		return false;
	}

	FString Label(Verb);
	if (!Target.IsEmpty())
	{
		Label += FString::Printf(TEXT("(\"%s\")"), *Target);
	}

	Session.Log(FString::Printf(
		TEXT("[FAIL] %s -> refused while PIE is active. "
		     "Stop the play session first (playtest_stop()) — editing a material the rendering "
		     "thread is currently sampling races CacheResourceShadersForRendering and can AV the editor."),
		*Label));
	return true;
}

static bool IsMPCParameterArray(const FString& ArrayName)
{
	return ArrayName.Equals(TEXT("ScalarParameters"), ESearchCase::IgnoreCase)
		|| ArrayName.Equals(TEXT("VectorParameters"), ESearchCase::IgnoreCase);
}

// UMaterialParameterCollection::GetBaseParameterCollection (parent-collection chain)
// was added in UE 5.7; on older engines we treat the head MPC as the only collection.
static UMaterialParameterCollection* NSAI_OpenAsset_NextMPCInChain(UMaterialParameterCollection* Collection)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	return Collection ? Collection->GetBaseParameterCollection() : nullptr;
#else
	(void)Collection;
	return nullptr;
#endif
}

static bool HasMPCParameterNameExceptCurrent(UMaterialParameterCollection* MPC, FName NewName, const FString& CurrentArrayName, int32 CurrentIndex)
{
	if (!MPC) return false;
	for (UMaterialParameterCollection* Collection = MPC; Collection; Collection = NSAI_OpenAsset_NextMPCInChain(Collection))
	{
		for (int32 Index = 0; Index < Collection->ScalarParameters.Num(); ++Index)
		{
			const bool bIsCurrent = Collection == MPC
				&& CurrentArrayName.Equals(TEXT("ScalarParameters"), ESearchCase::IgnoreCase)
				&& Index == CurrentIndex;
			if (!bIsCurrent && Collection->ScalarParameters[Index].ParameterName == NewName)
			{
				return true;
			}
		}
		for (int32 Index = 0; Index < Collection->VectorParameters.Num(); ++Index)
		{
			const bool bIsCurrent = Collection == MPC
				&& CurrentArrayName.Equals(TEXT("VectorParameters"), ESearchCase::IgnoreCase)
				&& Index == CurrentIndex;
			if (!bIsCurrent && Collection->VectorParameters[Index].ParameterName == NewName)
			{
				return true;
			}
		}
	}
	return false;
}

static bool RefuseMPCParameterRenameConflict(UMaterialParameterCollection* MPC, const FString& ArrayName, int32 FoundIdx, sol::table Props, FLuaSessionData& Session)
{
	if (!MPC || !IsMPCParameterArray(ArrayName))
	{
		return false;
	}

	sol::object RenameObj = Props["ParameterName"];
	if (!RenameObj.valid() || RenameObj == sol::lua_nil || !RenameObj.is<std::string>())
	{
		return false;
	}

	FName CurrentName = NAME_None;
	if (ArrayName.Equals(TEXT("ScalarParameters"), ESearchCase::IgnoreCase))
	{
		if (!MPC->ScalarParameters.IsValidIndex(FoundIdx)) return false;
		CurrentName = MPC->ScalarParameters[FoundIdx].ParameterName;
	}
	else
	{
		if (!MPC->VectorParameters.IsValidIndex(FoundIdx)) return false;
		CurrentName = MPC->VectorParameters[FoundIdx].ParameterName;
	}

	const FName NewName(NeoLuaStr::ToFString(RenameObj.as<std::string>()));
	if (NewName == CurrentName)
	{
		return false;
	}
	if (HasMPCParameterNameExceptCurrent(MPC, NewName, ArrayName, FoundIdx))
	{
		Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\") -> ParameterName '%s' already exists in this material parameter collection"),
			*ArrayName, *NewName.ToString()));
		return true;
	}
	return false;
}

// Map a UMaterial::bUsedWithXxx UPROPERTY name to its EMaterialUsage enum.
//
// Why this routing exists: when a Lua user writes
// `mat:set("bUsedWithNiagaraSprites", true)`, the generic UPROPERTY ImportText
// path bypasses the FMaterialUpdateContext rendering-thread sync that
// SetMaterialUsage performs — and the engine rejects the value during
// PostEditChangeProperty, so the bitfield reads back unchanged. Route
// SET-TO-TRUE through SetMaterialUsage so the flag actually sticks and the
// shader recompile is properly serialised.
//
// Why this table is hand-rolled (rather than read from UMaterial::GetUsageName):
// GetUsageName is declared but not ENGINE_API-exported, so a plugin link
// against it fails with LNK2019 in shipping builds. The static_assert below
// catches engine bumps that add new MATUSAGE_* values — when Epic adds an
// entry, MATUSAGE_MAX changes and the build fails loudly here, pointing the
// next maintainer at this table. Source of truth: UMaterial::GetUsageName at
// Engine/Source/Runtime/Engine/Private/Materials/Material.cpp.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
static_assert((int32)MATUSAGE_MAX == 27 || (int32)MATUSAGE_MAX == 28,
	"EMaterialUsage gained a new entry — add the matching bUsedWithXxx UPROPERTY "
	"name to UsageNamePairs in LuaBinding_OpenAsset.cpp. "
	"See UMaterial::GetUsageName in the engine for the canonical mapping.");
#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
static_assert((int32)MATUSAGE_MAX == 23,
	"EMaterialUsage gained a new entry — add the matching bUsedWithXxx UPROPERTY "
	"name to UsageNamePairs in LuaBinding_OpenAsset.cpp. "
	"See UMaterial::GetUsageName in the engine for the canonical mapping.");
#endif
// Pre-5.7: enum count varies (5.4/5.5 = 21, 5.6 added one). The static_assert
// is a tripwire for the 5.7-target; older engines just need the version-gated
// entries to compile, no integrity check needed.

static const TMap<FName, EMaterialUsage>& GetMaterialUsagePropertyMap()
{
	static const TMap<FName, EMaterialUsage> Map = []()
	{
		TMap<FName, EMaterialUsage> M;
		struct FUsagePair { const TCHAR* PropertyName; EMaterialUsage Usage; };
		const FUsagePair UsageNamePairs[] = {
			{ TEXT("bUsedWithSkeletalMesh"),           MATUSAGE_SkeletalMesh },
			{ TEXT("bUsedWithParticleSprites"),        MATUSAGE_ParticleSprites },
			{ TEXT("bUsedWithBeamTrails"),             MATUSAGE_BeamTrails },
			{ TEXT("bUsedWithMeshParticles"),          MATUSAGE_MeshParticles },
			{ TEXT("bUsedWithStaticLighting"),         MATUSAGE_StaticLighting },
			{ TEXT("bUsedWithMorphTargets"),           MATUSAGE_MorphTargets },
			{ TEXT("bUsedWithSplineMeshes"),           MATUSAGE_SplineMesh },
			{ TEXT("bUsedWithInstancedStaticMeshes"),  MATUSAGE_InstancedStaticMeshes },
			{ TEXT("bUsedWithGeometryCollections"),    MATUSAGE_GeometryCollections },
			{ TEXT("bUsedWithClothing"),               MATUSAGE_Clothing },
			{ TEXT("bUsedWithNiagaraSprites"),         MATUSAGE_NiagaraSprites },
			{ TEXT("bUsedWithNiagaraRibbons"),         MATUSAGE_NiagaraRibbons },
			{ TEXT("bUsedWithNiagaraMeshParticles"),   MATUSAGE_NiagaraMeshParticles },
			{ TEXT("bUsedWithGeometryCache"),          MATUSAGE_GeometryCache },
			{ TEXT("bUsedWithWater"),                  MATUSAGE_Water },
			{ TEXT("bUsedWithHairStrands"),            MATUSAGE_HairStrands },
			{ TEXT("bUsedWithLidarPointCloud"),        MATUSAGE_LidarPointCloud },
			{ TEXT("bUsedWithVirtualHeightfieldMesh"), MATUSAGE_VirtualHeightfieldMesh },
			{ TEXT("bUsedWithNanite"),                 MATUSAGE_Nanite },
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			{ TEXT("bUsedWithVoxels"),                 MATUSAGE_Voxels },
#endif
			{ TEXT("bUsedWithVolumetricCloud"),        MATUSAGE_VolumetricCloud },
			{ TEXT("bUsedWithHeterogeneousVolumes"),   MATUSAGE_HeterogeneousVolumes },
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			{ TEXT("bUsedWithStaticMesh"),             MATUSAGE_StaticMesh },
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			{ TEXT("bUsedWithEditorCompositing"),      MATUSAGE_EditorCompositing },
			{ TEXT("bUsedWithNeuralNetworks"),         MATUSAGE_NeuralNetworks },
			{ TEXT("bUsedWithMeshDeformer"),           MATUSAGE_MeshDeformer },
			{ TEXT("bUsedWithInstancedSkinnedMesh"),   MATUSAGE_InstancedSkinnedMesh },
#endif
		};
		for (const FUsagePair& P : UsageNamePairs)
		{
			M.Add(FName(P.PropertyName), P.Usage);
		}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		if ((int32)MATUSAGE_MAX > 27)
		{
			M.Add(TEXT("bUsedWithCurves"), static_cast<EMaterialUsage>(27));
		}
#endif
		// Tripwire: if the table count drifts from MATUSAGE_MAX, log loudly at
		// startup. The static_assert above catches the common case (Epic adds
		// a new entry) but a typo'd duplicate in our table would slip past it.
		ensureMsgf(M.Num() == (int32)MATUSAGE_MAX,
			TEXT("UsageNamePairs has %d entries, MATUSAGE_MAX is %d — table out of sync."),
			M.Num(), (int32)MATUSAGE_MAX);
		return M;
	}();
	return Map;
}

static bool MaterialUsageFromBoolPropertyName(FName PropName, EMaterialUsage& OutUsage)
{
	if (const EMaterialUsage* Found = GetMaterialUsagePropertyMap().Find(PropName))
	{
		OutUsage = *Found;
		return true;
	}
	return false;
}

static bool IsUnsafeEmptySoundWaveRebuildProperty(USoundWave* Wave, const FString& RequestedProperty, FProperty* Property, FString& OutReason)
{
	if (!Wave)
	{
		return false;
	}

	FString RequestedLeaf = RequestedProperty;
	int32 DotIndex = INDEX_NONE;
	if (RequestedLeaf.FindLastChar(TEXT('.'), DotIndex))
	{
		RequestedLeaf.RightChopInline(DotIndex + 1);
	}

	const FName PropName = Property ? Property->GetFName() : NAME_None;
	const bool bTriggersAudioRebuild =
		PropName == FName(TEXT("SoundAssetCompressionType")) ||
		PropName == FName(TEXT("LoadingBehavior")) ||
		PropName == FName(TEXT("bStreaming")) ||
		PropName == FName(TEXT("SizeOfFirstAudioChunkInSeconds")) ||
		PropName == FName(TEXT("InitialChunkSize_DEPRECATED")) ||
		PropName == FName(TEXT("bEnableCloudStreaming")) ||
		RequestedLeaf.Equals(TEXT("SoundAssetCompressionType"), ESearchCase::IgnoreCase) ||
		RequestedLeaf.Equals(TEXT("LoadingBehavior"), ESearchCase::IgnoreCase) ||
		RequestedLeaf.Equals(TEXT("bStreaming"), ESearchCase::IgnoreCase) ||
		RequestedLeaf.Equals(TEXT("SizeOfFirstAudioChunkInSeconds"), ESearchCase::IgnoreCase) ||
		RequestedLeaf.Equals(TEXT("InitialChunkSize_DEPRECATED"), ESearchCase::IgnoreCase) ||
		RequestedLeaf.Equals(TEXT("bEnableCloudStreaming"), ESearchCase::IgnoreCase);

	if (!bTriggersAudioRebuild)
	{
		return false;
	}

	if (Wave->NumChannels > 0 || Wave->Duration > 0.0f)
	{
		return false;
	}

	OutReason = FString::Printf(
		TEXT("property '%s' triggers USoundWave::UpdateAsset, but this SoundWave has no imported audio payload. ")
		TEXT("Import a real audio file first, or use simple reflected fields such as Volume/Pitch/Looping on synthetic test assets."),
		Property ? *Property->GetName() : *RequestedLeaf);
	return true;
}

static FProperty* FindProperty(UObject* Asset, const FString& PropertyName)
{
	if (!Asset) return nullptr;

	// Exact match first
	FProperty* Prop = PropertyAccessUtil::FindPropertyByName(FName(*PropertyName), Asset->GetClass());
	if (Prop) return Prop;

	// Case-insensitive fallback
	for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
	{
		if (PropIt->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
		{
			return *PropIt;
		}
	}
	return nullptr;
}

// Resolve dot-notation paths like "Axis[0].Min" or "MyMap{key}.SubField" into (Property, ContainerPtr)
struct FResolvedProperty
{
	FProperty* Property = nullptr;
	void* ContainerPtr = nullptr;
	void* ValuePtr = nullptr;
	TArray<FProperty*> ChainProperties;

	bool IsNested() const { return ChainProperties.Num() > 1; }
};

static void FillEditPropertyChain(const FResolvedProperty& Resolved, FEditPropertyChain& Chain)
{
	for (FProperty* ChainProperty : Resolved.ChainProperties)
	{
		if (ChainProperty)
		{
			Chain.AddTail(ChainProperty);
		}
	}

	if (Resolved.Property)
	{
		Chain.SetActivePropertyNode(Resolved.Property);
		if (Resolved.ChainProperties.Num() > 0)
		{
			Chain.SetActiveMemberPropertyNode(Resolved.ChainProperties[0]);
		}
	}
}

static FResolvedProperty ResolvePropertyPath(UObject* Object, const FString& PropertyPath)
{
	FResolvedProperty Result;
	if (!Object || PropertyPath.IsEmpty()) return Result;

	// No dots and no brackets — flat lookup
	if (!PropertyPath.Contains(TEXT(".")) && !PropertyPath.Contains(TEXT("[")))
	{
		Result.Property = FindProperty(Object, PropertyPath);
		Result.ContainerPtr = Object;
		if (Result.Property)
		{
			Result.ValuePtr = Result.Property->ContainerPtrToValuePtr<void>(Result.ContainerPtr);
			Result.ChainProperties.Add(Result.Property);
		}
		return Result;
	}

	TArray<FString> Segments;
	PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() == 0 || Segments.Num() > 10) return Result;

	UStruct* CurrentStruct = Object->GetClass();
	void* CurrentContainer = Object;

	for (int32 i = 0; i < Segments.Num(); i++)
	{
		FString Segment = Segments[i];
		bool bIsLast = (i == Segments.Num() - 1);

		// Parse optional array index: "PropertyName[N]"
		int32 ArrayIndex = INDEX_NONE;
		int32 BracketPos = INDEX_NONE;
		// Parse optional map key: "PropertyName{key}"
		FString MapKeyStr;
		int32 BracePos = INDEX_NONE;

		if (Segment.FindChar(TEXT('['), BracketPos))
		{
			FString IndexStr = Segment.Mid(BracketPos + 1);
			IndexStr.RemoveFromEnd(TEXT("]"));
			if (IndexStr.IsNumeric())
			{
				ArrayIndex = FCString::Atoi(*IndexStr);
			}
			else
			{
				return FResolvedProperty();
			}
			Segment = Segment.Left(BracketPos);
		}
		else if (Segment.FindChar(TEXT('{'), BracePos))
		{
			MapKeyStr = Segment.Mid(BracePos + 1);
			MapKeyStr.RemoveFromEnd(TEXT("}"));
			Segment = Segment.Left(BracePos);
		}

		// Find property in current struct
		FProperty* Prop = PropertyAccessUtil::FindPropertyByName(FName(*Segment), CurrentStruct);
		if (!Prop)
		{
			for (TFieldIterator<FProperty> PropIt(CurrentStruct); PropIt; ++PropIt)
			{
				if (PropIt->GetName().Equals(Segment, ESearchCase::IgnoreCase))
				{
					Prop = *PropIt;
					break;
				}
			}
		}
		if (!Prop) return FResolvedProperty();

		// Handle array indexing
		if (ArrayIndex != INDEX_NONE)
		{
			void* PropContainer = Prop->ContainerPtrToValuePtr<void>(CurrentContainer);
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp) return FResolvedProperty();

			FScriptArrayHelper ArrayHelper(ArrayProp, PropContainer);
			if (ArrayIndex < 0 || ArrayIndex >= ArrayHelper.Num()) return FResolvedProperty();

			void* ElementPtr = ArrayHelper.GetRawPtr(ArrayIndex);
			FProperty* InnerProp = ArrayProp->Inner;

			if (bIsLast)
			{
				Result.Property = InnerProp;
				Result.ContainerPtr = ElementPtr;
				Result.ValuePtr = ElementPtr;
				Result.ChainProperties.Add(Prop);
				Result.ChainProperties.Add(InnerProp);
				return Result;
			}

			FStructProperty* InnerStruct = CastField<FStructProperty>(InnerProp);
			Result.ChainProperties.Add(Prop);
			Result.ChainProperties.Add(InnerProp);
			if (InnerStruct)
			{
				CurrentContainer = ElementPtr;
				CurrentStruct = InnerStruct->Struct;
				continue;
			}
			if (FObjectPropertyBase* InnerObject = CastField<FObjectPropertyBase>(InnerProp))
			{
				UObject* NestedObject = InnerObject->GetObjectPropertyValue(ElementPtr);
				if (!NestedObject) return FResolvedProperty();
				CurrentContainer = NestedObject;
				CurrentStruct = NestedObject->GetClass();
				continue;
			}
			return FResolvedProperty();
		}

		// Handle map key lookup: "MapProp{key}.SubField"
		if (!MapKeyStr.IsEmpty())
		{
			void* PropContainer = Prop->ContainerPtrToValuePtr<void>(CurrentContainer);
			FMapProperty* MapProp = CastField<FMapProperty>(Prop);
			if (!MapProp) return FResolvedProperty();

			FScriptMapHelper MapHelper(MapProp, PropContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();
			FProperty* ValProp = MapHelper.GetValueProperty();

			// Allocate temp key and parse the string into it
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			const TCHAR* ParseResult = KeyProp->ImportText_Direct(*MapKeyStr, TempKey, nullptr, PPF_None);
			if (!ParseResult)
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				return FResolvedProperty();
			}

			uint8* ValuePtr = MapHelper.FindValueFromHash(TempKey);
			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);

			if (!ValuePtr) return FResolvedProperty();

			if (bIsLast)
			{
				Result.Property = ValProp;
				Result.ContainerPtr = ValuePtr;
				Result.ValuePtr = ValuePtr;
				Result.ChainProperties.Add(Prop);
				Result.ChainProperties.Add(ValProp);
				return Result;
			}

			// Walk into the value if it's a struct
			FStructProperty* ValStruct = CastField<FStructProperty>(ValProp);
			Result.ChainProperties.Add(Prop);
			Result.ChainProperties.Add(ValProp);
			if (ValStruct)
			{
				CurrentContainer = ValuePtr;
				CurrentStruct = ValStruct->Struct;
				continue;
			}
			if (FObjectPropertyBase* ValObject = CastField<FObjectPropertyBase>(ValProp))
			{
				UObject* NestedObject = ValObject->GetObjectPropertyValue(ValuePtr);
				if (!NestedObject) return FResolvedProperty();
				CurrentContainer = NestedObject;
				CurrentStruct = NestedObject->GetClass();
				continue;
			}
			return FResolvedProperty();
		}

		if (bIsLast)
		{
			Result.Property = Prop;
			Result.ContainerPtr = CurrentContainer;
			Result.ValuePtr = Prop->ContainerPtrToValuePtr<void>(CurrentContainer);
			Result.ChainProperties.Add(Prop);
			return Result;
		}

		// Walk into struct
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		Result.ChainProperties.Add(Prop);
		if (StructProp)
		{
			CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			CurrentStruct = StructProp->Struct;
			continue;
		}
		if (FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
		{
			void* ObjectValuePtr = ObjectProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			UObject* NestedObject = ObjectProp->GetObjectPropertyValue(ObjectValuePtr);
			if (!NestedObject) return FResolvedProperty();
			CurrentContainer = NestedObject;
			CurrentStruct = NestedObject->GetClass();
			continue;
		}
		return FResolvedProperty();
	}

	return Result;
}

// ---- Map/Set/Metadata helpers ----

static FMapProperty* FindMapProperty(UObject* Asset, const FString& PropertyName)
{
	FProperty* Prop = FindProperty(Asset, PropertyName);
	if (!Prop) return nullptr;
	return CastField<FMapProperty>(Prop);
}

static FSetProperty* FindSetProperty(UObject* Asset, const FString& PropertyName)
{
	FProperty* Prop = FindProperty(Asset, PropertyName);
	if (!Prop) return nullptr;
	return CastField<FSetProperty>(Prop);
}

// Export a single property value from raw memory to string
static FString ExportPropertyValue(FProperty* Prop, const void* ValuePtr, UObject* OwnerObject)
{
	FString Out;
	Prop->ExportText_Direct(Out, ValuePtr, nullptr, OwnerObject, PPF_None);
	return Out;
}

// Check if a property holds a GameplayTag or GameplayTagContainer struct.
// These need PPF_SerializedAsImportText so that tags not yet in the registry are preserved.
static bool IsGameplayTagProperty(FProperty* Prop)
{
	FStructProperty* StructProp = CastField<FStructProperty>(Prop);
	if (!StructProp) return false;
	const FName StructName = StructProp->Struct->GetFName();
	return StructName == TEXT("GameplayTagContainer") || StructName == TEXT("GameplayTag");
}

// Import a string into a property value in raw memory. Returns true on success.
static bool ImportPropertyValue(FProperty* Prop, void* ValuePtr, const FString& ValueStr, UObject* OwnerObject)
{
	const int32 Flags = IsGameplayTagProperty(Prop) ? PPF_SerializedAsImportText : PPF_None;
	const TCHAR* Result = Prop->ImportText_Direct(*ValueStr, ValuePtr, OwnerObject, Flags);
	if (Result) return true;

	// Boolean fixup
	FString Transformed = ValueStr;
	if (ValueStr.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
	else if (ValueStr.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");
	Result = Prop->ImportText_Direct(*Transformed, ValuePtr, OwnerObject, Flags);
	return Result != nullptr;
}

static FString GetPropertyValue(UObject* Asset, FProperty* Property, void* Container)
{
	return NeoStackToolUtils::GetPropertyValueAsString(Container, Property, Asset);
}

static bool SetPropertyValue(UObject* Asset, FProperty* Property, void* Container, const FString& Value, FString& OutError)
{
	if (!Asset || !Property)
	{
		OutError = TEXT("Invalid asset or property");
		return false;
	}

	const int32 Flags = IsGameplayTagProperty(Property) ? PPF_SerializedAsImportText : PPF_None;
	const TCHAR* Result = Property->ImportText_InContainer(*Value, Container, Asset, Flags);
	if (!Result)
	{
		// Boolean case fixup
		FString Transformed = Value;
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
		else if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");

		Result = Property->ImportText_InContainer(*Transformed, Container, Asset, Flags);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Failed to set value '%s'. Use list_properties() to see valid format."), *Value);
			return false;
		}
	}
	return true;
}

// Asset-specific post-edit hooks (Material recompile, BlendSpace resample, etc.)
// Called AFTER PreEditChange/PostEditChangeProperty have already been done.
static void PostEdit(UObject* Asset, FProperty* ChangedProperty)
{
	if (!Asset) return;

	// MaterialInstance: generic UPROPERTY writes to BasePropertyOverrides only fire
	// PostEditChangeProperty; they do NOT run UpdateStaticPermutation, which is what
	// actually recompiles the shader permutation for the new overrides. The dedicated
	// configure("base_property") path does this, so match that behavior here.
	if (UMaterialInstance* MI = Cast<UMaterialInstance>(Asset))
	{
		if (ChangedProperty && ChangedProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialInstance, BasePropertyOverrides))
		{
			FStaticParameterSet StaticParams = MI->GetStaticParameters();
			MI->UpdateStaticPermutation(StaticParams, MI->BasePropertyOverrides, /*bForceRecompile*/true);
			MI->MarkPackageDirty();
		}
	}

	// Material: notify the open editor only.
	// Do NOT call Mat->ForceRecompileForRendering() here — UE 5.7's
	// UMaterial::PostEditChangePropertyInternal already calls
	// CacheResourceShadersForRendering, which rebuilds ShadingModels and queues
	// the shader compile. Calling ForceRecompileForRendering a second time
	// queues a parallel CacheResourceShadersForRendering pass that invalidates
	// the FMaterialResource the first job was about to consume; the async
	// shader compiler then crashes in FMaterialResource::GetShadingModels()
	// dereferencing the swapped-out resource. Repro: create a fresh Material,
	// asset:set("BlendMode", "BLEND_Additive"); asset:set("ShadingModel",
	// "MSM_Unlit") — editor AVs on the next AssetCompilingManager tick.
	if (UMaterial* Mat = Cast<UMaterial>(Asset))
	{
		if (GEditor)
		{
			UAssetEditorSubsystem* Sub = NeoLuaSubsystem::GetEditor<UAssetEditorSubsystem>();
			if (Sub)
			{
				IAssetEditorInstance* Ed = Sub->FindEditorForAsset(Mat, false);
				if (Ed && Ed->GetEditorName() == TEXT("MaterialEditor"))
				{
					static_cast<IMaterialEditor*>(Ed)->MarkMaterialDirty();
				}
			}
		}
	}
	// Blueprint: recompile
	else if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
	// AnimMontage: rebuild cache
	else if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
	{
		Montage->RefreshCacheData();
	}
	// BlendSpace: only rebuild cached grid/triangulation data for properties
	// that actually change sample positions, sample contents, or interpolation
	// topology. UE's PostEditChangeProperty already handles ordinary scalar
	// fields such as bLoop.
	else if (UBlendSpace* BS = Cast<UBlendSpace>(Asset))
	{
		if (!ChangedProperty)
		{
			return;
		}

		const FName ChangedName = ChangedProperty->GetFName();
		const bool bAffectsBlendData =
			ChangedName == TEXT("SampleData") ||
			ChangedName == TEXT("BlendParameters") ||
			ChangedName == TEXT("bInterpolateUsingGrid") ||
			ChangedName == TEXT("PreferredTriangulationDirection") ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendParameter, Min) ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendParameter, Max) ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendParameter, GridNum) ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendParameter, bSnapToGrid) ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendSample, Animation) ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendSample, SampleValue) ||
			ChangedName == GET_MEMBER_NAME_CHECKED(FBlendSample, RateScale)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			|| ChangedName == GET_MEMBER_NAME_CHECKED(FBlendSample, bUseSingleFrameForBlending)
			|| ChangedName == GET_MEMBER_NAME_CHECKED(FBlendSample, FrameIndexToSample)
#endif
			;

		if (bAffectsBlendData)
		{
			BS->ResampleData();
		}
	}
	// BehaviorTree node: re-init from asset
	else if (UBTNode* BTNode = Cast<UBTNode>(Asset))
	{
		UBehaviorTree* OwnerTree = BTNode->GetTypedOuter<UBehaviorTree>();
		if (OwnerTree)
		{
			BTNode->InitializeFromAsset(*OwnerTree);
			OwnerTree->MarkPackageDirty();
		}
	}
}

// ---- Instanced object slot helpers ----

enum class EObjectSlotKind : uint8
{
	None,
	Object,
	Array
};

struct FObjectSlotInfo
{
	FProperty* Property = nullptr;
	FObjectPropertyBase* ObjectProperty = nullptr;
	EObjectSlotKind Kind = EObjectSlotKind::None;
	UClass* BaseClass = nullptr;
};

static bool IsClassUsableForInlineObject(UClass* Class)
{
	return Class
		&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		&& Class->IsChildOf(UObject::StaticClass());
}

static bool IsObjectSlotInstanced(FProperty* SlotProperty, FObjectPropertyBase* ObjectProperty)
{
	if (!SlotProperty || !ObjectProperty || !ObjectProperty->PropertyClass)
	{
		return false;
	}

	return SlotProperty->HasAnyPropertyFlags(CPF_InstancedReference)
		|| SlotProperty->HasAnyPropertyFlags(CPF_ContainsInstancedReference)
		|| ObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference)
		|| ObjectProperty->HasAnyPropertyFlags(CPF_ContainsInstancedReference)
		|| ObjectProperty->PropertyClass->HasAnyClassFlags(CLASS_EditInlineNew | CLASS_DefaultToInstanced);
}

static bool ResolveObjectSlot(UObject* Asset, const FString& PropertyName, FObjectSlotInfo& OutInfo, FString& OutError)
{
	OutInfo = FObjectSlotInfo();
	FProperty* Prop = FindProperty(Asset, PropertyName);
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("property '%s' not found"), *PropertyName);
		return false;
	}

	if (!Prop->HasAnyPropertyFlags(CPF_Edit) || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
	{
		OutError = FString::Printf(TEXT("property '%s' is not an editable object slot"), *PropertyName);
		return false;
	}

	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
	{
		if (!IsObjectSlotInstanced(Prop, ObjProp))
		{
			OutError = FString::Printf(TEXT("property '%s' is an object reference, not an instanced/edit-inline object slot"), *PropertyName);
			return false;
		}

		OutInfo.Property = Prop;
		OutInfo.ObjectProperty = ObjProp;
		OutInfo.Kind = EObjectSlotKind::Object;
		OutInfo.BaseClass = ObjProp->PropertyClass;
		return true;
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		if (FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner))
		{
			if (!IsObjectSlotInstanced(Prop, InnerObjProp))
			{
				OutError = FString::Printf(TEXT("array '%s' stores object references, not instanced/edit-inline objects"), *PropertyName);
				return false;
			}

			OutInfo.Property = Prop;
			OutInfo.ObjectProperty = InnerObjProp;
			OutInfo.Kind = EObjectSlotKind::Array;
			OutInfo.BaseClass = InnerObjProp->PropertyClass;
			return true;
		}
	}

	OutError = FString::Printf(TEXT("property '%s' is not an object property or array of objects"), *PropertyName);
	return false;
}

static UClass* ResolveInlineObjectClass(const FString& ClassName, UClass* BaseClass, FString& OutError)
{
	UClass* SubClass = NeoTypeResolver::FindClassRobust(ClassName);
	if (!SubClass)
	{
		SubClass = LoadObject<UClass>(nullptr, *ClassName);
	}
	if (!SubClass)
	{
		OutError = FString::Printf(TEXT("class '%s' not found"), *ClassName);
		return nullptr;
	}
	if (!IsClassUsableForInlineObject(SubClass))
	{
		OutError = FString::Printf(TEXT("class '%s' is abstract, deprecated, or not a UObject class"), *SubClass->GetName());
		return nullptr;
	}
	if (BaseClass && !SubClass->IsChildOf(BaseClass))
	{
		OutError = FString::Printf(TEXT("class '%s' is not a child of required base '%s'"), *SubClass->GetName(), *BaseClass->GetName());
		return nullptr;
	}
	return SubClass;
}

static UObject* GetObjectFromSlot(UObject* Asset, const FObjectSlotInfo& Slot, int32 OneBasedIndex, FString& OutError)
{
	if (!Asset || !Slot.Property || !Slot.ObjectProperty)
	{
		OutError = TEXT("invalid object slot");
		return nullptr;
	}

	if (Slot.Kind == EObjectSlotKind::Object)
	{
		void* PropContainer = Slot.Property->ContainerPtrToValuePtr<void>(Asset);
		return Slot.ObjectProperty->GetObjectPropertyValue(PropContainer);
	}

	if (Slot.Kind == EObjectSlotKind::Array)
	{
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(Slot.Property);
		void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
		FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);
		const int32 ZeroIndex = OneBasedIndex - 1;
		if (ZeroIndex < 0 || ZeroIndex >= ArrayHelper.Num())
		{
			OutError = FString::Printf(TEXT("index %d out of range (count=%d)"), OneBasedIndex, ArrayHelper.Num());
			return nullptr;
		}

		return Slot.ObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(ZeroIndex));
	}

	OutError = TEXT("unsupported object slot kind");
	return nullptr;
}

// ---- AssetUserData helpers ----

static IInterface_AssetUserData* GetAssetUserDataInterface(UObject* Asset)
{
	return Cast<IInterface_AssetUserData>(Asset);
}

static FString GetUserDataClassName(UAssetUserData* UserData)
{
	if (!UserData) return TEXT("Unknown");
	FString ClassName = UserData->GetClass()->GetName();
	return ClassName;
}

static UClass* FindUserDataClass(const FString& ClassName)
{
	// Try exact match first (e.g. "UAnimationModifiersAssetUserData" or "AnimationModifiersAssetUserData")
	FString SearchName = ClassName;
	if (!SearchName.StartsWith(TEXT("U")))
	{
		SearchName = TEXT("U") + SearchName;
	}

	// Search loaded classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(UAssetUserData::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			if (Class->GetName().Equals(SearchName, ESearchCase::IgnoreCase) ||
				Class->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
	}

	// Try partial match (e.g. "AnimationModifiers" matches "UAnimationModifiersAssetUserData")
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(UAssetUserData::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			if (Class->GetName().Contains(ClassName, ESearchCase::IgnoreCase))
			{
				return Class;
			}
		}
	}

	return nullptr;
}

} // namespace LuaAssetHelper

// ============================================================================
// Lua Binding: open_asset
// ============================================================================

static sol::table BuildSubobjectHandle(sol::state_view Lua, FLuaSessionData& Session, UObject* Owner, UObject* SubObject, const FString& Label)
{
	sol::table Obj = Lua.create_table();
	if (!SubObject)
	{
		return Obj;
	}

	Obj["name"] = TCHAR_TO_UTF8(*SubObject->GetName());
	Obj["class_name"] = TCHAR_TO_UTF8(*SubObject->GetClass()->GetName());
	Obj["class_path"] = TCHAR_TO_UTF8(*SubObject->GetClass()->GetPathName());
	Obj["path"] = TCHAR_TO_UTF8(*SubObject->GetPathName());

	TWeakObjectPtr<UObject> WeakOwner(Owner);
	TWeakObjectPtr<UObject> WeakSubObject(SubObject);
	const FString HandleLabel = Label;

	Obj.set_function("get", [WeakSubObject, HandleLabel, &Session](sol::table,
		const std::string& PropertyName, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UObject* Target = WeakSubObject.Get();
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] %s:get -> subobject no longer valid"), *HandleLabel));
			return sol::lua_nil;
		}

		FString FProp = NeoLuaStr::ToFString(PropertyName);
		LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Target, FProp);
		if (!Resolved.Property)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] %s:get(\"%s\") -> property not found"), *HandleLabel, *FProp));
			return sol::lua_nil;
		}

		return NeoLuaProperty::ReadPropertyAsSol(Resolved.Property, Resolved.ValuePtr, LuaView);
	});

	Obj.set_function("set", [WeakOwner, WeakSubObject, HandleLabel, &Session](sol::table,
		const std::string& PropertyName, sol::object Value, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UObject* OwnerObj = WeakOwner.Get();
		UObject* Target = WeakSubObject.Get();
		if (!OwnerObj || !Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] %s:set -> owner or subobject no longer valid"), *HandleLabel));
			return sol::lua_nil;
		}

		FString FProp = NeoLuaStr::ToFString(PropertyName);
		if (LuaAssetHelper::RefuseUnsafeMaterialMutation(OwnerObj, TEXT("subobject:set"), Session, FProp))
		{
			return sol::lua_nil;
		}

		LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Target, FProp);
		if (!Resolved.Property)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] %s:set(\"%s\") -> property not found"), *HandleLabel, *FProp));
			return sol::lua_nil;
		}

		OwnerObj->Modify();
		Target->Modify();
		Target->PreEditChange(Resolved.Property);

		FString Error;
		if (!NeoLuaProperty::ApplySolValueToProperty(Resolved.Property, Resolved.ValuePtr, Target, Value, Error))
		{
			FPropertyChangedEvent FailEvent(Resolved.Property, EPropertyChangeType::ValueSet);
			Target->PostEditChangeProperty(FailEvent);
			Session.Log(FString::Printf(TEXT("[FAIL] %s:set(\"%s\") -> %s"), *HandleLabel, *FProp, *Error));
			return sol::lua_nil;
		}

		FPropertyChangedEvent SuccessEvent(Resolved.Property, EPropertyChangeType::ValueSet);
		Target->PostEditChangeProperty(SuccessEvent);
		OwnerObj->MarkPackageDirty();
		LuaAssetHelper::PostEdit(OwnerObj, nullptr);

		Session.Log(FString::Printf(TEXT("[OK] %s:set(\"%s\")"), *HandleLabel, *FProp));
		return sol::make_object(LuaView, true);
	});

	Obj.set_function("list_properties", [WeakSubObject, HandleLabel, &Session](sol::table,
		sol::optional<std::string> Filter, sol::optional<bool> IncludeAll,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UObject* Target = WeakSubObject.Get();
		if (!Target)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] %s:list_properties -> subobject no longer valid"), *HandleLabel));
			return sol::lua_nil;
		}

		FString FFilter = NeoLuaStr::ToFStringOpt(Filter);
		const bool bAll = IncludeAll.value_or(true);
		sol::table Result = LuaView.create_table();
		int32 Index = 1;

		for (TFieldIterator<FProperty> PropIt(Target->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
			if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;

			FString Name = Property->GetName();
			if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

			FString Value = NeoStackToolUtils::GetPropertyValueAsString(Target, Property, Target);
			if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Name);
			Entry["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Property));
			Entry["value"] = TCHAR_TO_UTF8(*Value);
			Entry["category"] = TCHAR_TO_UTF8(*Property->GetMetaData(TEXT("Category")));
			Result[Index++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] %s:list_properties(%s) -> %d properties"),
			*HandleLabel, FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
		return Result;
	});

	Obj.set_function("info", [WeakSubObject](sol::table, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		UObject* Target = WeakSubObject.Get();
		if (!Target) return sol::lua_nil;

		sol::table Result = LuaView.create_table();
		Result["name"] = TCHAR_TO_UTF8(*Target->GetName());
		Result["class_name"] = TCHAR_TO_UTF8(*Target->GetClass()->GetName());
		Result["class_path"] = TCHAR_TO_UTF8(*Target->GetClass()->GetPathName());
		Result["path"] = TCHAR_TO_UTF8(*Target->GetPathName());
		return Result;
	});

	return Obj;
}

static TArray<FLuaFunctionDoc> OpenAssetDocs = {
	{ TEXT("open_asset(path)"), TEXT("Open any Unreal asset — returns handle with reflection + asset-specific methods; call help() to discover"), TEXT("asset table or nil") },
	{ TEXT("open_editor(path)"), TEXT("Pop the asset editor window in the UE UI for the given asset (focuses if already open). Side-effect only — doesn't return a handle."), TEXT("true on success, nil if the asset wasn't found or the editor failed to open") },
};

static void BindOpenAsset(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("open_asset", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		const FString FPath = NeoLuaAsset::NormalizePath(NeoLuaStr::ToFString(Path));

		// Load the asset (with Asset Registry fallback for PCG-style short paths).
		UObject* Asset = NeoLuaAsset::ResolveWithRegistry(FPath);

		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] open_asset(\"%s\") -> asset not found"), *FPath));
			return sol::lua_nil;
		}

		// ControlRig blueprints use their own enrichment path — MUST skip the generic
		// _open_blueprint path because the Kismet compiler does CastChecked<UEdGraphSchema_K2>
		// on all BP graphs, which crashes on ControlRig's URigVMEdGraphSchema.
		{
			FString BPClassName = Asset->GetClass()->GetName();
			if (BPClassName == TEXT("ControlRigBlueprint") || BPClassName == TEXT("ControlRigBlueprintLegacy"))
			{
				sol::protected_function EnrichCR = LuaView["_enrich_control_rig"];
				if (EnrichCR.valid())
				{
					sol::table CRTable = LuaView.create_table();
					CRTable["path"] = TCHAR_TO_UTF8(*FPath);
					CRTable["type"] = TCHAR_TO_UTF8(*BPClassName);
					CRTable["name"] = TCHAR_TO_UTF8(*Asset->GetName());
					EnrichCR(CRTable);
					Session.Log(FString::Printf(TEXT("[OK] open_asset(\"%s\") -> ControlRig enrichment"), *FPath));
					return CRTable;
				}
			}
		}

		// For Blueprints, delegate to internal _open_blueprint for the richer API
		if (Asset->IsA<UBlueprint>())
		{
			sol::protected_function OpenBP = LuaView["_open_blueprint"];
			if (OpenBP.valid())
			{
				auto BPResult = OpenBP(Path);
				if (BPResult.valid())
				{
					// Enrich Widget Blueprints with widget tree methods (core binding)
					if (Asset->IsA<UWidgetBlueprint>())
					{
						sol::protected_function EnrichWB = LuaView["_enrich_widget_blueprint"];
						if (EnrichWB.valid()) EnrichWB(BPResult);
					}

					// Extension-owned Blueprint subclasses (GameplayEffect, GameplayAbility, ...)
					// register via FNeoStackExtensionRegistrar::RegisterAssetCapability.
					if (const FLuaAssetCapability* Capability = FLuaAssetCapabilityRegistry::Get().FindOwner(Asset))
					{
						if (!Capability->EnrichFunctionName.IsEmpty())
						{
							FTCHARToUTF8 EnrichNameUtf8(*Capability->EnrichFunctionName);
							sol::protected_function Enrich = LuaView[EnrichNameUtf8.Get()];
							if (Enrich.valid()) Enrich(BPResult);
						}
					}

					Session.Log(FString::Printf(TEXT("[OK] open_asset(\"%s\") -> delegated to open_blueprint (%s)"),
						*FPath, *Asset->GetClass()->GetName()));
					return BPResult;
				}
			}
		}

		FString AssetClassName = Asset->GetClass()->GetName();
		Session.Log(FString::Printf(TEXT("[OK] open_asset(\"%s\") -> %s"), *FPath, *AssetClassName));

		// Build the asset handle table
		sol::table AssetObj = LuaView.create_table();
		AssetObj["path"] = TCHAR_TO_UTF8(*FPath);
		AssetObj["type"] = TCHAR_TO_UTF8(*AssetClassName);
		AssetObj["class"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetPathName());

		// GC-safe weak reference — all lambdas capture this instead of raw Asset*
		TWeakObjectPtr<UObject> WeakAsset(Asset);

		// ----------------------------------------------------------------
		// asset:get(property) -> string value
		// ----------------------------------------------------------------
		AssetObj.set_function("get", [WeakAsset, FPath, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] get -> asset no longer valid"));
				return sol::lua_nil;
			}

			// Try dot-notation resolution
			LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Asset, FProp);
			if (!Resolved.Property)
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(Asset, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\") -> %s"), *FProp, *Error));
				return sol::lua_nil;
			}

			FString Value = LuaAssetHelper::GetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr);
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Value)));
		});

		// ----------------------------------------------------------------
		// asset:set(property, value) -> true/nil
		//
		// Value may be either:
		//   - string: imported via FProperty::ImportText_InContainer (e.g. "(X=0,Y=0)")
		//   - table/number/bool: routed through NeoLuaProperty::ApplySolValueToProperty,
		//     which uses registered struct handlers for FVector, FLinearColor, FIntPoint, ... —
		//     so `asset:set("SourceUV", {x=0, y=0})` works alongside `asset:set("Foo", "value")`.
		// ----------------------------------------------------------------
		AssetObj.set_function("set", [WeakAsset, FPath, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::object Value, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("set"), Session, FProp))
			{
				return sol::lua_nil;
			}

			if (USoundWave* Wave = Cast<USoundWave>(Asset))
			{
				FString GuardReason;
				if (LuaAssetHelper::IsUnsafeEmptySoundWaveRebuildProperty(Wave, FProp, nullptr, GuardReason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> %s"), *FProp, *GuardReason));
					return sol::lua_nil;
				}
			}

			LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Asset, FProp);
			if (!Resolved.Property)
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(Asset, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> %s"), *FProp, *Error));
				return sol::lua_nil;
			}

			// Check writable
			EPropertyAccessResultFlags AccessResult = PropertyAccessUtil::CanSetPropertyValue(
				Resolved.Property, PropertyAccessUtil::EditorReadOnlyFlags,
				PropertyAccessUtil::IsObjectTemplate(Asset));
			if (EnumHasAnyFlags(AccessResult, EPropertyAccessResultFlags::PermissionDenied))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> property is read-only"), *FProp));
				return sol::lua_nil;
			}

			// MaterialInstance Parent writes must go through SetParentEditorOnly so the
			// engine performs cycle checks, parent-type validation, resource teardown, and
			// the OnBaseMaterialSetEvent broadcast. The generic UPROPERTY write path only
			// runs ValidateStaticPermutationAllowed(), which is not equivalent.
			// (Evidence: MaterialInstance.cpp:3877 SetParentInternal vs :4687 PostEditChangeProperty)
			if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset))
			{
				if (Resolved.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialInstance, Parent))
				{
					UMaterialInterface* NewParent = nullptr;
					if (Value.is<std::string>())
					{
						FString FValue = NeoLuaStr::ToFString(Value.as<std::string>());
						if (!FValue.IsEmpty() && !FValue.Equals(TEXT("none"), ESearchCase::IgnoreCase))
						{
							NewParent = NeoLuaAsset::Resolve<UMaterialInterface>(FValue);
							if (!NewParent)
							{
								Session.Log(FString::Printf(TEXT("[FAIL] set(\"Parent\") -> not found: %s"), *FValue));
								return sol::lua_nil;
							}
						}
					}
					else
					{
						Session.Log(TEXT("[FAIL] set(\"Parent\") -> pass an asset path string, e.g. \"/Game/Mats/M_Base\""));
						return sol::lua_nil;
					}

					MIC->Modify();
					MIC->SetParentEditorOnly(NewParent);
					MIC->PostEditChange();
					MIC->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] set(\"Parent\") -> %s (via SetParentEditorOnly)"),
						NewParent ? *NewParent->GetPathName() : TEXT("<none>")));
					return sol::make_object(Lua, true);
				}
			}

			// UMaterial bUsedWith* flags: a generic UPROPERTY ImportText write does
			// not survive UMaterial::PostEditChangeProperty's reentrancy gate — the
			// flag reads back unchanged and `[OK] set(...) = "False" (was "False")`
			// silently lies to the agent. The engine's documented path is
			// SetMaterialUsage, which opens a FMaterialUpdateContext with
			// SyncWithRenderingThread, flips the flag via SetUsageByFlag, queues a
			// proper recompile, and marks the package dirty.
			//
			// SetMaterialUsage only handles set-to-true (the common case for
			// "make this material work with Niagara/particles/water/..."). For
			// set-to-false we fall through to the generic path — the engine doesn't
			// gate clearing usage flags.
			if (UMaterial* Mat = Cast<UMaterial>(Asset))
			{
				EMaterialUsage Usage;
				if (LuaAssetHelper::MaterialUsageFromBoolPropertyName(Resolved.Property->GetFName(), Usage))
				{
					bool bDesired = false;
					bool bHaveBool = false;
					if (Value.is<bool>())
					{
						bDesired = Value.as<bool>();
						bHaveBool = true;
					}
					else if (Value.is<std::string>())
					{
						const FString FlagStr = NeoLuaStr::ToFString(Value.as<std::string>());
						if (FlagStr.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
						    FlagStr.Equals(TEXT("1")))
						{
							bDesired = true; bHaveBool = true;
						}
						else if (FlagStr.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
						         FlagStr.Equals(TEXT("0")))
						{
							bDesired = false; bHaveBool = true;
						}
					}

					if (bHaveBool && bDesired)
					{
						const bool bBefore = Mat->GetUsageByFlag(Usage);
						bool bNeedsRecompile = false;
						const bool bOk = Mat->SetMaterialUsage(bNeedsRecompile, Usage);
						const bool bAfter = Mat->GetUsageByFlag(Usage);

						if (bAfter)
						{
							Session.Log(FString::Printf(
								TEXT("[OK] set(\"%s\") -> True (via SetMaterialUsage%s%s)"),
								*FProp,
								bBefore ? TEXT(", was already True") : TEXT(""),
								bNeedsRecompile ? TEXT(", recompile queued") : TEXT("")));
							return sol::make_object(Lua, true);
						}

						// SetMaterialUsage didn't take. Reasons (per engine source):
						//   • MaterialDomain is not Surface/DeferredDecal/Volume → returns false early
						//   • bAutomaticallySetUsageInEditor is false → engine warns + returns false
						//   • bUsedAsSpecialEngineMaterial is true → returns true but skips the flip
						// Report the failure rather than silently falling back to a generic write
						// the engine would also reject.
						Session.Log(FString::Printf(
							TEXT("[FAIL] set(\"%s\") -> SetMaterialUsage did not flip the flag (returned %s). "
							     "MaterialDomain must be Surface/DeferredDecal/Volume, "
							     "bAutomaticallySetUsageInEditor must be true, "
							     "and bUsedAsSpecialEngineMaterial must be false."),
							*FProp, bOk ? TEXT("true") : TEXT("false")));
						return sol::lua_nil;
					}
					// bHaveBool && !bDesired: fall through to generic clear path.
				}
			}

			FString OldValue = LuaAssetHelper::GetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr);

			if (USoundWave* Wave = Cast<USoundWave>(Asset))
			{
				FString GuardReason;
				if (LuaAssetHelper::IsUnsafeEmptySoundWaveRebuildProperty(Wave, FProp, Resolved.Property, GuardReason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> %s"), *FProp, *GuardReason));
					return sol::lua_nil;
				}
			}

			// Bracket the change with UE-style PreEditChange/PostEditChange notification.
			// Dot-notation paths edit nested struct/container fields, so notify with an
			// FEditPropertyChain just like the PropertyEditor does. Some engine classes
			// key validation/update work off the active member property rather than only
			// the leaf field.
			Asset->Modify();
			if (UPhysicalMaterial* PhysMat = Cast<UPhysicalMaterial>(Asset))
			{
				// UPhysicalMaterial::PostEditChangeProperty immediately updates the
				// Chaos material handle. Ensure the handle exists before editor
				// notification/write/post-edit only propagate changed values.
				PhysMat->GetPhysicsMaterial();
			}

			FEditPropertyChain PropertyChain;
			const bool bUsePropertyChain = Resolved.IsNested();
			if (bUsePropertyChain)
			{
				LuaAssetHelper::FillEditPropertyChain(Resolved, PropertyChain);
				Asset->PreEditChange(PropertyChain);
			}
			else
			{
				Asset->PreEditChange(Resolved.Property);
			}

			TArray<const UObject*> TopLevelObjects;
			TopLevelObjects.Add(Asset);
			auto PostEditForSet = [&]()
			{
				FPropertyChangedEvent ChangeEvent(Resolved.Property, EPropertyChangeType::ValueSet, MakeArrayView(TopLevelObjects));
				if (bUsePropertyChain)
				{
					if (Resolved.ChainProperties.Num() > 0)
					{
						ChangeEvent.SetActiveMemberProperty(Resolved.ChainProperties[0]);
					}
					FPropertyChangedChainEvent ChainEvent(PropertyChain, ChangeEvent);
					Asset->PostEditChangeChainProperty(ChainEvent);
				}
				else
				{
					Asset->PostEditChangeProperty(ChangeEvent);
				}
			};

			FString Error;
			bool bOk = false;
			FString ValueDesc;

			if (Value.is<std::string>())
			{
				// String path — FProperty::ImportText_InContainer (handles every UPROPERTY type).
				FString FValue = NeoLuaStr::ToFString(Value.as<std::string>());
				ValueDesc = FValue;
				bOk = LuaAssetHelper::SetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr, FValue, Error);
				if (bOk)
				{
					NeoLuaProperty::ClampNumericPropertyValueFromMetaData(Resolved.Property, Resolved.ValuePtr);
				}
			}
			else
			{
				// Table / number / bool path — routed through the plugin-wide property helper.
				ValueDesc = TEXT("<table|number|bool>");
				bOk = NeoLuaProperty::ApplySolValueToProperty(Resolved.Property, Resolved.ValuePtr, Asset, Value, Error);
			}

			if (!bOk)
			{
				// Still need to close the bracket even on failure
				PostEditForSet();
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\", %s) -> %s"), *FProp, *ValueDesc, *Error));
				return sol::lua_nil;
			}

			Asset->MarkPackageDirty();
			PostEditForSet();

			// Asset-specific post-edit hooks (Material recompile, BlendSpace resample, etc.).
			// Pass the property so type-specific hooks can key on WHICH property changed —
			// e.g. MaterialInstance needs UpdateStaticPermutation only for BasePropertyOverrides.
			LuaAssetHelper::PostEdit(Asset, Resolved.Property);

			FString NewValue = LuaAssetHelper::GetPropertyValue(Asset, Resolved.Property, Resolved.ContainerPtr);
			// Detect no-op writes — engine-reverted property changes (e.g. UMaterial
			// usage flags written without SetMaterialUsage) used to log [OK] even
			// when the bitfield read back unchanged, which let the agent build on
			// a false success. Surface that as a WARN so the caller sees it.
			if (NewValue.Equals(OldValue, ESearchCase::CaseSensitive))
			{
				Session.Log(FString::Printf(
					TEXT("[WARN] set(\"%s\") = \"%s\" (no-op: value already equal, or engine reverted the write)"),
					*FProp, *NewValue));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[OK] set(\"%s\") = \"%s\" (was \"%s\")"),
					*FProp, *NewValue, *OldValue));
			}
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:list_properties(filter?, all?) -> table
		// ----------------------------------------------------------------
		AssetObj.set_function("list_properties", [WeakAsset, &Session](sol::table /*self*/,
			sol::optional<std::string> Filter, sol::optional<bool> IncludeAll,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] list_properties -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString FFilter = NeoLuaStr::ToFStringOpt(Filter);
			bool bAll = IncludeAll.value_or(false);

			sol::table Result = Lua.create_table();
			int32 Index = 1;

			for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
				if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;

				FString Name = Property->GetName();
				if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

				FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
				FString Value = NeoStackToolUtils::GetPropertyValueAsString(Asset, Property, Asset);
				FString Category = Property->GetMetaData(TEXT("Category"));
				if (Category.IsEmpty()) Category = TEXT("Default");

				// Truncate long values for readability
				if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

				sol::table Entry = Lua.create_table();
				Entry["name"] = TCHAR_TO_UTF8(*Name);
				Entry["type"] = TCHAR_TO_UTF8(*Type);
				Entry["value"] = TCHAR_TO_UTF8(*Value);
				Entry["category"] = TCHAR_TO_UTF8(*Category);
				Result[Index++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_properties(%s) -> %d properties"),
				FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:array_count(property) -> integer
		// ----------------------------------------------------------------
		AssetObj.set_function("array_count", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] array_count -> asset no longer valid"));
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_count(\"%s\") -> property not found"), *FProp));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_count(\"%s\") -> not an array property"), *FProp));
				return sol::lua_nil;
			}

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);

			return sol::make_object(Lua, ArrayHelper.Num());
		});

		// ----------------------------------------------------------------
		// asset:array_add(property, value?) -> new count
		// ----------------------------------------------------------------
		AssetObj.set_function("array_add", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::optional<std::string> Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] array_add -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("array_add"), Session, FProp))
			{
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_add(\"%s\") -> property not found"), *FProp));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_add(\"%s\") -> not an array property"), *FProp));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(Prop);

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);
			int32 NewIndex = ArrayHelper.AddValue();

			// If value provided, set it via ImportText on the inner property
			if (Value.has_value())
			{
				FString FValue = NeoLuaStr::ToFStringOpt(Value);
				void* ElementPtr = ArrayHelper.GetRawPtr(NewIndex);
				FProperty* InnerProp = ArrayProp->Inner;

				const TCHAR* ImportResult = InnerProp->ImportText_Direct(*FValue, ElementPtr, Asset, PPF_None);
				if (!ImportResult)
				{
					// Try boolean fixup
					FString Transformed = FValue;
					if (FValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)) Transformed = TEXT("True");
					else if (FValue.Equals(TEXT("false"), ESearchCase::IgnoreCase)) Transformed = TEXT("False");

					ImportResult = InnerProp->ImportText_Direct(*Transformed, ElementPtr, Asset, PPF_None);
					if (!ImportResult)
					{
						// Remove the element we just added — it has garbage data
						ArrayHelper.RemoveValues(NewIndex, 1);
						FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
						Asset->PostEditChangeProperty(Event);

						Session.Log(FString::Printf(TEXT("[FAIL] array_add(\"%s\") -> failed to parse value '%s'"),
							*FProp, *FValue));
						return sol::lua_nil;
					}
				}
			}

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ArrayAdd);
			Asset->PostEditChangeProperty(Event);

			// Run asset-specific post-edit
			LuaAssetHelper::PostEdit(Asset, nullptr);

			int32 NewCount = ArrayHelper.Num();
			Session.Log(FString::Printf(TEXT("[OK] array_add(\"%s\") -> count = %d"), *FProp, NewCount));
			return sol::make_object(Lua, NewCount);
		});

		// ----------------------------------------------------------------
		// asset:array_remove(property, index) -> new count
		// 1-based index to match Lua convention
		// ----------------------------------------------------------------
		AssetObj.set_function("array_remove", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, int Index,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] array_remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("array_remove"), Session, FProp))
			{
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_remove(\"%s\") -> property not found"), *FProp));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] array_remove(\"%s\") -> not an array property"), *FProp));
				return sol::lua_nil;
			}

			// Convert 1-based Lua index to 0-based
			int32 ZeroIndex = Index - 1;

			Asset->Modify();
			Asset->PreEditChange(Prop);

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);

			if (ZeroIndex < 0 || ZeroIndex >= ArrayHelper.Num())
			{
				FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(Event);
				Session.Log(FString::Printf(TEXT("[FAIL] array_remove(\"%s\", %d) -> index out of range (count=%d)"),
					*FProp, Index, ArrayHelper.Num()));
				return sol::lua_nil;
			}

			ArrayHelper.RemoveValues(ZeroIndex, 1);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ArrayRemove);
			Asset->PostEditChangeProperty(Event);

			LuaAssetHelper::PostEdit(Asset, nullptr);

			int32 NewCount = ArrayHelper.Num();
			Session.Log(FString::Printf(TEXT("[OK] array_remove(\"%s\", %d) -> count = %d"), *FProp, Index, NewCount));
			return sol::make_object(Lua, NewCount);
		});

		// ----------------------------------------------------------------
		// asset:configure_at(array_prop, key, props) -> true/nil
		//
		// Finds one element in an array property by name (string key) or
		// 1-based index (number key), then applies a properties table onto
		// the element via NeoLuaProperty::ApplyTable (UObject elements) or
		// ApplyTableToStruct (USTRUCT elements). Replaces the "lookup by
		// name + mutate fields" pattern duplicated across Skeleton sockets,
		// MaterialInstance params, PhysicsAsset bodies, PoseAsset poses, etc.
		//
		// Name matching tries (in order): GetFName() for UObject elements,
		// then FName/FString properties named "Name", "ParameterName",
		// "SocketName", "BoneName", "GroupName", "VirtualBoneName",
		// "CurveName", "MarkerName", "EventName".
		// ----------------------------------------------------------------
		AssetObj.set_function("configure_at", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& ArrayPropName, sol::object Key, sol::table Props,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FArrayName = NeoLuaStr::ToFString(ArrayPropName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] configure_at -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("configure_at"), Session, FArrayName))
			{
				return sol::lua_nil;
			}

			FProperty* Prop = LuaAssetHelper::FindProperty(Asset, FArrayName);
			if (!Prop)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\") -> array property not found"), *FArrayName));
				return sol::lua_nil;
			}
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\") -> not an array property"), *FArrayName));
				return sol::lua_nil;
			}

			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);
			FProperty* InnerProp = ArrayProp->Inner;
			const int32 ArrayNum = ArrayHelper.Num();

			// ---- resolve key → element index ----
			int32 FoundIdx = INDEX_NONE;
			FString KeyDesc;

			if (Key.is<int>() || Key.is<double>())
			{
				int32 OneBased = Key.is<int>() ? Key.as<int>() : static_cast<int32>(Key.as<double>());
				KeyDesc = FString::Printf(TEXT("%d"), OneBased);
				FoundIdx = OneBased - 1;
				if (FoundIdx < 0 || FoundIdx >= ArrayNum)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\", %d) -> index out of range (count=%d)"),
						*FArrayName, OneBased, ArrayNum));
					return sol::lua_nil;
				}
			}
			else if (Key.is<std::string>())
			{
				FString KeyStr = NeoLuaStr::ToFString(Key.as<std::string>());
				FName KeyName(*KeyStr);
				KeyDesc = FString::Printf(TEXT("\"%s\""), *KeyStr);

				static const TArray<FName> NameFieldCandidates = {
					FName(TEXT("Name")),
					FName(TEXT("ParameterName")),
					FName(TEXT("SocketName")),
					FName(TEXT("BoneName")),
					FName(TEXT("GroupName")),
					FName(TEXT("VirtualBoneName")),
					FName(TEXT("CurveName")),
					FName(TEXT("MarkerName")),
					FName(TEXT("EventName"))
				};

				auto MatchField = [&](FProperty* FieldProp, const void* FieldContainer) -> bool
				{
					if (!FieldProp) return false;
					if (FNameProperty* NP = CastField<FNameProperty>(FieldProp))
					{
						return NP->GetPropertyValue_InContainer(FieldContainer) == KeyName;
					}
					if (FStrProperty* SP = CastField<FStrProperty>(FieldProp))
					{
						return SP->GetPropertyValue_InContainer(FieldContainer).Equals(KeyStr, ESearchCase::IgnoreCase);
					}
					return false;
				};

				FObjectProperty* ObjInnerProp = CastField<FObjectProperty>(InnerProp);
				FStructProperty* StructInnerProp = CastField<FStructProperty>(InnerProp);

				for (int32 i = 0; i < ArrayNum && FoundIdx == INDEX_NONE; i++)
				{
					void* ElementPtr = ArrayHelper.GetRawPtr(i);

					if (ObjInnerProp)
					{
						UObject* SubObj = ObjInnerProp->GetObjectPropertyValue(ElementPtr);
						if (!SubObj) continue;
						if (SubObj->GetFName() == KeyName) { FoundIdx = i; break; }
						for (const FName& FieldName : NameFieldCandidates)
						{
							if (MatchField(SubObj->GetClass()->FindPropertyByName(FieldName), SubObj))
							{
								FoundIdx = i;
								break;
							}
						}
					}
					else if (StructInnerProp)
					{
						for (const FName& FieldName : NameFieldCandidates)
						{
							if (MatchField(StructInnerProp->Struct->FindPropertyByName(FieldName), ElementPtr))
							{
								FoundIdx = i;
								break;
							}
						}
					}
				}

				if (FoundIdx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\", \"%s\") -> no element matched (tried GetFName + Name/ParameterName/SocketName/BoneName/GroupName/VirtualBoneName/CurveName/MarkerName/EventName)"),
						*FArrayName, *KeyStr));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\") -> key must be string (name) or number (1-based index)"), *FArrayName));
				return sol::lua_nil;
			}

			if (UMaterialParameterCollection* MPC = Cast<UMaterialParameterCollection>(Asset))
			{
				if (LuaAssetHelper::RefuseMPCParameterRenameConflict(MPC, FArrayName, FoundIdx, Props, Session))
				{
					return sol::lua_nil;
				}
			}

			// ---- apply properties onto element ----
			Asset->Modify();
			Asset->PreEditChange(Prop);

			void* ElementPtr = ArrayHelper.GetRawPtr(FoundIdx);
			FString ApplyError;
			TArray<FString> Warnings;
			bool bOk = false;

			if (FObjectProperty* ObjProp = CastField<FObjectProperty>(InnerProp))
			{
				UObject* SubObj = ObjProp->GetObjectPropertyValue(ElementPtr);
				if (SubObj)
				{
					SubObj->Modify();
					bOk = NeoLuaProperty::ApplyTable(SubObj, Props, ApplyError, &Warnings);
				}
				else
				{
					ApplyError = TEXT("element is null");
				}
			}
			else if (FStructProperty* StructProp = CastField<FStructProperty>(InnerProp))
			{
				bOk = NeoLuaProperty::ApplyTableToStruct(StructProp->Struct, ElementPtr, Asset, Props, ApplyError, &Warnings);
			}
			else
			{
				ApplyError = FString::Printf(TEXT("inner type %s not supported (must be object or struct)"), *InnerProp->GetCPPType());
			}

			FPropertyChangedEvent PostEvent(Prop, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(PostEvent);
			Asset->MarkPackageDirty();
			LuaAssetHelper::PostEdit(Asset, nullptr);

			for (const FString& W : Warnings)
			{
				Session.Log(FString::Printf(TEXT("[WARN] configure_at(\"%s\", %s) -> %s"), *FArrayName, *KeyDesc, *W));
			}

			if (!bOk)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_at(\"%s\", %s) -> %s"),
					*FArrayName, *KeyDesc, ApplyError.IsEmpty() ? TEXT("no fields applied") : *ApplyError));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] configure_at(\"%s\", %s) -> applied (index %d)"),
				*FArrayName, *KeyDesc, FoundIdx));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Map property operations
		// ================================================================

		// ----------------------------------------------------------------
		// asset:map_count(property) -> integer
		// ----------------------------------------------------------------
		AssetObj.set_function("map_count", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_count -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_count(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);

			return sol::make_object(Lua, MapHelper.Num());
		});

		// ----------------------------------------------------------------
		// asset:map_keys(property) -> table of key strings
		// ----------------------------------------------------------------
		AssetObj.set_function("map_keys", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_keys -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_keys(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();

			sol::table Result = Lua.create_table();
			int32 Index = 1;
			for (FScriptMapHelper::FIterator It = MapHelper.CreateIterator(); It; ++It)
			{
				const uint8* KeyPtr = MapHelper.GetKeyPtr(It);
				FString KeyStr = LuaAssetHelper::ExportPropertyValue(KeyProp, KeyPtr, Asset);
				Result[Index++] = TCHAR_TO_UTF8(*KeyStr);
			}

			Session.Log(FString::Printf(TEXT("[OK] map_keys(\"%s\") -> %d keys"), *FProp, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:map_get(property, key) -> value string
		// ----------------------------------------------------------------
		AssetObj.set_function("map_get", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Key,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);
			FString FKey = NeoLuaStr::ToFString(Key);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_get -> asset no longer valid"));
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_get(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();
			FProperty* ValProp = MapHelper.GetValueProperty();

			// Parse the key string
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			if (!LuaAssetHelper::ImportPropertyValue(KeyProp, TempKey, FKey, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				Session.Log(FString::Printf(TEXT("[FAIL] map_get(\"%s\", \"%s\") -> failed to parse key"), *FProp, *FKey));
				return sol::lua_nil;
			}

			uint8* ValuePtr = MapHelper.FindValueFromHash(TempKey);
			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);

			if (!ValuePtr)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_get(\"%s\", \"%s\") -> key not found"), *FProp, *FKey));
				return sol::lua_nil;
			}

			FString ValueStr = LuaAssetHelper::ExportPropertyValue(ValProp, ValuePtr, Asset);
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ValueStr)));
		});

		// ----------------------------------------------------------------
		// asset:map_set(property, key, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("map_set", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Key, const std::string& Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);
			FString FKey = NeoLuaStr::ToFString(Key);
			FString FValue = NeoLuaStr::ToFString(Value);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_set -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("map_set"), Session, FProp))
			{
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_set(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();
			FProperty* ValProp = MapHelper.GetValueProperty();

			// Parse key
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			if (!LuaAssetHelper::ImportPropertyValue(KeyProp, TempKey, FKey, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				Session.Log(FString::Printf(TEXT("[FAIL] map_set(\"%s\", \"%s\") -> failed to parse key"), *FProp, *FKey));
				return sol::lua_nil;
			}

			// Parse value
			void* TempVal = FMemory::Malloc(ValProp->GetSize(), ValProp->GetMinAlignment());
			ValProp->InitializeValue(TempVal);

			if (!LuaAssetHelper::ImportPropertyValue(ValProp, TempVal, FValue, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				ValProp->DestroyValue(TempVal);
				FMemory::Free(TempVal);
				Session.Log(FString::Printf(TEXT("[FAIL] map_set(\"%s\", \"%s\", \"%s\") -> failed to parse value"), *FProp, *FKey, *FValue));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			MapHelper.AddPair(TempKey, TempVal);

			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);
			ValProp->DestroyValue(TempVal);
			FMemory::Free(TempVal);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] map_set(\"%s\", \"%s\", \"%s\")"), *FProp, *FKey, *FValue));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:map_remove(property, key) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("map_remove", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Key,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);
			FString FKey = NeoLuaStr::ToFString(Key);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] map_remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("map_remove"), Session, FProp))
			{
				return sol::lua_nil;
			}

			FMapProperty* MapProp = LuaAssetHelper::FindMapProperty(Asset, FProp);
			if (!MapProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_remove(\"%s\") -> not a map property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* MapContainer = MapProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptMapHelper MapHelper(MapProp, MapContainer);
			FProperty* KeyProp = MapHelper.GetKeyProperty();

			// Parse key
			void* TempKey = FMemory::Malloc(KeyProp->GetSize(), KeyProp->GetMinAlignment());
			KeyProp->InitializeValue(TempKey);

			if (!LuaAssetHelper::ImportPropertyValue(KeyProp, TempKey, FKey, Asset))
			{
				KeyProp->DestroyValue(TempKey);
				FMemory::Free(TempKey);
				Session.Log(FString::Printf(TEXT("[FAIL] map_remove(\"%s\", \"%s\") -> failed to parse key"), *FProp, *FKey));
				return sol::lua_nil;
			}

			int32 FoundIndex = MapHelper.FindMapPairIndexFromHash(TempKey);
			KeyProp->DestroyValue(TempKey);
			FMemory::Free(TempKey);

			if (FoundIndex == INDEX_NONE)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] map_remove(\"%s\", \"%s\") -> key not found"), *FProp, *FKey));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			MapHelper.RemoveAt(FoundIndex);
			MapHelper.Rehash();

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] map_remove(\"%s\", \"%s\") -> count = %d"), *FProp, *FKey, MapHelper.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Set property operations
		// ================================================================

		// ----------------------------------------------------------------
		// asset:set_count(property) -> integer
		// ----------------------------------------------------------------
		AssetObj.set_function("set_count", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_count -> asset no longer valid"));
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_count(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);

			return sol::make_object(Lua, SetHelper.Num());
		});

		// ----------------------------------------------------------------
		// asset:set_values(property) -> table of value strings
		// ----------------------------------------------------------------
		AssetObj.set_function("set_values", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_values -> asset no longer valid"));
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_values(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);
			FProperty* ElemProp = SetHelper.GetElementProperty();

			sol::table Result = Lua.create_table();
			int32 Index = 1;
			for (FScriptSetHelper::FIterator It = SetHelper.CreateIterator(); It; ++It)
			{
				const uint8* ElemPtr = SetHelper.GetElementPtr(It);
				FString ElemStr = LuaAssetHelper::ExportPropertyValue(ElemProp, ElemPtr, Asset);
				Result[Index++] = TCHAR_TO_UTF8(*ElemStr);
			}

			Session.Log(FString::Printf(TEXT("[OK] set_values(\"%s\") -> %d elements"), *FProp, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:set_add(property, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("set_add", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);
			FString FValue = NeoLuaStr::ToFString(Value);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_add -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("set_add"), Session, FProp))
			{
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_add(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);
			FProperty* ElemProp = SetHelper.GetElementProperty();

			// Parse value
			void* TempElem = FMemory::Malloc(ElemProp->GetSize(), ElemProp->GetMinAlignment());
			ElemProp->InitializeValue(TempElem);

			if (!LuaAssetHelper::ImportPropertyValue(ElemProp, TempElem, FValue, Asset))
			{
				ElemProp->DestroyValue(TempElem);
				FMemory::Free(TempElem);
				Session.Log(FString::Printf(TEXT("[FAIL] set_add(\"%s\", \"%s\") -> failed to parse value"), *FProp, *FValue));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			SetHelper.AddElement(TempElem);

			ElemProp->DestroyValue(TempElem);
			FMemory::Free(TempElem);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] set_add(\"%s\", \"%s\") -> count = %d"), *FProp, *FValue, SetHelper.Num()));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:set_remove(property, value) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("set_remove", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, const std::string& Value,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);
			FString FValue = NeoLuaStr::ToFString(Value);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("set_remove"), Session, FProp))
			{
				return sol::lua_nil;
			}

			FSetProperty* SetProp = LuaAssetHelper::FindSetProperty(Asset, FProp);
			if (!SetProp)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_remove(\"%s\") -> not a set property"), *FProp));
				return sol::lua_nil;
			}

			FProperty* RawProp = LuaAssetHelper::FindProperty(Asset, FProp);

			void* SetContainer = SetProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptSetHelper SetHelper(SetProp, SetContainer);
			FProperty* ElemProp = SetHelper.GetElementProperty();

			// Parse value
			void* TempElem = FMemory::Malloc(ElemProp->GetSize(), ElemProp->GetMinAlignment());
			ElemProp->InitializeValue(TempElem);

			if (!LuaAssetHelper::ImportPropertyValue(ElemProp, TempElem, FValue, Asset))
			{
				ElemProp->DestroyValue(TempElem);
				FMemory::Free(TempElem);
				Session.Log(FString::Printf(TEXT("[FAIL] set_remove(\"%s\", \"%s\") -> failed to parse value"), *FProp, *FValue));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(RawProp);

			bool bRemoved = SetHelper.RemoveElement(TempElem);

			ElemProp->DestroyValue(TempElem);
			FMemory::Free(TempElem);

			if (!bRemoved)
			{
				FPropertyChangedEvent FailEvent(RawProp, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(FailEvent);
				Session.Log(FString::Printf(TEXT("[FAIL] set_remove(\"%s\", \"%s\") -> element not found"), *FProp, *FValue));
				return sol::lua_nil;
			}

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(RawProp, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, nullptr);

			Session.Log(FString::Printf(TEXT("[OK] set_remove(\"%s\", \"%s\") -> count = %d"), *FProp, *FValue, SetHelper.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Property metadata
		// ================================================================

		// ----------------------------------------------------------------
		// asset:property_meta(property) -> table with metadata keys
		// ----------------------------------------------------------------
		AssetObj.set_function("property_meta", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& PropertyName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FProp = NeoLuaStr::ToFString(PropertyName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] property_meta -> asset no longer valid"));
				return sol::lua_nil;
			}

			// Use ResolvePropertyPath for dot notation and array index support
			// (e.g. "Modifiers[0]", "Modifiers[0].ModifierOp")
			FProperty* Prop = nullptr;
			if (FProp.Contains(TEXT(".")) || FProp.Contains(TEXT("[")))
			{
				LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(Asset, FProp);
				Prop = Resolved.Property;
			}
			else
			{
				Prop = LuaAssetHelper::FindProperty(Asset, FProp);
			}
			if (!Prop)
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(Asset, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] property_meta(\"%s\") -> %s"), *FProp, *Error));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			// Property type info
			Result["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
			Result["cpp_type"] = TCHAR_TO_UTF8(*Prop->GetCPPType());

			// Flags
			Result["editable"] = Prop->HasAnyPropertyFlags(CPF_Edit);
			Result["blueprint_visible"] = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible);
			Result["blueprint_read_only"] = Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
			Result["config"] = Prop->HasAnyPropertyFlags(CPF_Config);
			Result["transient"] = Prop->HasAnyPropertyFlags(CPF_Transient);
			Result["deprecated"] = Prop->HasAnyPropertyFlags(CPF_Deprecated);

#if WITH_METADATA
			// All UE metadata (ClampMin, ClampMax, UIMin, UIMax, ToolTip, AllowedClasses, etc.)
			const TMap<FName, FString>* MetaMap = Prop->GetMetaDataMap();
			if (MetaMap)
			{
				sol::table Meta = Lua.create_table();
				for (const auto& Pair : *MetaMap)
				{
					Meta[TCHAR_TO_UTF8(*Pair.Key.ToString())] = TCHAR_TO_UTF8(*Pair.Value);
				}
				Result["metadata"] = Meta;
			}
#endif

			// Container info for array/map/set
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				Result["container"] = "array";
				Result["inner_type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(ArrayProp->Inner));
			}
			else if (FMapProperty* MapPropTyped = CastField<FMapProperty>(Prop))
			{
				Result["container"] = "map";
				Result["key_type"] = TCHAR_TO_UTF8(*MapPropTyped->GetKeyProperty()->GetCPPType());
				Result["value_type"] = TCHAR_TO_UTF8(*MapPropTyped->GetValueProperty()->GetCPPType());
			}
			else if (FSetProperty* SetPropTyped = CastField<FSetProperty>(Prop))
			{
				Result["container"] = "set";
				Result["element_type"] = TCHAR_TO_UTF8(*SetPropTyped->GetElementProperty()->GetCPPType());
			}
			else if (FStructProperty* StructPropTyped = CastField<FStructProperty>(Prop))
			{
				Result["container"] = "struct";
				Result["struct_name"] = TCHAR_TO_UTF8(*StructPropTyped->Struct->GetName());

				// List struct fields
				sol::table Fields = Lua.create_table();
				int32 FieldIdx = 1;
				for (TFieldIterator<FProperty> FieldIt(StructPropTyped->Struct); FieldIt; ++FieldIt)
				{
					sol::table FieldEntry = Lua.create_table();
					FieldEntry["name"] = TCHAR_TO_UTF8(*FieldIt->GetName());
					FieldEntry["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(*FieldIt));
					Fields[FieldIdx++] = FieldEntry;
				}
				Result["fields"] = Fields;
			}

			Session.Log(FString::Printf(TEXT("[OK] property_meta(\"%s\") -> %s"), *FProp, *Prop->GetCPPType()));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:list_object_slots(filter?) -> table of instanced/edit-inline object slots
		// ----------------------------------------------------------------
		AssetObj.set_function("list_object_slots", [WeakAsset, &Session](sol::table,
			sol::optional<std::string> Filter, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] list_object_slots -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FFilter = NeoLuaStr::ToFStringOpt(Filter);
			sol::table Result = Lua.create_table();
			int32 Index = 1;

			for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

				FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
				LuaAssetHelper::EObjectSlotKind Kind = LuaAssetHelper::EObjectSlotKind::Object;
				if (!ObjProp)
				{
					if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
					{
						ObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
						Kind = LuaAssetHelper::EObjectSlotKind::Array;
					}
				}
				if (!ObjProp || !LuaAssetHelper::IsObjectSlotInstanced(Prop, ObjProp)) continue;

				const FString Name = Prop->GetName();
				if (!FFilter.IsEmpty()
					&& !Name.Contains(FFilter, ESearchCase::IgnoreCase)
					&& !ObjProp->PropertyClass->GetName().Contains(FFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}

				sol::table Entry = Lua.create_table();
				Entry["name"] = TCHAR_TO_UTF8(*Name);
				Entry["kind"] = Kind == LuaAssetHelper::EObjectSlotKind::Array ? "array" : "object";
				Entry["base_class"] = ObjProp->PropertyClass ? TCHAR_TO_UTF8(*ObjProp->PropertyClass->GetName()) : "";
				Entry["base_class_path"] = ObjProp->PropertyClass ? TCHAR_TO_UTF8(*ObjProp->PropertyClass->GetPathName()) : "";
				Entry["cpp_type"] = TCHAR_TO_UTF8(*Prop->GetCPPType());
				Entry["instanced"] = true;
				Result[Index++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_object_slots(%s) -> %d slots"),
				FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:list_object_classes(slot, filter?) -> compatible loaded subclasses
		// ----------------------------------------------------------------
		AssetObj.set_function("list_object_classes", [WeakAsset, &Session](sol::table,
			const std::string& SlotName, sol::optional<std::string> Filter,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] list_object_classes -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FSlot = NeoLuaStr::ToFString(SlotName);
			const FString FFilter = NeoLuaStr::ToFStringOpt(Filter);
			LuaAssetHelper::FObjectSlotInfo Slot;
			FString Error;
			if (!LuaAssetHelper::ResolveObjectSlot(Asset, FSlot, Slot, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list_object_classes(\"%s\") -> %s"), *FSlot, *Error));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			int32 Index = 1;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!LuaAssetHelper::IsClassUsableForInlineObject(Class)) continue;
				if (Slot.BaseClass && !Class->IsChildOf(Slot.BaseClass)) continue;
				if (!LuaAssetHelper::IsObjectSlotInstanced(Slot.Property, Slot.ObjectProperty)
					&& !Class->HasAnyClassFlags(CLASS_EditInlineNew | CLASS_DefaultToInstanced))
				{
					continue;
				}
				if (!FFilter.IsEmpty()
					&& !Class->GetName().Contains(FFilter, ESearchCase::IgnoreCase)
					&& !Class->GetPathName().Contains(FFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}

				sol::table Entry = Lua.create_table();
				Entry["class_name"] = TCHAR_TO_UTF8(*Class->GetName());
				Entry["class_path"] = TCHAR_TO_UTF8(*Class->GetPathName());
				Entry["base_class"] = Slot.BaseClass ? TCHAR_TO_UTF8(*Slot.BaseClass->GetName()) : "";
				Result[Index++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_object_classes(\"%s\"%s) -> %d classes"),
				*FSlot, FFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", \"%s\""), *FFilter), Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:add_object(array_slot, class_name, params?) -> subobject handle
		// ----------------------------------------------------------------
		AssetObj.set_function("add_object", [WeakAsset, &Session](sol::table,
			const std::string& SlotName, const std::string& ClassName,
			sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] add_object -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FSlot = NeoLuaStr::ToFString(SlotName);
			const FString FClassName = NeoLuaStr::ToFString(ClassName);
			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("add_object"), Session, FSlot))
			{
				return sol::lua_nil;
			}

			LuaAssetHelper::FObjectSlotInfo Slot;
			FString Error;
			if (!LuaAssetHelper::ResolveObjectSlot(Asset, FSlot, Slot, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_object(\"%s\") -> %s"), *FSlot, *Error));
				return sol::lua_nil;
			}
			if (Slot.Kind != LuaAssetHelper::EObjectSlotKind::Array)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_object(\"%s\") -> slot is not an array; use set_object()"), *FSlot));
				return sol::lua_nil;
			}

			UClass* SubClass = LuaAssetHelper::ResolveInlineObjectClass(FClassName, Slot.BaseClass, Error);
			if (!SubClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_object(\"%s\", \"%s\") -> %s"), *FSlot, *FClassName, *Error));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Slot.Property);
			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);

			Asset->Modify();
			Asset->PreEditChange(Slot.Property);

			UObject* NewObj = NewObject<UObject>(Asset, SubClass, NAME_None, RF_Transactional);
			if (!NewObj)
			{
				FPropertyChangedEvent FailEvent(Slot.Property, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(FailEvent);
				Session.Log(FString::Printf(TEXT("[FAIL] add_object(\"%s\", \"%s\") -> NewObject failed"), *FSlot, *FClassName));
				return sol::lua_nil;
			}

			NewObj->Modify();
			if (Params.has_value())
			{
				TArray<FString> Warnings;
				if (!NeoLuaProperty::ApplyTable(NewObj, Params.value(), Error, &Warnings))
				{
					FPropertyChangedEvent FailEvent(Slot.Property, EPropertyChangeType::ValueSet);
					Asset->PostEditChangeProperty(FailEvent);
					Session.Log(FString::Printf(TEXT("[FAIL] add_object(\"%s\", \"%s\") -> %s"), *FSlot, *FClassName, *Error));
					return sol::lua_nil;
				}
				for (const FString& Warning : Warnings)
				{
					Session.Log(FString::Printf(TEXT("[WARN] add_object(\"%s\") -> %s"), *FSlot, *Warning));
				}
			}

			const int32 NewIndex = ArrayHelper.AddValue();
			Slot.ObjectProperty->SetObjectPropertyValue(ArrayHelper.GetRawPtr(NewIndex), NewObj);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Slot.Property, EPropertyChangeType::ArrayAdd);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, Slot.Property);

			Session.Log(FString::Printf(TEXT("[OK] add_object(\"%s\", \"%s\") -> index %d"),
				*FSlot, *SubClass->GetName(), NewIndex + 1));
			return sol::make_object(Lua, BuildSubobjectHandle(Lua, Session, Asset, NewObj, FString::Printf(TEXT("object:%s[%d]"), *FSlot, NewIndex + 1)));
		});

		// ----------------------------------------------------------------
		// asset:set_object(object_slot, class_name, params?) -> subobject handle
		// ----------------------------------------------------------------
		AssetObj.set_function("set_object", [WeakAsset, &Session](sol::table,
			const std::string& SlotName, const std::string& ClassName,
			sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] set_object -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FSlot = NeoLuaStr::ToFString(SlotName);
			const FString FClassName = NeoLuaStr::ToFString(ClassName);
			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("set_object"), Session, FSlot))
			{
				return sol::lua_nil;
			}

			LuaAssetHelper::FObjectSlotInfo Slot;
			FString Error;
			if (!LuaAssetHelper::ResolveObjectSlot(Asset, FSlot, Slot, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_object(\"%s\") -> %s"), *FSlot, *Error));
				return sol::lua_nil;
			}
			if (Slot.Kind != LuaAssetHelper::EObjectSlotKind::Object)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_object(\"%s\") -> slot is an array; use add_object()"), *FSlot));
				return sol::lua_nil;
			}

			UClass* SubClass = LuaAssetHelper::ResolveInlineObjectClass(FClassName, Slot.BaseClass, Error);
			if (!SubClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_object(\"%s\", \"%s\") -> %s"), *FSlot, *FClassName, *Error));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(Slot.Property);

			UObject* NewObj = NewObject<UObject>(Asset, SubClass, NAME_None, RF_Transactional);
			if (!NewObj)
			{
				FPropertyChangedEvent FailEvent(Slot.Property, EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(FailEvent);
				Session.Log(FString::Printf(TEXT("[FAIL] set_object(\"%s\", \"%s\") -> NewObject failed"), *FSlot, *FClassName));
				return sol::lua_nil;
			}

			NewObj->Modify();
			if (Params.has_value())
			{
				TArray<FString> Warnings;
				if (!NeoLuaProperty::ApplyTable(NewObj, Params.value(), Error, &Warnings))
				{
					FPropertyChangedEvent FailEvent(Slot.Property, EPropertyChangeType::ValueSet);
					Asset->PostEditChangeProperty(FailEvent);
					Session.Log(FString::Printf(TEXT("[FAIL] set_object(\"%s\", \"%s\") -> %s"), *FSlot, *FClassName, *Error));
					return sol::lua_nil;
				}
				for (const FString& Warning : Warnings)
				{
					Session.Log(FString::Printf(TEXT("[WARN] set_object(\"%s\") -> %s"), *FSlot, *Warning));
				}
			}

			void* PropContainer = Slot.Property->ContainerPtrToValuePtr<void>(Asset);
			Slot.ObjectProperty->SetObjectPropertyValue(PropContainer, NewObj);

			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Slot.Property, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, Slot.Property);

			Session.Log(FString::Printf(TEXT("[OK] set_object(\"%s\", \"%s\")"), *FSlot, *SubClass->GetName()));
			return sol::make_object(Lua, BuildSubobjectHandle(Lua, Session, Asset, NewObj, FString::Printf(TEXT("object:%s"), *FSlot)));
		});

		// ----------------------------------------------------------------
		// asset:get_object(slot, index?) -> subobject handle
		// ----------------------------------------------------------------
		AssetObj.set_function("get_object", [WeakAsset, &Session](sol::table,
			const std::string& SlotName, sol::optional<int> Index,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] get_object -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FSlot = NeoLuaStr::ToFString(SlotName);
			LuaAssetHelper::FObjectSlotInfo Slot;
			FString Error;
			if (!LuaAssetHelper::ResolveObjectSlot(Asset, FSlot, Slot, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_object(\"%s\") -> %s"), *FSlot, *Error));
				return sol::lua_nil;
			}

			const int32 OneBasedIndex = Index.value_or(1);
			UObject* SubObject = LuaAssetHelper::GetObjectFromSlot(Asset, Slot, OneBasedIndex, Error);
			if (!SubObject)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_object(\"%s\") -> %s"), *FSlot, Error.IsEmpty() ? TEXT("slot is empty") : *Error));
				return sol::lua_nil;
			}

			const FString Label = Slot.Kind == LuaAssetHelper::EObjectSlotKind::Array
				? FString::Printf(TEXT("object:%s[%d]"), *FSlot, OneBasedIndex)
				: FString::Printf(TEXT("object:%s"), *FSlot);
			Session.Log(FString::Printf(TEXT("[OK] get_object(\"%s\") -> %s"), *FSlot, *SubObject->GetClass()->GetName()));
			return sol::make_object(Lua, BuildSubobjectHandle(Lua, Session, Asset, SubObject, Label));
		});

		// ----------------------------------------------------------------
		// asset:remove_object(array_slot, index) / asset:clear_object(object_slot)
		// ----------------------------------------------------------------
		AssetObj.set_function("remove_object", [WeakAsset, &Session](sol::table,
			const std::string& SlotName, int Index,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] remove_object -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FSlot = NeoLuaStr::ToFString(SlotName);
			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("remove_object"), Session, FSlot))
			{
				return sol::lua_nil;
			}

			LuaAssetHelper::FObjectSlotInfo Slot;
			FString Error;
			if (!LuaAssetHelper::ResolveObjectSlot(Asset, FSlot, Slot, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_object(\"%s\") -> %s"), *FSlot, *Error));
				return sol::lua_nil;
			}
			if (Slot.Kind != LuaAssetHelper::EObjectSlotKind::Array)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_object(\"%s\") -> slot is not an array; use clear_object()"), *FSlot));
				return sol::lua_nil;
			}

			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Slot.Property);
			void* ArrayContainer = ArrayProp->ContainerPtrToValuePtr<void>(Asset);
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayContainer);
			const int32 ZeroIndex = Index - 1;
			if (ZeroIndex < 0 || ZeroIndex >= ArrayHelper.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_object(\"%s\", %d) -> index out of range (count=%d)"),
					*FSlot, Index, ArrayHelper.Num()));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(Slot.Property);
			ArrayHelper.RemoveValues(ZeroIndex, 1);
			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Slot.Property, EPropertyChangeType::ArrayRemove);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, Slot.Property);

			Session.Log(FString::Printf(TEXT("[OK] remove_object(\"%s\", %d) -> count = %d"),
				*FSlot, Index, ArrayHelper.Num()));
			return sol::make_object(Lua, true);
		});

		AssetObj.set_function("clear_object", [WeakAsset, &Session](sol::table,
			const std::string& SlotName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] clear_object -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FString FSlot = NeoLuaStr::ToFString(SlotName);
			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("clear_object"), Session, FSlot))
			{
				return sol::lua_nil;
			}

			LuaAssetHelper::FObjectSlotInfo Slot;
			FString Error;
			if (!LuaAssetHelper::ResolveObjectSlot(Asset, FSlot, Slot, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] clear_object(\"%s\") -> %s"), *FSlot, *Error));
				return sol::lua_nil;
			}
			if (Slot.Kind != LuaAssetHelper::EObjectSlotKind::Object)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] clear_object(\"%s\") -> slot is an array; use remove_object()"), *FSlot));
				return sol::lua_nil;
			}

			Asset->Modify();
			Asset->PreEditChange(Slot.Property);
			void* PropContainer = Slot.Property->ContainerPtrToValuePtr<void>(Asset);
			Slot.ObjectProperty->SetObjectPropertyValue(PropContainer, nullptr);
			Asset->MarkPackageDirty();
			FPropertyChangedEvent Event(Slot.Property, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
			LuaAssetHelper::PostEdit(Asset, Slot.Property);

			Session.Log(FString::Printf(TEXT("[OK] clear_object(\"%s\")"), *FSlot));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:list_user_data() -> table of attached AssetUserData
		// ----------------------------------------------------------------
		AssetObj.set_function("list_user_data", [WeakAsset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] list_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[OK] list_user_data() -> asset does not support AssetUserData"));
				return Lua.create_table();
			}

			const TArray<UAssetUserData*>* UserDataArray = UDInterface->GetAssetUserDataArray();
			if (!UserDataArray || UserDataArray->Num() == 0)
			{
				Session.Log(TEXT("[OK] list_user_data() -> 0 items"));
				return Lua.create_table();
			}

			sol::table Result = Lua.create_table();
			int32 Index = 1;

			for (UAssetUserData* UD : *UserDataArray)
			{
				if (!UD) continue;

				sol::table Entry = Lua.create_table();
				Entry["index"] = Index;
				Entry["class_name"] = TCHAR_TO_UTF8(*LuaAssetHelper::GetUserDataClassName(UD));
				Entry["class_path"] = TCHAR_TO_UTF8(*UD->GetClass()->GetPathName());

				// Count properties
				int32 PropCount = 0;
				for (TFieldIterator<FProperty> PropIt(UD->GetClass()); PropIt; ++PropIt)
				{
					if (!(*PropIt)->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
						PropCount++;
				}
				Entry["properties"] = PropCount;

				Result[Index++] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_user_data() -> %d items"), Index - 1));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:get_user_data(class_name_or_index) -> sub-object table with get/set/list_properties
		// ----------------------------------------------------------------
		AssetObj.set_function("get_user_data", [WeakAsset, &Session](sol::table /*self*/,
			sol::object Identifier, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] get_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[FAIL] get_user_data -> asset does not support AssetUserData"));
				return sol::lua_nil;
			}

			UAssetUserData* FoundUD = nullptr;

			if (Identifier.is<int>())
			{
				// By 1-based index
				int32 ZeroIndex = Identifier.as<int>() - 1;
				const TArray<UAssetUserData*>* Arr = UDInterface->GetAssetUserDataArray();
				if (Arr && ZeroIndex >= 0 && ZeroIndex < Arr->Num())
				{
					FoundUD = (*Arr)[ZeroIndex];
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_user_data(%d) -> index out of range"), ZeroIndex + 1));
					return sol::lua_nil;
				}
			}
			else if (Identifier.is<std::string>())
			{
				// By class name
				FString ClassName = NeoLuaStr::ToFString(Identifier.as<std::string>());
				const TArray<UAssetUserData*>* Arr = UDInterface->GetAssetUserDataArray();
				if (Arr)
				{
					for (UAssetUserData* UD : *Arr)
					{
						if (UD && (UD->GetClass()->GetName().Contains(ClassName, ESearchCase::IgnoreCase)))
						{
							FoundUD = UD;
							break;
						}
					}
				}
				if (!FoundUD)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] get_user_data(\"%s\") -> not found"), *ClassName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] get_user_data -> pass class name (string) or index (number)"));
				return sol::lua_nil;
			}

			if (!FoundUD)
			{
				Session.Log(TEXT("[FAIL] get_user_data -> user data is null"));
				return sol::lua_nil;
			}

			// Build a sub-object table with get/set/list_properties — reuse the same reflection pattern
			sol::table UDObj = Lua.create_table();
			UDObj["class_name"] = TCHAR_TO_UTF8(*LuaAssetHelper::GetUserDataClassName(FoundUD));
			UDObj["class_path"] = TCHAR_TO_UTF8(*FoundUD->GetClass()->GetPathName());

			// GC-safe weak references for nested lambdas
			TWeakObjectPtr<UAssetUserData> WeakFoundUD(FoundUD);

			// get(property)
			UDObj.set_function("get", [WeakFoundUD, &Session](sol::table /*self*/,
				const std::string& PropertyName, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				FString FProp = NeoLuaStr::ToFString(PropertyName);

				UAssetUserData* FoundUD = WeakFoundUD.Get();
				if (!FoundUD)
				{
					Session.Log(TEXT("[FAIL] user_data:get -> no longer valid"));
					return sol::lua_nil;
				}

				LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(FoundUD, FProp);
				if (!Resolved.Property)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] user_data:get(\"%s\") -> property not found"), *FProp));
					return sol::lua_nil;
				}

				FString Value = LuaAssetHelper::GetPropertyValue(FoundUD, Resolved.Property, Resolved.ContainerPtr);
				return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Value)));
			});

			// set(property, value)
			UDObj.set_function("set", [WeakFoundUD, WeakAsset, &Session](sol::table /*self*/,
				const std::string& PropertyName, const std::string& Value, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				FString FProp = NeoLuaStr::ToFString(PropertyName);
				FString FValue = NeoLuaStr::ToFString(Value);

				UAssetUserData* FoundUD = WeakFoundUD.Get();
				UObject* Asset = WeakAsset.Get();
				if (!FoundUD || !Asset)
				{
					Session.Log(TEXT("[FAIL] user_data:set -> asset or user data no longer valid"));
					return sol::lua_nil;
				}

				if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("user_data:set"), Session, FProp))
				{
					return sol::lua_nil;
				}

				LuaAssetHelper::FResolvedProperty Resolved = LuaAssetHelper::ResolvePropertyPath(FoundUD, FProp);
				if (!Resolved.Property)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] user_data:set(\"%s\") -> property not found"), *FProp));
					return sol::lua_nil;
				}

				Asset->Modify();
				FoundUD->PreEditChange(Resolved.Property);

				FString Error;
				if (!LuaAssetHelper::SetPropertyValue(FoundUD, Resolved.Property, Resolved.ContainerPtr, FValue, Error))
				{
					FPropertyChangedEvent FailEvent(Resolved.Property, EPropertyChangeType::ValueSet);
					FoundUD->PostEditChangeProperty(FailEvent);
					Session.Log(FString::Printf(TEXT("[FAIL] user_data:set(\"%s\", \"%s\") -> %s"), *FProp, *FValue, *Error));
					return sol::lua_nil;
				}

				Asset->MarkPackageDirty();
				FPropertyChangedEvent SuccessEvent(Resolved.Property, EPropertyChangeType::ValueSet);
				FoundUD->PostEditChangeProperty(SuccessEvent);

				Session.Log(FString::Printf(TEXT("[OK] user_data:set(\"%s\") = \"%s\""), *FProp, *FValue));
				return sol::make_object(Lua, true);
			});

			// list_properties(filter?, all?)
			UDObj.set_function("list_properties", [WeakFoundUD, &Session](sol::table /*self*/,
				sol::optional<std::string> Filter, sol::optional<bool> IncludeAll,
				sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				UAssetUserData* FoundUD = WeakFoundUD.Get();
				if (!FoundUD)
				{
					Session.Log(TEXT("[FAIL] user_data:list_properties -> no longer valid"));
					return sol::lua_nil;
				}

				FString FFilter = NeoLuaStr::ToFStringOpt(Filter);
				bool bAll = IncludeAll.value_or(true); // Default to all for user data (less noise than main asset)

				sol::table Result = Lua.create_table();
				int32 Index = 1;

				for (TFieldIterator<FProperty> PropIt(FoundUD->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Property = *PropIt;
					if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
					if (!bAll && !Property->HasAnyPropertyFlags(CPF_Edit)) continue;

					FString Name = Property->GetName();
					if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

					FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
					FString Value = NeoStackToolUtils::GetPropertyValueAsString(FoundUD, Property, FoundUD);
					FString Category = Property->GetMetaData(TEXT("Category"));
					if (Category.IsEmpty()) Category = TEXT("Default");

					if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Name);
					Entry["type"] = TCHAR_TO_UTF8(*Type);
					Entry["value"] = TCHAR_TO_UTF8(*Value);
					Entry["category"] = TCHAR_TO_UTF8(*Category);
					Result[Index++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] user_data:list_properties(%s) -> %d properties"),
					FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
				return Result;
			});

			Session.Log(FString::Printf(TEXT("[OK] get_user_data(\"%s\") -> sub-object with get/set/list_properties"),
				*LuaAssetHelper::GetUserDataClassName(FoundUD)));
			return UDObj;
		});

		// ----------------------------------------------------------------
		// asset:add_user_data(class_name) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("add_user_data", [WeakAsset, &Session](sol::table /*self*/,
			const std::string& ClassName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FClassName = NeoLuaStr::ToFString(ClassName);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] add_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("add_user_data"), Session, FClassName))
			{
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[FAIL] add_user_data -> asset does not support AssetUserData"));
				return sol::lua_nil;
			}

			UClass* UDClass = LuaAssetHelper::FindUserDataClass(FClassName);
			if (!UDClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_user_data(\"%s\") -> class not found. Must be a UAssetUserData subclass."), *FClassName));
				return sol::lua_nil;
			}

			bool bWasAlreadyAttached = (UDInterface->GetAssetUserDataOfClass(UDClass) != nullptr);

			Asset->Modify();
			UAssetUserData* NewUD = NewObject<UAssetUserData>(Asset, UDClass);
			UDInterface->AddAssetUserData(NewUD);
			Asset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add_user_data(\"%s\") -> %s %s"),
				*UDClass->GetName(), bWasAlreadyAttached ? TEXT("replaced on") : TEXT("attached to"),
				*Asset->GetName()));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:remove_user_data(class_name_or_index) -> true/nil
		// ----------------------------------------------------------------
		AssetObj.set_function("remove_user_data", [WeakAsset, &Session](sol::table /*self*/,
			sol::object Identifier, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] remove_user_data -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("remove_user_data"), Session))
			{
				return sol::lua_nil;
			}

			IInterface_AssetUserData* UDInterface = LuaAssetHelper::GetAssetUserDataInterface(Asset);
			if (!UDInterface)
			{
				Session.Log(TEXT("[FAIL] remove_user_data -> asset does not support AssetUserData"));
				return sol::lua_nil;
			}

			UClass* ClassToRemove = nullptr;

			if (Identifier.is<int>())
			{
				int32 ZeroIndex = Identifier.as<int>() - 1;
				const TArray<UAssetUserData*>* Arr = UDInterface->GetAssetUserDataArray();
				if (Arr && ZeroIndex >= 0 && ZeroIndex < Arr->Num() && (*Arr)[ZeroIndex])
				{
					ClassToRemove = (*Arr)[ZeroIndex]->GetClass();
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove_user_data(%d) -> index out of range"), ZeroIndex + 1));
					return sol::lua_nil;
				}
			}
			else if (Identifier.is<std::string>())
			{
				FString ClassName = NeoLuaStr::ToFString(Identifier.as<std::string>());
				ClassToRemove = LuaAssetHelper::FindUserDataClass(ClassName);
				if (!ClassToRemove)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove_user_data(\"%s\") -> class not found"), *ClassName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] remove_user_data -> pass class name (string) or index (number)"));
				return sol::lua_nil;
			}

			Asset->Modify();
			UDInterface->RemoveUserDataOfClass(ClassToRemove);
			Asset->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove_user_data(\"%s\") -> removed from %s"),
				*ClassToRemove->GetName(), *Asset->GetName()));
			return sol::make_object(Lua, true);
		});

		// ----------------------------------------------------------------
		// asset:help() -> string
		// ----------------------------------------------------------------
		AssetObj.set_function("help", [&Session](sol::table self, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string Out = "=== Generic methods (all assets) ===\n"
				"  get(property) — read property (dot-notation: \"Struct.Field[0].Sub\", map: \"Map{key}.Sub\")\n"
				"  set(property, value) — set property via ImportText\n"
				"  list_properties(filter?, all?) — list editable properties\n"
				"  property_meta(property) — metadata (ClampMin/Max, UIMin/Max, ToolTip, container info, struct fields)\n"
				"  array_add(property, value?) / array_remove(property, index) / array_count(property)\n"
				"  configure_at(array_prop, name_or_index, {field=value, ...}) — name: matches GetFName, Name, ParameterName, SocketName, BoneName, GroupName, VirtualBoneName, CurveName, MarkerName, EventName. Index: 1-based. Applies props via ApplyTable (UObject element) or ApplyTableToStruct (struct element).\n"
				"  map_get(prop,key) / map_set(prop,key,val) / map_remove(prop,key) / map_count(prop) / map_keys(prop)\n"
				"  set_add(prop,value) / set_remove(prop,value) / set_count(prop) / set_values(prop)\n"
				"  list_object_slots(filter?) / list_object_classes(slot, filter?) — discover instanced/edit-inline UObject slots\n"
				"  add_object(array_slot, class, params?) / remove_object(array_slot, index) — create/remove owned inline objects\n"
				"  set_object(object_slot, class, params?) / get_object(slot, index?) / clear_object(object_slot) — single inline object slots\n"
				"  save() — save to disk\n"
				"  info() — structured read of asset contents\n"
				"\n=== AssetUserData methods (StaticMesh, SkeletalMesh, Texture, AnimSequence, etc.) ===\n"
				"  list_user_data() — list all attached user data with class names\n"
				"  get_user_data(class_or_index) — get sub-object with get/set/list_properties\n"
				"  add_user_data(class_name) — attach new user data instance\n"
				"  remove_user_data(class_or_index) — remove attached user data\n";

			sol::optional<std::string> AssetHelp = self.get<sol::optional<std::string>>("_help_text");
			if (AssetHelp.has_value())
			{
				Out += "\n=== Asset-specific methods ===\n" + AssetHelp.value();
			}
			else
			{
				Out += "\nNo asset-specific methods. Use get/set/list_properties for any property.\n";
			}

			Session.Log(FString::Printf(TEXT("[OK] help()\n%s"), UTF8_TO_TCHAR(Out.c_str())));
			return sol::make_object(Lua, Out);
		});

		// ----------------------------------------------------------------
		// asset:info() -> table (default: type + property summary, enrichments override)
		// ----------------------------------------------------------------
		AssetObj.set_function("info", [WeakAsset, FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["type"] = TCHAR_TO_UTF8(*Asset->GetClass()->GetName());
			Result["path"] = TCHAR_TO_UTF8(*FPath);

			// Count editable properties
			int32 EditableCount = 0, TotalCount = 0;
			for (TFieldIterator<FProperty> PropIt(Asset->GetClass()); PropIt; ++PropIt)
			{
				if ((*PropIt)->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;
				TotalCount++;
				if ((*PropIt)->HasAnyPropertyFlags(CPF_Edit)) EditableCount++;
			}
			Result["editable_properties"] = EditableCount;
			Result["total_properties"] = TotalCount;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s (%d editable, %d total properties)"),
				*Asset->GetClass()->GetName(), EditableCount, TotalCount));
			return Result;
		});

		// ----------------------------------------------------------------
		// asset:save() -> true
		// ----------------------------------------------------------------
		AssetObj.set_function("save", [WeakAsset, FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UObject* Asset = WeakAsset.Get();
			if (!Asset)
			{
				Session.Log(TEXT("[FAIL] save -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (LuaAssetHelper::RefuseUnsafeMaterialMutation(Asset, TEXT("save"), Session))
			{
				return sol::lua_nil;
			}

			UPackage* Package = Asset->GetOutermost();
			if (!Package)
			{
				Session.Log(TEXT("[FAIL] save -> no package"));
				return sol::lua_nil;
			}

			// Material: push preview→source before save.
			//
			// LuaGraphResolver operates on the editor's PREVIEW material — a transient
			// UPreviewMaterial duplicate (see LuaGraphResolver.h:124-138). All add_node /
			// connect / set_node_property edits land on the preview. Without this push,
			// SaveLoadedAsset persists an empty source asset and a programmatically built
			// material renders solid black despite [OK] reports from every step.
			//
			// This mirrors the core of FMaterialEditor::UpdateOriginalMaterial at
			// MaterialEditor.cpp:2848: a StaticDuplicateObject from preview onto the
			// source's outer+name+class, then a fresh recompile request. We skip the
			// editor's thumbnail / metadata restoration — for programmatically authored
			// materials those weren't customized in the first place. SaveAsset_Execute is
			// protected on FAssetEditorToolkit so we can't reach it; doing the duplicate
			// inline is the only way without subclass-friend hacks.
			if (UMaterial* SrcMat = Cast<UMaterial>(Asset))
			{
				if (TSharedPtr<IToolkit> Toolkit = FToolkitManager::Get().FindEditorForAsset(Asset))
				{
					const TArray<UObject*>* Edited = Toolkit->GetObjectsCurrentlyBeingEdited();
					UMaterial* PreviewMat = nullptr;
					if (Edited)
					{
						for (UObject* Obj : *Edited)
						{
							UMaterial* CandidateMat = Cast<UMaterial>(Obj);
							if (CandidateMat && CandidateMat->GetOutermost() == GetTransientPackage())
							{
								PreviewMat = CandidateMat;
								break;
							}
						}
					}

					if (PreviewMat)
					{
						// One last sync — guarantees pin LinkedTo state on the preview's
						// graph is reflected in FExpressionInput.Expression slots before
						// we duplicate. Belt-and-braces with the connect-time sync.
						if (PreviewMat->MaterialGraph)
						{
							PreviewMat->MaterialGraph->LinkMaterialExpressionsFromGraph();
						}

						UPackage* SrcOuter = SrcMat->GetOutermost();
						FName SrcName = SrcMat->GetFName();
						UClass* SrcClass = SrcMat->GetClass();

						// StaticDuplicateObject with same outer+name renames the existing
						// source out of the way and creates a new object that takes the
						// name. The new object is what survives on disk.
						UMaterial* NewSrc = (UMaterial*)StaticDuplicateObject(
							PreviewMat, SrcOuter, SrcName, RF_AllFlags, SrcClass);
						if (NewSrc)
						{
							NewSrc->SetFlags(RF_Standalone);
							NewSrc->bUsedAsSpecialEngineMaterial = PreviewMat->bUsedAsSpecialEngineMaterial;

							// Update locals so the SaveLoadedAsset call below operates on
							// the freshly-duplicated source, not the renamed husk.
							Asset = NewSrc;
							Package = NewSrc->GetOutermost();
						}
					}
				}
			}

			FString PackageFilename;
			if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
			{
				PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(),
					FPackageName::GetAssetPackageExtension());
			}

			// Use the editor subsystem — raw UPackage::Save skipped SCC checkout and LFS read-only clearing, and could drop files silently.
			UEditorAssetSubsystem* AssetSub = NeoLuaSubsystem::GetEditor<UEditorAssetSubsystem>();
			if (!AssetSub)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> EditorAssetSubsystem unavailable"), *FPath));
				return sol::make_object(Lua, false);
			}

			TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
			const bool bApiOk = AssetSub->SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/false);

			// Verify the file exists on disk — the API can return success while the .uasset is missing (LFS / SCC edge cases).
			const bool bOnDisk = !PackageFilename.IsEmpty() && FPaths::FileExists(PackageFilename);
			const bool bSuccess = bApiOk && bOnDisk;

			if (bSuccess)
			{
				Session.Log(FString::Printf(TEXT("[OK] save(\"%s\")"), *FPath));
			}
			else if (bApiOk && !bOnDisk)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> API reported success but file is missing on disk (\"%s\") — check for LFS read-only / source control locks"),
					*FPath, *PackageFilename));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> save failed (check log for details)"), *FPath));
			}

			return sol::make_object(Lua, bSuccess);
		});

		// ----------------------------------------------------------------
		// Enrich with domain-specific methods based on asset type
		// Order matters: check UAnimMontage before UAnimSequenceBase (inheritance)
		// ----------------------------------------------------------------
		auto TryEnrich = [&](const char* FuncName)
		{
			sol::protected_function Fn = LuaView[FuncName];
			if (!Fn.valid()) return;

			sol::protected_function_result EnrichResult = Fn(AssetObj);
			if (!EnrichResult.valid())
			{
				sol::error Error = EnrichResult;
				Session.Log(FString::Printf(TEXT("[FAIL] %s -> enrichment error: %s"),
					UTF8_TO_TCHAR(FuncName), UTF8_TO_TCHAR(Error.what())));
			}
		};

		if (Asset->IsA<UAnimMontage>())
			TryEnrich("_enrich_montage");
		else if (Asset->IsA<UAnimComposite>())
			TryEnrich("_enrich_anim_composite");
		else if (Asset->IsA<UAnimSequence>())
			TryEnrich("_enrich_anim_sequence");

		// Get class name once for runtime type checks (plugins no longer linked by core)
		const FString ClassName = Asset->GetClass()->GetName();

		const FLuaAssetCapability* ExtensionCapability = FLuaAssetCapabilityRegistry::Get().FindOwner(Asset);
		if (ExtensionCapability && !ExtensionCapability->EnrichFunctionName.IsEmpty())
		{
			FTCHARToUTF8 EnrichNameUtf8(*ExtensionCapability->EnrichFunctionName);
			TryEnrich(EnrichNameUtf8.Get());
		}
		else if (Asset->IsA<USkeleton>())
			TryEnrich("_enrich_skeleton");
		else if (Asset->IsA<UPhysicsAsset>())
			TryEnrich("_enrich_physics_asset");
		else if (Asset->IsA<UBehaviorTree>() || Asset->IsA<UBlackboardData>())
			TryEnrich("_enrich_behavior_tree");
		else if (Asset->IsA<ULevelSequence>())
			TryEnrich("_enrich_sequencer");
		else if (Asset->IsA<UPoseAsset>())
			TryEnrich("_enrich_pose_asset");
		else if (Asset->IsA<UBlendSpace>())
			TryEnrich("_enrich_blend_space");
		else if (Asset->IsA<UUserDefinedStruct>())
			TryEnrich("_enrich_user_defined_struct");
		else if (Asset->IsA<UUserDefinedEnum>())
			TryEnrich("_enrich_user_defined_enum");
		else if (Asset->IsA<UDataTable>())
			TryEnrich("_enrich_data_table");
		else if (Asset->IsA<UStringTable>())
			TryEnrich("_enrich_string_table");
		else if (Asset->IsA<UCurveTable>())
			TryEnrich("_enrich_curve_table");
		else if (Asset->IsA<UCurveBase>())
			TryEnrich("_enrich_curve");
		else if (Asset->IsA<UMaterialFunction>())
			TryEnrich("_enrich_material_function");
		else if (Asset->IsA<UMaterialInstance>())
			TryEnrich("_enrich_material_instance");
		else if (Asset->IsA<UStaticMesh>())
			TryEnrich("_enrich_static_mesh");
		else if (Asset->IsA<USkeletalMesh>())
			TryEnrich("_enrich_skeletal_mesh");
		else if (Asset->IsA<UTexture>())
			TryEnrich("_enrich_texture");
		else if (Asset->IsA<UPhysicalMaterial>())
			TryEnrich("_enrich_physical_material");
		else if (Asset->IsA<UMaterialParameterCollection>())
			TryEnrich("_enrich_material_param_collection");
		else if (Asset->IsA<USoundWave>())
			TryEnrich("_enrich_sound_wave");
		else if (Asset->IsA<USoundClass>())
			TryEnrich("_enrich_sound_class");
		else if (Asset->IsA<USoundAttenuation>())
			TryEnrich("_enrich_sound_attenuation");
		else if (Asset->IsA<USoundCue>())
			TryEnrich("_enrich_sound_cue");
		else if (Asset->IsA<USoundConcurrency>())
			TryEnrich("_enrich_sound_concurrency");
		else if (Asset->IsA<USoundMix>())
			TryEnrich("_enrich_sound_mix");
		else if (Asset->IsA<URuntimeVirtualTexture>())
			TryEnrich("_enrich_runtime_virtual_texture");
		else if (Asset->IsA<UFoliageType>())
			TryEnrich("_enrich_foliage_type");
		else if (Asset->IsA<ULandscapeGrassType>())
			TryEnrich("_enrich_landscape_grass_type");
		else if (Asset->IsA<UDialogueVoice>())
			TryEnrich("_enrich_dialogue_voice");
		else if (Asset->IsA<UDialogueWave>())
			TryEnrich("_enrich_dialogue_wave");

		return AssetObj;
	});

	Lua.set_function("open_editor", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		const FString FPath = NeoLuaAsset::NormalizePath(NeoLuaStr::ToFString(Path));

		UObject* Asset = NeoLuaAsset::ResolveWithRegistry(FPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] open_editor(\"%s\") -> asset not found"), *FPath));
			return sol::lua_nil;
		}

		IAssetEditorInstance* EditorInstance = NeoLuaEditor::OpenAssetEditorAndWait(Asset, /*MaxTickSteps*/ 60, /*bFocusIfOpen*/ true);
		if (!EditorInstance)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] open_editor(\"%s\") -> editor failed to open within tick budget"), *FPath));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] open_editor(\"%s\")"), *FPath));
		return sol::make_object(LuaView, true);
	});
}

REGISTER_LUA_BINDING(OpenAsset, OpenAssetDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindOpenAsset(Lua, Session);
});
