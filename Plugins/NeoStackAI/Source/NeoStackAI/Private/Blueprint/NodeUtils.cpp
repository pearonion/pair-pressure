#include "Blueprint/NodeUtils.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "BlueprintNodeSignature.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintActionFilter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "Lua/LuaEditorActions.h"
#include "BehaviorTree/BehaviorTree.h"
#if WITH_EQS
#include "EnvironmentQuery/EnvQuery.h"
#endif
#include "Materials/Material.h"
#include "Sound/SoundCue.h"

void NeoNodes::EnsureEditorOpenForSchema(UEdGraph* Graph)
{
	if (!Graph || !GEditor) return;
	UObject* Outer = Graph->GetOuter();
	if (!Outer) return;

	// Check if graph's owning asset needs its editor open for schema/notification init.
	// ControlRig: class name check since main module doesn't link ControlRig headers.
	FString OuterClassName = Outer->GetClass()->GetName();
	bool bNeedsEditor = Cast<UBehaviorTree>(Outer) != nullptr
#if WITH_EQS
		|| Cast<UEnvQuery>(Outer) != nullptr
#endif
		|| Cast<UMaterial>(Outer) != nullptr
		|| Cast<USoundCue>(Outer) != nullptr
		|| OuterClassName.Contains(TEXT("ControlRigBlueprint"))
		|| OuterClassName.Contains(TEXT("RigVMBlueprint"));
	if (!bNeedsEditor) return;

	NeoLuaEditor::OpenAssetEditorAndWait(Outer);
}

// Detects spawners whose owning class was recompiled (live coding / hot reload).
// Calling PrimeDefaultUiSpec or GetTemplateNode on these crashes.
static bool IsStaleSpawner(UBlueprintNodeSpawner const* Spawner)
{
	if (!Spawner) return true;

	UClass* ClassOwner = nullptr;
	if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
	{
		const UFunction* Func = FuncSpawner->GetFunction();
		if (!Func) return true;
		ClassOwner = Func->GetOwnerClass();
	}
	else if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
	{
		const FProperty* Prop = VarSpawner->GetVarProperty();
		if (!Prop) return true;
		ClassOwner = Prop->GetOwnerClass();
	}
	if (ClassOwner)
	{
		return ClassOwner->HasAnyClassFlags(CLASS_NewerVersionExists)
			|| ClassOwner->GetOutermost() == GetTransientPackage();
	}
	return false;
}

static int32 ScoreMatch(const FString& NameLower, const FString& QueryLower)
{
	if (NameLower == QueryLower)
	{
		return 100;
	}
	if (NameLower.StartsWith(QueryLower))
	{
		return 90;
	}

	// Word boundary match — query matches a word start
	// e.g. "print" matches "Set Print String"
	int32 WordIdx = NameLower.Find(FString(TEXT(" ")) + QueryLower);
	if (WordIdx != INDEX_NONE)
	{
		return 85;
	}

	if (NameLower.Contains(QueryLower))
	{
		return 70;
	}

	// Simple fuzzy: check if all query chars appear in order
	int32 NamePos = 0;
	int32 Matched = 0;
	for (int32 i = 0; i < QueryLower.Len(); i++)
	{
		int32 Found = INDEX_NONE;
		for (int32 j = NamePos; j < NameLower.Len(); j++)
		{
			if (NameLower[j] == QueryLower[i])
			{
				Found = j;
				break;
			}
		}
		if (Found != INDEX_NONE)
		{
			NamePos = Found + 1;
			Matched++;
		}
	}

	if (Matched == QueryLower.Len())
	{
		float Ratio = (float)QueryLower.Len() / (float)NameLower.Len();
		return FMath::Clamp((int32)(50.0f * Ratio + 10.0f), 10, 55);
	}

	return 0;
}

static void ExtractPinNames(UEdGraphNode* Node, TArray<FString>& InPins, TArray<FString>& OutPins)
{
	if (!Node) return;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;

		FString PinName = Pin->GetDisplayName().IsEmpty()
			? Pin->PinName.ToString()
			: Pin->GetDisplayName().ToString();

		if (Pin->Direction == EGPD_Input)
		{
			InPins.Add(PinName);
		}
		else
		{
			OutPins.Add(PinName);
		}
	}
}

static bool ResultAlreadyContainsSpawner(const TArray<FNodeSearchResult>& Results, const UBlueprintNodeSpawner* Spawner, const FString& Name)
{
	for (const FNodeSearchResult& Existing : Results)
	{
		if (Existing.Spawner.Get() == Spawner || (Existing.Name == Name && Existing.Signature == Spawner->GetSpawnerSignature().ToString()))
		{
			return true;
		}
	}
	return false;
}

