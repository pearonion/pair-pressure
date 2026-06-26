#include "Blueprint/BlueprintUtils.h"
#include "Blueprint/AssetTypeAliasRegistry.h"
#include "NeoStackAIModule.h"
#include "Lua/LuaAssetResolver.h"
#include "Utils/NeoTypeResolver.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Misc/StringOutputDevice.h"
#endif
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "UObject/SavePackage.h"
#include "AssetToolsModule.h"
#include "Factories/BlueprintFactory.h"
#include "UObject/UObjectGlobals.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/EnumProperty.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Engine/UserDefinedEnum.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsAssetUtils.h"

// AnimBP support
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_LinkedAnimLayer.h"

// Recursively collect SubGraphs (composite/collapsed node bound graphs)
static void CollectSubGraphs(UEdGraph* Graph, TArray<UEdGraph*>& OutSubGraphs)
{
	for (UEdGraph* Sub : Graph->SubGraphs)
	{
		if (Sub)
		{
			OutSubGraphs.Add(Sub);
			CollectSubGraphs(Sub, OutSubGraphs);
		}
	}
}

static UEdGraph* FindInSubGraphs(UEdGraph* Graph, const FString& GraphName)
{
	if (!Graph) return nullptr;
	for (UEdGraph* Sub : Graph->SubGraphs)
	{
		if (!Sub) continue;
		if (Sub->GetName() == GraphName) return Sub;
		UEdGraph* Found = FindInSubGraphs(Sub, GraphName);
		if (Found) return Found;
	}
	return nullptr;
}

FBlueprintInfo NeoBlueprint::LoadBlueprint(const FString& AssetPath)
{
	FBlueprintInfo Info;

	UBlueprint* BP = NeoLuaAsset::ResolveWithRegistry<UBlueprint>(AssetPath);

	if (!BP)
	{
		return Info;
	}

	Info.Blueprint = BP;
	Info.Name = BP->GetName();
	Info.AssetPath = AssetPath;
	Info.ParentClass = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph)
		{
			Info.Graphs.Add(Graph->GetName());
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			Info.Graphs.Add(Graph->GetName());
		}
	}

	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		if (Graph)
		{
			Info.Graphs.Add(Graph->GetName());
		}
	}

	// Collect composite/collapsed sub-graphs recursively
	TArray<UEdGraph*> AllTopLevel;
	AllTopLevel.Append(BP->UbergraphPages);
	AllTopLevel.Append(BP->FunctionGraphs);
	AllTopLevel.Append(BP->MacroGraphs);
	for (UEdGraph* Graph : AllTopLevel)
	{
		if (!Graph) continue;
		TArray<UEdGraph*> SubGraphs;
		CollectSubGraphs(Graph, SubGraphs);
		for (UEdGraph* Sub : SubGraphs)
		{
			Info.Graphs.Add(Sub->GetName());
		}
	}

	return Info;
}

UEdGraph* NeoBlueprint::FindGraph(UBlueprint* BP, const FString& GraphName)
{
	if (!BP) return nullptr;

	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Search composite/collapsed sub-graphs recursively
	TArray<UEdGraph*> AllTopLevel;
	AllTopLevel.Append(BP->UbergraphPages);
	AllTopLevel.Append(BP->FunctionGraphs);
	AllTopLevel.Append(BP->MacroGraphs);
	for (UEdGraph* Graph : AllTopLevel)
	{
		if (!Graph) continue;
		UEdGraph* Found = FindInSubGraphs(Graph, GraphName);
		if (Found) return Found;
	}

	// AnimBP: resolve selector-style paths (e.g. "Locomotion/Idle", "Locomotion/Idle->Run")
	TArray<TPair<FString, UEdGraph*>> AnimGraphs;
	CollectAnimBPGraphs(BP, AnimGraphs);
	for (const auto& Pair : AnimGraphs)
	{
		if (Pair.Key == GraphName)
		{
			return Pair.Value;
		}
	}

	return nullptr;
}

// ============================================================================
// Animation Blueprint helpers
// ============================================================================

UEdGraph* NeoBlueprint::FindAnimGraph(UBlueprint* BP)
{
	if (!BP) return nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
		{
			return Graph;
		}
	}
	return nullptr;
}

// Safe helpers for transition node traversal (GetPreviousState/GetNextState crash on malformed nodes)
static UAnimStateNodeBase* SafeGetPreviousState(UAnimStateTransitionNode* TransNode)
{
	if (!TransNode || TransNode->Pins.Num() < 2) return nullptr;
	UEdGraphPin* InputPin = TransNode->GetInputPin();
	if (!InputPin || InputPin->LinkedTo.Num() == 0) return nullptr;
	return TransNode->GetPreviousState();
}

static UAnimStateNodeBase* SafeGetNextState(UAnimStateTransitionNode* TransNode)
{
	if (!TransNode || TransNode->Pins.Num() < 2) return nullptr;
	UEdGraphPin* OutputPin = TransNode->GetOutputPin();
	if (!OutputPin || OutputPin->LinkedTo.Num() == 0) return nullptr;
	return TransNode->GetNextState();
}

static FString GetStateName(UEdGraphNode* Node)
{
	if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
	{
		return StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
	}
	if (Cast<UAnimStateEntryNode>(Node))
	{
		return TEXT("[Entry]");
	}
	if (UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(Node))
	{
		return Conduit->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
	}
	return Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
}

void NeoBlueprint::CollectAnimBPGraphs(UBlueprint* BP, TArray<TPair<FString, UEdGraph*>>& OutGraphs)
{
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(BP);
	if (!AnimBP) return;

	// Traverse all FunctionGraphs looking for UAnimationGraph instances
	for (UEdGraph* FuncGraph : AnimBP->FunctionGraphs)
	{
		UAnimationGraph* AnimGraph = Cast<UAnimationGraph>(FuncGraph);
		if (!AnimGraph) continue;

		// Find state machine nodes within this anim graph
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
			if (!SMNode || !SMNode->EditorStateMachineGraph) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			FString SMName = SMGraph->GetName();

			// Add the state machine graph itself
			OutGraphs.Add(TPair<FString, UEdGraph*>(SMName, SMGraph));

			// Traverse nodes inside the state machine
			for (UEdGraphNode* SMChild : SMGraph->Nodes)
			{
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChild))
				{
					if (StateNode->BoundGraph)
					{
						FString StateName = StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						FString Selector = FString::Printf(TEXT("%s/%s"), *SMName, *StateName);
						OutGraphs.Add(TPair<FString, UEdGraph*>(Selector, StateNode->BoundGraph));
					}
				}
				else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(SMChild))
				{
					UAnimStateNodeBase* PrevState = SafeGetPreviousState(TransNode);
					UAnimStateNodeBase* NextState = SafeGetNextState(TransNode);

					FString FromName = PrevState ? GetStateName(PrevState) : TEXT("[Entry]");
					FString ToName = NextState ? GetStateName(NextState) : TEXT("?");

					if (TransNode->BoundGraph)
					{
						FString Selector = FString::Printf(TEXT("%s/%s->%s"), *SMName, *FromName, *ToName);
						OutGraphs.Add(TPair<FString, UEdGraph*>(Selector, TransNode->BoundGraph));
					}
				}
				else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(SMChild))
				{
					if (ConduitNode->BoundGraph)
					{
						FString ConduitName = ConduitNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						FString Selector = FString::Printf(TEXT("%s/%s"), *SMName, *ConduitName);
						OutGraphs.Add(TPair<FString, UEdGraph*>(Selector, ConduitNode->BoundGraph));
					}
				}
			}
		}
	}

	// Collect animation layer graphs from implemented interfaces
	for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
	{
		for (UEdGraph* LayerGraph : Desc.Graphs)
		{
			if (LayerGraph && Cast<UAnimationGraph>(LayerGraph))
			{
				OutGraphs.Add(TPair<FString, UEdGraph*>(LayerGraph->GetName(), LayerGraph));
			}
		}
	}
}

FCompileResult NeoBlueprint::CompileBlueprint(UBlueprint* BP)
{
	FCompileResult Result;
	if (!BP) return Result;

	// Must use StructurallyModified to trigger full recompile after graph mutations
	// (MarkBlueprintAsModified only marks dirty without invalidating compiled bytecode)
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);

	Result.bSuccess = (BP->Status != BS_Error);
	return Result;
}

bool NeoBlueprint::SaveBlueprint(UBlueprint* BP)
{
	if (!BP) return false;

	UPackage* Package = BP->GetOutermost();
	if (!Package) return false;

	FString PackageFileName;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName))
	{
		PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	return UPackage::SavePackage(Package, BP, *PackageFileName, SaveArgs);
}

UBlueprint* NeoBlueprint::CreateBlueprint(const FString& AssetPath, const FString& ParentClassName)
{
	// Special case: creating an Interface Blueprint
	FString LowerParent = ParentClassName.ToLower();
	if (LowerParent == TEXT("interface") || LowerParent == TEXT("blueprintinterface"))
	{
		return CreateInterfaceBlueprint(AssetPath);
	}

	UClass* ParentClass = nullptr;

	// Try common names first
	static const TMap<FString, FString> ClassAliases = {
		{TEXT("actor"), TEXT("/Script/Engine.Actor")},
		{TEXT("pawn"), TEXT("/Script/Engine.Pawn")},
		{TEXT("character"), TEXT("/Script/Engine.Character")},
		{TEXT("playercontroller"), TEXT("/Script/Engine.PlayerController")},
		{TEXT("gamemode"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("gamemodebase"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("hud"), TEXT("/Script/Engine.HUD")},
		{TEXT("actorcomponent"), TEXT("/Script/Engine.ActorComponent")},
		{TEXT("scenecomponent"), TEXT("/Script/Engine.SceneComponent")},
		{TEXT("object"), TEXT("/Script/CoreUObject.Object")},
	};

	FString LowerName = ParentClassName.ToLower();
	if (const FString* FullPath = ClassAliases.Find(LowerName))
	{
		ParentClass = LoadObject<UClass>(nullptr, **FullPath);
	}

	if (!ParentClass)
	{
		// Shared resolver handles prefix-dance (U/A) + loaded-object scan, so
		// callers can pass "Character" / "Actor" without the prefix.
		ParentClass = NeoTypeResolver::FindClassRobust(ParentClassName);
	}

	if (!ParentClass)
	{
		ParentClass = LoadObject<UClass>(nullptr, *ParentClassName);
	}

	if (!ParentClass)
	{
		return nullptr;
	}

	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
	return Cast<UBlueprint>(NewAsset);
}

// Type-string parsing lives in Utils/NeoTypeResolver.{h,cpp}. Call
// NeoTypeResolver::ResolveTypeString / FindClassRobust / FindEnumRobust /
// FindStructRobust from wherever you need them.

bool NeoBlueprint::AddVariable(UBlueprint* BP, const FString& VarName, const FString& VarType, const FString& DefaultValue)
{
	if (!BP) return false;

	FEdGraphPinType PinType;
	if (!NeoTypeResolver::ResolveTypeString(VarType, PinType))
	{
		return false;
	}

	if (!FBlueprintEditorUtils::AddMemberVariable(BP, FName(*VarName), PinType, DefaultValue))
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);
	return true;
}

bool NeoBlueprint::DeleteNode(UEdGraphNode* Node)
{
	if (!Node) return false;

	UEdGraph* Graph = Node->GetGraph();
	if (!Graph) return false;

	// Refuse to delete most nodes in /Engine/Transient — they're commonly orphaned
	// from cross-graph corruption. Material editor preview graphs are a legitimate
	// transient working copy and still use the generic graph deletion flow below.
	if (Graph->GetOutermost() == GetTransientPackage() && !Graph->IsA<UMaterialGraph>())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("DeleteNode: Refusing to delete orphaned node '%s' in /Engine/Transient"),
			*Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
		return false;
	}

	// Use FBlueprintEditorUtils::RemoveNode for BP graphs (handles breakpoints, pin watches, schema notifications)
	UBlueprint* OwnerBP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (OwnerBP)
	{
		FBlueprintEditorUtils::RemoveNode(OwnerBP, Node, true);
		return true;
	}

	// Non-BP graphs: follow the engine flow manually
	const UEdGraphSchema* Schema = Graph->GetSchema();
	Graph->Modify();
	Node->Modify();
	if (Schema)
	{
		Schema->BreakNodeLinks(*Node);
	}
	Node->DestroyNode();
	return true;
}

USCS_Node* NeoBlueprint::AddComponent(UBlueprint* BP, const FString& ComponentName, const FString& ComponentClassName, const FString& ParentName)
{
	if (!BP || !BP->SimpleConstructionScript) return nullptr;
	if (!BP->ParentClass || !BP->ParentClass->IsChildOf(AActor::StaticClass())) return nullptr;

	// Resolve via shared FindClassRobust (handles U/A-prefix dance + loaded-class scan).
	UClass* ComponentClass = NeoTypeResolver::FindClassRobust(ComponentClassName);
	// Also accept "StaticMesh" → "StaticMeshComponent" (engine pattern) when first lookup fails.
	if (!ComponentClass && !ComponentClassName.EndsWith(TEXT("Component"), ESearchCase::IgnoreCase))
	{
		ComponentClass = NeoTypeResolver::FindClassRobust(ComponentClassName + TEXT("Component"));
	}
	// Must be a non-abstract, non-deprecated UActorComponent subclass.
	if (!ComponentClass
		|| !ComponentClass->IsChildOf(UActorComponent::StaticClass())
		|| ComponentClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		return nullptr;
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	FName VarName = SCS->GenerateNewComponentName(ComponentClass, FName(*ComponentName));
	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, VarName);
	if (!NewNode) return nullptr;

	if (!ParentName.IsEmpty())
	{
		USCS_Node* ParentNode = SCS->FindSCSNode(FName(*ParentName));
		if (ParentNode)
		{
			ParentNode->AddChildNode(NewNode);
		}
		else
		{
			SCS->AddNode(NewNode);
		}
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return NewNode;
}

UEdGraph* NeoBlueprint::AddFunctionGraph(UBlueprint* BP, const FString& FunctionName)
{
	if (!BP) return nullptr;

	// Upsert: find existing function graph first
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			return Graph;
		}
	}

	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!FunctionGraph) return nullptr;

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(
		BP,
		FunctionGraph,
		true,
		nullptr
	);

	return FunctionGraph;
}