static void AddResultFromSpawner(const FString& QueryLower, const FBlueprintActionContext& Context,
	UBlueprintNodeSpawner* Spawner, TArray<FNodeSearchResult>& Results)
{
	if (!Spawner || IsStaleSpawner(Spawner))
	{
		return;
	}

	const FBlueprintActionUiSpec UiSpec = Spawner->GetUiSpec(Context, IBlueprintNodeBinder::FBindingSet());
	const FString NodeName = UiSpec.MenuName.ToString();
	if (NodeName.IsEmpty())
	{
		return;
	}

	const int32 Score = ScoreMatch(NodeName.ToLower(), QueryLower);
	if (Score <= 0 || ResultAlreadyContainsSpawner(Results, Spawner, NodeName))
	{
		return;
	}

	FNodeSearchResult Result;
	Result.Name = NodeName;
	Result.Score = Score;
	Result.Spawner = Spawner;
	Result.Signature = Spawner->GetSpawnerSignature().ToString();
	Result.Category = UiSpec.Category.ToString();
	Result.Tooltip = UiSpec.Tooltip.ToString().Left(200);

	UEdGraphNode* NodeCDO = Spawner->GetTemplateNode();
	if (NodeCDO)
	{
		ExtractPinNames(NodeCDO, Result.InputPins, Result.OutputPins);
	}

	Results.Add(MoveTemp(Result));
}