bool NeoBlueprint::MoveNode(UEdGraphNode* Node, double X, double Y)
{
	if (!Node) return false;
	Node->NodePosX = static_cast<int32>(X);
	Node->NodePosY = static_cast<int32>(Y);
	return true;
}

UEdGraph* NeoBlueprint::AddEventDispatcher(UBlueprint* BP, const FString& Name, const TArray<FParamDesc>& Params)
{
	if (!BP) return nullptr;

	FName DispatcherName(*Name);
	if (FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, DispatcherName))
	{
		return nullptr;
	}

	// Step 1: Create a member variable of type MC_Delegate
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(BP, DispatcherName, DelegateType))
	{
		return nullptr;
	}

	// Step 2: Create the delegate signature graph
	UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, DispatcherName,
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	if (!SignatureGraph)
	{
		FBlueprintEditorUtils::RemoveMemberVariable(BP, DispatcherName);
		return nullptr;
	}

	SignatureGraph->bEditable = false;

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
	K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
	K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);

	BP->DelegateSignatureGraphs.Add(SignatureGraph);

	// Step 3: Add parameters to the signature graph's entry node
	if (Params.Num() > 0)
	{
		TArray<UK2Node_FunctionEntry*> EntryNodes;
		SignatureGraph->GetNodesOfClass(EntryNodes);

		if (EntryNodes.Num() > 0)
		{
			UK2Node_FunctionEntry* EntryNode = EntryNodes[0];
			for (const FParamDesc& Param : Params)
			{
				FEdGraphPinType PinType;
				if (NeoTypeResolver::ResolveTypeString(Param.Type, PinType))
				{
					EntryNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return SignatureGraph;
}

bool NeoBlueprint::AddFunctionParam(UBlueprint* BP, const FString& FuncName, const FString& ParamName, const FString& ParamType)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	Graph->GetNodesOfClass(EntryNodes);
	if (EntryNodes.Num() == 0) return false;

	FEdGraphPinType PinType;
	if (!NeoTypeResolver::ResolveTypeString(ParamType, PinType)) return false;

	EntryNodes[0]->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::AddFunctionReturn(UBlueprint* BP, const FString& FuncName, const FString& ReturnName, const FString& ReturnType)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	// Find or create result node
	TArray<UK2Node_FunctionResult*> ResultNodes;
	Graph->GetNodesOfClass(ResultNodes);

	UK2Node_FunctionResult* ResultNode = nullptr;
	if (ResultNodes.Num() > 0)
	{
		ResultNode = ResultNodes[0];
	}
	else
	{
		// Create a result node if one doesn't exist
		ResultNode = NewObject<UK2Node_FunctionResult>(Graph);
		ResultNode->NodePosX = 600;
		ResultNode->NodePosY = 0;
		Graph->AddNode(ResultNode, false, false);
		ResultNode->CreateNewGuid();
		ResultNode->PostPlacedNewNode();
		// Note: do NOT call AllocateDefaultPins() here — PostPlacedNewNode()
		// already syncs with the entry node and calls ReconstructNode() which
		// allocates pins. A second call would create a duplicate exec pin.
	}

	FEdGraphPinType PinType;
	if (!NeoTypeResolver::ResolveTypeString(ReturnType, PinType)) return false;

	ResultNode->CreateUserDefinedPin(FName(*ReturnName), PinType, EGPD_Input);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::SetFunctionFlags(UBlueprint* BP, const FString& FuncName, uint32 AddFlags, uint32 RemoveFlags)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	Graph->GetNodesOfClass(EntryNodes);
	if (EntryNodes.Num() == 0) return false;

	UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

	if (AddFlags != 0)
	{
		EntryNode->AddExtraFlags(AddFlags);
	}

	if (RemoveFlags != 0)
	{
		EntryNode->ClearExtraFlags(RemoveFlags);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::SetVariableProperty(UBlueprint* BP, const FString& VarName, const FString& Key, const FString& Value)
{
	if (!BP) return false;

	FName FVarName(*VarName);
	FString KeyLower = Key.ToLower();

	// Property flags
	uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(BP, FVarName);
	if (!Flags) return false;

	if (KeyLower == TEXT("edit_anywhere"))
	{
		if (Value.ToBool()) *Flags |= CPF_Edit;
		else *Flags &= ~CPF_Edit;
	}
	else if (KeyLower == TEXT("edit_defaults_only"))
	{
		if (Value.ToBool())
		{
			*Flags |= CPF_Edit | CPF_DisableEditOnInstance;
		}
		else
		{
			*Flags &= ~CPF_DisableEditOnInstance;
		}
	}
	else if (KeyLower == TEXT("edit_instance_only"))
	{
		if (Value.ToBool())
		{
			*Flags |= CPF_Edit;
			*Flags &= ~CPF_DisableEditOnInstance;
			*Flags |= CPF_DisableEditOnTemplate;
		}
		else
		{
			*Flags &= ~CPF_DisableEditOnTemplate;
		}
	}
	else if (KeyLower == TEXT("blueprint_read_only"))
	{
		FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(BP, FVarName, Value.ToBool());
		return true;
	}
	else if (KeyLower == TEXT("replicated"))
	{
		if (Value.ToBool()) *Flags |= CPF_Net;
		else *Flags &= ~(CPF_Net | CPF_RepNotify);
	}
	else if (KeyLower == TEXT("rep_notify"))
	{
		if (!Value.IsEmpty() && Value != TEXT("false") && Value != TEXT("0"))
		{
			*Flags |= CPF_Net | CPF_RepNotify;
			FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(BP, FVarName, FName(*Value));
		}
	}
	else if (KeyLower == TEXT("transient"))
	{
		FBlueprintEditorUtils::SetVariableTransientFlag(BP, FVarName, Value.ToBool());
		return true;
	}
	else if (KeyLower == TEXT("save_game"))
	{
		FBlueprintEditorUtils::SetVariableSaveGameFlag(BP, FVarName, Value.ToBool());
		return true;
	}
	else if (KeyLower == TEXT("expose_on_spawn"))
	{
		if (Value.ToBool()) *Flags |= CPF_ExposeOnSpawn;
		else *Flags &= ~CPF_ExposeOnSpawn;
	}
	// Metadata (non-flag properties)
	else if (KeyLower == TEXT("category"))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, FVarName, nullptr, FText::FromString(Value));
	}
	else if (KeyLower == TEXT("tooltip"))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, FVarName, nullptr, FName(TEXT("tooltip")), Value);
	}
	else
	{
		// Generic metadata fallback
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(BP, FVarName, nullptr, FName(*Key), Value);
		return true;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Generic property access via reflection
// ============================================================================

bool NeoBlueprint::SetObjectProperty(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("null object");
		return false;
	}

	FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		OutError = FuzzyMatchProperty(Object, PropertyName);
		return false;
	}

	// Full edit notification sequence for proper engine integration
	// (component templates, widgets, etc. rely on PostEditChangeProperty)
	Object->Modify();
	Object->PreEditChange(Prop);

	FStringOutputDevice ErrorText;
	const TCHAR* Result = Prop->ImportText_InContainer(*Value, Object, Object, 0, &ErrorText);

	if (!Result || ErrorText.Len() > 0)
	{
		// Close the PreEditChange bracket to prevent corrupted undo state
		FPropertyChangedEvent FailEvent(Prop, EPropertyChangeType::Unspecified);
		Object->PostEditChangeProperty(FailEvent);

		// Build better error for enums — list valid values
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				OutError = FString::Printf(TEXT("invalid value \"%s\". Valid: "), *Value);
				for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
				{
					if (i > 0) OutError += TEXT(", ");
					OutError += Enum->GetNameStringByIndex(i);
				}
				return false;
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (UEnum* Enum = ByteProp->Enum)
			{
				OutError = FString::Printf(TEXT("invalid value \"%s\". Valid: "), *Value);
				for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
				{
					if (i > 0) OutError += TEXT(", ");
					OutError += Enum->GetNameStringByIndex(i);
				}
				return false;
			}
		}

		OutError = FString::Printf(TEXT("ImportText failed for \"%s\" (type: %s). Value: \"%s\""),
			*PropertyName, *Prop->GetCPPType(), *Value);
		if (ErrorText.Len() > 0)
		{
			OutError += FString::Printf(TEXT(" Error: %s"), *ErrorText);
		}
		return false;
	}

	FPropertyChangedEvent PropertyEvent(Prop, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(PropertyEvent);
	Object->MarkPackageDirty();

	return true;
}

bool NeoBlueprint::GetObjectProperty(UObject* Object, const FString& PropertyName, FString& OutValue)
{
	if (!Object) return false;

	FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop) return false;

	Prop->ExportTextItem_InContainer(OutValue, Object, nullptr, Object, 0);
	return true;
}

FString NeoBlueprint::FuzzyMatchProperty(UObject* Object, const FString& PropertyName)
{
	if (!Object) return TEXT("property not found");

	FString NameLower = PropertyName.ToLower();
	TArray<TPair<int32, FString>> Scored;

	for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop || Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;

		FString PropName = Prop->GetName();
		FString PropLower = PropName.ToLower();

		int32 Score = 0;
		if (PropLower == NameLower) Score = 100;
		else if (PropLower.Contains(NameLower) || NameLower.Contains(PropLower)) Score = 70;
		else
		{
			// Subsequence match
			int32 Pos = 0, Matched = 0;
			for (int32 i = 0; i < NameLower.Len() && Pos < PropLower.Len(); i++)
			{
				for (int32 j = Pos; j < PropLower.Len(); j++)
				{
					if (PropLower[j] == NameLower[i]) { Pos = j + 1; Matched++; break; }
				}
			}
			if (Matched > NameLower.Len() / 2) Score = 30;
		}

		if (Score > 0)
		{
			Scored.Add(TPair<int32, FString>(Score, FString::Printf(TEXT("%s (%s)"), *PropName, *Prop->GetCPPType())));
		}
	}

	Scored.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key > B.Key; });

	FString Suggestions;
	int32 Max = FMath::Min(Scored.Num(), 8);
	for (int32 i = 0; i < Max; i++)
	{
		if (i > 0) Suggestions += TEXT(", ");
		Suggestions += Scored[i].Value;
	}

	if (Suggestions.IsEmpty())
	{
		return FString::Printf(TEXT("property \"%s\" not found on %s"), *PropertyName, *Object->GetClass()->GetName());
	}

	return FString::Printf(TEXT("property \"%s\" not found. Similar: %s"), *PropertyName, *Suggestions);
}

FString NeoBlueprint::FuzzyMatchFunction(UClass* Class, const FString& FunctionName)
{
	if (!Class) return TEXT("function not found");

	FString NameLower = FunctionName.ToLower();
	TArray<TPair<int32, FString>> Scored;

	for (TFieldIterator<UFunction> FnIt(Class); FnIt; ++FnIt)
	{
		UFunction* Fn = *FnIt;
		if (!Fn) continue;

		FString FnName = Fn->GetName();
		FString FnLower = FnName.ToLower();

		int32 Score = 0;
		if (FnLower == NameLower) Score = 100;
		else if (FnLower.Contains(NameLower) || NameLower.Contains(FnLower)) Score = 70;
		else
		{
			int32 Pos = 0, Matched = 0;
			for (int32 i = 0; i < NameLower.Len() && Pos < FnLower.Len(); i++)
			{
				for (int32 j = Pos; j < FnLower.Len(); j++)
				{
					if (FnLower[j] == NameLower[i]) { Pos = j + 1; Matched++; break; }
				}
			}
			if (Matched > NameLower.Len() / 2) Score = 30;
		}

		if (Score > 0) Scored.Add(TPair<int32, FString>(Score, FnName));
	}

	Scored.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key > B.Key; });

	FString Suggestions;
	int32 Max = FMath::Min(Scored.Num(), 8);
	for (int32 i = 0; i < Max; i++)
	{
		if (i > 0) Suggestions += TEXT(", ");
		Suggestions += Scored[i].Value;
	}

	if (Suggestions.IsEmpty())
	{
		return FString::Printf(TEXT("function \"%s\" not found on %s"), *FunctionName, *Class->GetName());
	}

	return FString::Printf(TEXT("function \"%s\" not found. Similar: %s"), *FunctionName, *Suggestions);
}