static void AddMemberSearchTargetClass(UClass* Class, TArray<UClass*>& TargetClasses)
{
	if (!Class)
	{
		return;
	}
	if (UBlueprint* ClassBlueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
	{
		if (ClassBlueprint->SkeletonGeneratedClass)
		{
			Class = ClassBlueprint->SkeletonGeneratedClass;
		}
	}
	TargetClasses.AddUnique(Class);
}

static bool IsActionOwnedByAnyClass(const UBlueprintNodeSpawner* Spawner, const TArray<UClass*>& TargetClasses)
{
	if (!Spawner || TargetClasses.Num() == 0)
	{
		return false;
	}

	UClass const* OwnerClass = nullptr;
	if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
	{
		if (const UFunction* Func = FuncSpawner->GetFunction())
		{
			OwnerClass = Func->GetOwnerClass();
		}
	}
	else if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
	{
		if (const FProperty* VarProp = VarSpawner->GetVarProperty())
		{
			OwnerClass = VarProp->GetOwnerClass();
		}
	}

	if (!OwnerClass)
	{
		return false;
	}

	OwnerClass = OwnerClass->GetAuthoritativeClass();
	for (UClass* TargetClass : TargetClasses)
	{
		if (TargetClass && TargetClass->IsChildOf(OwnerClass))
		{
			return true;
		}
	}
	return false;
}

static void AddMemberActionsForBlueprintObjectRefs(const FString& QueryLower, UBlueprint* ContextBP,
	const FBlueprintActionContext& Context, TArray<FNodeSearchResult>& Results)
{
	if (!ContextBP)
	{
		return;
	}

	TArray<UClass*> TargetClasses;
	auto AddObjectPropertyClass = [&TargetClasses](FProperty* Prop)
	{
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			AddMemberSearchTargetClass(ObjProp->PropertyClass, TargetClasses);
		}
	};

	for (const FBPVariableDescription& VarDesc : ContextBP->NewVariables)
	{
		FProperty* VarProp = nullptr;
		if (ContextBP->SkeletonGeneratedClass)
		{
			VarProp = FindFProperty<FProperty>(ContextBP->SkeletonGeneratedClass, VarDesc.VarName);
		}
		if (!VarProp && ContextBP->GeneratedClass)
		{
			VarProp = FindFProperty<FProperty>(ContextBP->GeneratedClass, VarDesc.VarName);
		}
		AddObjectPropertyClass(VarProp);
	}

	if (UClass* ContextClass = ContextBP->SkeletonGeneratedClass ? ContextBP->SkeletonGeneratedClass : ContextBP->GeneratedClass)
	{
		for (TFieldIterator<FObjectProperty> PropIt(ContextClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
		{
			AddObjectPropertyClass(*PropIt);
		}
	}

	if (TargetClasses.Num() == 0)
	{
		return;
	}

	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();
	for (auto& ActionPair : ActionDB.GetAllActions())
	{
		for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
		{
			if (IsActionOwnedByAnyClass(Spawner, TargetClasses))
			{
				AddResultFromSpawner(QueryLower, Context, Spawner, Results);
			}
		}
	}
}

static TArray<FNodeSearchResult> FindNodesWithBlueprintContext(const FString& Query, UBlueprint* ContextBP, UEdGraph* TargetGraph, int32 MaxResults, UEdGraphPin* ContextPin = nullptr)
{
	TArray<FNodeSearchResult> Results;
	if (!ContextBP) return Results;

	FString QueryLower = Query.ToLower();

	FBlueprintActionContext Context;
	Context.Blueprints.Add(ContextBP);
	if (TargetGraph)
	{
		Context.Graphs.Add(TargetGraph);
	}
	if (ContextPin)
	{
		Context.Pins.Add(ContextPin);
	}

	FBlueprintActionDatabase::Get().RefreshAssetActions(ContextBP);
	if (!ContextPin)
	{
		FKismetEditorUtilities::CompileBlueprint(ContextBP);
		FBlueprintActionDatabase::Get().RefreshAssetActions(ContextBP);
	}

	FBlueprintActionMenuBuilder MenuBuilder;
	const uint32 TargetMask = ContextPin
		? (EContextTargetFlags::TARGET_Blueprint
			| EContextTargetFlags::TARGET_SubComponents
			| EContextTargetFlags::TARGET_NodeTarget
			| EContextTargetFlags::TARGET_PinObject
			| EContextTargetFlags::TARGET_SiblingPinObjects
			| EContextTargetFlags::TARGET_BlueprintLibraries
			| EContextTargetFlags::TARGET_NonImportedTypes)
		: (EContextTargetFlags::TARGET_Blueprint | EContextTargetFlags::TARGET_BlueprintLibraries);
	FBlueprintActionMenuUtils::MakeContextMenu(Context, /*bIsContextSensitive=*/true,
		TargetMask, MenuBuilder);

	const int32 NumActions = MenuBuilder.GetNumActions();
	for (int32 i = 0; i < NumActions; i++)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		TSharedPtr<FEdGraphSchemaAction> Action = MenuBuilder.GetSchemaAction(i);
#else
		FGraphActionListBuilderBase::ActionGroup& Group = MenuBuilder.GetAction(i);
		TSharedPtr<FEdGraphSchemaAction> Action = Group.Actions.Num() > 0 ? Group.Actions[0] : nullptr;
#endif
		if (!Action.IsValid()) continue;

		FString NodeName = Action->GetMenuDescription().ToString();
		if (NodeName.IsEmpty()) continue;

		int32 Score = ScoreMatch(NodeName.ToLower(), QueryLower);
		if (Score <= 0) continue;

		FNodeSearchResult Result;
		Result.Name = NodeName;
		Result.Score = Score;
		Result.Category = Action->GetCategory().ToString();
		Result.Tooltip = Action->GetTooltipDescription().ToString().Left(200);

		UBlueprintNodeSpawner* Spawner = nullptr;
		if (Action->GetTypeId() == FBlueprintActionMenuItem::StaticGetTypeId())
		{
			FBlueprintActionMenuItem* MenuItem = static_cast<FBlueprintActionMenuItem*>(Action.Get());
			Spawner = const_cast<UBlueprintNodeSpawner*>(MenuItem->GetRawAction());
		}
		const bool bSpawnerUsable = Spawner && !IsStaleSpawner(Spawner);
		if (!TargetGraph && !bSpawnerUsable)
		{
			continue;
		}

		if (TargetGraph)
		{
			Result.SchemaAction = Action;
			Result.SchemaGraph = TargetGraph;
		}
		else if (bSpawnerUsable)
		{
			Result.Spawner = Spawner;
			Result.Signature = Spawner->GetSpawnerSignature().ToString();
		}

		if (bSpawnerUsable)
		{
			UEdGraphNode* NodeCDO = Spawner->GetTemplateNode();
			if (NodeCDO)
			{
				ExtractPinNames(NodeCDO, Result.InputPins, Result.OutputPins);
			}
		}

		Results.Add(MoveTemp(Result));
	}

	if (!ContextPin)
	{
		AddMemberActionsForBlueprintObjectRefs(QueryLower, ContextBP, Context, Results);
	}

	UClass* ContextClass = ContextBP->SkeletonGeneratedClass ? ContextBP->SkeletonGeneratedClass : ContextBP->GeneratedClass;
	if (ContextClass)
	{
		FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();
		for (auto& ActionPair : ActionDB.GetAllActions())
		{
			for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
			{
				UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner);
				if (!VarSpawner || IsStaleSpawner(VarSpawner))
				{
					continue;
				}

				const FProperty* VarProp = VarSpawner->GetVarProperty();
				if (!VarProp || VarProp->GetOwnerClass() != ContextClass)
				{
					continue;
				}

				const FBlueprintActionUiSpec UiSpec = VarSpawner->GetUiSpec(Context, IBlueprintNodeBinder::FBindingSet());
				FString NodeName = UiSpec.MenuName.ToString();
				if (NodeName.IsEmpty())
				{
					continue;
				}

				int32 Score = ScoreMatch(NodeName.ToLower(), QueryLower);
				if (Score <= 0)
				{
					continue;
				}

				bool bAlreadyAdded = false;
				for (const FNodeSearchResult& Existing : Results)
				{
					if (Existing.Spawner.Get() == VarSpawner)
					{
						bAlreadyAdded = true;
						break;
					}
				}
				if (bAlreadyAdded)
				{
					continue;
				}

				FNodeSearchResult Result;
				Result.Name = NodeName;
				Result.Score = Score;
				Result.Spawner = VarSpawner;
				Result.Signature = VarSpawner->GetSpawnerSignature().ToString();
				Result.Category = UiSpec.Category.ToString();
				Result.Tooltip = UiSpec.Tooltip.ToString().Left(200);
				Results.Add(MoveTemp(Result));
			}
		}
	}

	for (const FBPVariableDescription& VarDesc : ContextBP->NewVariables)
	{
		FProperty* VarProp = nullptr;
		if (ContextBP->SkeletonGeneratedClass)
		{
			VarProp = FindFProperty<FProperty>(ContextBP->SkeletonGeneratedClass, VarDesc.VarName);
		}
		if (!VarProp && ContextBP->GeneratedClass)
		{
			VarProp = FindFProperty<FProperty>(ContextBP->GeneratedClass, VarDesc.VarName);
		}
		if (!VarProp)
		{
			continue;
		}

		auto AddManualVariableSpawner = [&](TSubclassOf<UK2Node_Variable> NodeClass)
		{
			UBlueprintVariableNodeSpawner* VarSpawner = UBlueprintVariableNodeSpawner::CreateFromMemberOrParam(NodeClass, VarProp);
			if (!VarSpawner || IsStaleSpawner(VarSpawner))
			{
				return;
			}

			const FBlueprintActionUiSpec UiSpec = VarSpawner->GetUiSpec(Context, IBlueprintNodeBinder::FBindingSet());
			const bool bGetter = NodeClass->IsChildOf(UK2Node_VariableGet::StaticClass());
			const FString ExpectedPrefix = bGetter ? TEXT("Get ") : TEXT("Set ");
			FString NodeName = ExpectedPrefix + VarDesc.VarName.ToString();
			if (NodeName.IsEmpty())
			{
				return;
			}

			int32 Score = ScoreMatch(NodeName.ToLower(), QueryLower);
			if (Score <= 0)
			{
				return;
			}

			bool bAlreadyAdded = false;
			for (const FNodeSearchResult& Existing : Results)
			{
				const UBlueprintVariableNodeSpawner* ExistingVarSpawner = Cast<UBlueprintVariableNodeSpawner>(Existing.Spawner.Get());
				if (!ExistingVarSpawner)
				{
					continue;
				}
				if (ExistingVarSpawner->GetVarProperty() == VarProp && Existing.Name == NodeName)
				{
					bAlreadyAdded = true;
					break;
				}
			}
			if (bAlreadyAdded)
			{
				return;
			}

			FNodeSearchResult Result;
			Result.Name = NodeName;
			Result.Score = Score;
			Result.Spawner = VarSpawner;
			Result.Signature = VarSpawner->GetSpawnerSignature().ToString();
			Result.Category = UiSpec.Category.ToString();
			Result.Tooltip = UiSpec.Tooltip.ToString().Left(200);
			Results.Add(MoveTemp(Result));
		};

		AddManualVariableSpawner(UK2Node_VariableGet::StaticClass());
		AddManualVariableSpawner(UK2Node_VariableSet::StaticClass());
	}

	Results.Sort([](const FNodeSearchResult& A, const FNodeSearchResult& B)
	{
		return A.Score > B.Score;
	});

	if (Results.Num() > MaxResults)
	{
		Results.SetNum(MaxResults);
	}

	return Results;
}