void NeoBlueprint::ListObjectProperties(UObject* Object, const FString& Filter, TArray<TPair<FString, FString>>& OutProps)
{
	if (!Object) return;

	FString FilterLower = Filter.ToLower();

	for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;

		FString PropName = Prop->GetName();

		if (!FilterLower.IsEmpty() && !PropName.ToLower().Contains(FilterLower))
		{
			continue;
		}

		FString Value;
		Prop->ExportTextItem_InContainer(Value, Object, nullptr, Object, 0);

		FString TypeStr = Prop->GetCPPType();
		OutProps.Add(TPair<FString, FString>(
			FString::Printf(TEXT("%s (%s)"), *PropName, *TypeStr),
			Value
		));
	}
}

UActorComponent* NeoBlueprint::GetComponentTemplate(UBlueprint* BP, const FString& ComponentName)
{
	if (!BP || !BP->SimpleConstructionScript) return nullptr;

	USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
	if (!Node) return nullptr;

	return Node->ComponentTemplate;
}

// ============================================================================
// Remove
// ============================================================================

bool NeoBlueprint::RemoveVariable(UBlueprint* BP, const FString& VarName)
{
	if (!BP) return false;

	FName FVarName(*VarName);
	int32 Idx = FBlueprintEditorUtils::FindNewVariableIndex(BP, FVarName);
	if (Idx == INDEX_NONE) return false;

	// If it's an event dispatcher, also remove its signature graph
	if (BP->NewVariables[Idx].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
	{
		if (UEdGraph* SigGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, FVarName))
		{
			FBlueprintEditorUtils::RemoveGraph(BP, SigGraph);
		}
	}

	FBlueprintEditorUtils::RemoveMemberVariable(BP, FVarName);
	return true;
}

bool NeoBlueprint::RemoveFunction(UBlueprint* BP, const FString& FuncName)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	FBlueprintEditorUtils::RemoveGraph(BP, Graph);
	return true;
}

bool NeoBlueprint::RemoveComponent(UBlueprint* BP, const FString& ComponentName)
{
	if (!BP || !BP->SimpleConstructionScript) return false;

	USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName));
	if (!Node) return false;

	FName VarName = Node->GetVariableName();

	// Remove variable accessor nodes from BP graphs (Get/Set nodes referencing this component)
	FBlueprintEditorUtils::RemoveVariableNodes(BP, VarName);

	// Remove from SCS tree, promoting children to parent
	BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);

	// Rename the template object so the name can be reused without recompiling
	// (mirrors SSCSEditor.cpp cleanup logic)
	if (Node->ComponentTemplate != nullptr)
	{
		const FName TemplateName = Node->ComponentTemplate->GetFName();
		const FString RemovedName = VarName.ToString() + TEXT("_REMOVED_") + FGuid::NewGuid().ToString();

		Node->ComponentTemplate->Modify();
		Node->ComponentTemplate->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);

		// Destroy archetype instances on world actors
		TArray<UObject*> ArchetypeInstances;
		Node->ComponentTemplate->GetArchetypeInstances(ArchetypeInstances);
		for (UObject* Instance : ArchetypeInstances)
		{
			if (!Instance->HasAllFlags(RF_ArchetypeObject | RF_InheritableComponentTemplate))
			{
				if (UActorComponent* Comp = Cast<UActorComponent>(Instance)) { Comp->DestroyComponent(); }
				Instance->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);
			}
		}

		// Clean up child class inherited component templates
		if (BP->GeneratedClass)
		{
			TArray<UClass*> ChildClasses;
			GetDerivedClasses(BP->GeneratedClass, ChildClasses);
			for (UClass* ChildClass : ChildClasses)
			{
				if (UActorComponent* ChildTemplate = Cast<UActorComponent>(static_cast<UObject*>(FindObjectWithOuter(ChildClass, UActorComponent::StaticClass(), TemplateName))))
				{
					ChildTemplate->Modify();
					ChildTemplate->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);

					ArchetypeInstances.Reset();
					ChildTemplate->GetArchetypeInstances(ArchetypeInstances);
					for (UObject* Instance : ArchetypeInstances)
					{
						if (!Instance->HasAllFlags(RF_ArchetypeObject | RF_InheritableComponentTemplate))
						{
							if (UActorComponent* Comp = Cast<UActorComponent>(Instance)) { Comp->DestroyComponent(); }
							Instance->Rename(*RemovedName, nullptr, REN_DontCreateRedirectors);
						}
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Rename
// ============================================================================

bool NeoBlueprint::RenameVariable(UBlueprint* BP, const FString& OldName, const FString& NewName)
{
	if (!BP) return false;

	FName FOld(*OldName);
	FName FNew(*NewName);

	int32 Idx = FBlueprintEditorUtils::FindNewVariableIndex(BP, FOld);
	if (Idx == INDEX_NONE) return false;

	FBlueprintEditorUtils::RenameMemberVariable(BP, FOld, FNew);
	return true;
}

bool NeoBlueprint::RenameFunction(UBlueprint* BP, const FString& OldName, const FString& NewName)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, OldName);
	if (!Graph) return false;

	FBlueprintEditorUtils::RenameGraph(Graph, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Local variables
// ============================================================================

bool NeoBlueprint::AddLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& VarName, const FString& VarType, const FString& DefaultValue)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	FEdGraphPinType PinType;
	if (!NeoTypeResolver::ResolveTypeString(VarType, PinType)) return false;

	return FBlueprintEditorUtils::AddLocalVariable(BP, Graph, FName(*VarName), PinType, DefaultValue);
}

bool NeoBlueprint::RemoveLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& VarName)
{
	if (!BP) return false;

	UEdGraph* Graph = FindGraph(BP, FuncName);
	if (!Graph) return false;

	// Get the scope struct from the skeleton class
	UFunction* Func = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	if (!Func)
	{
		Func = BP->GeneratedClass ? BP->GeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	}
	if (!Func) return false;

	FBlueprintEditorUtils::RemoveLocalVariable(BP, Func, FName(*VarName));
	return true;
}

bool NeoBlueprint::RenameLocalVariable(UBlueprint* BP, const FString& FuncName, const FString& OldName, const FString& NewName)
{
	if (!BP) return false;

	UFunction* Func = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	if (!Func) return false;

	FBPVariableDescription* Var = FBlueprintEditorUtils::FindLocalVariable(BP, Func, FName(*OldName));
	if (!Var) return false;

	FBlueprintEditorUtils::RenameLocalVariable(BP, Func, FName(*OldName), FName(*NewName));
	return true;
}

// ============================================================================
// Change variable type
// ============================================================================

static void ApplyBlueprintVariableType(FBPVariableDescription& Description, const FEdGraphPinType& NewPinType)
{
	Description.VarType = NewPinType;
}

bool NeoBlueprint::ChangeVariableType(UBlueprint* BP, const FString& VarName, const FString& NewType)
{
	if (!BP) return false;

	FName VarFName(*VarName);
	int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
	if (VarIndex == INDEX_NONE) return false;

	FEdGraphPinType NewPinType;
	if (!NeoTypeResolver::ResolveTypeString(NewType, NewPinType)) return false;

	FBPVariableDescription& Variable = BP->NewVariables[VarIndex];
	if (Variable.VarType == NewPinType) return true; // Already the right type

	FBlueprintEditorUtils::ChangeMemberVariableType(BP, VarFName, NewPinType);
	VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, VarFName);
	return VarIndex != INDEX_NONE && BP->NewVariables[VarIndex].VarType == NewPinType;
}

bool NeoBlueprint::ChangeLocalVariableType(UBlueprint* BP, const FString& FuncName, const FString& VarName, const FString& NewType)
{
	if (!BP) return false;

	UEdGraph* ScopeGraph = FindGraph(BP, FuncName);
	if (!ScopeGraph) return false;

	UK2Node_FunctionEntry* FunctionEntry = nullptr;
	FBPVariableDescription* GraphVar = FBlueprintEditorUtils::FindLocalVariable(BP, ScopeGraph, FName(*VarName), &FunctionEntry);
	if (!GraphVar) return false;

	UFunction* Func = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	if (!Func)
	{
		Func = BP->GeneratedClass ? BP->GeneratedClass->FindFunctionByName(FName(*FuncName)) : nullptr;
	}

	FEdGraphPinType NewPinType;
	if (!NeoTypeResolver::ResolveTypeString(NewType, NewPinType)) return false;

	if (GraphVar->VarType == NewPinType) return true;

	if (Func)
	{
		FBlueprintEditorUtils::ChangeLocalVariableType(BP, Func, FName(*VarName), NewPinType);
		GraphVar = FBlueprintEditorUtils::FindLocalVariable(BP, ScopeGraph, FName(*VarName), &FunctionEntry);
		if (GraphVar && GraphVar->VarType == NewPinType)
		{
			return true;
		}
	}

	if (!FunctionEntry) return false;
	FunctionEntry->Modify();
	ApplyBlueprintVariableType(*GraphVar, NewPinType);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	GraphVar = FBlueprintEditorUtils::FindLocalVariable(BP, ScopeGraph, FName(*VarName), &FunctionEntry);
	return GraphVar && GraphVar->VarType == NewPinType;
}

// ============================================================================
// Reorder variables
// ============================================================================

bool NeoBlueprint::MoveVariableBefore(UBlueprint* BP, const FString& VarName, const FString& TargetVarName)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!BP || !BP->SkeletonGeneratedClass) return false;
	return FBlueprintEditorUtils::MoveVariableBeforeVariable(BP, BP->SkeletonGeneratedClass, FName(*VarName), FName(*TargetVarName), false);
#else
	return false;
#endif
}

bool NeoBlueprint::MoveVariableAfter(UBlueprint* BP, const FString& VarName, const FString& TargetVarName)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	if (!BP || !BP->SkeletonGeneratedClass) return false;
	return FBlueprintEditorUtils::MoveVariableAfterVariable(BP, BP->SkeletonGeneratedClass, FName(*VarName), FName(*TargetVarName), false);
#else
	return false;
#endif
}

// ============================================================================
// Query
// ============================================================================

bool NeoBlueprint::IsVariableUsed(UBlueprint* BP, const FString& VarName)
{
	if (!BP) return false;
	return FBlueprintEditorUtils::IsVariableUsed(BP, FName(*VarName));
}

// ============================================================================
// Interfaces
// ============================================================================

UBlueprint* NeoBlueprint::CreateInterfaceBlueprint(const FString& AssetPath)
{
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package) return nullptr;

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		UInterface::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Interface,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (NewBP)
	{
		FAssetRegistryModule::AssetCreated(NewBP);
		NewBP->MarkPackageDirty();
	}

	return NewBP;
}

static UClass* FindInterfaceClass(const FString& InterfaceName)
{
	// Shared resolver covers bare name + "U" prefix + loaded-object scan.
	// Filter to interface classes here — FindClassRobust is type-agnostic.
	if (UClass* Found = NeoTypeResolver::FindClassRobust(InterfaceName))
	{
		if (Found->IsChildOf(UInterface::StaticClass())) return Found;
	}

	auto TryBlueprintInterfaceAsset = [](const FString& BlueprintPath) -> UClass*
	{
		UBlueprint* InterfaceBP = NeoLuaAsset::ResolveWithRegistry<UBlueprint>(BlueprintPath);
		if (InterfaceBP && InterfaceBP->BlueprintType == BPTYPE_Interface && InterfaceBP->GeneratedClass)
		{
			return InterfaceBP->GeneratedClass;
		}
		return nullptr;
	};

	if (UClass* Found = TryBlueprintInterfaceAsset(InterfaceName))
	{
		return Found;
	}

	if (!InterfaceName.StartsWith(TEXT("/")))
	{
		if (UClass* Found = TryBlueprintInterfaceAsset(TEXT("/Game/") + InterfaceName))
		{
			return Found;
		}
	}

	// Try searching loaded objects by short name (fast, covers in-session created assets)
	if (!InterfaceName.StartsWith(TEXT("/")))
	{
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* BP = *It;
			if (BP && BP->GetName() == InterfaceName && BP->BlueprintType == BPTYPE_Interface && BP->GeneratedClass)
			{
				return BP->GeneratedClass;
			}
		}

		// Try Asset Registry for assets not yet loaded
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(FName(*(TEXT("/Game/") + InterfaceName)), Assets, true);
		if (Assets.Num() == 0)
		{
			// Search all paths — the asset might be in a subdirectory
			FARFilter Filter;
			Filter.bRecursiveClasses = true;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			AR.GetAssets(Filter, Assets);
		}

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString() == InterfaceName)
			{
				UBlueprint* InterfaceBP = Cast<UBlueprint>(Asset.GetAsset());
				if (InterfaceBP && InterfaceBP->BlueprintType == BPTYPE_Interface && InterfaceBP->GeneratedClass)
				{
					return InterfaceBP->GeneratedClass;
				}
			}
		}
	}

	return nullptr;
}

bool NeoBlueprint::AddInterface(UBlueprint* BP, const FString& InterfaceName)
{
	if (!BP) return false;

	UClass* InterfaceClass = FindInterfaceClass(InterfaceName);
	if (!InterfaceClass) return false;

	// Check if already implemented
	if (FBlueprintEditorUtils::ImplementsInterface(BP, false, InterfaceClass))
	{
		return false;
	}

	FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
	return FBlueprintEditorUtils::ImplementNewInterface(BP, InterfacePath);
}

bool NeoBlueprint::RemoveInterface(UBlueprint* BP, const FString& InterfaceName, bool bPreserveFunctions)
{
	if (!BP) return false;

	UClass* InterfaceClass = FindInterfaceClass(InterfaceName);
	if (!InterfaceClass) return false;

	if (!FBlueprintEditorUtils::ImplementsInterface(BP, false, InterfaceClass))
	{
		return false;
	}

	FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
	FBlueprintEditorUtils::RemoveInterface(BP, InterfacePath, bPreserveFunctions);
	return true;
}

// ============================================================================
// Macros
// ============================================================================

UEdGraph* NeoBlueprint::AddMacroGraph(UBlueprint* BP, const FString& MacroName)
{
	if (!BP) return nullptr;

	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP,
		FName(*MacroName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!MacroGraph) return nullptr;

	FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);
	return MacroGraph;
}

// ============================================================================
// Custom Events (upsert — find or create, then apply params/flags)
// ============================================================================

UK2Node_CustomEvent* NeoBlueprint::FindCustomEvent(UBlueprint* BP, const FString& EventName)
{
	if (!BP) return nullptr;
	// FBlueprintEditorUtils::FindCustomEventNode (UE 5.7 header L491, public block) returns
	// UK2Node_Event* — custom events are a subclass, so Cast narrows to the expected type.
	return Cast<UK2Node_CustomEvent>(FBlueprintEditorUtils::FindCustomEventNode(BP, FName(*EventName)));
}

UK2Node_CustomEvent* NeoBlueprint::AddCustomEvent(UBlueprint* BP, const FString& EventName, const TArray<FParamDesc>& Params)
{
	if (!BP) return nullptr;

	// Upsert: find existing or create new
	UK2Node_CustomEvent* EventNode = FindCustomEvent(BP, EventName);
	bool bExisting = (EventNode != nullptr);

	if (!bExisting)
	{
		// Find the EventGraph (first ubergraph page)
		UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
		if (!EventGraph) return nullptr;

		EventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
		EventNode->CustomFunctionName = FName(*EventName);
		EventNode->SetFlags(RF_Transactional);

		EventGraph->Modify();
		EventGraph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		EventNode->CreateNewGuid();
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();
	}
	else if (Params.Num() > 0)
	{
		// Remove existing user-defined pins (keep exec/delegate pins)
		TArray<TSharedPtr<FUserPinInfo>> OldPins = EventNode->UserDefinedPins;
		for (int32 i = OldPins.Num() - 1; i >= 0; i--)
		{
			EventNode->RemoveUserDefinedPin(OldPins[i]);
		}
	}

	// Add parameters as output pins
	for (const FParamDesc& Param : Params)
	{
		FEdGraphPinType PinType;
		if (NeoTypeResolver::ResolveTypeString(Param.Type, PinType))
		{
			EventNode->CreateUserDefinedPin(FName(*Param.Name), PinType, EGPD_Output);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return EventNode;
}

// ============================================================================
// Timelines (upsert — find or create, then apply properties/tracks)
// ============================================================================

static void SetTimelineError(FString* OutError, const FString& Error)
{
	if (OutError)
	{
		*OutError = Error;
	}
}

static bool ValidateTimelineTracks(UTimelineTemplate* Template, const TArray<NeoBlueprint::FTimelineTrackDesc>& Tracks, FString* OutError)
{
	TSet<FName> RequestedTrackNames;
	for (const NeoBlueprint::FTimelineTrackDesc& TrackDesc : Tracks)
	{
		const FName TrackName(*TrackDesc.Name);
		if (TrackName == NAME_None)
		{
			SetTimelineError(OutError, TEXT("track name is required"));
			return false;
		}

		if (RequestedTrackNames.Contains(TrackName))
		{
			SetTimelineError(OutError, FString::Printf(TEXT("track '%s' is duplicated in the requested track list"), *TrackName.ToString()));
			return false;
		}

		if (Template && !Template->IsNewTrackNameValid(TrackName))
		{
			SetTimelineError(OutError, FString::Printf(TEXT("track '%s' is already in use"), *TrackName.ToString()));
			return false;
		}

		RequestedTrackNames.Add(TrackName);
	}

	return true;
}

// Shared helper: add a single track to a template
static bool AddTrackToTemplate(UBlueprint* BP, UTimelineTemplate* Template, const NeoBlueprint::FTimelineTrackDesc& TrackDesc, FString* OutError)
{
	FString TypeLower = TrackDesc.Type.ToLower();
	const FName TrackName(*TrackDesc.Name);
	if (!Template || !Template->IsNewTrackNameValid(TrackName))
	{
		SetTimelineError(OutError, FString::Printf(TEXT("track '%s' is already in use"), *TrackDesc.Name));
		return false;
	}

	if (TypeLower == TEXT("float"))
	{
		UCurveFloat* Curve = NewObject<UCurveFloat>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			Curve->FloatCurve.AddKey(Key.Key, FCString::Atof(*Key.Value));
		}
		Curve->FloatCurve.AutoSetTangents();

		FTTFloatTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveFloat = Curve;
		Track.bIsExternalCurve = false;
		Template->FloatTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, Template->FloatTracks.Num() - 1));
	}
	else if (TypeLower == TEXT("vector"))
	{
		UCurveVector* Curve = NewObject<UCurveVector>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			FVector Vec(ForceInitToZero);
			TArray<FString> Parts;
			Key.Value.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() >= 3)
			{
				Vec.X = FCString::Atof(*Parts[0]);
				Vec.Y = FCString::Atof(*Parts[1]);
				Vec.Z = FCString::Atof(*Parts[2]);
			}
			Curve->FloatCurves[0].AddKey(Key.Key, Vec.X);
			Curve->FloatCurves[1].AddKey(Key.Key, Vec.Y);
			Curve->FloatCurves[2].AddKey(Key.Key, Vec.Z);
		}
		for (int32 i = 0; i < 3; i++) Curve->FloatCurves[i].AutoSetTangents();

		FTTVectorTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveVector = Curve;
		Track.bIsExternalCurve = false;
		Template->VectorTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, Template->VectorTracks.Num() - 1));
	}
	else if (TypeLower == TEXT("color") || TypeLower == TEXT("linearcolor"))
	{
		UCurveLinearColor* Curve = NewObject<UCurveLinearColor>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			TArray<FString> Parts;
			Key.Value.ParseIntoArray(Parts, TEXT(","));
			float R = Parts.Num() >= 1 ? FCString::Atof(*Parts[0]) : 0.f;
			float G = Parts.Num() >= 2 ? FCString::Atof(*Parts[1]) : 0.f;
			float B = Parts.Num() >= 3 ? FCString::Atof(*Parts[2]) : 0.f;
			float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3]) : 1.f;
			Curve->FloatCurves[0].AddKey(Key.Key, R);
			Curve->FloatCurves[1].AddKey(Key.Key, G);
			Curve->FloatCurves[2].AddKey(Key.Key, B);
			Curve->FloatCurves[3].AddKey(Key.Key, A);
		}
		for (int32 i = 0; i < 4; i++) Curve->FloatCurves[i].AutoSetTangents();

		FTTLinearColorTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveLinearColor = Curve;
		Track.bIsExternalCurve = false;
		Template->LinearColorTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, Template->LinearColorTracks.Num() - 1));
	}
	else if (TypeLower == TEXT("event"))
	{
		UCurveFloat* Curve = NewObject<UCurveFloat>(BP, NAME_None, RF_Transactional);
		for (const auto& Key : TrackDesc.Keys)
		{
			Curve->FloatCurve.AddKey(Key.Key, 0.0f);
		}

		FTTEventTrack Track;
		Track.SetTrackName(FName(*TrackDesc.Name), Template);
		Track.CurveKeys = Curve;
		Track.bIsExternalCurve = false;
		Template->EventTracks.Add(Track);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, Template->EventTracks.Num() - 1));
	}

	return true;
}

UK2Node_Timeline* NeoBlueprint::FindTimelineNode(UBlueprint* BP, const FString& TimelineName)
{
	if (!BP) return nullptr;
	// FBlueprintEditorUtils::FindNodeForTimeline (UE 5.7 header L1595, public block) takes the
	// template as input — look it up by name first, then resolve to the placed node.
	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(FName(*TimelineName));
	return Template ? FBlueprintEditorUtils::FindNodeForTimeline(BP, Template) : nullptr;
}