TArray<FNodeSearchResult> NeoNodes::FindNodes(const FString& Query, UBlueprint* ContextBP, int32 MaxResults)
{
	TArray<FNodeSearchResult> Results;
	FString QueryLower = Query.ToLower();

	if (ContextBP)
	{
		return FindNodesWithBlueprintContext(Query, ContextBP, nullptr, MaxResults);
	}

	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();

	for (auto& ActionPair : ActionDB.GetAllActions())
	{
		for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
		{
			if (!Spawner) continue;
			if (IsStaleSpawner(Spawner)) continue;

			// Use PrimeDefaultUiSpec for the display name — safe without context.
			// GetUiSpec() crashes for variable spawners (accesses Context.Blueprints[0] unchecked).
			// PrimeDefaultUiSpec is safe and spawners pre-populate MenuName at creation time
			// (e.g., "Get Health", "Set Health", "Add StaticMeshComponent", "Subtract").
			const FBlueprintActionUiSpec& UiSpec = Spawner->PrimeDefaultUiSpec(nullptr);

			FString NodeName = UiSpec.MenuName.ToString();
			if (NodeName.IsEmpty()) continue;

			int32 Score = ScoreMatch(NodeName.ToLower(), QueryLower);
			if (Score <= 0) continue;

			FNodeSearchResult Result;
			Result.Name = NodeName;
			Result.Score = Score;
			Result.Spawner = Spawner;
			Result.Signature = Spawner->GetSpawnerSignature().ToString();
			Result.Category = UiSpec.Category.ToString();
			Result.Tooltip = UiSpec.Tooltip.ToString().Left(200);

			// Deprioritize "Execution" category spawners — they often fail to spawn
			// because they are schema-level action wrappers, not proper BlueprintNodeSpawners.
			// The same nodes exist under "Utilities|Flow Control" etc. and work reliably.
			if (Result.Category == TEXT("Execution"))
			{
				Result.Score = FMath::Max(1, Result.Score - 50);
			}

			UEdGraphNode* NodeCDO = Spawner->GetTemplateNode();
			if (NodeCDO)
			{
				ExtractPinNames(NodeCDO, Result.InputPins, Result.OutputPins);
			}

			Results.Add(MoveTemp(Result));
		}
	}

	// Sort by score descending
	Results.Sort([](const FNodeSearchResult& A, const FNodeSearchResult& B)
	{
		return A.Score > B.Score;
	});

	if (Results.Num() > MaxResults)
	{
		Results.SetNum(MaxResults);
	}

	return Results;
}