UK2Node_Timeline* NeoBlueprint::AddTimeline(UBlueprint* BP, const FString& TimelineName, float Length,
	bool bAutoPlay, bool bLoop, const TArray<FTimelineTrackDesc>& Tracks, FString* OutError)
{
	if (!BP) return nullptr;

	FName TLName(*TimelineName);

	// Upsert: find existing template or create new
	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(TLName);
	UK2Node_Timeline* TimelineNode = nullptr;
	bool bExisting = (Template != nullptr);

	if (!ValidateTimelineTracks(Template, Tracks, OutError))
	{
		return nullptr;
	}

	if (!bExisting)
	{
		// Find EventGraph
		UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
		if (!EventGraph) return nullptr;

		Template = FBlueprintEditorUtils::AddNewTimeline(BP, TLName);
		if (!Template) return nullptr;

		if (!ValidateTimelineTracks(Template, Tracks, OutError))
		{
			FBlueprintEditorUtils::RemoveTimeline(BP, Template, /*bDontRecompile=*/true);
			return nullptr;
		}

		// Create the timeline node in EventGraph (standard node creation flow)
		TimelineNode = NewObject<UK2Node_Timeline>(EventGraph);
		TimelineNode->CreateNewGuid();
		TimelineNode->TimelineName = TLName;
		TimelineNode->TimelineGuid = Template->TimelineGuid;
		TimelineNode->SetFlags(RF_Transactional);
		TimelineNode->PostPlacedNewNode();
		TimelineNode->AllocateDefaultPins();
		EventGraph->AddNode(TimelineNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	}
	else
	{
		TimelineNode = FindTimelineNode(BP, TimelineName);
	}

	// Update template properties
	Template->TimelineLength = Length;
	Template->bAutoPlay = bAutoPlay;
	Template->bLoop = bLoop;

	// Add tracks (appends — doesn't remove existing tracks)
	for (const FTimelineTrackDesc& TrackDesc : Tracks)
	{
		if (!AddTrackToTemplate(BP, Template, TrackDesc, OutError))
		{
			return nullptr;
		}
	}

	// Sync node properties and reconstruct pins
	if (TimelineNode)
	{
		TimelineNode->bAutoPlay = bAutoPlay;
		TimelineNode->bLoop = bLoop;
		TimelineNode->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return TimelineNode;
}

bool NeoBlueprint::AddTimelineTrack(UBlueprint* BP, const FString& TimelineName, const FTimelineTrackDesc& Track, FString* OutError)
{
	if (!BP) return false;

	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(FName(*TimelineName));
	if (!Template) return false;

	TArray<FTimelineTrackDesc> Tracks;
	Tracks.Add(Track);
	if (!ValidateTimelineTracks(Template, Tracks, OutError))
	{
		return false;
	}

	if (!AddTrackToTemplate(BP, Template, Track, OutError))
	{
		return false;
	}

	// Reconstruct the node to add new pins
	UK2Node_Timeline* Node = FindTimelineNode(BP, TimelineName);
	if (Node)
	{
		Node->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

bool NeoBlueprint::RemoveTimeline(UBlueprint* BP, const FString& TimelineName)
{
	if (!BP) return false;

	UTimelineTemplate* Template = BP->FindTimelineTemplateByVariableName(FName(*TimelineName));
	if (!Template) return false;

	if (UK2Node_Timeline* TimelineNode = FBlueprintEditorUtils::FindNodeForTimeline(BP, Template))
	{
		FBlueprintEditorUtils::RemoveNode(BP, TimelineNode, /*bDontRecompile=*/true);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return true;
	}

	FBlueprintEditorUtils::RemoveTimeline(BP, Template, /*bDontRecompile=*/true);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	return true;
}

// ============================================================================
// Generic Asset Creation
// ============================================================================

// Resolve a class name (short or full path) to a UClass*
static UClass* ResolveClassName(const FString& ClassName)
{
	if (ClassName.IsEmpty()) return nullptr;

	// Common aliases
	static const TMap<FString, FString> ClassAliases = {
		// Regular Blueprint parent classes
		{TEXT("actor"), TEXT("/Script/Engine.Actor")},
		{TEXT("pawn"), TEXT("/Script/Engine.Pawn")},
		{TEXT("character"), TEXT("/Script/Engine.Character")},
		{TEXT("playercontroller"), TEXT("/Script/Engine.PlayerController")},
		{TEXT("gamemode"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("gamemodebase"), TEXT("/Script/Engine.GameModeBase")},
		{TEXT("hud"), TEXT("/Script/Engine.HUD")},
		{TEXT("actorcomponent"), TEXT("/Script/Engine.ActorComponent")},
		{TEXT("scenecomponent"), TEXT("/Script/Engine.SceneComponent")},
		{TEXT("object"), TEXT("/Script/CoreUObject.Object")},
		// AnimBP parent classes
		{TEXT("animinstance"), TEXT("/Script/Engine.AnimInstance")},
		// Data assets
		{TEXT("dataasset"), TEXT("/Script/Engine.DataAsset")},
		{TEXT("primarydataasset"), TEXT("/Script/Engine.PrimaryDataAsset")},
	};

	FString LowerName = ClassName.ToLower();
	if (const FString* FullPath = ClassAliases.Find(LowerName))
	{
		UClass* Found = LoadObject<UClass>(nullptr, **FullPath);
		if (Found) return Found;
	}

	// Shared resolver covers bare name + U/A prefix dance + loaded-object scan.
	if (UClass* Found = NeoTypeResolver::FindClassRobust(ClassName)) return Found;

	// Final fallback: asset path loader for "/Game/..." refs
	if (UClass* Found = LoadObject<UClass>(nullptr, *ClassName)) return Found;

	return nullptr;
}

static UClass* ResolveStateTreeSchemaClass(const FString& SchemaName)
{
	if (SchemaName.IsEmpty()) return nullptr;

	const TCHAR* ModuleNames[] =
	{
		TEXT("StateTreeModule"),
		TEXT("GameplayStateTreeModule"),
		TEXT("GameplayInteractionsModule"),
		TEXT("MassAIBehavior"),
		TEXT("GameplayCameras"),
		TEXT("UAFStateTree"),
	};

	for (const TCHAR* ModuleName : ModuleNames)
	{
		FModuleManager::Get().LoadModulePtr<IModuleInterface>(ModuleName);
	}

	TArray<FString> Names;
	Names.Add(SchemaName);
	if (!SchemaName.StartsWith(TEXT("U")))
	{
		Names.Add(TEXT("U") + SchemaName);
	}
	if (SchemaName.Equals(TEXT("AnimNextStateTreeSchema"), ESearchCase::IgnoreCase))
	{
		Names.Add(TEXT("StateTreeAnimNextSchema"));
		Names.Add(TEXT("UStateTreeAnimNextSchema"));
	}

	FString SearchName = SchemaName;
	if (SchemaName.StartsWith(TEXT("/Script/")))
	{
		if (UClass* DirectClass = LoadObject<UClass>(nullptr, *SchemaName))
		{
			return DirectClass;
		}

		// Users often provide the right class with the wrong module, e.g.
		// /Script/StateTreeModule.StateTreeAIComponentSchema. Keep resolving by class name.
		int32 DotIndex = INDEX_NONE;
		if (SchemaName.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < SchemaName.Len())
		{
			SearchName = SchemaName.Mid(DotIndex + 1);
			Names.Reset();
			Names.Add(SearchName);
			if (!SearchName.StartsWith(TEXT("U")))
			{
				Names.Add(TEXT("U") + SearchName);
			}
		}
	}

	static const TCHAR* SchemaPaths[] =
	{
		TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema"),
		TEXT("/Script/GameplayStateTreeModule.StateTreeAIComponentSchema"),
		TEXT("/Script/GameplayInteractionsModule.GameplayInteractionStateTreeSchema"),
		TEXT("/Script/MassAIBehavior.MassStateTreeSchema"),
		TEXT("/Script/GameplayCameras.CameraDirectorStateTreeSchema"),
		TEXT("/Script/UAFStateTree.StateTreeAnimNextSchema"),
	};

	for (const TCHAR* SchemaPath : SchemaPaths)
	{
		if (UClass* Class = LoadObject<UClass>(nullptr, SchemaPath))
		{
			for (const FString& Name : Names)
			{
				if (Class->GetName().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Class;
				}
			}
		}
	}

	if (UClass* Class = ResolveClassName(SearchName))
	{
		return Class;
	}

	for (const FString& Name : Names)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
	}

	return nullptr;
}

static FString BuildStateTreeSchemaResolutionError(const FString& SchemaName)
{
	const bool bGameplayStateTreeLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("GameplayStateTreeModule"));
	const TSharedPtr<IPlugin> GameplayStateTreePlugin = IPluginManager::Get().FindPlugin(TEXT("GameplayStateTree"));
	const FString GameplayStateTreeStatus = GameplayStateTreePlugin.IsValid()
		? (GameplayStateTreePlugin->IsEnabled() ? TEXT("enabled") : TEXT("disabled"))
		: TEXT("not found");

	return FString::Printf(TEXT("Could not find StateTree schema class \"%s\". ")
		TEXT("Common schemas: /Script/GameplayStateTreeModule.StateTreeComponentSchema, ")
		TEXT("/Script/GameplayStateTreeModule.StateTreeAIComponentSchema, ")
		TEXT("/Script/GameplayInteractionsModule.GameplayInteractionStateTreeSchema, ")
		TEXT("/Script/MassAIBehavior.MassStateTreeSchema, ")
		TEXT("/Script/GameplayCameras.CameraDirectorStateTreeSchema. ")
		TEXT("AI/component schemas require the GameplayStateTree plugin/module (plugin: %s, module loaded: %s)."),
		*SchemaName,
		*GameplayStateTreeStatus,
		bGameplayStateTreeLoaded ? TEXT("yes") : TEXT("no"));
}

// Known asset type aliases -> UClass path for supported class
static const TMap<FString, FString>& GetAssetTypeAliases()
{
	static const TMap<FString, FString> Aliases = {
		// Blueprints
		{TEXT("blueprint"), TEXT("/Script/Engine.Blueprint")},
		{TEXT("bp"), TEXT("/Script/Engine.Blueprint")},
		{TEXT("animblueprint"), TEXT("/Script/Engine.AnimBlueprint")},
		{TEXT("animbp"), TEXT("/Script/Engine.AnimBlueprint")},
		{TEXT("widgetblueprint"), TEXT("/Script/UMGEditor.WidgetBlueprint")},
		{TEXT("widget"), TEXT("/Script/UMGEditor.WidgetBlueprint")},
		// Data
		{TEXT("datatable"), TEXT("/Script/Engine.DataTable")},
		{TEXT("curvetable"), TEXT("/Script/Engine.CurveTable")},
		{TEXT("dataasset"), TEXT("/Script/Engine.DataAsset")},
		{TEXT("stringtable"), TEXT("/Script/Engine.StringTable")},
		// Structures & Enums
		{TEXT("userdefinedenum"), TEXT("/Script/Engine.UserDefinedEnum")},
		{TEXT("enum"), TEXT("/Script/Engine.UserDefinedEnum")},
		{TEXT("userdefinedstruct"), TEXT("/Script/Engine.UserDefinedStruct")},
		{TEXT("struct"), TEXT("/Script/Engine.UserDefinedStruct")},
		// Animation
		{TEXT("animsequence"), TEXT("/Script/Engine.AnimSequence")},
		{TEXT("animmontage"), TEXT("/Script/Engine.AnimMontage")},
		{TEXT("animcomposite"), TEXT("/Script/Engine.AnimComposite")},
		{TEXT("blendspace"), TEXT("/Script/Engine.BlendSpace")},
		{TEXT("blendspace1d"), TEXT("/Script/Engine.BlendSpace1D")},
		{TEXT("aimoffsetblendspace"), TEXT("/Script/Engine.AimOffsetBlendSpace")},
		{TEXT("aimoffsetblendspace1d"), TEXT("/Script/Engine.AimOffsetBlendSpace1D")},
		{TEXT("aimoffset"), TEXT("/Script/Engine.AimOffsetBlendSpace")},
		{TEXT("aimoffset1d"), TEXT("/Script/Engine.AimOffsetBlendSpace1D")},
		{TEXT("poseasset"), TEXT("/Script/Engine.PoseAsset")},
		{TEXT("mirrordatatable"), TEXT("/Script/Engine.MirrorDataTable")},
		{TEXT("mirrortable"), TEXT("/Script/Engine.MirrorDataTable")},
		{TEXT("skeleton"), TEXT("/Script/Engine.Skeleton")},
		// Materials
		{TEXT("material"), TEXT("/Script/Engine.Material")},
		{TEXT("materialinstance"), TEXT("/Script/Engine.MaterialInstanceConstant")},
		{TEXT("materialfunction"), TEXT("/Script/Engine.MaterialFunction")},
		{TEXT("texturerendertarget2d"), TEXT("/Script/Engine.TextureRenderTarget2D")},
		{TEXT("rendertarget2d"), TEXT("/Script/Engine.TextureRenderTarget2D")},
		{TEXT("canvasrendertarget2d"), TEXT("/Script/Engine.CanvasRenderTarget2D")},
		// Physics
		{TEXT("physicsasset"), TEXT("/Script/Engine.PhysicsAsset")},
		{TEXT("physicsmaterial"), TEXT("/Script/PhysicsCore.PhysicalMaterial")},
		// Curves
		{TEXT("curvefloat"), TEXT("/Script/Engine.CurveFloat")},
		{TEXT("curvevector"), TEXT("/Script/Engine.CurveVector")},
		{TEXT("curvelinearcolor"), TEXT("/Script/Engine.CurveLinearColor")},
		// AI
		{TEXT("behaviortree"), TEXT("/Script/AIModule.BehaviorTree")},
		{TEXT("bt"), TEXT("/Script/AIModule.BehaviorTree")},
		{TEXT("blackboarddata"), TEXT("/Script/AIModule.BlackboardData")},
		{TEXT("blackboard"), TEXT("/Script/AIModule.BlackboardData")},
		{TEXT("statetree"), TEXT("/Script/StateTreeModule.StateTree")},
		// EQS
		{TEXT("environmentquery"), TEXT("/Script/AIModule.EnvQuery")},
		{TEXT("envquery"), TEXT("/Script/AIModule.EnvQuery")},
		{TEXT("eqs"), TEXT("/Script/AIModule.EnvQuery")},
		// Sequencer
		{TEXT("levelsequence"), TEXT("/Script/LevelSequence.LevelSequence")},
		{TEXT("materialinstanceconstant"), TEXT("/Script/Engine.MaterialInstanceConstant")},
		// IK
		{TEXT("ikrig"), TEXT("/Script/IKRig.IKRigDefinition")},
		{TEXT("ikretargeter"), TEXT("/Script/IKRig.IKRetargeter")},
		// Editor utilities
		{TEXT("editorutilityblueprint"), TEXT("/Script/Blutility.EditorUtilityBlueprint")},
		{TEXT("editorutilitybp"), TEXT("/Script/Blutility.EditorUtilityBlueprint")},
		{TEXT("editorutilitywidgetblueprint"), TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint")},
		{TEXT("editorutilitywidget"), TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint")},
		// ControlRig
		{TEXT("controlrigblueprint"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint")},
		{TEXT("controlrig"), TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint")},
		// Enhanced Input
		{TEXT("inputaction"), TEXT("/Script/EnhancedInput.InputAction")},
		{TEXT("inputmappingcontext"), TEXT("/Script/EnhancedInput.InputMappingContext")},
			// Chooser
			{TEXT("choosertable"), TEXT("/Script/Chooser.ChooserTable")},
			{TEXT("chooser"), TEXT("/Script/Chooser.ChooserTable")},
			{TEXT("proxyasset"), TEXT("/Script/ProxyTable.ProxyAsset")},
			{TEXT("proxytable"), TEXT("/Script/ProxyTable.ProxyTable")},
			// Sound
		{TEXT("soundcue"), TEXT("/Script/Engine.SoundCue")},
		{TEXT("audiobus"), TEXT("/Script/Engine.AudioBus")},
		{TEXT("soundsourcebus"), TEXT("/Script/Engine.SoundSourceBus")},
		{TEXT("sourcebus"), TEXT("/Script/Engine.SoundSourceBus")},
		{TEXT("soundsubmix"), TEXT("/Script/Engine.SoundSubmix")},
		{TEXT("submixeffectreverbpreset"), TEXT("/Script/AudioMixer.SubmixEffectReverbPreset")},
		{TEXT("soundsubmixeffectreverbpreset"), TEXT("/Script/AudioMixer.SubmixEffectReverbPreset")},
		{TEXT("submixeffecteqpreset"), TEXT("/Script/AudioMixer.SubmixEffectSubmixEQPreset")},
		{TEXT("submixeffectsubmixeqpreset"), TEXT("/Script/AudioMixer.SubmixEffectSubmixEQPreset")},
		{TEXT("submixeffectdynamicspreset"), TEXT("/Script/AudioMixer.SubmixEffectDynamicsProcessorPreset")},
		{TEXT("submixeffectdynamicsprocessorpreset"), TEXT("/Script/AudioMixer.SubmixEffectDynamicsProcessorPreset")},
		{TEXT("sourceeffectchain"), TEXT("/Script/Engine.SoundEffectSourcePresetChain")},
		{TEXT("soundeffectsourcepresetchain"), TEXT("/Script/Engine.SoundEffectSourcePresetChain")},
		// Paper2D
		{TEXT("papersprite"), TEXT("/Script/Paper2D.PaperSprite")},
		{TEXT("paperflipbook"), TEXT("/Script/Paper2D.PaperFlipbook")},
		{TEXT("papertileset"), TEXT("/Script/Paper2D.PaperTileSet")},
		{TEXT("papertilemap"), TEXT("/Script/Paper2D.PaperTileMap")},
		// SmartObjects
		{TEXT("smartobjectdefinition"), TEXT("/Script/SmartObjectsModule.SmartObjectDefinition")},
		{TEXT("smartobject"), TEXT("/Script/SmartObjectsModule.SmartObjectDefinition")},
		// Foliage / Landscape
		{TEXT("foliagetype"), TEXT("/Script/Foliage.FoliageType_InstancedStaticMesh")},
		{TEXT("instancedstaticmeshfoliagetype"), TEXT("/Script/Foliage.FoliageType_InstancedStaticMesh")},
		{TEXT("foliagetypeinstancedstaticmesh"), TEXT("/Script/Foliage.FoliageType_InstancedStaticMesh")},
		{TEXT("landscapegrasstype"), TEXT("/Script/Landscape.LandscapeGrassType")},
		// Cloth / Outfit
		{TEXT("chaosclothasset"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAsset")},
		{TEXT("clothasset"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAsset")},
		{TEXT("chaosoutfitasset"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfitAsset")},
		{TEXT("outfitasset"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfitAsset")},
		// MetaHuman wardrobe
		{TEXT("metahumanwardrobeitem"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem")},
		{TEXT("wardrobeitem"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem")},
		// Gameplay Abilities
		{TEXT("gameplayability"), TEXT("/Script/GameplayAbilities.GameplayAbilityBlueprint")},
		{TEXT("gameplayabilityblueprint"), TEXT("/Script/GameplayAbilities.GameplayAbilityBlueprint")},
		{TEXT("gameplayabilitybp"), TEXT("/Script/GameplayAbilities.GameplayAbilityBlueprint")},
		{TEXT("ga"), TEXT("/Script/GameplayAbilities.GameplayAbilityBlueprint")},
		// HLOD
		{TEXT("hlodlayer"), TEXT("/Script/Engine.HLODLayer")},
		// Movie Render Pipeline
		{TEXT("moviepipelineprimaryconfig"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelinePrimaryConfig")},
		{TEXT("moviegraphconfig"), TEXT("/Script/MovieRenderPipelineCore.MovieGraphConfig")},
		// Motion Design / Avalanche
		{TEXT("avatransitiontree"), TEXT("/Script/AvalancheTransition.AvaTransitionTree")},
		{TEXT("motiondesigntransitiontree"), TEXT("/Script/AvalancheTransition.AvaTransitionTree")},
		{TEXT("avatagcollection"), TEXT("/Script/AvalancheTag.AvaTagCollection")},
		{TEXT("motiondesigntagcollection"), TEXT("/Script/AvalancheTag.AvaTagCollection")},
		{TEXT("avarundown"), TEXT("/Script/AvalancheMedia.AvaRundown")},
		{TEXT("motiondesignrundown"), TEXT("/Script/AvalancheMedia.AvaRundown")},
		{TEXT("avarundownmacrocollection"), TEXT("/Script/AvalancheMedia.AvaRundownMacroCollection")},
		{TEXT("avaplaybackgraph"), TEXT("/Script/AvalancheMedia.AvaPlaybackGraph")},
		// Virtual production / enterprise authoring assets
		{TEXT("remotecontrolpreset"), TEXT("/Script/RemoteControl.RemoteControlPreset")},
		{TEXT("levelvariantsets"), TEXT("/Script/VariantManagerContent.LevelVariantSets")},
		{TEXT("dataprepasset"), TEXT("/Script/DataprepCore.DataprepAsset")},
		{TEXT("dataprepassetinstance"), TEXT("/Script/DataprepCore.DataprepAssetInstance")},
		// Cameras / deformation / cached geometry
		{TEXT("cameraasset"), TEXT("/Script/GameplayCameras.CameraAsset")},
		{TEXT("camerarigasset"), TEXT("/Script/GameplayCameras.CameraRigAsset")},
		{TEXT("geometrycache"), TEXT("/Script/GeometryCache.GeometryCache")},
		{TEXT("groomasset"), TEXT("/Script/HairStrandsCore.GroomAsset")},
		{TEXT("mldeformerasset"), TEXT("/Script/MLDeformerFramework.MLDeformerAsset")},
		{TEXT("optimusdeformer"), TEXT("/Script/OptimusCore.OptimusDeformer")},
	};
	return Aliases;
}

// Known required properties for common factory types
struct FFactoryRequirement
{
	FString PropertyName;
	FString Description;
	bool bRequired;
};

static TArray<FFactoryRequirement> GetKnownRequirements(const FString& AssetTypeLower)
{
	TArray<FFactoryRequirement> Reqs;

	if (AssetTypeLower == TEXT("animblueprint") || AssetTypeLower == TEXT("animbp"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("TSubclassOf<UAnimInstance> — e.g. \"AnimInstance\" or custom subclass"), true});
		Reqs.Add({TEXT("TargetSkeleton"), TEXT("USkeleton asset path — e.g. \"/Game/Characters/SK_Mesh\""), false});
		Reqs.Add({TEXT("bTemplate"), TEXT("bool — set true to skip TargetSkeleton"), false});
	}
	else if (AssetTypeLower == TEXT("blueprint") || AssetTypeLower == TEXT("bp"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("Parent class — e.g. \"Actor\", \"Character\", full path. Defaults to Actor"), false});
	}
	else if (AssetTypeLower == TEXT("datatable"))
	{
		Reqs.Add({TEXT("Struct"), TEXT("UScriptStruct — row struct e.g. \"/Script/MyGame.FMyRow\""), true});
	}
	else if (AssetTypeLower == TEXT("dataasset"))
	{
		Reqs.Add({TEXT("DataAssetClass"), TEXT("TSubclassOf<UDataAsset> — e.g. \"PrimaryDataAsset\""), true});
	}
	else if (AssetTypeLower == TEXT("poseasset"))
	{
		Reqs.Add({TEXT("SourceAnimation"), TEXT("UAnimSequence asset path used by UPoseAssetFactory to generate poses non-interactively"), true});
		Reqs.Add({TEXT("TargetSkeleton"), TEXT("USkeleton asset path — accepted for compatibility; SourceAnimation determines the PoseAsset skeleton"), false});
	}
	else if (AssetTypeLower == TEXT("mirrordatatable") || AssetTypeLower == TEXT("mirrortable"))
	{
		Reqs.Add({TEXT("Struct"), TEXT("UScriptStruct row struct, normally /Script/Engine.MirrorTableRow"), true});
		Reqs.Add({TEXT("Skeleton"), TEXT("USkeleton asset path used by UMirrorDataTableFactory"), true});
	}
	else if (AssetTypeLower == TEXT("animsequence") || AssetTypeLower == TEXT("animmontage") ||
		AssetTypeLower == TEXT("animcomposite") || AssetTypeLower == TEXT("blendspace") ||
		AssetTypeLower == TEXT("blendspace1d") ||
		AssetTypeLower == TEXT("aimoffsetblendspace") || AssetTypeLower == TEXT("aimoffsetblendspace1d") ||
		AssetTypeLower == TEXT("aimoffset") || AssetTypeLower == TEXT("aimoffset1d"))
	{
		Reqs.Add({TEXT("TargetSkeleton"), TEXT("USkeleton asset path — e.g. \"/Game/Characters/SK_Mesh\""), true});
	}
	else if (AssetTypeLower == TEXT("physicsasset") || AssetTypeLower == TEXT("skeleton"))
	{
		Reqs.Add({TEXT("TargetSkeletalMesh"), TEXT("USkeletalMesh asset path"), true});
	}
	else if (AssetTypeLower == TEXT("materialinstance") || AssetTypeLower == TEXT("materialinstanceconstant"))
	{
		Reqs.Add({TEXT("ParentMaterial"), TEXT("Parent material path — e.g. \"/Game/Materials/M_Base\""), false});
	}
	else if (AssetTypeLower == TEXT("functionlibrary") || AssetTypeLower == TEXT("macrolibrary") ||
		AssetTypeLower == TEXT("animlayerinterface"))
	{
		// Handled as special Blueprint sub-types, no factory requirements
	}
	else if (AssetTypeLower == TEXT("editorutilityblueprint") || AssetTypeLower == TEXT("editorutilitybp"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("Parent class — defaults to EditorUtilityObject"), false});
	}
	else if (AssetTypeLower == TEXT("editorutilitywidgetblueprint") || AssetTypeLower == TEXT("editorutilitywidget"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("Parent class — defaults to EditorUtilityWidget"), false});
	}
	else if (AssetTypeLower == TEXT("gameplayability") || AssetTypeLower == TEXT("gameplayabilityblueprint") ||
		AssetTypeLower == TEXT("gameplayabilitybp") || AssetTypeLower == TEXT("ga"))
	{
		Reqs.Add({TEXT("ParentClass"), TEXT("TSubclassOf<UGameplayAbility> — e.g. \"GameplayAbility\" or a custom subclass path. Defaults to GameplayAbility"), false});
	}

	return Reqs;
}

static const FString* FindOptionCaseInsensitive(const TMap<FString, FString>& Options, const TCHAR* Key)
{
	if (const FString* Exact = Options.Find(Key))
	{
		return Exact;
	}

	for (const TPair<FString, FString>& Pair : Options)
	{
		if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

static bool ReadBoolOption(const TMap<FString, FString>& Options, const TCHAR* Key, bool DefaultValue)
{
	const FString* Value = FindOptionCaseInsensitive(Options, Key);
	if (!Value)
	{
		return DefaultValue;
	}
	return Value->ToBool();
}

static int32 ReadIntOption(const TMap<FString, FString>& Options, const TCHAR* Key, int32 DefaultValue)
{
	const FString* Value = FindOptionCaseInsensitive(Options, Key);
	return Value ? FCString::Atoi(**Value) : DefaultValue;
}

static float ReadFloatOption(const TMap<FString, FString>& Options, const TCHAR* Key, float DefaultValue)
{
	const FString* Value = FindOptionCaseInsensitive(Options, Key);
	return Value ? FCString::Atof(**Value) : DefaultValue;
}

static EPhysAssetFitGeomType ParsePhysicsAssetGeomType(const FString& Value)
{
	if (Value.Equals(TEXT("box"), ESearchCase::IgnoreCase)) return EFG_Box;
	if (Value.Equals(TEXT("sphere"), ESearchCase::IgnoreCase)) return EFG_Sphere;
	if (Value.Equals(TEXT("capsule"), ESearchCase::IgnoreCase)) return EFG_Sphyl;
	if (Value.Equals(TEXT("sphyl"), ESearchCase::IgnoreCase)) return EFG_Sphyl;
	if (Value.Equals(TEXT("tapered_capsule"), ESearchCase::IgnoreCase)) return EFG_TaperedCapsule;
	if (Value.Equals(TEXT("single_convex"), ESearchCase::IgnoreCase)) return EFG_SingleConvexHull;
	if (Value.Equals(TEXT("single_convex_hull"), ESearchCase::IgnoreCase)) return EFG_SingleConvexHull;
	if (Value.Equals(TEXT("multi_convex"), ESearchCase::IgnoreCase)) return EFG_MultiConvexHull;
	if (Value.Equals(TEXT("multi_convex_hull"), ESearchCase::IgnoreCase)) return EFG_MultiConvexHull;
	return EFG_Sphyl;
}

// Find a factory that supports creating the given asset class
static UFactory* FindFactoryForClass(UClass* AssetClass)
{
	if (!AssetClass) return nullptr;

	UClass* BestFactoryClass = nullptr;
	bool bBestIsExact = false;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* FactoryClass = *It;
		if (!FactoryClass->IsChildOf(UFactory::StaticClass()) ||
			FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		UFactory* CDO = FactoryClass->GetDefaultObject<UFactory>();
		if (!CDO->CanCreateNew()) continue;

		UClass* Supported = CDO->GetSupportedClass();
		if (!Supported) continue;

		if (AssetClass == Supported)
		{
			// Exact match — use immediately
			BestFactoryClass = FactoryClass;
			bBestIsExact = true;
			break;
		}
		else if (AssetClass->IsChildOf(Supported) && !bBestIsExact)
		{
			// Compatible match — keep first found
			if (!BestFactoryClass)
			{
				BestFactoryClass = FactoryClass;
			}
		}
	}

	if (!BestFactoryClass) return nullptr;
	return NewObject<UFactory>(GetTransientPackage(), BestFactoryClass);
}

// Collect UPROPERTY fields from a factory for error reporting
static void CollectFactoryProperties(UFactory* Factory, TArray<FString>& OutProps)
{
	if (!Factory) return;

	for (TFieldIterator<FProperty> PropIt(Factory->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		// Skip inherited UFactory base properties
		if (Prop->GetOwnerClass() == UFactory::StaticClass()) continue;

		FString Value;
		Prop->ExportText_InContainer(0, Value, Factory, Factory, Factory, PPF_None);

		FString TypeName = Prop->GetCPPType();
		FString Entry = FString::Printf(TEXT("  %s : %s = %s"),
			*Prop->GetName(), *TypeName, Value.IsEmpty() ? TEXT("(empty)") : *Value);
		OutProps.Add(Entry);
	}
}

void NeoBlueprint::ListFactoryProperties(const FString& AssetTypeName, TArray<TPair<FString, FString>>& OutProps)
{
	FString LowerType = AssetTypeName.ToLower();
	const TMap<FString, FString>& Aliases = GetAssetTypeAliases();

	UClass* AssetClass = nullptr;
	if (const FString* ClassPath = Aliases.Find(LowerType))
	{
		AssetClass = LoadObject<UClass>(nullptr, **ClassPath);
	}
	if (!AssetClass)
	{
		FString ExtensionClassPath;
		if (NeoAssetTypeAliases::ResolveAlias(LowerType, ExtensionClassPath))
		{
			AssetClass = LoadObject<UClass>(nullptr, *ExtensionClassPath);
		}
	}
	if (!AssetClass)
	{
		AssetClass = ResolveClassName(AssetTypeName);
	}
	if (!AssetClass) return;

	UFactory* Factory = FindFactoryForClass(AssetClass);
	if (!Factory) return;

	for (TFieldIterator<FProperty> PropIt(Factory->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Prop->GetOwnerClass() == UFactory::StaticClass()) continue;

		OutProps.Add(TPair<FString, FString>(Prop->GetName(), Prop->GetCPPType()));
	}
}

void NeoBlueprint::ListAssetTypeAliases(TArray<TPair<FString, FString>>& OutTypes)
{
	const TMap<FString, FString>& Aliases = GetAssetTypeAliases();
	for (const auto& Pair : Aliases)
	{
		OutTypes.Add(TPair<FString, FString>(Pair.Key, Pair.Value));
	}
	// Add special types not in the alias map
	OutTypes.Add(TPair<FString, FString>(TEXT("interface"), TEXT("Blueprint Interface")));
	OutTypes.Add(TPair<FString, FString>(TEXT("functionlibrary"), TEXT("Blueprint Function Library")));
	OutTypes.Add(TPair<FString, FString>(TEXT("macrolibrary"), TEXT("Blueprint Macro Library")));
	OutTypes.Add(TPair<FString, FString>(TEXT("animlayerinterface"), TEXT("AnimLayer Interface Blueprint")));
	NeoAssetTypeAliases::ListAliases(OutTypes);
}

NeoBlueprint::FCreateAssetResult NeoBlueprint::CreateAssetGeneric(const FString& AssetPath,
	const FString& AssetTypeName, const TMap<FString, FString>& Options)
{
	FCreateAssetResult Result;

	// ---- Validate path ----
	if (AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
	{
		Result.Error = FString::Printf(TEXT("Invalid asset path \"%s\" — must be an absolute content path (e.g. \"/Game/MyFolder/MyAsset\")."), *AssetPath);
		return Result;
	}
	FText PathReason;
	if (!FPackageName::IsValidLongPackageName(AssetPath, false, &PathReason))
	{
		Result.Error = FString::Printf(TEXT("Invalid asset path \"%s\" — %s"), *AssetPath, *PathReason.ToString());
		return Result;
	}

	// ---- Check for existing asset ----
	const FString AssetObjectPath = FString::Printf(TEXT("%s.%s"),
		*AssetPath, *FPackageName::GetLongPackageAssetName(AssetPath));
	if (StaticFindObject(UObject::StaticClass(), nullptr, *AssetObjectPath)
		|| FPackageName::DoesPackageExist(AssetPath))
	{
		Result.Error = FString::Printf(TEXT("Asset already exists at \"%s\". Use duplicate_asset() to copy, or choose a different path."), *AssetPath);
		return Result;
	}

	FString LowerType = AssetTypeName.ToLower();
	const TMap<FString, FString>& Aliases = GetAssetTypeAliases();

	// ---- Resolve asset type to UClass ----
	UClass* AssetClass = nullptr;
	if (const FString* ClassPath = Aliases.Find(LowerType))
	{
		AssetClass = LoadObject<UClass>(nullptr, **ClassPath);
	}
	if (!AssetClass)
	{
		FString ExtensionClassPath;
		if (NeoAssetTypeAliases::ResolveAlias(LowerType, ExtensionClassPath))
		{
			AssetClass = LoadObject<UClass>(nullptr, *ExtensionClassPath);
		}
	}
	if (!AssetClass)
	{
		AssetClass = ResolveClassName(AssetTypeName);
	}
	if (!AssetClass)
	{
		// Allow types that are handled as special cases below without needing a UClass
		static const TSet<FString> SpecialCaseTypes = {
			TEXT("interface"), TEXT("blueprintinterface"),
			TEXT("functionlibrary"), TEXT("macrolibrary"),
			TEXT("animlayerinterface")
		};
		if (!SpecialCaseTypes.Contains(LowerType))
		{
			Result.Error = FString::Printf(TEXT("Unknown asset type \"%s\". Common types: Blueprint, AnimBlueprint, DataTable, AnimSequence, AnimMontage, Material, MaterialInstance, Enum, Struct, PhysicsAsset, BlendSpace, CurveFloat, DataAsset, BehaviorTree, StateTree, LevelSequence, EQS, FunctionLibrary, MacroLibrary, AnimLayerInterface"),
				*AssetTypeName);
			return Result;
		}
	}

	// ---- Special cases: types that don't use factories ----

	// Interface Blueprint
	if (LowerType == TEXT("interface") || LowerType == TEXT("blueprintinterface"))
	{
		Result.Asset = CreateInterfaceBlueprint(AssetPath);
		if (!Result.Asset) Result.Error = TEXT("Failed to create Interface Blueprint");
		return Result;
	}

	// Material — no factory, just NewObject
	if (AssetClass && AssetClass->IsChildOf(UMaterial::StaticClass()) && LowerType == TEXT("material"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = NewObject<UMaterial>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create Material");
		return Result;
	}

	// UserDefinedEnum — use FEnumEditorUtils for proper initialization
	if (AssetClass == UUserDefinedEnum::StaticClass())
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create UserDefinedEnum");
		return Result;
	}

	// UserDefinedStruct — no factory
	if (AssetClass == UUserDefinedStruct::StaticClass())
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create UserDefinedStruct");
		return Result;
	}

	// Generic NewObject creation for asset types that have no factory (PCGGraph, etc.)
	// These are simple UObject subclasses that just need NewObject + asset registration.
	if (AssetClass && !AssetClass->HasAnyClassFlags(CLASS_Abstract)
		&& !AssetClass->IsChildOf(UBlueprint::StaticClass())
		&& !AssetClass->IsChildOf(AActor::StaticClass()))
	{
		// Check if any factory can produce this class — if not, use NewObject
		bool bHasFactory = false;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UFactory::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
			{
				UFactory* FactoryCDO = It->GetDefaultObject<UFactory>();
				if (FactoryCDO && FactoryCDO->SupportedClass == AssetClass)
				{
					bHasFactory = true;
					break;
				}
			}
		}

		if (!bHasFactory)
		{
			FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
			UPackage* Package = CreatePackage(*AssetPath);
			if (Package)
			{
				Result.Asset = NewObject<UObject>(Package, AssetClass, FName(*AssetName),
					RF_Public | RF_Standalone | RF_Transactional);
				if (Result.Asset)
				{
					FAssetRegistryModule::AssetCreated(Result.Asset);
					Package->MarkPackageDirty();
				}
			}
			if (!Result.Asset) Result.Error = FString::Printf(TEXT("Failed to create %s via NewObject"), *AssetTypeName);
			return Result;
		}
	}

	// ---- Check known requirements before creating factory ----
	TArray<FFactoryRequirement> KnownReqs = GetKnownRequirements(LowerType);
	for (const FFactoryRequirement& Req : KnownReqs)
	{
		if (Req.bRequired && !FindOptionCaseInsensitive(Options, *Req.PropertyName))
		{
			// Build helpful error with all requirements
			Result.Error = FString::Printf(TEXT("Missing required property \"%s\" (%s)."),
				*Req.PropertyName, *Req.Description);

			FString AllReqs;
			for (const FFactoryRequirement& R : KnownReqs)
			{
				AllReqs += FString::Printf(TEXT("\n  %s%s : %s"),
					*R.PropertyName, R.bRequired ? TEXT(" [REQUIRED]") : TEXT(""),
					*R.Description);
			}
			Result.Error += FString::Printf(TEXT("\nProperties for %s:%s"), *AssetTypeName, *AllReqs);
			return Result;
		}
	}

	// ---- PhysicsAsset special case ----
	// UPhysicsAssetFactory opens two modal dialogs even when TargetSkeletalMesh is
	// already set: a skeletal mesh picker from ConfigureProperties and the "New
	// Physics Asset" generation-options dialog from CreatePhysicsAssetFromMesh.
	// Agent/tool calls must be non-interactive, so create the asset directly and
	// call the same lower-level generator used by USkeletalMeshEditorSubsystem.
	if (LowerType == TEXT("physicsasset"))
	{
		const FString* TargetMeshPath = FindOptionCaseInsensitive(Options, TEXT("TargetSkeletalMesh"));
		USkeletalMesh* TargetMesh = TargetMeshPath
			? NeoLuaAsset::ResolveWithRegistry<USkeletalMesh>(*TargetMeshPath)
			: nullptr;
		if (!TargetMesh)
		{
			Result.Error = FString::Printf(TEXT("Could not resolve TargetSkeletalMesh \"%s\". Pass a USkeletalMesh asset path such as /Game/Characters/SK_Mannequin."),
				TargetMeshPath ? **TargetMeshPath : TEXT("(missing)"));
			return Result;
		}

		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			Result.Error = FString::Printf(TEXT("Failed to create package for PhysicsAsset \"%s\"."), *AssetPath);
			return Result;
		}

		UPhysicsAsset* NewPhysicsAsset = NewObject<UPhysicsAsset>(Package, FName(*AssetName),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!NewPhysicsAsset)
		{
			Result.Error = FString::Printf(TEXT("Failed to allocate PhysicsAsset \"%s\"."), *AssetPath);
			return Result;
		}

		FPhysAssetCreateParams CreateParams;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		CreateParams.LodIndex = ReadIntOption(Options, TEXT("LodIndex"), ReadIntOption(Options, TEXT("lod_index"), 0));
#endif
		CreateParams.MinBoneSize = ReadFloatOption(Options, TEXT("MinBoneSize"), ReadFloatOption(Options, TEXT("min_bone_size"), CreateParams.MinBoneSize));
		CreateParams.MinWeldSize = ReadFloatOption(Options, TEXT("MinWeldSize"), ReadFloatOption(Options, TEXT("min_weld_size"), CreateParams.MinWeldSize));
		CreateParams.bAutoOrientToBone = ReadBoolOption(Options, TEXT("bAutoOrientToBone"), ReadBoolOption(Options, TEXT("auto_orient_to_bone"), CreateParams.bAutoOrientToBone));
		CreateParams.bCreateConstraints = ReadBoolOption(Options, TEXT("bCreateConstraints"), ReadBoolOption(Options, TEXT("create_constraints"), CreateParams.bCreateConstraints));
		CreateParams.bWalkPastSmall = ReadBoolOption(Options, TEXT("bWalkPastSmall"), ReadBoolOption(Options, TEXT("walk_past_small"), CreateParams.bWalkPastSmall));
		CreateParams.bBodyForAll = ReadBoolOption(Options, TEXT("bBodyForAll"), ReadBoolOption(Options, TEXT("body_for_all"), CreateParams.bBodyForAll));
		CreateParams.bDisableCollisionsByDefault = ReadBoolOption(Options, TEXT("bDisableCollisionsByDefault"), ReadBoolOption(Options, TEXT("disable_collisions"), CreateParams.bDisableCollisionsByDefault));
		CreateParams.HullCount = ReadIntOption(Options, TEXT("HullCount"), ReadIntOption(Options, TEXT("hull_count"), CreateParams.HullCount));
		CreateParams.MaxHullVerts = ReadIntOption(Options, TEXT("MaxHullVerts"), ReadIntOption(Options, TEXT("max_hull_verts"), CreateParams.MaxHullVerts));
		if (const FString* GeomType = FindOptionCaseInsensitive(Options, TEXT("GeomType")))
		{
			CreateParams.GeomType = ParsePhysicsAssetGeomType(*GeomType);
		}
		else if (const FString* GeomTypeSnake = FindOptionCaseInsensitive(Options, TEXT("geom_type")))
		{
			CreateParams.GeomType = ParsePhysicsAssetGeomType(*GeomTypeSnake);
		}

		const bool bSetToMesh = ReadBoolOption(Options, TEXT("bSetToMesh"), ReadBoolOption(Options, TEXT("set_to_mesh"), true));
		FText ErrorMessage;
		const bool bSuccess = FPhysicsAssetUtils::CreateFromSkeletalMesh(
			NewPhysicsAsset,
			TargetMesh,
			CreateParams,
			ErrorMessage,
			bSetToMesh,
			/*bShowProgress=*/false);
		if (!bSuccess)
		{
			NewPhysicsAsset->ClearFlags(RF_Public | RF_Standalone);
			NewPhysicsAsset->MarkAsGarbage();
			Result.Error = ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Could not generate PhysicsAsset \"%s\" from SkeletalMesh \"%s\"."), *AssetPath, *TargetMesh->GetPathName())
				: ErrorMessage.ToString();
			return Result;
		}

		FAssetRegistryModule::AssetCreated(NewPhysicsAsset);
		Package->MarkPackageDirty();
		if (bSetToMesh)
		{
			TargetMesh->MarkPackageDirty();
		}
		Result.Asset = NewPhysicsAsset;
		return Result;
	}

	// ---- StateTree special case — factory requires a schema class ----
	if (LowerType == TEXT("statetree"))
	{
		// Resolve schema class from options, default to UStateTreeComponentSchema (most common)
		FString SchemaStr = TEXT("StateTreeComponentSchema");
		if (const FString* Val = FindOptionCaseInsensitive(Options, TEXT("StateTreeSchema")))
		{
			SchemaStr = *Val;
		}
		else if (const FString* SchemaVal = FindOptionCaseInsensitive(Options, TEXT("Schema")))
		{
			SchemaStr = *SchemaVal;
		}

		UClass* SchemaClass = ResolveStateTreeSchemaClass(SchemaStr);
		if (!SchemaClass)
		{
			Result.Error = BuildStateTreeSchemaResolutionError(SchemaStr);
			return Result;
		}

		// Find and configure the factory
		FString LastFactoryError;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UFactory::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
			{
				UFactory* Factory = It->GetDefaultObject<UFactory>();
				if (Factory && Factory->SupportedClass == AssetClass)
				{
					// Set the schema via the property (StateTreeSchemaClass is protected, use property reflection)
					FProperty* SchemaProp = FindFProperty<FProperty>(Factory->GetClass(), TEXT("StateTreeSchemaClass"));
					FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(SchemaProp);
					if (ObjProp)
					{
						// Create a mutable instance for the factory
						UFactory* FactoryInstance = NewObject<UFactory>(GetTransientPackage(), *It);
						ObjProp->SetObjectPropertyValue(SchemaProp->ContainerPtrToValuePtr<void>(FactoryInstance), SchemaClass);

						FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
						UPackage* Package = CreatePackage(*AssetPath);
						if (Package)
						{
							Result.Asset = FactoryInstance->FactoryCreateNew(AssetClass, Package, FName(*AssetName),
								RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
							if (Result.Asset)
							{
								FAssetRegistryModule::AssetCreated(Result.Asset);
								Package->MarkPackageDirty();
								return Result;
							}
							LastFactoryError = FString::Printf(TEXT("%s returned null"), *It->GetName());
						}
						else
						{
							LastFactoryError = FString::Printf(TEXT("could not create package %s"), *AssetPath);
						}
					}
					else
					{
						LastFactoryError = FString::Printf(TEXT("%s does not expose StateTreeSchemaClass"), *It->GetName());
					}
				}
			}
		}
		if (!Result.Asset)
		{
			Result.Error = LastFactoryError.IsEmpty()
				? TEXT("Failed to create StateTree - no configurable StateTree factory was found")
				: FString::Printf(TEXT("Failed to create StateTree with schema \"%s\" - %s"), *SchemaClass->GetName(), *LastFactoryError);
		}
		return Result;
	}

	// ---- Blueprint special case (uses FKismetEditorUtilities) ----
	if (LowerType == TEXT("blueprint") || LowerType == TEXT("bp"))
	{
		FString ParentStr = TEXT("Actor");
		const FString* ParentValue = Options.Find(TEXT("ParentClass"));
		if (!ParentValue) ParentValue = Options.Find(TEXT("parent_class"));
		if (!ParentValue) ParentValue = Options.Find(TEXT("parent"));
		if (!ParentValue) ParentValue = Options.Find(TEXT("Parent"));
		if (ParentValue)
		{
			ParentStr = *ParentValue;
		}
		Result.Asset = CreateBlueprint(AssetPath, ParentStr);
		if (!Result.Asset)
		{
			Result.Error = FString::Printf(TEXT("Failed to create Blueprint with parent \"%s\". Check the class name."), *ParentStr);
		}
		return Result;
	}

	// ---- FunctionLibrary special case ----
	if (LowerType == TEXT("functionlibrary"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FKismetEditorUtilities::CreateBlueprint(
				UBlueprintFunctionLibrary::StaticClass(),
				Package,
				FName(*AssetName),
				BPTYPE_FunctionLibrary,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass()
			);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Result.Asset->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create Function Library Blueprint");
		return Result;
	}

	// ---- MacroLibrary special case ----
	if (LowerType == TEXT("macrolibrary"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = FKismetEditorUtilities::CreateBlueprint(
				UObject::StaticClass(),
				Package,
				FName(*AssetName),
				BPTYPE_MacroLibrary,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass()
			);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Result.Asset->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create Macro Library Blueprint");
		return Result;
	}

	// ---- AnimLayerInterface special case ----
	if (LowerType == TEXT("animlayerinterface"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			UClass* AnimLayerClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimLayerInterface"));
			if (!AnimLayerClass)
				AnimLayerClass = LoadObject<UClass>(nullptr, TEXT("/Script/Engine.AnimLayerInterface"));
			if (!AnimLayerClass)
			{
				Result.Error = TEXT("UAnimLayerInterface class not found — Engine module may not be loaded");
				return Result;
			}
			UClass* ParentClass = AnimLayerClass;

			Result.Asset = FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				FName(*AssetName),
				BPTYPE_Interface,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass()
			);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Result.Asset->MarkPackageDirty();
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create AnimLayer Interface Blueprint");
		return Result;
	}

	// ---- MaterialInstance special case ----
	if (LowerType == TEXT("materialinstance") || LowerType == TEXT("materialinstanceconstant"))
	{
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (MIC)
			{
				// Set parent material if provided
				if (const FString* ParentPath = Options.Find(TEXT("ParentMaterial")))
				{
					UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, **ParentPath);
					if (Parent)
					{
						MIC->SetParentEditorOnly(Parent);
					}
				}
				FAssetRegistryModule::AssetCreated(MIC);
				Package->MarkPackageDirty();
				Result.Asset = MIC;
			}
		}
		if (!Result.Asset) Result.Error = TEXT("Failed to create MaterialInstanceConstant");
		return Result;
	}

	// ---- EditorUtilityBlueprint default ParentClass ----
	if ((LowerType == TEXT("editorutilityblueprint") || LowerType == TEXT("editorutilitybp"))
		&& !Options.Contains(TEXT("ParentClass")))
	{
		// Default to Actor so the factory doesn't fail with null ParentClass
		const_cast<TMap<FString, FString>&>(Options).Add(TEXT("ParentClass"), TEXT("Actor"));
	}

	// ---- DataTable: resolve Struct option (supports UserDefinedStruct asset paths) ----
	if (LowerType == TEXT("datatable") && Options.Contains(TEXT("Struct")))
	{
		const FString& StructPath = Options[TEXT("Struct")];
		// First try loading as an asset (UserDefinedStruct, etc.)
		UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
		if (!RowStruct)
		{
			// Try with .AssetName suffix
			FString AssetName = FPackageName::GetShortName(StructPath);
			RowStruct = LoadObject<UScriptStruct>(nullptr, *(StructPath + TEXT(".") + AssetName));
		}
		if (!RowStruct)
		{
			// Fall back to C++ struct lookup (FindStructRobust handles /Script/ paths)
			RowStruct = NeoTypeResolver::FindStructRobust(StructPath);
		}
		if (RowStruct)
		{
			// Directly set the factory's Struct property instead of relying on ImportText
			UFactory* DTFactory = FindFactoryForClass(AssetClass);
			if (DTFactory)
			{
				FObjectPropertyBase* StructProp = CastField<FObjectPropertyBase>(DTFactory->GetClass()->FindPropertyByName(TEXT("Struct")));
				if (StructProp)
				{
					StructProp->SetObjectPropertyValue(StructProp->ContainerPtrToValuePtr<void>(DTFactory), const_cast<UScriptStruct*>(RowStruct));
				}

				FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
				FString DTAssetName = FPackageName::GetLongPackageAssetName(AssetPath);
				Result.Asset = DTFactory->FactoryCreateNew(AssetClass, CreatePackage(*AssetPath), FName(*DTAssetName),
					RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
				if (Result.Asset)
				{
					FAssetRegistryModule::AssetCreated(Result.Asset);
					Result.Asset->MarkPackageDirty();
				}
				else
				{
					Result.Error = FString::Printf(TEXT("FactoryCreateNew returned null for DataTable with struct \"%s\""), *StructPath);
				}
				return Result;
			}
		}
		else
		{
			Result.Error = FString::Printf(TEXT("Could not resolve struct \"%s\". For engine structs use \"/Script/Module.FStructName\", for UserDefinedStructs use \"/Game/Path/StructName\""), *StructPath);
			return Result;
		}
	}

	// ---- MirrorDataTable: UMirrorDataTableFactory stores setup fields as private non-edit properties ----
	if ((LowerType == TEXT("mirrordatatable") || LowerType == TEXT("mirrortable")) &&
		Options.Contains(TEXT("Struct")) && Options.Contains(TEXT("Skeleton")))
	{
		const FString& StructPath = Options[TEXT("Struct")];
		UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
		if (!RowStruct)
		{
			const FString AssetNameForStruct = FPackageName::GetShortName(StructPath);
			RowStruct = LoadObject<UScriptStruct>(nullptr, *(StructPath + TEXT(".") + AssetNameForStruct));
		}
		if (!RowStruct)
		{
			RowStruct = NeoTypeResolver::FindStructRobust(StructPath);
		}
		if (!RowStruct)
		{
			Result.Error = FString::Printf(TEXT("Could not resolve mirror table row struct \"%s\""), *StructPath);
			return Result;
		}

		const FString& SkeletonPath = Options[TEXT("Skeleton")];
		USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
		if (!Skeleton)
		{
			const FString SkeletonAssetName = FPackageName::GetShortName(SkeletonPath);
			Skeleton = LoadObject<USkeleton>(nullptr, *(SkeletonPath + TEXT(".") + SkeletonAssetName));
		}
		if (!Skeleton)
		{
			Result.Error = FString::Printf(TEXT("Could not resolve mirror table skeleton \"%s\""), *SkeletonPath);
			return Result;
		}

		UFactory* MirrorFactory = FindFactoryForClass(AssetClass);
		if (!MirrorFactory)
		{
			Result.Error = TEXT("No factory found for MirrorDataTable");
			return Result;
		}

		if (FObjectPropertyBase* StructProp = CastField<FObjectPropertyBase>(MirrorFactory->GetClass()->FindPropertyByName(TEXT("Struct"))))
		{
			StructProp->SetObjectPropertyValue(StructProp->ContainerPtrToValuePtr<void>(MirrorFactory), RowStruct);
		}
		if (FObjectPropertyBase* SkeletonProp = CastField<FObjectPropertyBase>(MirrorFactory->GetClass()->FindPropertyByName(TEXT("Skeleton"))))
		{
			SkeletonProp->SetObjectPropertyValue(SkeletonProp->ContainerPtrToValuePtr<void>(MirrorFactory), Skeleton);
		}

		const FString MirrorAssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		Result.Asset = MirrorFactory->FactoryCreateNew(AssetClass, Package, FName(*MirrorAssetName),
			RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
		if (Result.Asset)
		{
			FAssetRegistryModule::AssetCreated(Result.Asset);
			Result.Asset->MarkPackageDirty();
		}
		else
		{
			Result.Error = FString::Printf(TEXT("FactoryCreateNew returned null for MirrorDataTable with struct \"%s\" and skeleton \"%s\""),
				*StructPath, *SkeletonPath);
		}
		return Result;
	}

	// ---- Find factory ----
	UFactory* Factory = FindFactoryForClass(AssetClass);
	if (!Factory)
	{
		// No factory found — try direct NewObject
		FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (Package)
		{
			Result.Asset = NewObject<UObject>(Package, AssetClass, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			if (Result.Asset)
			{
				FAssetRegistryModule::AssetCreated(Result.Asset);
				Package->MarkPackageDirty();
			}
		}
		if (!Result.Asset)
		{
			Result.Error = FString::Printf(TEXT("No factory found for \"%s\" and direct creation failed."), *AssetTypeName);
		}
		return Result;
	}

	// ---- Set factory properties from options ----
	int32 PropsSet = 0;
	for (const auto& Pair : Options)
	{
		FProperty* Prop = Factory->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop)
		{
			// Try case-insensitive match
			for (TFieldIterator<FProperty> It(Factory->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(Pair.Key, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}

		if (!Prop) continue;

		FString Value = Pair.Value;

		// Special handling for TSubclassOf — resolve short names to full class paths
		if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
		{
			UClass* Resolved = ResolveClassName(Value);
			if (Resolved && (!ClassProp->MetaClass || Resolved->IsChildOf(ClassProp->MetaClass)))
			{
				ClassProp->SetPropertyValue_InContainer(Factory, Resolved);
				PropsSet++;
				continue;
			}
			if (Value.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("null"), ESearchCase::IgnoreCase))
			{
				ClassProp->SetPropertyValue_InContainer(Factory, nullptr);
				PropsSet++;
				continue;
			}
		}

		// Special handling for object properties — resolve to full asset paths
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			// Ensure proper format for ImportText
			if (!Value.IsEmpty() && !Value.Contains(TEXT("'")))
			{
				// Try loading directly to validate
				UObject* Loaded = StaticLoadObject(ObjProp->PropertyClass, nullptr, *Value);
				if (Loaded)
				{
					Value = Loaded->GetPathName();
				}
			}
		}

		FStringOutputDevice ErrorText;
		const TCHAR* ImportResult = Prop->ImportText_InContainer(*Value, Factory, Factory, 0, &ErrorText);
		if (ImportResult)
		{
			PropsSet++;
		}
	}

	// ---- Create the asset ----
	FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	Result.Asset = AssetTools.CreateAsset(AssetName, PackagePath, AssetClass, Factory);

	if (!Result.Asset)
	{
		// Collect factory properties for debugging
		CollectFactoryProperties(Factory, Result.FactoryProperties);

		Result.Error = FString::Printf(TEXT("FactoryCreateNew returned null for \"%s\"."), *AssetTypeName);

		if (Result.FactoryProperties.Num() > 0)
		{
			Result.Error += TEXT("\nFactory properties after configuration:");
			for (const FString& PropInfo : Result.FactoryProperties)
			{
				Result.Error += TEXT("\n") + PropInfo;
			}
		}

		// Add known requirements hint
		if (KnownReqs.Num() > 0)
		{
			Result.Error += TEXT("\nKnown requirements:");
			for (const FFactoryRequirement& Req : KnownReqs)
			{
				const FString* ProvidedVal = Options.Find(Req.PropertyName);
				Result.Error += FString::Printf(TEXT("\n  %s%s = %s"),
					*Req.PropertyName, Req.bRequired ? TEXT(" [REQUIRED]") : TEXT(""),
					ProvidedVal ? **ProvidedVal : TEXT("(not set)"));
			}
		}
	}

	return Result;
}