TArray<FNodeSearchResult> NeoNodes::FindNodesForGraph(const FString& Query, UBlueprint* ContextBP, UEdGraph* TargetGraph, int32 MaxResults, UEdGraphPin* ContextPin)
{
	if (!ContextBP || !TargetGraph) return FindNodes(Query, ContextBP, MaxResults);
	return FindNodesWithBlueprintContext(Query, ContextBP, TargetGraph, MaxResults, ContextPin);
}

UBlueprintNodeSpawner* NeoNodes::FindSpawnerBySignature(const FString& Signature)
{
	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();

	for (auto& ActionPair : ActionDB.GetAllActions())
	{
		for (UBlueprintNodeSpawner* Spawner : ActionPair.Value)
		{
			if (!Spawner) continue;
			if (IsStaleSpawner(Spawner)) continue;
			if (Spawner->GetSpawnerSignature().ToString() == Signature)
			{
				return Spawner;
			}
		}
	}

	return nullptr;
}

UEdGraphNode* NeoNodes::SpawnNode(UEdGraph* Graph, UBlueprintNodeSpawner* Spawner, double X, double Y, UEdGraphPin* FromPin)
{
	if (!Graph || !Spawner) return nullptr;

	IBlueprintNodeBinder::FBindingSet Bindings;
	// Invoke handles SetFlags(RF_Transactional), AllocateDefaultPins(), PostPlacedNewNode(),
	// and AddNode() internally via SpawnEdGraphNode — do NOT call them again.
	// Double PostPlacedNewNode() crashes on UK2Node_CallFunction (check() assertion on enabled state).
	UEdGraphNode* Node = Spawner->Invoke(Graph, Bindings, FVector2D(X, Y));

	if (Node)
	{
		// AutowireNewNode is the hook wildcard K2 nodes use to resolve their
		// pin types from the drag source. Without this, UK2Node_PromotableOperator
		// (Add|Math, Multiply, Equal(byte), etc.) stays as Wildcard and the BP
		// compiler rejects it — matches the "spawn failed" complaint from users.
		if (FromPin)
		{
			Node->AutowireNewNode(FromPin);
		}

		UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (BP)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
	}

	return Node;
}
