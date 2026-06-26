#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaPinHelper.h"
#include "Lua/LuaCurveHelper.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaGraphResolver.h"
#include "Lua/LuaStr.h"
#include "Utils/NeoTypeResolver.h"
#include "Blueprint/BlueprintUtils.h"
#include "Blueprint/NodeUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Timeline.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphNode_Comment.h"
#include "Engine/BlueprintGeneratedClass.h"

// AnimBP support
#include "Kismet2/Kismet2NameValidators.h"
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_TransitionResult.h"

// Widget Blueprint support
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/NamedSlotInterface.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"

// Event binding support
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_FunctionEntry.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

// Linked anim layer support
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "Animation/AnimLayerInterface.h"
#include "Engine/MemberReference.h"

#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

// Widget rename/replace support
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "Templates/WidgetTemplateClass.h"

// Safe GetNodeTitle — some AnimGraph nodes can crash on GetNodeTitle
static FString SafeGetNodeTitle(UEdGraphNode* Node)
{
	// AnimGraph module nodes can crash in GetNodeTitle if inner anim node isn't fully initialized
	UClass* NodeClass = Node->GetClass();
	UPackage* Package = NodeClass->GetOuterUPackage();
	if (Package && Package->GetName().Contains(TEXT("AnimGraph")))
	{
		// Use the editable title for anim state nodes (returns the user-facing name)
		if (Cast<UAnimStateNode>(Node) || Cast<UAnimStateTransitionNode>(Node) || Cast<UAnimStateEntryNode>(Node))
		{
			return Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
		}
		// For other anim nodes, try MenuTitle but fallback to class name on failure
		FText Title = Node->GetNodeTitle(ENodeTitleType::MenuTitle);
		if (!Title.IsEmpty())
		{
			return Title.ToString();
		}
		return NodeClass->GetName();
	}
	return Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
}

static bool IsAnimLayerInterfaceClass(const UClass* Class)
{
	return Class && Class->IsChildOf(UAnimLayerInterface::StaticClass());
}

static bool AnimLayerInterfaceMatchesInput(const UClass* InterfaceClass, const FString& Input)
{
	if (!InterfaceClass) return false;

	const FString Trimmed = Input.TrimStartAndEnd();
	const FString Normalized = NeoLuaAsset::NormalizePath(Trimmed);
	const FString PackagePath = NeoLuaAsset::ToPackagePath(Normalized);
	const FString ObjectPath = NeoLuaAsset::ToObjectPath(Normalized);

	if (InterfaceClass->GetPathName() == Trimmed || InterfaceClass->GetPathName() == ObjectPath ||
		InterfaceClass->GetPathName() == ObjectPath + TEXT("_C"))
	{
		return true;
	}

	const FString ClassName = InterfaceClass->GetName();
	if (ClassName == Trimmed || (ClassName.EndsWith(TEXT("_C")) && ClassName.LeftChop(2) == Trimmed))
	{
		return true;
	}

	if (const UBlueprint* InterfaceBP = Cast<UBlueprint>(InterfaceClass->ClassGeneratedBy))
	{
		if (InterfaceBP->GetName() == Trimmed ||
			InterfaceBP->GetPathName() == Trimmed ||
			InterfaceBP->GetPathName() == ObjectPath ||
			InterfaceBP->GetOutermost()->GetName() == PackagePath)
		{
			return true;
		}
	}

	return false;
}

static UClass* ResolveAnimLayerInterfaceClass(const FString& InterfaceName)
{
	const FString Trimmed = InterfaceName.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return nullptr;

	auto Accept = [](UClass* Candidate) -> UClass*
	{
		return IsAnimLayerInterfaceClass(Candidate) ? Candidate : nullptr;
	};

	if (UClass* Found = Accept(FindObject<UClass>(nullptr, *Trimmed))) return Found;
	if (UClass* Found = Accept(LoadObject<UClass>(nullptr, *Trimmed))) return Found;

	const FString ObjectPath = NeoLuaAsset::ToObjectPath(Trimmed);
	if (UClass* Found = Accept(FindObject<UClass>(nullptr, *ObjectPath))) return Found;
	if (UClass* Found = Accept(LoadObject<UClass>(nullptr, *ObjectPath))) return Found;
	if (UClass* Found = Accept(FindObject<UClass>(nullptr, *(ObjectPath + TEXT("_C"))))) return Found;
	if (UClass* Found = Accept(LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C"))))) return Found;

	if (UBlueprint* InterfaceBP = NeoLuaAsset::ResolveWithRegistry<UBlueprint>(Trimmed))
	{
		if (UClass* Found = Accept(InterfaceBP->GeneratedClass)) return Found;
	}

	if (UClass* Found = Accept(NeoTypeResolver::FindClassRobust(Trimmed))) return Found;

	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* BP = *It;
		if (BP && BP->GeneratedClass && IsAnimLayerInterfaceClass(BP->GeneratedClass) &&
			(BP->GetName() == Trimmed || BP->GetPathName() == ObjectPath ||
				BP->GetOutermost()->GetName() == NeoLuaAsset::ToPackagePath(Trimmed)))
		{
			return BP->GeneratedClass;
		}
	}

	return nullptr;
}

static int32 FindImplementedAnimLayerInterfaceIndex(const UAnimBlueprint* AnimBP, const UClass* InterfaceClass, const FString& Input = FString())
{
	if (!AnimBP) return INDEX_NONE;

	for (int32 Index = 0; Index < AnimBP->ImplementedInterfaces.Num(); ++Index)
	{
		const FBPInterfaceDescription& Desc = AnimBP->ImplementedInterfaces[Index];
		if (!IsAnimLayerInterfaceClass(Desc.Interface)) continue;

		if ((InterfaceClass && Desc.Interface == InterfaceClass) ||
			(!Input.IsEmpty() && AnimLayerInterfaceMatchesInput(Desc.Interface, Input)))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

static UFunction* FindAnimLayerFunctionByName(UClass* InterfaceClass, FName LayerName)
{
	if (!InterfaceClass || LayerName.IsNone()) return nullptr;

	for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Function = *FuncIt;
		if (Function && Function->GetFName() == LayerName &&
			Function->HasMetaData(FBlueprintMetadata::MD_AnimBlueprintFunction))
		{
			return Function;
		}
	}

	return nullptr;
}

static void CollectAnimLayerFunctionNames(UClass* InterfaceClass, const FBPInterfaceDescription* ImplementedDesc, TArray<FName>& OutNames)
{
	auto AddUniqueName = [&OutNames](FName Name)
	{
		if (!Name.IsNone())
		{
			OutNames.AddUnique(Name);
		}
	};

	if (ImplementedDesc)
	{
		for (UEdGraph* Graph : ImplementedDesc->Graphs)
		{
			if (Graph && Graph->IsA<UAnimationGraph>())
			{
				AddUniqueName(Graph->GetFName());
			}
		}
	}

	if (OutNames.Num() == 0 && InterfaceClass)
	{
		for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (Function && Function->HasMetaData(FBlueprintMetadata::MD_AnimBlueprintFunction))
			{
				AddUniqueName(Function->GetFName());
			}
		}
	}
}

static UK2Node_FunctionResult* FindFunctionResultNode(UEdGraph* Graph)
{
	if (!Graph)
	{
		return nullptr;
	}

	TArray<UK2Node_FunctionResult*> ResultNodes;
	Graph->GetNodesOfClass(ResultNodes);
	return ResultNodes.Num() > 0 ? ResultNodes[0] : nullptr;
}

static bool IsNameArrayFunctionResultPin(const UEdGraphPin* Pin)
{
	return Pin
		&& Pin->Direction == EGPD_Input
		&& Pin->PinType.ContainerType == EPinContainerType::Array
		&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name;
}

static TArray<FString> LuaStringArrayToFStrings(sol::table Values)
{
	TArray<FString> Result;
	for (auto&& Entry : Values)
	{
		if (Entry.second.is<std::string>())
		{
			const FString Value = NeoLuaStr::ToFString(Entry.second.as<std::string>()).TrimStartAndEnd();
			if (!Value.IsEmpty())
			{
				Result.Add(Value);
			}
		}
	}
	return Result;
}

static UK2Node_MakeArray* CreateNameMakeArrayNode(UEdGraph* Graph, UEdGraphPin* TargetPin, const TArray<FString>& Values, int32 NodeIndex)
{
	if (!Graph || !TargetPin || Values.Num() == 0)
	{
		return nullptr;
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		return nullptr;
	}

	UK2Node_MakeArray* MakeArray = NewObject<UK2Node_MakeArray>(Graph);
	if (!MakeArray)
	{
		return nullptr;
	}

	Graph->Modify();
	MakeArray->SetFlags(RF_Transactional);
	MakeArray->NodePosX = TargetPin->GetOwningNode() ? TargetPin->GetOwningNode()->NodePosX - 360 : 300;
	MakeArray->NodePosY = TargetPin->GetOwningNode() ? TargetPin->GetOwningNode()->NodePosY + 120 + NodeIndex * 180 : NodeIndex * 180;
	Graph->AddNode(MakeArray, true, false);
	MakeArray->CreateNewGuid();
	MakeArray->PostPlacedNewNode();
	MakeArray->AllocateDefaultPins();

	while (MakeArray->Pins.FilterByPredicate([](UEdGraphPin* Pin) { return Pin && Pin->Direction == EGPD_Input; }).Num() < Values.Num())
	{
		MakeArray->AddInputPin();
	}

	UEdGraphPin* OutputPin = MakeArray->GetOutputPin();
	if (!OutputPin)
	{
		return nullptr;
	}

	FEdGraphPinType InputType = TargetPin->PinType;
	InputType.ContainerType = EPinContainerType::None;
	OutputPin->PinType = TargetPin->PinType;

	int32 ValueIndex = 0;
	for (UEdGraphPin* Pin : MakeArray->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			continue;
		}
		Pin->PinType = InputType;
		if (ValueIndex < Values.Num())
		{
			Schema->TrySetDefaultValue(*Pin, Values[ValueIndex]);
		}
		++ValueIndex;
	}

	Schema->BreakPinLinks(*TargetPin, true);
	if (!Schema->TryCreateConnection(OutputPin, TargetPin))
	{
		return nullptr;
	}

	MakeArray->PinConnectionListChanged(OutputPin);
	TargetPin->GetOwningNode()->PinConnectionListChanged(TargetPin);
	MakeArray->NodeConnectionListChanged();
	TargetPin->GetOwningNode()->NodeConnectionListChanged();
	return MakeArray;
}

static sol::table InspectFunctionNameArrayOutputs(sol::state_view Lua, UEdGraph* Graph)
{
	sol::table Result = Lua.create_table();
	UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(Graph);
	if (!ResultNode)
	{
		return Result;
	}

	for (UEdGraphPin* ResultPin : ResultNode->Pins)
	{
		if (!IsNameArrayFunctionResultPin(ResultPin))
		{
			continue;
		}

		sol::table Row = Lua.create_table();
		Row["pin"] = TCHAR_TO_UTF8(*ResultPin->PinName.ToString());
		Row["connected"] = ResultPin->LinkedTo.Num() > 0;

		sol::table Values = Lua.create_table();
		int32 ValueLuaIndex = 1;
		if (ResultPin->LinkedTo.Num() > 0)
		{
			UEdGraphPin* SourcePin = ResultPin->LinkedTo[0];
			UEdGraphNode* SourceNode = SourcePin ? SourcePin->GetOwningNodeUnchecked() : nullptr;
			Row["source_node"] = SourceNode ? TCHAR_TO_UTF8(*SourceNode->GetClass()->GetName()) : "";
			Row["source_pin"] = SourcePin ? TCHAR_TO_UTF8(*NeoLuaPin::GetUsablePinName(SourcePin)) : "";

			if (UK2Node_MakeArray* MakeArray = Cast<UK2Node_MakeArray>(SourceNode))
			{
				for (UEdGraphPin* ArrayPin : MakeArray->Pins)
				{
					if (ArrayPin && ArrayPin->Direction == EGPD_Input && ArrayPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
					{
						const FString Value = ArrayPin->GetDefaultAsString();
						if (!Value.IsEmpty())
						{
							Values[ValueLuaIndex++] = TCHAR_TO_UTF8(*Value);
						}
					}
				}
			}
		}
		Row["values"] = Values;
		Row["value_count"] = ValueLuaIndex - 1;
		Result[TCHAR_TO_UTF8(*ResultPin->PinName.ToString())] = Row;
	}

	return Result;
}

static bool ConfigureLinkedAnimLayerNode(UAnimBlueprint* AnimBP, UAnimGraphNode_LinkedAnimLayer* LayerNode, UClass* InterfaceClass, FName LayerName)
{
	if (!LayerNode || LayerName.IsNone())
	{
		return false;
	}

	LayerNode->Node.Interface = InterfaceClass;
	LayerNode->Node.Layer = LayerName;

	FStructProperty* FunctionReferenceProperty = FindFProperty<FStructProperty>(LayerNode->GetClass(), TEXT("FunctionReference"));
	FMemberReference* FunctionReference = FunctionReferenceProperty
		? FunctionReferenceProperty->ContainerPtrToValuePtr<FMemberReference>(LayerNode)
		: nullptr;
	if (!FunctionReference)
	{
		return false;
	}

	if (InterfaceClass)
	{
		FGuid FunctionGuid;
		FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(
			FBlueprintEditorUtils::GetMostUpToDateClass(InterfaceClass),
			LayerName,
			FunctionGuid);
		FunctionReference->SetExternalMember(LayerName, InterfaceClass, FunctionGuid);
	}
	else
	{
		FunctionReference->SetSelfMember(LayerName);
	}

	LayerNode->InterfaceGuid.Invalidate();
	if (AnimBP)
	{
		for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
		{
			for (UEdGraph* InterfaceGraph : Desc.Graphs)
			{
				if (InterfaceGraph && InterfaceGraph->GetFName() == LayerName)
				{
					LayerNode->InterfaceGuid = InterfaceGraph->InterfaceGuid;
					return true;
				}
			}
		}
	}

	return true;
}

static UFunction* ResolveCallableParentFunction(UBlueprint* BP, UFunction* Function)
{
	if (!BP || !Function) return nullptr;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Schema) return nullptr;

	UClass* FunctionOwner = Function->GetOwnerClass();
	const UClass* GeneratedClass = BP->GeneratedClass;
	const UClass* SkeletonClass = BP->SkeletonGeneratedClass;

	if ((GeneratedClass && FunctionOwner == GeneratedClass) || (SkeletonClass && FunctionOwner == SkeletonClass))
	{
		return Schema->GetCallableParentFunction(Function);
	}

	if (BP->ParentClass && FunctionOwner && FunctionOwner->IsChildOf(BP->ParentClass))
	{
		return Function;
	}

	return Schema->GetCallableParentFunction(Function);
}

static UK2Node_Event* FindOverrideEventNode(UBlueprint* BP, const FName FunctionName)
{
	if (!BP || FunctionName.IsNone()) return nullptr;

	UFunction* OverrideFunc = nullptr;
	UClass* OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(BP, FunctionName, &OverrideFunc);
	if (OverrideFunc && OverrideFuncClass)
	{
		if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(BP, OverrideFuncClass, FunctionName))
		{
			return Existing;
		}
	}

	TArray<UK2Node_Event*> AllEvents;
	FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(BP, AllEvents);
	for (UK2Node_Event* Event : AllEvents)
	{
		if (Event && Event->GetFunctionName() == FunctionName)
		{
			return Event;
		}
	}
	return nullptr;
}

static UK2Node_FunctionEntry* FindFunctionEntryNode(UBlueprint* BP, const FName FunctionName)
{
	if (!BP || FunctionName.IsNone()) return nullptr;

	UEdGraph* Graph = FindObject<UEdGraph>(BP, *FunctionName.ToString());
	if (!Graph)
	{
		Graph = NeoBlueprint::FindGraph(BP, FunctionName.ToString());
	}
	if (!Graph) return nullptr;

	return Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(Graph));
}

static UEdGraphPin* FindFirstExecOutputPin(UEdGraphNode* Node)
{
	if (!Node) return nullptr;
	if (UEdGraphPin* ThenPin = Node->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
	{
		return ThenPin;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}
	return nullptr;
}

static sol::table BuildParentCallResult(sol::state_view& Lua, UK2Node_CallParentFunction* Node)
{
	sol::table Result = Lua.create_table();
	Result["handle"] = TCHAR_TO_UTF8(*Node->NodeGuid.ToString());
	Result["name"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
	Result["type"] = "K2Node_CallParentFunction";
	Result["class"] = "K2Node_CallParentFunction";
	Result["x"] = Node->NodePosX;
	Result["y"] = Node->NodePosY;
	return Result;
}

static UClass* ResolveBlueprintParentClassInput(const FString& ParentInput)
{
	const FString Trimmed = ParentInput.TrimStartAndEnd();
	if (Trimmed.IsEmpty()) return nullptr;

	auto TryClassPath = [](const FString& Path) -> UClass*
	{
		if (Path.IsEmpty()) return nullptr;
		if (UClass* Found = FindObject<UClass>(nullptr, *Path)) return Found;
		return LoadObject<UClass>(nullptr, *Path);
	};

	if (UClass* Found = TryClassPath(Trimmed)) return Found;

	const FString ObjectPath = NeoLuaAsset::ToObjectPath(Trimmed);
	if (UClass* Found = TryClassPath(ObjectPath)) return Found;
	if (UClass* Found = TryClassPath(ObjectPath + TEXT("_C"))) return Found;

	if (UBlueprint* ParentBlueprint = NeoLuaAsset::ResolveWithRegistry<UBlueprint>(Trimmed))
	{
		if (ParentBlueprint->GeneratedClass) return ParentBlueprint->GeneratedClass;
		if (ParentBlueprint->SkeletonGeneratedClass) return ParentBlueprint->SkeletonGeneratedClass;
	}

	if (UClass* Found = NeoTypeResolver::FindClassRobust(Trimmed)) return Found;

	if (!Trimmed.Contains(TEXT(".")) && !Trimmed.Contains(TEXT("/")))
	{
		if (UClass* Found = TryClassPath(TEXT("/Script/Engine.") + Trimmed)) return Found;
		if (!Trimmed.StartsWith(TEXT("A")))
		{
			if (UClass* Found = TryClassPath(TEXT("/Script/Engine.A") + Trimmed)) return Found;
		}
		if (!Trimmed.StartsWith(TEXT("U")))
		{
			if (UClass* Found = TryClassPath(TEXT("/Script/UMG.U") + Trimmed)) return Found;
		}
	}

	return nullptr;
}

// Shared widget-class resolver used by bp:add_widget, bp:wrap_widgets, and bp:replace_widget.
// Matches on short class name (e.g. "Button") or short-name + "Widget" (e.g. "ButtonWidget");
// case-insensitive. Skips abstract classes. BaseClass narrows to the required widget subtype
// (UWidget for add/replace; UPanelWidget for wrap).
static UClass* FindWidgetClassByName(const FString& Name, UClass* BaseClass)
{
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(BaseClass) || It->HasAnyClassFlags(CLASS_Abstract)) continue;
		const FString ClassName = It->GetName();
		if (ClassName.Equals(Name, ESearchCase::IgnoreCase)
			|| ClassName.Equals(Name + TEXT("Widget"), ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

// Applies function-metadata keys (category, description, keywords, compact_title, deprecated,
// deprecation_message, call_in_editor, thread_safe) from a Lua options table onto a function
// graph's metadata. Calls ModifyFunctionMetaData once before the first write, and
// MarkBlueprintAsStructurallyModified at the end if anything changed. Returns change count.
//
// A missing key is skipped. An explicit-empty string IS applied (matches bp:set_property's
// prior behavior; bp:add_function previously silently skipped empty strings — this aligns the two).
static int32 ApplyFunctionMetadata(UBlueprint* BP, UEdGraph* Graph, const sol::table& Opts)
{
	FKismetUserDeclaredFunctionMetadata* FuncMeta = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
	if (!FuncMeta) return 0;

	int32 Changed = 0;
	auto EnsureModified = [&]()
	{
		if (Changed == 0) FBlueprintEditorUtils::ModifyFunctionMetaData(Graph);
	};

	auto ApplyText = [&](const char* Key, FText& Field)
	{
		sol::object Obj = Opts[Key];
		if (Obj.valid() && Obj.is<std::string>())
		{
			EnsureModified();
			Field = FText::FromString(NeoLuaStr::ToFString(Obj.as<std::string>()));
			Changed++;
		}
	};
	auto ApplyString = [&](const char* Key, FString& Field)
	{
		sol::object Obj = Opts[Key];
		if (Obj.valid() && Obj.is<std::string>())
		{
			EnsureModified();
			Field = NeoLuaStr::ToFString(Obj.as<std::string>());
			Changed++;
		}
	};
	auto ApplyBool = [&](const char* Key, bool& Field)
	{
		sol::object Obj = Opts[Key];
		if (Obj.valid() && Obj.get_type() != sol::type::lua_nil)
		{
			EnsureModified();
			Field = Obj.as<bool>();
			Changed++;
		}
	};

	ApplyText("category", FuncMeta->Category);
	ApplyText("description", FuncMeta->ToolTip);
	ApplyText("keywords", FuncMeta->Keywords);
	ApplyText("compact_title", FuncMeta->CompactNodeTitle);
	ApplyBool("deprecated", FuncMeta->bIsDeprecated);
	ApplyString("deprecation_message", FuncMeta->DeprecationMessage);
	ApplyBool("call_in_editor", FuncMeta->bCallInEditor);
	ApplyBool("thread_safe", FuncMeta->bThreadSafe);

	if (Changed > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
	return Changed;
}

static sol::table BuildNodeTable(sol::state_view& LuaView, UEdGraphNode* Node)
{
	FString NodeGuid = Node->NodeGuid.ToString();

	sol::table NodeEntry = LuaView.create_table();
	NodeEntry["handle"] = TCHAR_TO_UTF8(*NodeGuid);
	NodeEntry["name"] = TCHAR_TO_UTF8(*SafeGetNodeTitle(Node));
	NodeEntry["type"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
	NodeEntry["x"] = Node->NodePosX;
	NodeEntry["y"] = Node->NodePosY;

	sol::table InPins = LuaView.create_table();
	sol::table OutPins = LuaView.create_table();
	sol::table Connections = LuaView.create_table();
	int32 InIdx = 1, OutIdx = 1, ConnIdx = 1;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;

		sol::table PinInfo = NeoLuaPin::BuildPinTable(LuaView, Pin);
		FString PinName = Pin->GetDisplayName().IsEmpty()
			? Pin->PinName.ToString()
			: Pin->GetDisplayName().ToString();

		if (Pin->Direction == EGPD_Input)
		{
			InPins[InIdx++] = PinInfo;
		}
		else
		{
			OutPins[OutIdx++] = PinInfo;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

			sol::table Conn = LuaView.create_table();
			Conn["from_node"] = TCHAR_TO_UTF8(*NodeGuid);
			Conn["from_pin"] = TCHAR_TO_UTF8(*PinName);
			Conn["to_node"] = TCHAR_TO_UTF8(*LinkedPin->GetOwningNode()->NodeGuid.ToString());

			FString LinkedPinName = LinkedPin->GetDisplayName().IsEmpty()
				? LinkedPin->PinName.ToString()
				: LinkedPin->GetDisplayName().ToString();
			Conn["to_pin"] = TCHAR_TO_UTF8(*LinkedPinName);

			Connections[ConnIdx++] = Conn;
		}
	}

	NodeEntry["pins_in"] = InPins;
	NodeEntry["pins_out"] = OutPins;
	NodeEntry["connections"] = Connections;
	return NodeEntry;
}

static sol::table BuildGraphObject(sol::state_view& LuaView, FLuaSessionData& Session,
	UEdGraph* Graph, const std::string& BPPath, const std::string& GraphName)
{
	Session.RegisterGraphNodes(Graph);

	sol::table GraphObj = LuaView.create_table();
	GraphObj["name"] = GraphName;

	sol::table NodesTable = LuaView.create_table();
	int32 NodeIdx = 1;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		NodesTable[NodeIdx++] = BuildNodeTable(LuaView, Node);
	}
	GraphObj["nodes"] = NodesTable;

	// graph:add_node(node, x?, y?) — delegates to global add_node(bp_path, graph_name, ...)
	GraphObj.set_function("add_node", [BPPath, GraphName](sol::table /*self*/, sol::object NodeArg,
		sol::optional<double> X, sol::optional<double> Y, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["add_node"];
		auto result = fn(BPPath, GraphName, NodeArg, X, Y);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:connect(from_handle, from_pin, to_handle, to_pin)
	GraphObj.set_function("connect", [](sol::table /*self*/,
		const std::string& FromHandle, const std::string& FromPin,
		const std::string& ToHandle, const std::string& ToPin,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["connect"];
		auto result = fn(FromHandle, FromPin, ToHandle, ToPin);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:disconnect(handle, pin_name)
	GraphObj.set_function("disconnect", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["disconnect"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:set_pin(handle, pin_name, value)
	GraphObj.set_function("set_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName, const std::string& Value,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["set_pin"];
		auto result = fn(Handle, PinName, Value);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:delete_node(handle)
	GraphObj.set_function("delete_node", [](sol::table /*self*/,
		const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["delete_node"];
		auto result = fn(Handle);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:get_pin(handle, pin_name)
	GraphObj.set_function("get_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["get_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:move_node(handle, x, y, relative?)
	// If relative is true, x and y are deltas (dx, dy) rather than absolute positions
	GraphObj.set_function("move_node", [&Session](sol::table /*self*/,
		const std::string& Handle, double X, double Y, sol::optional<bool> Relative, sol::this_state S) -> sol::object
	{
		FString FHandle = NeoLuaStr::ToFString(Handle);
		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] move_node -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}
		if (Relative.value_or(false))
		{
			Node->Modify();
			Node->NodePosX += (int32)X;
			Node->NodePosY += (int32)Y;
			Session.Log(FString::Printf(TEXT("[OK] move_node(\"%s\") -> relative (%+.0f, %+.0f) = (%d, %d)"),
				*FHandle, X, Y, Node->NodePosX, Node->NodePosY));
		}
		else
		{
			NeoBlueprint::MoveNode(Node, X, Y);
			Session.Log(FString::Printf(TEXT("[OK] move_node(\"%s\") -> (%.0f, %.0f)"), *FHandle, X, Y));
		}
		return sol::make_object(S, true);
	});

	// graph:disconnect_from(from_handle, from_pin, to_handle, to_pin) — delegates to global
	GraphObj.set_function("disconnect_from", [](sol::table /*self*/,
		const std::string& FromHandle, const std::string& FromPin,
		const std::string& ToHandle, const std::string& ToPin,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["disconnect_from"];
		auto result = fn(FromHandle, FromPin, ToHandle, ToPin);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:split_pin(handle, pin_name) — delegates to global
	GraphObj.set_function("split_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["split_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:recombine_pin(handle, pin_name) — delegates to global
	GraphObj.set_function("recombine_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["recombine_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:add_exec_pin(handle) — delegates to global
	GraphObj.set_function("add_exec_pin", [](sol::table /*self*/,
		const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["add_exec_pin"];
		auto result = fn(Handle);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:remove_exec_pin(handle) — delegates to global
	GraphObj.set_function("remove_exec_pin", [](sol::table /*self*/,
		const std::string& Handle, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["remove_exec_pin"];
		auto result = fn(Handle);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:reset_pin(handle, pin_name) — delegates to global
	GraphObj.set_function("reset_pin", [](sol::table /*self*/,
		const std::string& Handle, const std::string& PinName,
		sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["reset_pin"];
		auto result = fn(Handle, PinName);
		if (result.valid()) return result;
		return sol::lua_nil;
	});

	// graph:set_node_comment(handle, text, visible?) — delegates to global
	GraphObj.set_function("set_node_comment", [](sol::table /*self*/,
		const std::string& Handle, const std::string& Text,
		sol::optional<bool> Visible, sol::this_state S) -> sol::object
	{
		sol::state_view lua(S);
		sol::protected_function fn = lua["set_node_comment"];
		if (Visible)
			{ auto result = fn(Handle, Text, *Visible); if (result.valid()) return result; }
		else
			{ auto result = fn(Handle, Text); if (result.valid()) return result; }
		return sol::lua_nil;
	});

	return GraphObj;
}

static sol::table BuildComponentTable(sol::state_view& LuaView, USCS_Node* Node)
{
	sol::table Comp = LuaView.create_table();
	Comp["name"] = TCHAR_TO_UTF8(*Node->GetVariableName().ToString());
	Comp["class"] = Node->ComponentClass ? TCHAR_TO_UTF8(*Node->ComponentClass->GetName()) : "None";

	if (Node->ParentComponentOrVariableName != NAME_None)
	{
		Comp["parent"] = TCHAR_TO_UTF8(*Node->ParentComponentOrVariableName.ToString());
	}

	sol::table Children = LuaView.create_table();
	int32 ChildIdx = 1;
	for (USCS_Node* Child : Node->GetChildNodes())
	{
		if (Child)
		{
			Children[ChildIdx++] = BuildComponentTable(LuaView, Child);
		}
	}
	if (ChildIdx > 1)
	{
		Comp["children"] = Children;
	}

	return Comp;
}

static sol::table BuildVariablesTable(sol::state_view& LuaView, UBlueprint* BP)
{
	sol::table Vars = LuaView.create_table();
	int32 VarIdx = 1;

	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		sol::table VarInfo = LuaView.create_table();
		VarInfo["name"] = TCHAR_TO_UTF8(*Var.VarName.ToString());
		VarInfo["type"] = TCHAR_TO_UTF8(*Var.VarType.PinCategory.ToString());
		if (Var.VarType.PinSubCategoryObject.IsValid())
		{
			VarInfo["sub_type"] = TCHAR_TO_UTF8(*Var.VarType.PinSubCategoryObject->GetName());
		}
		if (!Var.DefaultValue.IsEmpty())
		{
			VarInfo["default"] = TCHAR_TO_UTF8(*Var.DefaultValue);
		}
		if (Var.VarType.ContainerType == EPinContainerType::Array)
			VarInfo["container"] = "array";
		else if (Var.VarType.ContainerType == EPinContainerType::Set)
			VarInfo["container"] = "set";
		else if (Var.VarType.ContainerType == EPinContainerType::Map)
		{
			VarInfo["container"] = "map";
			VarInfo["value_type"] = TCHAR_TO_UTF8(*Var.VarType.PinValueType.TerminalCategory.ToString());
			if (Var.VarType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				VarInfo["value_sub_type"] = TCHAR_TO_UTF8(*Var.VarType.PinValueType.TerminalSubCategoryObject->GetName());
			}
		}

		// Variable flags
		uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(BP, Var.VarName);
		if (Flags)
		{
			if (*Flags & CPF_Edit) VarInfo["edit_anywhere"] = true;
			if (*Flags & CPF_DisableEditOnInstance) VarInfo["edit_defaults_only"] = true;
			if (*Flags & CPF_DisableEditOnTemplate) VarInfo["edit_instance_only"] = true;
			if (*Flags & CPF_BlueprintReadOnly) VarInfo["blueprint_read_only"] = true;
			if (*Flags & CPF_Net) VarInfo["replicated"] = true;
			if (*Flags & CPF_RepNotify) VarInfo["rep_notify"] = true;
			if (*Flags & CPF_Transient) VarInfo["transient"] = true;
			if (*Flags & CPF_SaveGame) VarInfo["save_game"] = true;
			if (*Flags & CPF_ExposeOnSpawn) VarInfo["expose_on_spawn"] = true;
			if (*Flags & CPF_Interp) VarInfo["interp"] = true;
			if (*Flags & CPF_AdvancedDisplay) VarInfo["advanced_display"] = true;
			if (*Flags & CPF_Deprecated) VarInfo["deprecated"] = true;
		}

		// Variable metadata (category, tooltip, custom metadata)
		FText Category = FBlueprintEditorUtils::GetBlueprintVariableCategory(BP, Var.VarName, nullptr);
		if (!Category.IsEmpty())
		{
			VarInfo["category"] = TCHAR_TO_UTF8(*Category.ToString());
		}
		FString TooltipMeta;
		if (FBlueprintEditorUtils::GetBlueprintVariableMetaData(BP, Var.VarName, nullptr, FName(TEXT("tooltip")), TooltipMeta) && !TooltipMeta.IsEmpty())
		{
			VarInfo["tooltip"] = TCHAR_TO_UTF8(*TooltipMeta);
		}

		Vars[TCHAR_TO_UTF8(*Var.VarName.ToString())] = VarInfo;
		Vars[VarIdx++] = VarInfo;
	}

	return Vars;
}

static bool RefreshBlueprintStateTables(sol::state_view LuaView, FLuaSessionData& Session, sol::table BP, const std::string& Path)
{
	FString FPath = NeoLuaStr::ToFString(Path);
	FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
	if (!Info.Blueprint)
	{
		Session.Log(FString::Printf(TEXT("[FAIL] refresh() -> blueprint '%s' not found"), *FPath));
		return false;
	}

	BP["name"] = TCHAR_TO_UTF8(*Info.Name);
	BP["path"] = TCHAR_TO_UTF8(*Info.AssetPath);
	BP["parent_class"] = TCHAR_TO_UTF8(*Info.ParentClass);

	sol::table Graphs = LuaView.create_table();
	int32 GraphIdx = 1;

	auto BuildGraph = [&](UEdGraph* Graph)
	{
		if (!Graph) return;
		std::string GName = TCHAR_TO_UTF8(*Graph->GetName());
		sol::table GraphObj = BuildGraphObject(LuaView, Session, Graph, Path, GName);
		Graphs[GName] = GraphObj;
		Graphs[GraphIdx++] = GraphObj;
	};

	TSet<UEdGraph*> BuiltGraphs;
	TArray<UEdGraph*> AllGraphs;
	Info.Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || BuiltGraphs.Contains(Graph)) continue;
		BuiltGraphs.Add(Graph);
		BuildGraph(Graph);
	}

	TArray<TPair<FString, UEdGraph*>> AnimGraphs;
	NeoBlueprint::CollectAnimBPGraphs(Info.Blueprint, AnimGraphs);
	for (const auto& Pair : AnimGraphs)
	{
		if (!Pair.Value) continue;
		std::string SelectorName = TCHAR_TO_UTF8(*Pair.Key);
		sol::table GraphObj = BuildGraphObject(LuaView, Session, Pair.Value, Path, SelectorName);
		Graphs[SelectorName] = GraphObj;
		Graphs[GraphIdx++] = GraphObj;
	}
	BP["graphs"] = Graphs;

	BP["variables"] = BuildVariablesTable(LuaView, Info.Blueprint);

	sol::table Components = LuaView.create_table();
	if (Info.Blueprint->SimpleConstructionScript)
	{
		int32 CompIdx = 1;
		for (USCS_Node* SCSNode : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (!SCSNode) continue;
			sol::table CompEntry = BuildComponentTable(LuaView, SCSNode);

			if (!CompEntry["parent"].valid())
			{
				USCS_Node* ParentNode = Info.Blueprint->SimpleConstructionScript->FindParentNode(SCSNode);
				if (ParentNode)
				{
					CompEntry["parent"] = TCHAR_TO_UTF8(*ParentNode->GetVariableName().ToString());
				}
			}

			Components[TCHAR_TO_UTF8(*SCSNode->GetVariableName().ToString())] = CompEntry;
			Components[CompIdx++] = CompEntry;
		}
	}
	BP["components"] = Components;

	sol::table Interfaces = LuaView.create_table();
	int32 IfIdx = 1;
	for (const FBPInterfaceDescription& Desc : Info.Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface)
		{
			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*Desc.Interface->GetName());
			Entry["class_path"] = TCHAR_TO_UTF8(*Desc.Interface->GetClassPathName().ToString());

			sol::table Funcs = LuaView.create_table();
			int32 FIdx = 1;
			for (UEdGraph* FuncGraph : Desc.Graphs)
			{
				if (FuncGraph)
				{
					Funcs[FIdx++] = TCHAR_TO_UTF8(*FuncGraph->GetName());
				}
			}
			Entry["functions"] = Funcs;
			Interfaces[IfIdx++] = Entry;
		}
	}
	BP["interfaces"] = Interfaces;

	return true;
}

// No docs — _open_blueprint is internal-only, called by open_asset for Blueprint assets
static TArray<FLuaFunctionDoc> BlueprintDocs = {};

static void BindBlueprint(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_open_blueprint", [&Session](const std::string& Path, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FPath = NeoLuaStr::ToFString(Path);

		FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
		if (!Info.Blueprint)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] open_blueprint(\"%s\") -> not found"), *FPath));
			return sol::lua_nil;
		}

		sol::table BP = LuaView.create_table();
		BP["name"] = TCHAR_TO_UTF8(*Info.Name);
		BP["path"] = TCHAR_TO_UTF8(*Info.AssetPath);
		BP["parent_class"] = TCHAR_TO_UTF8(*Info.ParentClass);

		// Build graphs table keyed by name (and by index)
		sol::table Graphs = LuaView.create_table();
		int32 GraphIdx = 1;

		auto BuildGraph = [&](UEdGraph* Graph)
		{
			if (!Graph) return;
			std::string GName = TCHAR_TO_UTF8(*Graph->GetName());
			sol::table GraphObj = BuildGraphObject(LuaView, Session, Graph, Path, GName);
			Graphs[GName] = GraphObj;
			Graphs[GraphIdx++] = GraphObj;
		};

		TSet<UEdGraph*> BuiltGraphs;
		TArray<UEdGraph*> AllGraphs;
		Info.Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || BuiltGraphs.Contains(Graph)) continue;
			BuiltGraphs.Add(Graph);
			BuildGraph(Graph);
		}

		// AnimBP: collect nested state machine / state / transition / layer graphs
		{
			TArray<TPair<FString, UEdGraph*>> AnimGraphs;
			NeoBlueprint::CollectAnimBPGraphs(Info.Blueprint, AnimGraphs);
			for (const auto& Pair : AnimGraphs)
			{
				if (!Pair.Value) continue;
				std::string SelectorName = TCHAR_TO_UTF8(*Pair.Key);
				sol::table GraphObj = BuildGraphObject(LuaView, Session, Pair.Value, Path, SelectorName);
				Graphs[SelectorName] = GraphObj;
				Graphs[GraphIdx++] = GraphObj;
			}
		}

		BP["graphs"] = Graphs;

		// bp.variables — keyed by name
		BP["variables"] = BuildVariablesTable(LuaView, Info.Blueprint);

		// bp.components — from SCS tree (all nodes including children)
		sol::table Components = LuaView.create_table();
		if (Info.Blueprint->SimpleConstructionScript)
		{
			int32 CompIdx = 1;
			for (USCS_Node* SCSNode : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (!SCSNode) continue;
				sol::table CompEntry = BuildComponentTable(LuaView, SCSNode);

				// Add SCS tree parent if this is a child node (not a root)
				if (!CompEntry["parent"].valid())
				{
					USCS_Node* ParentNode = Info.Blueprint->SimpleConstructionScript->FindParentNode(SCSNode);
					if (ParentNode)
					{
						CompEntry["parent"] = TCHAR_TO_UTF8(*ParentNode->GetVariableName().ToString());
					}
				}

				Components[TCHAR_TO_UTF8(*SCSNode->GetVariableName().ToString())] = CompEntry;
				Components[CompIdx++] = CompEntry;
			}
		}
		BP["components"] = Components;

		// bp.interfaces — list of implemented interfaces
		sol::table Interfaces = LuaView.create_table();
		{
			int32 IfIdx = 1;
			for (const FBPInterfaceDescription& Desc : Info.Blueprint->ImplementedInterfaces)
			{
				if (Desc.Interface)
				{
					sol::table Entry = LuaView.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Desc.Interface->GetName());
					Entry["class_path"] = TCHAR_TO_UTF8(*Desc.Interface->GetClassPathName().ToString());

					sol::table Funcs = LuaView.create_table();
					int32 FIdx = 1;
					for (UEdGraph* FuncGraph : Desc.Graphs)
					{
						if (FuncGraph)
						{
							Funcs[FIdx++] = TCHAR_TO_UTF8(*FuncGraph->GetName());
						}
					}
					Entry["functions"] = Funcs;
					Interfaces[IfIdx++] = Entry;
				}
			}
		}
		BP["interfaces"] = Interfaces;

		std::string PathStr = Path;
		auto RefreshSelf = [&Session, PathStr](sol::table Self, sol::this_state State) -> bool
		{
			sol::state_view RefreshLua(State);
			return RefreshBlueprintStateTables(RefreshLua, Session, Self, PathStr);
		};

		// bp:refresh() - rebuild graphs/variables/components/interfaces on this handle
		BP.set_function("refresh", [RefreshSelf](sol::table Self, sol::this_state State) -> sol::object
		{
			if (!RefreshSelf(Self, State))
			{
				return sol::lua_nil;
			}
			sol::state_view RefreshLua(State);
			return sol::make_object(RefreshLua, Self);
		});

		// bp:find_nodes(query, max?) — context-aware search
		BP.set_function("find_nodes", [PathStr](sol::table /*self*/,
			const std::string& Query, sol::optional<int> Max,
			sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			sol::protected_function fn = lua["find_nodes"];
			auto result = fn(Query, PathStr, Max);
			if (result.valid()) return result;
			return sol::lua_nil;
		});

		// bp:add_variable(name, type, options?) — options table or plain default value
		BP.set_function("add_variable", [PathStr, RefreshSelf](sol::table Self,
			const std::string& Name, const std::string& Type,
			sol::optional<sol::object> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			sol::protected_function fn = lua["add_variable"];
			auto result = fn(PathStr, Name, Type, Options);
			if (result.valid())
			{
				RefreshSelf(Self, S);
				return result;
			}
			return sol::lua_nil;
		});

		// bp:add_component(name, class, parent?)
		BP.set_function("add_component", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& Name, const std::string& ClassName,
			sol::optional<std::string> Parent, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FName = NeoLuaStr::ToFString(Name);
			FString FClass = NeoLuaStr::ToFString(ClassName);
			FString FParent = NeoLuaStr::ToFStringOpt(Parent);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_component -> blueprint not found")));
				return sol::lua_nil;
			}

			USCS_Node* Node = NeoBlueprint::AddComponent(Info.Blueprint, FName, FClass, FParent);
			if (!Node)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_component(\"%s\", \"%s\") -> failed. Class not found or not a component type."),
					*FName, *FClass));
				return sol::lua_nil;
			}

			sol::table Result = BuildComponentTable(lua, Node);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] add_component(\"%s\", \"%s\") -> added"),
				*FName, *FClass));
			return Result;
		});

		// bp:add_function(name, options?) -> returns graph object (upsert: creates or updates)
		// Options: {pure, const_func, category, params={{name,type},...}, returns={{name,type},...}}
		BP.set_function("add_function", [&Session, FPath, PathStr, RefreshSelf](sol::table Self,
			const std::string& FuncName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FFuncName = NeoLuaStr::ToFString(FuncName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_function -> blueprint not found")));
				return sol::lua_nil;
			}

			bool bFuncExisted = (NeoBlueprint::FindGraph(Info.Blueprint, FFuncName) != nullptr);
			UEdGraph* FuncGraph = NeoBlueprint::AddFunctionGraph(Info.Blueprint, FFuncName);
			if (!FuncGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_function(\"%s\") -> failed"), *FFuncName));
				return sol::lua_nil;
			}

			// Apply options
			if (Options.has_value())
			{
				sol::table Opts = Options.value();

				// Function flags (supports both adding and removing)
				uint32 AddFlags = 0, RemoveFlags = 0;
				sol::object PureObj = Opts["pure"];
				if (PureObj.valid() && PureObj.get_type() != sol::type::lua_nil)
				{
					if (PureObj.as<bool>()) AddFlags |= FUNC_BlueprintPure;
					else RemoveFlags |= FUNC_BlueprintPure;
				}
				sol::object ConstObj = Opts["const_func"];
				if (ConstObj.valid() && ConstObj.get_type() != sol::type::lua_nil)
				{
					if (ConstObj.as<bool>()) AddFlags |= FUNC_Const;
					else RemoveFlags |= FUNC_Const;
				}
				if (AddFlags != 0 || RemoveFlags != 0)
				{
					NeoBlueprint::SetFunctionFlags(Info.Blueprint, FFuncName, AddFlags, RemoveFlags);
				}

				// Params — accept both "params" and "inputs" keys
				auto AddParamsFromTable = [&](sol::table ParamTable)
				{
					for (auto& Pair : ParamTable)
					{
						if (Pair.second.is<sol::table>())
						{
							sol::table P = Pair.second.as<sol::table>();
							std::string PName = P.get_or<std::string>("name", "");
							std::string PType = P.get_or<std::string>("type", "");
							if (!PName.empty() && !PType.empty())
							{
								FString FPName = NeoLuaStr::ToFString(PName);
								FString FPType = NeoLuaStr::ToFString(PType);
								if (NeoBlueprint::AddFunctionParam(Info.Blueprint, FFuncName, FPName, FPType))
								{
									Session.Log(FString::Printf(TEXT("[OK] add_function param \"%s\" (%s)"), *FPName, *FPType));
								}
							}
						}
					}
				};
				sol::object ParamsObj = Opts["params"];
				if (!ParamsObj.valid() || !ParamsObj.is<sol::table>()) ParamsObj = Opts["inputs"];
				if (ParamsObj.valid() && ParamsObj.is<sol::table>())
				{
					AddParamsFromTable(ParamsObj.as<sol::table>());
				}

				// Returns — accept both "returns" and "outputs" keys
				auto AddReturnsFromTable = [&](sol::table ReturnTable)
				{
					for (auto& Pair : ReturnTable)
					{
						if (Pair.second.is<sol::table>())
						{
							sol::table R = Pair.second.as<sol::table>();
							std::string RName = R.get_or<std::string>("name", "");
							std::string RType = R.get_or<std::string>("type", "");
							if (!RName.empty() && !RType.empty())
							{
								FString FRName = NeoLuaStr::ToFString(RName);
								FString FRType = NeoLuaStr::ToFString(RType);
								if (NeoBlueprint::AddFunctionReturn(Info.Blueprint, FFuncName, FRName, FRType))
								{
									Session.Log(FString::Printf(TEXT("[OK] add_function return \"%s\" (%s)"), *FRName, *FRType));
								}
							}
						}
					}
				};
				sol::object ReturnsObj = Opts["returns"];
				if (!ReturnsObj.valid() || !ReturnsObj.is<sol::table>()) ReturnsObj = Opts["outputs"];
				if (ReturnsObj.valid() && ReturnsObj.is<sol::table>())
				{
					AddReturnsFromTable(ReturnsObj.as<sol::table>());
				}

				// Function metadata (category/description/keywords/compact_title/deprecated/
				// deprecation_message/call_in_editor/thread_safe) — applied via shared helper.
				ApplyFunctionMetadata(Info.Blueprint, FuncGraph, Opts);
			}

			sol::table GraphObj = BuildGraphObject(lua, Session, FuncGraph, PathStr, FuncName);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] add_function(\"%s\") -> %s with %d nodes"),
				*FFuncName, bFuncExisted ? TEXT("updated") : TEXT("created"), FuncGraph->Nodes.Num()));
			return GraphObj;
		});

		// bp:set_property(name, props_table) — set metadata on existing variable or function
		BP.set_function("set_property", [&Session, FPath](sol::table /*self*/,
			const std::string& TargetName, sol::table Props, sol::this_state S) -> sol::object
		{
			FString FTarget = NeoLuaStr::ToFString(TargetName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_property -> blueprint not found")));
				return sol::lua_nil;
			}

			// Check if target is a function (has graph) — apply function flags + metadata
			UEdGraph* Graph = NeoBlueprint::FindGraph(Info.Blueprint, FTarget);
			if (Graph)
			{
				uint32 AddFlags = 0, RemoveFlags = 0;

				sol::object PureObj = Props["pure"];
				if (PureObj.valid() && PureObj.get_type() != sol::type::lua_nil)
				{
					if (PureObj.as<bool>()) AddFlags |= FUNC_BlueprintPure;
					else RemoveFlags |= FUNC_BlueprintPure;
				}

				sol::object ConstObj = Props["const_func"];
				if (ConstObj.valid() && ConstObj.get_type() != sol::type::lua_nil)
				{
					if (ConstObj.as<bool>()) AddFlags |= FUNC_Const;
					else RemoveFlags |= FUNC_Const;
				}

				if (AddFlags != 0 || RemoveFlags != 0)
				{
					NeoBlueprint::SetFunctionFlags(Info.Blueprint, FTarget, AddFlags, RemoveFlags);
					Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\") -> function flags updated"), *FTarget));
				}

				// Function metadata via shared helper — same keys as bp:add_function.
				if (ApplyFunctionMetadata(Info.Blueprint, Graph, Props) > 0)
				{
					Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\") -> function metadata updated"), *FTarget));
				}
			}

			// Apply variable properties (works for any variable including delegates)
			auto SolToStr = [](const sol::object& Obj) -> FString
			{
				if (Obj.is<std::string>()) return NeoLuaStr::ToFString(Obj.as<std::string>());
				if (Obj.is<bool>()) return Obj.as<bool>() ? TEXT("true") : TEXT("false");
				if (Obj.is<double>())
				{
					double V = Obj.as<double>();
					if (FMath::IsNearlyEqual(V, FMath::RoundToDouble(V)))
						return FString::Printf(TEXT("%d"), (int64)V);
					return FString::Printf(TEXT("%g"), V);
				}
				return TEXT("");
			};

			static const TArray<const char*> VarKeys = {
				"category", "tooltip", "replicated", "rep_notify", "edit_anywhere",
				"edit_defaults_only", "edit_instance_only", "blueprint_read_only",
				"save_game", "transient", "expose_on_spawn"
			};

			bool bAnySet = false;
			FName VarName(*FTarget);
			for (const char* Key : VarKeys)
			{
				sol::object Val = Props[Key];
				if (Val.valid() && Val.get_type() != sol::type::lua_nil)
				{
					FString StrVal = SolToStr(Val);
					if (NeoBlueprint::SetVariableProperty(Info.Blueprint, FTarget, FString(Key), StrVal))
					{
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"%s\") = %s"), *FTarget, UTF8_TO_TCHAR(Key), *StrVal));
						bAnySet = true;
					}
				}
			}

			// Interp flag (expose to Sequencer/Cinematics)
			sol::object InterpObj = Props["interp"];
			if (InterpObj.valid() && InterpObj.get_type() != sol::type::lua_nil)
			{
				FBlueprintEditorUtils::SetInterpFlag(Info.Blueprint, VarName, InterpObj.as<bool>());
				Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"interp\") = %s"), *FTarget, InterpObj.as<bool>() ? TEXT("true") : TEXT("false")));
				bAnySet = true;
			}

			// Advanced Display flag
			sol::object AdvancedObj = Props["advanced_display"];
			if (AdvancedObj.valid() && AdvancedObj.get_type() != sol::type::lua_nil)
			{
				FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(Info.Blueprint, VarName, AdvancedObj.as<bool>());
				Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"advanced_display\") = %s"), *FTarget, AdvancedObj.as<bool>() ? TEXT("true") : TEXT("false")));
				bAnySet = true;
			}

			// Deprecated flag
			sol::object DeprecatedObj = Props["deprecated"];
			if (DeprecatedObj.valid() && DeprecatedObj.get_type() != sol::type::lua_nil)
			{
				FBlueprintEditorUtils::SetVariableDeprecatedFlag(Info.Blueprint, VarName, DeprecatedObj.as<bool>());
				Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"deprecated\") = %s"), *FTarget, DeprecatedObj.as<bool>() ? TEXT("true") : TEXT("false")));
				bAnySet = true;
			}

	
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				// Variable reordering
				sol::optional<std::string> MoveBeforeOpt = Props["move_before"];
				if (MoveBeforeOpt.has_value())
				{
					FString MoveBeforeTarget = NeoLuaStr::ToFStringOpt(MoveBeforeOpt);
					bool bOk = FBlueprintEditorUtils::MoveVariableBeforeVariable(Info.Blueprint, nullptr, VarName, FName(*MoveBeforeTarget), false);
					if (bOk) Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"move_before\") = %s"), *FTarget, *MoveBeforeTarget));
					else Session.Log(FString::Printf(TEXT("[FAIL] set_property -> move_before '%s' failed"), *MoveBeforeTarget));
				}

				sol::optional<std::string> MoveAfterOpt = Props["move_after"];
				if (MoveAfterOpt.has_value())
				{
					FString MoveAfterTarget = NeoLuaStr::ToFStringOpt(MoveAfterOpt);
					bool bOk = FBlueprintEditorUtils::MoveVariableAfterVariable(Info.Blueprint, nullptr, VarName, FName(*MoveAfterTarget), false);
					if (bOk) Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", \"move_after\") = %s"), *FTarget, *MoveAfterTarget));
					else Session.Log(FString::Printf(TEXT("[FAIL] set_property -> move_after '%s' failed"), *MoveAfterTarget));
				}
#else
				sol::optional<std::string> MoveBeforeOpt = Props["move_before"];
				sol::optional<std::string> MoveAfterOpt = Props["move_after"];
				if (MoveBeforeOpt.has_value() || MoveAfterOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] variable reordering (move_before/move_after) requires UE 5.5+"));
				}
#endif

// Arbitrary metadata (set or remove)
			sol::optional<sol::table> MetaData = Props.get<sol::optional<sol::table>>("metadata");
			if (MetaData.has_value())
			{
				for (auto& KV : MetaData.value())
				{
					if (!KV.first.is<std::string>()) continue;
					FString MetaKey = NeoLuaStr::ToFString(KV.first.as<std::string>());
					if (KV.second.is<std::string>())
					{
						FString MetaValue = NeoLuaStr::ToFString(KV.second.as<std::string>());
						FBlueprintEditorUtils::SetBlueprintVariableMetaData(Info.Blueprint, VarName, nullptr, FName(*MetaKey), MetaValue);
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", metadata.\"%s\") = \"%s\""), *FTarget, *MetaKey, *MetaValue));
						bAnySet = true;
					}
					else if (KV.second.is<sol::lua_nil_t>())
					{
						FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Info.Blueprint, VarName, nullptr, FName(*MetaKey));
						Session.Log(FString::Printf(TEXT("[OK] set_property(\"%s\", metadata.\"%s\") -> removed"), *FTarget, *MetaKey));
						bAnySet = true;
					}
				}
			}

			return sol::make_object(S, bAnySet || Graph != nullptr);
		});

		// bp:set(target, property, value) or bp:set(property, value) — universal property setter via reflection
		// target: component name, "self" for CDO, or variable name (defaults to "self" when omitted)
		// value: string or table (for struct types like Vector {x=,y=,z=}, Rotator {pitch=,yaw=,roll=}, Color {r=,g=,b=,a=})
		BP.set_function("set", [&Session, FPath](sol::table /*self*/,
			const std::string& FirstArg, sol::object SecondObj, sol::optional<sol::object> ThirdObj,
			sol::this_state S) -> sol::object
		{
			FString FTarget, FProp;
			sol::object ValueObj;
			if (ThirdObj.has_value())
			{
				// 3-arg form: set(target, property, value)
				FTarget = NeoLuaStr::ToFString(FirstArg);
				FProp = SecondObj.is<std::string>() ? NeoLuaStr::ToFString(SecondObj.as<std::string>()) : TEXT("");
				ValueObj = ThirdObj.value();
			}
			else
			{
				// 2-arg form: set(property, value) — target defaults to "self"
				FTarget = TEXT("self");
				FProp = NeoLuaStr::ToFString(FirstArg);
				ValueObj = SecondObj;
			}

			// Convert value to string — handle tables for struct types
			FString FValue;
			if (ValueObj.is<std::string>())
			{
				FValue = NeoLuaStr::ToFString(ValueObj.as<std::string>());
			}
			else if (ValueObj.is<bool>())
			{
				FValue = ValueObj.as<bool>() ? TEXT("true") : TEXT("false");
			}
			else if (ValueObj.is<double>())
			{
				double V = ValueObj.as<double>();
				if (FMath::IsNearlyEqual(V, FMath::RoundToDouble(V)))
					FValue = FString::Printf(TEXT("%lld"), (long long)V);
				else
					FValue = FString::Printf(TEXT("%g"), V);
			}
			else if (ValueObj.is<sol::table>())
			{
				sol::table T = ValueObj.as<sol::table>();
				// Detect struct type from table keys — use UE parenthesized struct format
				if (T["x"].valid() || T["X"].valid())
				{
					// Vector/Vector2D: {x=,y=,z=} -> (X=...,Y=...,Z=...)
					double X = T.get_or("x", T.get_or("X", 0.0));
					double Y = T.get_or("y", T.get_or("Y", 0.0));
					double Z = T.get_or("z", T.get_or("Z", 0.0));
					FValue = FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), X, Y, Z);
				}
				else if (T["pitch"].valid() || T["Pitch"].valid())
				{
					// Rotator: {pitch=,yaw=,roll=} -> (Pitch=...,Yaw=...,Roll=...)
					double P = T.get_or("pitch", T.get_or("Pitch", 0.0));
					double Y = T.get_or("yaw", T.get_or("Yaw", 0.0));
					double R = T.get_or("roll", T.get_or("Roll", 0.0));
					FValue = FString::Printf(TEXT("(Pitch=%g,Yaw=%g,Roll=%g)"), P, Y, R);
				}
				else if (T["r"].valid() || T["R"].valid())
				{
					// Color: {r=,g=,b=,a=} -> (R=...,G=...,B=...,A=...)
					double R = T.get_or("r", T.get_or("R", 0.0));
					double G = T.get_or("g", T.get_or("G", 0.0));
					double B = T.get_or("b", T.get_or("B", 0.0));
					double A = T.get_or("a", T.get_or("A", 1.0));
					FValue = FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"), R, G, B, A);
				}
				else
				{
					Session.Log(TEXT("[FAIL] set -> table value not recognized. Use {x=,y=,z=} for Vector, {pitch=,yaw=,roll=} for Rotator, {r=,g=,b=,a=} for Color"));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] set -> value must be a string, number, bool, or table"));
				return sol::lua_nil;
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set -> blueprint not found")));
				return sol::lua_nil;
			}

			UObject* TargetObj = nullptr;
			FString TargetDesc;

			if (FTarget.ToLower() == TEXT("self"))
			{
				// Blueprint CDO
				if (Info.Blueprint->GeneratedClass)
				{
					TargetObj = Info.Blueprint->GeneratedClass->GetDefaultObject();
					TargetDesc = TEXT("CDO");
				}
			}
			else
			{
				// Try component template first
				TargetObj = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FTarget);
				if (TargetObj)
				{
					TargetDesc = FString::Printf(TEXT("component %s"), *FTarget);
				}
			}

			if (!TargetObj)
			{
				// Try custom event node
				UK2Node_CustomEvent* CE = NeoBlueprint::FindCustomEvent(Info.Blueprint, FTarget);
				if (CE)
				{
					FString PropLower = FProp.ToLower();
					if (PropLower == TEXT("replicated"))
					{
						// Clear existing net flags first
						CE->FunctionFlags &= ~(FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient);
						FString ValLower = FValue.ToLower();
						if (ValLower == TEXT("multicast")) CE->FunctionFlags |= (FUNC_Net | FUNC_NetMulticast);
						else if (ValLower == TEXT("server")) CE->FunctionFlags |= (FUNC_Net | FUNC_NetServer);
						else if (ValLower == TEXT("client")) CE->FunctionFlags |= (FUNC_Net | FUNC_NetClient);
					}
					else if (PropLower == TEXT("reliable"))
					{
						if (FValue.ToLower() == TEXT("true")) CE->FunctionFlags |= FUNC_NetReliable;
						else CE->FunctionFlags &= ~FUNC_NetReliable;
					}
					else if (PropLower == TEXT("call_in_editor"))
					{
						CE->bCallInEditor = (FValue.ToLower() == TEXT("true"));
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (custom event)"),
						*FTarget, *FProp, *FValue));
					return sol::make_object(S, true);
				}

				// Try timeline template
				UTimelineTemplate* TLTemplate = Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTarget));
				if (TLTemplate)
				{
					FString PropLower = FProp.ToLower();
					if (PropLower == TEXT("length")) TLTemplate->TimelineLength = FCString::Atof(*FValue);
					else if (PropLower == TEXT("auto_play")) TLTemplate->bAutoPlay = (FValue.ToLower() == TEXT("true"));
					else if (PropLower == TEXT("loop")) TLTemplate->bLoop = (FValue.ToLower() == TEXT("true"));
					else if (PropLower == TEXT("replicated")) TLTemplate->bReplicated = (FValue.ToLower() == TEXT("true"));
					else if (PropLower == TEXT("ignore_time_dilation")) TLTemplate->bIgnoreTimeDilation = (FValue.ToLower() == TEXT("true"));
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\", \"%s\") -> unknown timeline property. Use: length, auto_play, loop, replicated, ignore_time_dilation"),
							*FTarget, *FProp));
						return sol::lua_nil;
					}
					// Sync node flags
					UK2Node_Timeline* TLNode = NeoBlueprint::FindTimelineNode(Info.Blueprint, FTarget);
					if (TLNode)
					{
						TLNode->bAutoPlay = TLTemplate->bAutoPlay;
						TLNode->bLoop = TLTemplate->bLoop;
						TLNode->bReplicated = TLTemplate->bReplicated;
						TLNode->bIgnoreTimeDilation = TLTemplate->bIgnoreTimeDilation;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (timeline)"),
						*FTarget, *FProp, *FValue));
					return sol::make_object(S, true);
				}

				// Try variable metadata via set_property
				if (NeoBlueprint::SetVariableProperty(Info.Blueprint, FTarget, FProp, FValue))
				{
					Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (variable metadata)"),
						*FTarget, *FProp, *FValue));
					return sol::make_object(S, true);
				}

				// Try treating target as a variable name — set its default value on the CDO
				if (FProp.ToLower() == TEXT("default") && Info.Blueprint->GeneratedClass)
				{
					UObject* CDO = Info.Blueprint->GeneratedClass->GetDefaultObject();
					if (CDO)
					{
						FString CDOError;
						if (NeoBlueprint::SetObjectProperty(CDO, FTarget, FValue, CDOError))
						{
							Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"default\") = \"%s\" (variable default on CDO)"),
								*FTarget, *FValue));
							return sol::make_object(S, true);
						}
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\") -> not found as component, CDO, custom event, timeline, or variable"),
					*FTarget));
				return sol::lua_nil;
			}

			FString Error;
			if (!NeoBlueprint::SetObjectProperty(TargetObj, FProp, FValue, Error))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set(\"%s\", \"%s\") -> %s"), *FTarget, *FProp, *Error));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] set(\"%s\", \"%s\") = \"%s\" (%s)"),
				*FTarget, *FProp, *FValue, *TargetDesc));
			return sol::make_object(S, true);
		});

		// bp:get(target, property) or bp:get(property) — universal property getter
		// When called with 1 arg, target defaults to "self" (class defaults)
		BP.set_function("get", [&Session, FPath](sol::table /*self*/,
			const std::string& FirstArg, sol::optional<std::string> SecondArg,
			sol::this_state S) -> sol::object
		{
			FString FTarget, FProp;
			if (SecondArg.has_value())
			{
				FTarget = NeoLuaStr::ToFString(FirstArg);
				FProp = NeoLuaStr::ToFStringOpt(SecondArg);
			}
			else
			{
				FTarget = TEXT("self");
				FProp = NeoLuaStr::ToFString(FirstArg);
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get -> blueprint not found")));
				return sol::lua_nil;
			}

			UObject* TargetObj = nullptr;
			if (FTarget.ToLower() == TEXT("self"))
			{
				if (Info.Blueprint->GeneratedClass)
					TargetObj = Info.Blueprint->GeneratedClass->GetDefaultObject();
			}
			else
			{
				TargetObj = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FTarget);
			}

			if (!TargetObj)
			{
				// Try treating target as a variable name — get its default value from the CDO
				if (FProp.ToLower() == TEXT("default") && Info.Blueprint->GeneratedClass)
				{
					UObject* CDO = Info.Blueprint->GeneratedClass->GetDefaultObject();
					if (CDO)
					{
						FString Value;
						if (NeoBlueprint::GetObjectProperty(CDO, FTarget, Value))
						{
							return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
						}
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\") -> target not found"), *FTarget));
				return sol::lua_nil;
			}

			FString Value;
			if (!NeoBlueprint::GetObjectProperty(TargetObj, FProp, Value))
			{
				FString Error = NeoBlueprint::FuzzyMatchProperty(TargetObj, FProp);
				Session.Log(FString::Printf(TEXT("[FAIL] get(\"%s\", \"%s\") -> %s"), *FTarget, *FProp, *Error));
				return sol::lua_nil;
			}

			return sol::make_object(S, std::string(TCHAR_TO_UTF8(*Value)));
		});

		// bp:list_properties(target, filter?) — discover available properties
		BP.set_function("list_properties", [&Session, FPath](sol::table /*self*/,
			const std::string& Target, sol::optional<std::string> Filter,
			sol::this_state S) -> sol::object
		{
			sol::state_view LuaView(S);
			FString FTarget = NeoLuaStr::ToFString(Target);
			FString FFilter = NeoLuaStr::ToFStringOpt(Filter);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] list_properties -> blueprint not found"));
				return sol::lua_nil;
			}

			UObject* TargetObj = nullptr;
			if (FTarget.ToLower() == TEXT("self"))
			{
				if (Info.Blueprint->GeneratedClass)
					TargetObj = Info.Blueprint->GeneratedClass->GetDefaultObject();
			}
			else
			{
				TargetObj = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FTarget);
			}

			if (!TargetObj)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list_properties(\"%s\") -> target not found"), *FTarget));
				return sol::lua_nil;
			}

			TArray<TPair<FString, FString>> Props;
			NeoBlueprint::ListObjectProperties(TargetObj, FFilter, Props);

			sol::table Result = LuaView.create_table();
			for (int32 i = 0; i < Props.Num(); i++)
			{
				sol::table Entry = LuaView.create_table();
				Entry["property"] = TCHAR_TO_UTF8(*Props[i].Key);
				Entry["value"] = TCHAR_TO_UTF8(*Props[i].Value);
				Result[i + 1] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_properties(\"%s\"%s) -> %d properties"),
				*FTarget, FFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", \"%s\""), *FFilter), Props.Num()));
			return Result;
		});

		// bp:remove(name) — unified remove: detects variable/function/component
		// bp:remove(name) or bp:remove("component", name) — unified remove
		BP.set_function("remove", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& NameOrType, sol::optional<std::string> OptName, sol::this_state S) -> sol::object
		{
			// Support two-arg format: remove("component", "MyAudio") where first arg is a type hint
			FString FName;
			if (OptName.has_value())
			{
				FName = NeoLuaStr::ToFString(*OptName);
			}
			else
			{
				FName = NeoLuaStr::ToFString(NameOrType);
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] remove -> blueprint not found"));
				return sol::lua_nil;
			}

			// Try component first
			if (NeoBlueprint::RemoveComponent(Info.Blueprint, FName))
			{
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> component removed"), *FName));
				return sol::make_object(S, true);
			}

			// Try function graph
			if (NeoBlueprint::RemoveFunction(Info.Blueprint, FName))
			{
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> function removed"), *FName));
				return sol::make_object(S, true);
			}

			// Try variable (handles event dispatchers too)
			if (NeoBlueprint::RemoveVariable(Info.Blueprint, FName))
			{
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> variable removed"), *FName));
				return sol::make_object(S, true);
			}

			// Try timeline
			if (NeoBlueprint::RemoveTimeline(Info.Blueprint, FName))
			{
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"%s\") -> timeline removed"), *FName));
				return sol::make_object(S, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> not found as component, function, variable, or timeline"), *FName));
			return sol::lua_nil;
		});

		// bp:rename(old_name, new_name) — unified rename: detects variable/function
		BP.set_function("rename", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& OldName, const std::string& NewName,
			sol::this_state S) -> sol::object
		{
			FString FOld = NeoLuaStr::ToFString(OldName);
			FString FNew = NeoLuaStr::ToFString(NewName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] rename -> blueprint not found"));
				return sol::lua_nil;
			}

			// Try variable rename first (handles delegates too)
			if (NeoBlueprint::RenameVariable(Info.Blueprint, FOld, FNew))
			{
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] rename(\"%s\") -> \"%s\" (variable)"), *FOld, *FNew));
				return sol::make_object(S, true);
			}

			// Try function rename
			if (NeoBlueprint::RenameFunction(Info.Blueprint, FOld, FNew))
			{
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] rename(\"%s\") -> \"%s\" (function)"), *FOld, *FNew));
				return sol::make_object(S, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] rename(\"%s\") -> not found as variable or function"), *FOld));
			return sol::lua_nil;
		});

		// bp:add_interface(name) — implement a Blueprint Interface
		BP.set_function("add_interface", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& InterfaceName, sol::this_state S) -> sol::object
		{
			FString FName = NeoLuaStr::ToFString(InterfaceName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_interface -> blueprint not found"));
				return sol::lua_nil;
			}

			if (!NeoBlueprint::AddInterface(Info.Blueprint, FName))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_interface(\"%s\") -> interface not found or already implemented"), *FName));
				return sol::lua_nil;
			}

			// Count the generated function stubs
			int32 FuncCount = 0;
			for (const FBPInterfaceDescription& Desc : Info.Blueprint->ImplementedInterfaces)
			{
				if (Desc.Interface && Desc.Interface->GetName().Contains(FName.Replace(TEXT("/Game/"), TEXT(""))))
				{
					FuncCount = Desc.Graphs.Num();
					break;
				}
			}

			Session.Log(FString::Printf(TEXT("[OK] add_interface(\"%s\") -> implemented with %d function stubs"),
				*FName, FuncCount));
			RefreshSelf(Self, S);
			return sol::make_object(S, true);
		});

		// bp:remove_interface(name, preserve_functions?) — remove an implemented interface
		BP.set_function("remove_interface", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& InterfaceName, sol::optional<bool> Preserve, sol::this_state S) -> sol::object
		{
			FString FName = NeoLuaStr::ToFString(InterfaceName);
			bool bPreserve = Preserve.value_or(false);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] remove_interface -> blueprint not found"));
				return sol::lua_nil;
			}

			if (!NeoBlueprint::RemoveInterface(Info.Blueprint, FName, bPreserve))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_interface(\"%s\") -> not found or not implemented"), *FName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] remove_interface(\"%s\")%s"),
				*FName, bPreserve ? TEXT(" (functions preserved)") : TEXT("")));
			RefreshSelf(Self, S);
			return sol::make_object(S, true);
		});

		// bp:add_macro(name) — create a macro graph
		BP.set_function("add_macro", [&Session, FPath, PathStr, RefreshSelf](sol::table Self,
			const std::string& MacroName, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FMacroName = NeoLuaStr::ToFString(MacroName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_macro -> blueprint not found"));
				return sol::lua_nil;
			}

			UEdGraph* MacroGraph = NeoBlueprint::AddMacroGraph(Info.Blueprint, FMacroName);
			if (!MacroGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_macro(\"%s\") -> failed"), *FMacroName));
				return sol::lua_nil;
			}

			sol::table GraphObj = BuildGraphObject(lua, Session, MacroGraph, PathStr, MacroName);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] add_macro(\"%s\") -> created with %d nodes"),
				*FMacroName, MacroGraph->Nodes.Num()));
			return GraphObj;
		});

		// bp:add_custom_event(name, options?) — create a custom event node in EventGraph
		// Options: {params={{name,type},...}, replicated="multicast"|"server"|"client", reliable=bool, call_in_editor=bool}
		// Also accepts: add_custom_event(name, {{name,type},...}) — direct param array
		BP.set_function("add_custom_event", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& EventName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FEventName = NeoLuaStr::ToFString(EventName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_custom_event -> blueprint not found"));
				return sol::lua_nil;
			}

			// Helper: parse a param array table into TArray<FParamDesc>
			auto ParseParamArray = [](sol::table ParamTable, TArray<FParamDesc>& OutParams)
			{
				for (auto& Pair : ParamTable)
				{
					if (Pair.second.is<sol::table>())
					{
						sol::table P = Pair.second.as<sol::table>();
						FParamDesc Desc;
						Desc.Name = NeoLuaStr::ToFString(P.get_or<std::string>("name", ""));
						Desc.Type = NeoLuaStr::ToFString(P.get_or<std::string>("type", ""));
						if (!Desc.Name.IsEmpty() && !Desc.Type.IsEmpty())
						{
							OutParams.Add(MoveTemp(Desc));
						}
					}
				}
			};

			// Parse params from options
			TArray<FParamDesc> Params;
			if (Options.has_value())
			{
				sol::table Opts = Options.value();
				sol::object ParamsObj = Opts["params"];
				if (ParamsObj.valid() && ParamsObj.is<sol::table>())
				{
					// Explicit {params={{name,type},...}} format
					ParseParamArray(ParamsObj.as<sol::table>(), Params);
				}
				else
				{
					// Check if the table itself is a direct param array: {{name,type},...}
					sol::object FirstEntry = Opts[1];
					if (FirstEntry.valid() && FirstEntry.is<sol::table>())
					{
						sol::table FirstTable = FirstEntry.as<sol::table>();
						if (FirstTable["name"].valid() && FirstTable["type"].valid())
						{
							ParseParamArray(Opts, Params);
						}
					}
				}
			}

			bool bExisted = (NeoBlueprint::FindCustomEvent(Info.Blueprint, FEventName) != nullptr);
			UK2Node_CustomEvent* EventNode = NeoBlueprint::AddCustomEvent(Info.Blueprint, FEventName, Params);
			if (!EventNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_custom_event(\"%s\") -> failed (no EventGraph?)"), *FEventName));
				return sol::lua_nil;
			}

			// Apply options
			if (Options.has_value())
			{
				sol::table Opts = Options.value();

				// RPC replication
				std::string RepStr = Opts.get_or<std::string>("replicated", "");
				if (!RepStr.empty())
				{
					FString Rep = FString(NeoLuaStr::ToFString(RepStr)).ToLower();
					if (Rep == TEXT("multicast"))
					{
						EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetMulticast);
					}
					else if (Rep == TEXT("server"))
					{
						EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetServer);
					}
					else if (Rep == TEXT("client"))
					{
						EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetClient);
					}
				}

				// Reliable
				if (Opts.get_or("reliable", false))
				{
					EventNode->FunctionFlags |= FUNC_NetReliable;
				}

				// Call in editor
				if (Opts.get_or("call_in_editor", false))
				{
					EventNode->bCallInEditor = true;
				}
			}

			// Register in session node map
			FString NodeGuid = EventNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, EventNode);

			Session.Log(FString::Printf(TEXT("[OK] add_custom_event(\"%s\") -> %s with %d params, handle=\"%s\""),
				*FEventName, bExisted ? TEXT("updated") : TEXT("created"), Params.Num(), *NodeGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = EventName;
			RefreshSelf(Self, S);
			return Result;
		});

		// bp:add_timeline(name, options?) — create a timeline node with tracks
		// Options: {length=5.0, auto_play=bool, loop=bool, replicated=bool, ignore_time_dilation=bool,
		//   tracks={
		//     {name="Alpha",  type="float",  keys={{time=0, value=0, interp_mode="Cubic"}, {time=1, value=1}, {time=2, value=0}}},
		//     {name="Offset", type="vector", keys={{time=0, x=0, y=0, z=0}, {time=1, x=0, y=0, z=200}}},
		//     {name="Tint",   type="color",  keys={{time=0, r=1, g=0, b=0, a=1}, {time=1, r=0, g=1, b=0, a=1}}},
		//     {name="OnHit",  type="event",  keys={2.5, 4.0}}
		//   }}
		// Keys also accept the positional form {time, value} for backward compat with older scripts.
		BP.set_function("add_timeline", [&Session, FPath](sol::table /*self*/,
			const std::string& TimelineName, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FTLName = NeoLuaStr::ToFString(TimelineName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_timeline -> blueprint not found"));
				return sol::lua_nil;
			}

			float Length = 5.0f;
			bool bAutoPlay = false;
			bool bLoop = false;
			TArray<NeoBlueprint::FTimelineTrackDesc> Tracks;

			if (Options.has_value())
			{
				sol::table Opts = Options.value();
				Length = Opts.get_or("length", 5.0f);
				bAutoPlay = Opts.get_or("auto_play", false);
				bLoop = Opts.get_or("loop", false);

				// Parse a track table into a FTimelineTrackDesc. Key format accepts:
				//   Named-field (preferred):  {time=0, value=0.5, ...}
				//   Multi-component form:     {time=0, x=1, y=2, z=3} or {time=0, r=1, g=0, b=0, a=1}
				//   Positional (legacy):      {0, 0.5}   -- [1]=time, [2]=value
				// A bare number is accepted for event tracks (marker time only).
				auto ParseTrackTable = [&Session](sol::table T, const FString& DefaultType) -> NeoBlueprint::FTimelineTrackDesc
				{
					NeoBlueprint::FTimelineTrackDesc Desc;
					Desc.Name = NeoLuaStr::ToFString(T.get_or<std::string>("name", ""));
					Desc.Type = NeoLuaStr::ToFString(T.get_or<std::string>("type", TCHAR_TO_UTF8(*DefaultType)));

					const bool bIsVectorTrack = Desc.Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase);
					const bool bIsColorTrack = Desc.Type.Equals(TEXT("color"), ESearchCase::IgnoreCase) ||
						Desc.Type.Equals(TEXT("linearcolor"), ESearchCase::IgnoreCase);
					const bool bIsEventTrack = Desc.Type.Equals(TEXT("event"), ESearchCase::IgnoreCase);

					sol::object KeysObj = T["keys"];
					if (!KeysObj.valid() || !KeysObj.is<sol::table>()) return Desc;

					int32 Index = 0;
					for (auto& KPair : KeysObj.as<sol::table>())
					{
						++Index;
						// Shorthand for event tracks: keys = {1.0, 2.5, 4.0}
						if (bIsEventTrack && KPair.second.is<double>())
						{
							Desc.Keys.Add(TPair<float, FString>(static_cast<float>(KPair.second.as<double>()), TEXT("0")));
							continue;
						}
						if (!KPair.second.is<sol::table>()) continue;
						sol::table KT = KPair.second.as<sol::table>();

						// Vector tracks: parse {time=, x=, y=, z=} or nested {time=, vector={x,y,z}}
						if (bIsVectorTrack)
						{
							float Time = 0.f;
							float XYZ[3] = {0.f, 0.f, 0.f};
							static const TCHAR* Ch[] = { TEXT("x"), TEXT("y"), TEXT("z") };
							static const float Def[] = { 0.f, 0.f, 0.f };
							if (!NeoLuaCurve::ReadMultiChannelKey(KT, Time,
								TArrayView<const TCHAR*>(Ch, 3), XYZ,
								TArrayView<const float>(Def, 3), TEXT("vector")))
							{
								// Positional legacy: {time, "x,y,z"}
								float FallbackTime = 0.f, FallbackVal = 0.f;
								if (NeoLuaCurve::ReadKeyTimeValue(KT, FallbackTime, FallbackVal, /*bAllowPositional=*/true))
								{
									Desc.Keys.Add(TPair<float, FString>(FallbackTime,
										FString::Printf(TEXT("%f,%f,%f"), FallbackVal, FallbackVal, FallbackVal)));
									continue;
								}
								Session.Log(FString::Printf(TEXT("[WARN] add_timeline track '%s' key #%d has no usable time, skipped"), *Desc.Name, Index));
								continue;
							}
							Desc.Keys.Add(TPair<float, FString>(Time, FString::Printf(TEXT("%f,%f,%f"), XYZ[0], XYZ[1], XYZ[2])));
						}
						// Color tracks: parse {time=, r=, g=, b=, a=} or nested {time=, color={r,g,b,a}}
						else if (bIsColorTrack)
						{
							float Time = 0.f;
							float RGBA[4] = {0.f, 0.f, 0.f, 1.f};
							static const TCHAR* Ch[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
							static const float Def[] = { 0.f, 0.f, 0.f, 1.f };
							if (!NeoLuaCurve::ReadMultiChannelKey(KT, Time,
								TArrayView<const TCHAR*>(Ch, 4), RGBA,
								TArrayView<const float>(Def, 4), TEXT("color")))
							{
								Session.Log(FString::Printf(TEXT("[WARN] add_timeline track '%s' key #%d has no usable time, skipped"), *Desc.Name, Index));
								continue;
							}
							Desc.Keys.Add(TPair<float, FString>(Time, FString::Printf(TEXT("%f,%f,%f,%f"), RGBA[0], RGBA[1], RGBA[2], RGBA[3])));
						}
						// Float / event tracks: named {time=, value=} or legacy positional {t, v}
						else
						{
							float Time = 0.f, Value = 0.f;
							if (!NeoLuaCurve::ReadKeyTimeValue(KT, Time, Value, /*bAllowPositional=*/true))
							{
								Session.Log(FString::Printf(TEXT("[WARN] add_timeline track '%s' key #%d has no usable time/value (accepts {time=, value=} or {time, value}), skipped"), *Desc.Name, Index));
								continue;
							}
							Desc.Keys.Add(TPair<float, FString>(Time, FString::Printf(TEXT("%f"), Value)));
						}
					}
					return Desc;
				};

				auto ParseTrackArray = [&](const char* Key, const FString& DefaultType)
				{
					sol::object Obj = Opts[Key];
					if (Obj.valid() && Obj.is<sol::table>())
					{
						for (auto& Pair : Obj.as<sol::table>())
						{
							if (!Pair.second.is<sol::table>()) continue;
							auto Desc = ParseTrackTable(Pair.second.as<sol::table>(), DefaultType);
							if (!Desc.Name.IsEmpty()) Tracks.Add(MoveTemp(Desc));
						}
					}
				};

				// Parse typed track arrays (float_tracks, vector_tracks, etc.)
				ParseTrackArray("float_tracks", TEXT("float"));
				ParseTrackArray("vector_tracks", TEXT("vector"));
				ParseTrackArray("color_tracks", TEXT("color"));
				ParseTrackArray("event_tracks", TEXT("event"));

				// Parse generic "tracks" array (type specified per-track)
				ParseTrackArray("tracks", TEXT("float"));
			}

			bool bTLExisted = (Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTLName)) != nullptr);
			FString TimelineError;
			UK2Node_Timeline* TimelineNode = NeoBlueprint::AddTimeline(
				Info.Blueprint, FTLName, Length, bAutoPlay, bLoop, Tracks, &TimelineError);
			if (!TimelineNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_timeline(\"%s\") -> %s"),
					*FTLName, TimelineError.IsEmpty() ? TEXT("failed") : *TimelineError));
				return sol::lua_nil;
			}

			// Apply extra options after creation — set on template (authoritative) then sync to node
			if (Options.has_value())
			{
				sol::table Opts = Options.value();
				UTimelineTemplate* TLTemplate = Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTLName));
				if (Opts.get_or("replicated", false))
				{
					if (TLTemplate) TLTemplate->bReplicated = true;
					TimelineNode->bReplicated = true;
				}
				if (Opts.get_or("ignore_time_dilation", false))
				{
					if (TLTemplate) TLTemplate->bIgnoreTimeDilation = true;
					TimelineNode->bIgnoreTimeDilation = true;
				}
			}

			// Register in session node map
			FString NodeGuid = TimelineNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, TimelineNode);

			Session.Log(FString::Printf(TEXT("[OK] add_timeline(\"%s\") -> %s (%.1fs, %d tracks), handle=\"%s\""),
				*FTLName, bTLExisted ? TEXT("updated") : TEXT("created"), Length, Tracks.Num(), *NodeGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = TimelineName;
			return Result;
		});

		// bp:add_timeline_track(timeline_name, track) — append a single track to an existing timeline
		// track: {name="Alpha", type="float"|"vector"|"color"|"event",
		//   keys={{time=0, value=0}, {time=1, value=1}}}
		BP.set_function("add_timeline_track", [&Session, FPath](sol::table /*self*/,
			const std::string& TimelineName, sol::table TrackTable, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FTLName = NeoLuaStr::ToFString(TimelineName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_timeline_track -> blueprint not found"));
				return sol::lua_nil;
			}

			if (!Info.Blueprint->FindTimelineTemplateByVariableName(FName(*FTLName)))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_timeline_track -> timeline '%s' not found; call add_timeline first"), *FTLName));
				return sol::lua_nil;
			}

			NeoBlueprint::FTimelineTrackDesc Desc;
			Desc.Name = NeoLuaStr::ToFString(TrackTable.get_or<std::string>("name", ""));
			Desc.Type = NeoLuaStr::ToFString(TrackTable.get_or<std::string>("type", "float"));
			if (Desc.Name.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] add_timeline_track -> track 'name' required"));
				return sol::lua_nil;
			}

			const bool bIsVectorTrack = Desc.Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase);
			const bool bIsColorTrack = Desc.Type.Equals(TEXT("color"), ESearchCase::IgnoreCase) ||
				Desc.Type.Equals(TEXT("linearcolor"), ESearchCase::IgnoreCase);
			const bool bIsEventTrack = Desc.Type.Equals(TEXT("event"), ESearchCase::IgnoreCase);

			sol::object KeysObj = TrackTable["keys"];
			if (KeysObj.valid() && KeysObj.is<sol::table>())
			{
				int32 Index = 0;
				for (auto& KPair : KeysObj.as<sol::table>())
				{
					++Index;
					if (bIsEventTrack && KPair.second.is<double>())
					{
						Desc.Keys.Add(TPair<float, FString>(static_cast<float>(KPair.second.as<double>()), TEXT("0")));
						continue;
					}
					if (!KPair.second.is<sol::table>()) continue;
					sol::table KT = KPair.second.as<sol::table>();

					if (bIsVectorTrack)
					{
						float Time = 0.f;
						float XYZ[3] = {0.f, 0.f, 0.f};
						static const TCHAR* Ch[] = { TEXT("x"), TEXT("y"), TEXT("z") };
						static const float Def[] = { 0.f, 0.f, 0.f };
						if (!NeoLuaCurve::ReadMultiChannelKey(KT, Time,
							TArrayView<const TCHAR*>(Ch, 3), XYZ,
							TArrayView<const float>(Def, 3), TEXT("vector")))
						{
							Session.Log(FString::Printf(TEXT("[WARN] add_timeline_track '%s' key #%d has no usable time, skipped"), *Desc.Name, Index));
							continue;
						}
						Desc.Keys.Add(TPair<float, FString>(Time, FString::Printf(TEXT("%f,%f,%f"), XYZ[0], XYZ[1], XYZ[2])));
					}
					else if (bIsColorTrack)
					{
						float Time = 0.f;
						float RGBA[4] = {0.f, 0.f, 0.f, 1.f};
						static const TCHAR* Ch[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
						static const float Def[] = { 0.f, 0.f, 0.f, 1.f };
						if (!NeoLuaCurve::ReadMultiChannelKey(KT, Time,
							TArrayView<const TCHAR*>(Ch, 4), RGBA,
							TArrayView<const float>(Def, 4), TEXT("color")))
						{
							Session.Log(FString::Printf(TEXT("[WARN] add_timeline_track '%s' key #%d has no usable time, skipped"), *Desc.Name, Index));
							continue;
						}
						Desc.Keys.Add(TPair<float, FString>(Time, FString::Printf(TEXT("%f,%f,%f,%f"), RGBA[0], RGBA[1], RGBA[2], RGBA[3])));
					}
					else
					{
						float Time = 0.f, Value = 0.f;
						if (!NeoLuaCurve::ReadKeyTimeValue(KT, Time, Value, /*bAllowPositional=*/true))
						{
							Session.Log(FString::Printf(TEXT("[WARN] add_timeline_track '%s' key #%d has no usable time/value, skipped"), *Desc.Name, Index));
							continue;
						}
						Desc.Keys.Add(TPair<float, FString>(Time, FString::Printf(TEXT("%f"), Value)));
					}
				}
			}

			FString TimelineError;
			if (!NeoBlueprint::AddTimelineTrack(Info.Blueprint, FTLName, Desc, &TimelineError))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_timeline_track(\"%s\", \"%s\") -> %s"),
					*FTLName, *Desc.Name, TimelineError.IsEmpty() ? TEXT("track add failed") : *TimelineError));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_timeline_track(\"%s\", \"%s\", type=\"%s\", %d keys)"),
				*FTLName, *Desc.Name, *Desc.Type, Desc.Keys.Num()));

			sol::table Result = lua.create_table();
			Result["timeline"] = TimelineName;
			Result["track"] = TCHAR_TO_UTF8(*Desc.Name);
			Result["keys"] = Desc.Keys.Num();
			return Result;
		});

		// ================================================================
		// AnimBP: bp:add_state_machine(name), bp:add_state(sm, name), bp:add_transition(sm, from, to)
		// ================================================================

		// bp:add_state_machine(name) -> {handle, name, graph}
		BP.set_function("add_state_machine", [&Session, FPath, PathStr, RefreshSelf](sol::table BP_Table,
			const std::string& SMName, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FSMName = NeoLuaStr::ToFString(SMName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_state_machine -> blueprint not found"));
				return sol::lua_nil;
			}

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP)
			{
				Session.Log(TEXT("[FAIL] add_state_machine -> not an Animation Blueprint"));
				return sol::lua_nil;
			}

			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			if (!AnimGraph)
			{
				Session.Log(TEXT("[FAIL] add_state_machine -> AnimGraph not found. Open the AnimBP in the editor first."));
				return sol::lua_nil;
			}

			// Check for duplicate
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				if (UAnimGraphNode_StateMachine* Existing = Cast<UAnimGraphNode_StateMachine>(Node))
				{
					if (Existing->EditorStateMachineGraph &&
						Existing->EditorStateMachineGraph->GetName().Equals(FSMName, ESearchCase::IgnoreCase))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_state_machine(\"%s\") -> already exists"), *FSMName));
						return sol::lua_nil;
					}
				}
			}

			AnimGraph->Modify();

			UAnimGraphNode_StateMachine* NewSMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
			NewSMNode->CreateNewGuid();
			AnimGraph->AddNode(NewSMNode, false, false);
			NewSMNode->SetFlags(RF_Transactional);
			NewSMNode->PostPlacedNewNode();
			NewSMNode->AllocateDefaultPins();

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(NewSMNode->EditorStateMachineGraph);
			if (!SMGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state_machine(\"%s\") -> graph creation failed"), *FSMName));
				return sol::lua_nil;
			}

			// Rename to requested name
			TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewSMNode);
			FBlueprintEditorUtils::RenameGraphWithSuggestion(SMGraph, NameValidator, FSMName);

			// Ensure subgraph registration
			if (AnimGraph->SubGraphs.Find(SMGraph) == INDEX_NONE)
			{
				AnimGraph->Modify();
				AnimGraph->SubGraphs.Add(SMGraph);
			}

			NewSMNode->NodePosX = 200;
			NewSMNode->NodePosY = 0;

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			RefreshSelf(BP_Table, S);

			FString NodeGuid = NewSMNode->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, NewSMNode);
			Session.RegisterGraphNodes(SMGraph);

			// Also register in bp.graphs
			std::string GraphKey = TCHAR_TO_UTF8(*SMGraph->GetName());
			sol::table GraphsTable = BP_Table["graphs"];
			if (GraphsTable.valid())
			{
				sol::table GraphObj = BuildGraphObject(lua, Session, SMGraph, PathStr, GraphKey);
				GraphsTable[GraphKey] = GraphObj;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_state_machine(\"%s\") -> created, handle=\"%s\""),
				*FSMName, *NodeGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = SMName;
			Result["graph"] = GraphKey;
			return Result;
		});

		// bp:add_state(state_machine_name, state_name) -> {handle, name, graph}
		BP.set_function("add_state", [&Session, FPath, PathStr, RefreshSelf](sol::table BP_Table,
			const std::string& SMName, const std::string& StateName, sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FSMName = NeoLuaStr::ToFString(SMName);
			FString FStateName = NeoLuaStr::ToFString(StateName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_state -> blueprint not found"));
				return sol::lua_nil;
			}

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP)
			{
				Session.Log(TEXT("[FAIL] add_state -> not an Animation Blueprint"));
				return sol::lua_nil;
			}

			// Find state machine node
			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			UAnimGraphNode_StateMachine* SMNode = nullptr;
			if (AnimGraph)
			{
				for (UEdGraphNode* Node : AnimGraph->Nodes)
				{
					if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
					{
						if (SM->EditorStateMachineGraph &&
							SM->EditorStateMachineGraph->GetName().Equals(FSMName, ESearchCase::IgnoreCase))
						{
							SMNode = SM;
							break;
						}
					}
				}
			}

			if (!SMNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> state machine \"%s\" not found"),
					*FStateName, *FSMName));
				return sol::lua_nil;
			}

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> state machine graph not found"), *FStateName));
				return sol::lua_nil;
			}

			// Check for duplicate state
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateNode* ExistingState = Cast<UAnimStateNode>(Node))
				{
					FString ExistingName = ExistingState->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
					if (ExistingName.Equals(FStateName, ESearchCase::IgnoreCase))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> already exists in \"%s\""),
							*FStateName, *FSMName));
						return sol::lua_nil;
					}
				}
			}

			SMGraph->Modify();

			UAnimStateNode* NewState = NewObject<UAnimStateNode>(SMGraph);
			NewState->CreateNewGuid();
			SMGraph->AddNode(NewState, false, false);
			NewState->SetFlags(RF_Transactional);
			NewState->PostPlacedNewNode();
			NewState->AllocateDefaultPins();

			if (!NewState->BoundGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_state(\"%s\") -> bound graph not created"), *FStateName));
				return sol::lua_nil;
			}

			// Rename
			TSharedPtr<INameValidatorInterface> NameValidator = FNameValidatorFactory::MakeValidator(NewState);
			FBlueprintEditorUtils::RenameGraphWithSuggestion(NewState->BoundGraph, NameValidator, FStateName);

			// Position below existing states
			int32 MaxY = 0;
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateNode* ExistingState = Cast<UAnimStateNode>(Node))
				{
					MaxY = FMath::Max(MaxY, static_cast<int32>(ExistingState->NodePosY));
				}
			}
			NewState->NodePosX = 300;
			NewState->NodePosY = MaxY + 150;

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			RefreshSelf(BP_Table, S);

			FString NodeGuid = NewState->NodeGuid.ToString();
			Session.Nodes.Add(NodeGuid, NewState);
			Session.RegisterGraphNodes(NewState->BoundGraph);

			// Build selector name and register in bp.graphs
			FString SelectorName = FString::Printf(TEXT("%s/%s"), *SMGraph->GetName(), *FStateName);
			std::string GraphKey = TCHAR_TO_UTF8(*SelectorName);
			sol::table GraphsTable = BP_Table["graphs"];
			if (GraphsTable.valid())
			{
				sol::table GraphObj = BuildGraphObject(lua, Session, NewState->BoundGraph, PathStr, GraphKey);
				GraphsTable[GraphKey] = GraphObj;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_state(\"%s\", \"%s\") -> created, handle=\"%s\", graph=\"%s\""),
				*FSMName, *FStateName, *NodeGuid, *SelectorName));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			Result["name"] = StateName;
			Result["graph"] = GraphKey;
			return Result;
		});

		// bp:add_transition(state_machine_name, from_state, to_state) -> {handle, graph, result_handle}
		BP.set_function("add_transition", [&Session, FPath, PathStr, RefreshSelf](sol::table BP_Table,
			const std::string& SMName, const std::string& FromState, const std::string& ToState,
			sol::this_state S) -> sol::object
		{
			sol::state_view lua(S);
			FString FSMName = NeoLuaStr::ToFString(SMName);
			FString FFromState = NeoLuaStr::ToFString(FromState);
			FString FToState = NeoLuaStr::ToFString(ToState);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_transition -> blueprint not found"));
				return sol::lua_nil;
			}

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP)
			{
				Session.Log(TEXT("[FAIL] add_transition -> not an Animation Blueprint"));
				return sol::lua_nil;
			}

			// Find state machine
			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			UAnimGraphNode_StateMachine* SMNode = nullptr;
			if (AnimGraph)
			{
				for (UEdGraphNode* Node : AnimGraph->Nodes)
				{
					if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
					{
						if (SM->EditorStateMachineGraph &&
							SM->EditorStateMachineGraph->GetName().Equals(FSMName, ESearchCase::IgnoreCase))
						{
							SMNode = SM;
							break;
						}
					}
				}
			}

			if (!SMNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> state machine \"%s\" not found"), *FSMName));
				return sol::lua_nil;
			}

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph)
			{
				Session.Log(TEXT("[FAIL] add_transition -> state machine graph not found"));
				return sol::lua_nil;
			}

			// Find source and destination nodes
			UEdGraphNode* FromNode = nullptr;
			UAnimStateNodeBase* ToStateNode = nullptr;
			bool bFromEntry = false;

			if (FFromState.Equals(TEXT("[Entry]"), ESearchCase::IgnoreCase) ||
				FFromState.Equals(TEXT("Entry"), ESearchCase::IgnoreCase))
			{
				for (UEdGraphNode* Node : SMGraph->Nodes)
				{
					if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
					{
						FromNode = EntryNode;
						bFromEntry = true;
						break;
					}
				}
			}
			else
			{
				for (UEdGraphNode* Node : SMGraph->Nodes)
				{
					if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
					{
						FString Name = StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						if (Name.Equals(FFromState, ESearchCase::IgnoreCase))
						{
							FromNode = StateNode;
							break;
						}
					}
				}
			}

			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
				{
					FString Name = StateNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
					if (Name.Equals(FToState, ESearchCase::IgnoreCase))
					{
						ToStateNode = StateNode;
						break;
					}
				}
			}

			if (!FromNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> source \"%s\" not found"), *FFromState));
				return sol::lua_nil;
			}
			if (!ToStateNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> target \"%s\" not found"), *FToState));
				return sol::lua_nil;
			}

			if (bFromEntry)
			{
				UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(FromNode);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				UEdGraphPin* EntryOutput = EntryNode ? EntryNode->GetOutputPin() : nullptr;
#else
				// Pre-5.7: UAnimStateEntryNode has no GetOutputPin(). The entry node's
				// sole output pin is the first non-hidden output pin on the node.
				UEdGraphPin* EntryOutput = nullptr;
				if (EntryNode)
				{
					for (UEdGraphPin* P : EntryNode->Pins)
					{
						if (P && P->Direction == EGPD_Output && !P->bHidden)
						{
							EntryOutput = P;
							break;
						}
					}
				}
#endif
				UEdGraphPin* ToInput = ToStateNode->GetInputPin();

				if (!EntryOutput || !ToInput)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_transition(\"%s\" -> \"%s\") -> entry/state pins not found"),
						*FFromState, *FToState));
					return sol::lua_nil;
				}

				SMGraph->Modify();
				const UEdGraphSchema* Schema = SMGraph->GetSchema();
				if (!Schema || !Schema->TryCreateConnection(EntryOutput, ToInput))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_transition(\"%s\" -> \"%s\") -> connection rejected"),
						*FFromState, *FToState));
					return sol::lua_nil;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
				RefreshSelf(BP_Table, S);

				FString EntryGuid = EntryNode->NodeGuid.ToString();
				Session.Nodes.Add(EntryGuid, EntryNode);

				Session.Log(FString::Printf(TEXT("[OK] add_transition(\"%s\", Entry -> \"%s\") -> initial state set"),
					*FSMName, *FToState));

				sol::table Result = lua.create_table();
				Result["handle"] = TCHAR_TO_UTF8(*EntryGuid);
				Result["graph"] = "";
				Result["result_handle"] = "none";
				Result["kind"] = "entry";
				return Result;
			}

			// Create transition node
			UAnimStateTransitionNode* TransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			TransNode->CreateNewGuid();
			SMGraph->AddNode(TransNode, false, false);
			TransNode->SetFlags(RF_Transactional);
			TransNode->PostPlacedNewNode();
			TransNode->AllocateDefaultPins();

			// Position between source and target
			TransNode->NodePosX = (FromNode->NodePosX + ToStateNode->NodePosX) / 2;
			TransNode->NodePosY = (FromNode->NodePosY + ToStateNode->NodePosY) / 2;

			UAnimationTransitionGraph* TransGraph = Cast<UAnimationTransitionGraph>(TransNode->BoundGraph);
			if (!TransGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition(\"%s\" -> \"%s\") -> transition graph not created"),
					*FFromState, *FToState));
				return sol::lua_nil;
			}

			if (TransNode->Pins.Num() < 2)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition(\"%s\" -> \"%s\") -> pins not created"),
					*FFromState, *FToState));
				return sol::lua_nil;
			}

			// Wire the transition
			SMGraph->Modify();

			UAnimStateNodeBase* FromStateNode = Cast<UAnimStateNodeBase>(FromNode);
			if (!FromStateNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_transition -> source \"%s\" is not a state node"), *FFromState));
				return sol::lua_nil;
			}
			TransNode->CreateConnections(FromStateNode, ToStateNode);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			RefreshSelf(BP_Table, S);

			FString TransGuid = TransNode->NodeGuid.ToString();
			Session.Nodes.Add(TransGuid, TransNode);
			Session.RegisterGraphNodes(TransGraph);

			// Get result node handle
			UAnimGraphNode_TransitionResult* ResultNode = TransGraph->MyResultNode;
			FString ResultGuid = ResultNode ? ResultNode->NodeGuid.ToString() : TEXT("none");
			if (ResultNode)
			{
				Session.Nodes.Add(ResultGuid, ResultNode);
			}

			// Build selector name and register in bp.graphs
			FString SelectorName = FString::Printf(TEXT("%s/%s->%s"), *SMGraph->GetName(), *FFromState, *FToState);
			std::string GraphKey = TCHAR_TO_UTF8(*SelectorName);
			sol::table GraphsTable = BP_Table["graphs"];
			if (GraphsTable.valid())
			{
				sol::table GraphObj = BuildGraphObject(lua, Session, TransGraph, PathStr, GraphKey);
				GraphsTable[GraphKey] = GraphObj;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_transition(\"%s\", \"%s\" -> \"%s\") -> handle=\"%s\", graph=\"%s\", result=\"%s\""),
				*FSMName, *FFromState, *FToState, *TransGuid, *SelectorName, *ResultGuid));

			sol::table Result = lua.create_table();
			Result["handle"] = TCHAR_TO_UTF8(*TransGuid);
			Result["graph"] = GraphKey;
			Result["result_handle"] = TCHAR_TO_UTF8(*ResultGuid);
			return Result;
		});

		// bp:rename_variable(old_name, new_name)
		BP.set_function("rename_variable", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) return sol::lua_nil;
			FString Old = NeoLuaStr::ToFString(OldName);
			FString New = NeoLuaStr::ToFString(NewName);
			FBlueprintEditorUtils::RenameMemberVariable(Blueprint, FName(Old), FName(New));
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] rename_variable(\"%s\" -> \"%s\")"), *Old, *New));
			return sol::make_object(Lua, true);
		});

		// bp:remove_variable(name)
		BP.set_function("remove_variable", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& VarName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) return sol::lua_nil;
			FString Name = NeoLuaStr::ToFString(VarName);
			if (!NeoBlueprint::RemoveVariable(Blueprint, Name))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_variable(\"%s\") -> variable not found"), *Name));
				return sol::lua_nil;
			}
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] remove_variable(\"%s\")"), *Name));
			return sol::make_object(Lua, true);
		});

		// bp:reparent(new_parent_class_path)
		BP.set_function("reparent", [&Session, FPath](sol::table /*self*/,
			const std::string& ParentPath, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
				if (!Blueprint) return sol::lua_nil;
				FString FParent = NeoLuaStr::ToFString(ParentPath);
				UClass* NewParent = ResolveBlueprintParentClassInput(FParent);
				if (!NewParent) { Session.Log(FString::Printf(TEXT("[FAIL] reparent -> class '%s' not found"), *FParent)); return sol::lua_nil; }
				if (NewParent == Blueprint->ParentClass)
				{
					Session.Log(FString::Printf(TEXT("[OK] reparent(\"%s\") -> already current parent"), *NewParent->GetName()));
					return sol::make_object(Lua, true);
				}

			const FScopedTransaction Transaction(NSLOCTEXT("AIK", "LuaReparent", "Reparent Blueprint"));

			// Mark blueprint and SCS for undo (mirrors engine ReparentBlueprint_NewParentChosen)
			Blueprint->Modify();
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				SCS->Modify();
				for (USCS_Node* Node : SCS->GetAllNodes())
				{
					if (Node) Node->Modify();
				}
			}

				Blueprint->ParentClass = NewParent;
				if (Blueprint->SimpleConstructionScript != nullptr)
				{
					Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
				}

				// Purge null graphs, upgrade cosmetically stale nodes
				FBlueprintEditorUtils::PurgeNullGraphs(Blueprint);
				FKismetEditorUtilities::UpgradeCosmeticallyStaleBlueprint(Blueprint);

			// Reconstruct all nodes to pick up new parent context
			FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

			// Conform sparse class data if applicable
			if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
			{
				if (UScriptStruct* SparseStruct = NewParent->GetSparseClassDataStruct())
				{
					BPGC->PrepareToConformSparseClassData(SparseStruct);
				}
				}

				EBlueprintCompileOptions CompileOptions =
					EBlueprintCompileOptions::SkipSave |
					EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing |
					EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions);
				Session.Log(FString::Printf(TEXT("[OK] reparent(\"%s\")"), *NewParent->GetName()));
				return sol::make_object(Lua, true);
			});

		// bp:add_comment(text, x?, y?, w?, h?) OR bp:add_comment({text=, x=, y=, ...})
		BP.set_function("add_comment", [&Session, FPath](sol::table /*self*/,
			sol::object Arg1, sol::optional<double> Arg2, sol::optional<double> Arg3,
			sol::optional<double> Arg4, sol::optional<double> Arg5, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) return sol::lua_nil;

			std::string GraphName = "EventGraph";
			std::string Text = "Comment";
			double PosX = 0, PosY = 0, Width = 400, Height = 200;
			std::string Color;

			if (Arg1.is<sol::table>())
			{
				// Table format: {text=, graph=, x=, y=, width=, height=, color=}
				sol::table Params = Arg1.as<sol::table>();
				GraphName = Params.get_or<std::string>("graph", "EventGraph");
				Text = Params.get_or<std::string>("text", "Comment");
				PosX = Params.get<sol::optional<double>>("x").value_or(0.0);
				PosY = Params.get<sol::optional<double>>("y").value_or(0.0);
				Width = Params.get<sol::optional<double>>("width").value_or(400.0);
				Height = Params.get<sol::optional<double>>("height").value_or(200.0);
				Color = Params.get_or<std::string>("color", "");
			}
			else if (Arg1.is<std::string>())
			{
				// Positional format: add_comment(text, x?, y?, w?, h?)
				Text = Arg1.as<std::string>();
				PosX = Arg2.value_or(0.0);
				PosY = Arg3.value_or(0.0);
				Width = Arg4.value_or(400.0);
				Height = Arg5.value_or(200.0);
			}
			else
			{
				Session.Log(TEXT("[FAIL] add_comment -> first argument must be a string (text) or table ({text=, x=, ...})"));
				return sol::lua_nil;
			}

			// Find graph, including collapsed/composite Blueprint subgraphs.
			FString FGraphName = NeoLuaStr::ToFString(GraphName);
			UEdGraph* Graph = LuaGraphResolver::FindGraph(Blueprint, FGraphName);
			if (!Graph) { Session.Log(TEXT("[FAIL] add_comment -> graph not found")); return sol::lua_nil; }

			Graph->SetFlags(RF_Transactional);
			Graph->Modify();
			UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
			Comment->CreateNewGuid();
			Comment->SetFlags(RF_Transactional);
			Comment->PostPlacedNewNode();
			Comment->NodeComment = NeoLuaStr::ToFString(Text);
			Comment->NodePosX = (int32)PosX;
			Comment->NodePosY = (int32)PosY;
			Comment->NodeWidth = (int32)Width;
			Comment->NodeHeight = (int32)Height;
			if (!Color.empty())
				Comment->CommentColor = FColor::FromHex(NeoLuaStr::ToFString(Color)).ReinterpretAsLinear();
			Graph->AddNode(Comment, false, false);
			Blueprint->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add_comment(\"%s\")"), UTF8_TO_TCHAR(Text.c_str())));
			return sol::make_object(Lua, TCHAR_TO_UTF8(*Comment->NodeGuid.ToString()));
		});

		// bp:break_connection(from_handle, from_pin, to_handle, to_pin)
		// Delegates to the global disconnect_from, which uses Schema->BreakSinglePinLink and
		// handles pin-name resolution (exec aliases, display name, case-insensitive) via NeoLuaPin::Find.
		BP.set_function("break_connection", [](sol::table /*self*/,
			const std::string& FromHandle, const std::string& FromPinName,
			const std::string& ToHandle, const std::string& ToPinName,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::protected_function Fn = Lua["disconnect_from"];
			auto Result = Fn(FromHandle, FromPinName, ToHandle, ToPinName);
			if (Result.valid()) return Result;
			return sol::lua_nil;
		});

		// bp:align_nodes(handles_array, axis, mode)
		BP.set_function("align_nodes", [&Session, FPath](sol::table /*self*/,
			sol::table Handles, const std::string& Axis, const std::string& Mode,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) return sol::lua_nil;

			FString FAxis = NeoLuaStr::ToFString(Axis);
			FString FMode = NeoLuaStr::ToFString(Mode);
			bool bAxisX = FAxis.Equals(TEXT("x"), ESearchCase::IgnoreCase);

			// Collect nodes
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			TArray<UEdGraphNode*> Nodes;
			for (auto& [_, val] : Handles)
			{
				if (!val.is<std::string>()) continue;
				FGuid Guid;
				FGuid::Parse(NeoLuaStr::ToFString(val.as<std::string>()), Guid);
				for (UEdGraph* G : AllGraphs)
					for (UEdGraphNode* N : G->Nodes)
						if (N->NodeGuid == Guid) { Nodes.Add(N); break; }
			}
			if (Nodes.Num() < 2) { Session.Log(TEXT("[FAIL] align_nodes -> need at least 2 nodes")); return sol::lua_nil; }

			auto GetCoord = [bAxisX](UEdGraphNode* N) { return bAxisX ? N->NodePosX : N->NodePosY; };

			int32 Target = 0;
			if (FMode.Equals(TEXT("min"), ESearchCase::IgnoreCase))
			{
				Target = INT_MAX;
				for (auto* N : Nodes) Target = FMath::Min(Target, GetCoord(N));
			}
			else if (FMode.Equals(TEXT("max"), ESearchCase::IgnoreCase))
			{
				Target = INT_MIN;
				for (auto* N : Nodes) Target = FMath::Max(Target, GetCoord(N));
			}
			else // center
			{
				int64 Sum = 0;
				for (auto* N : Nodes) Sum += GetCoord(N);
				Target = (int32)(Sum / Nodes.Num());
			}

			for (auto* N : Nodes)
			{
				N->Modify();
				if (bAxisX) N->NodePosX = Target;
				else N->NodePosY = Target;
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] align_nodes(%d nodes, %s, %s)"), Nodes.Num(), *FAxis, *FMode));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// Widget Blueprint operations
		// ================================================================
		//
		// add_widget / configure_widget / rename_widget are provided by
		// LuaBinding_WidgetBlueprint.cpp's _enrich_widget_blueprint, which runs AFTER
		// _open_blueprint for any UWidgetBlueprint (LuaBinding_OpenAsset.cpp:541). The
		// previous base-binding versions had divergent signatures (positional args vs opts
		// table) and divergent preflight behavior, and were dead code for normal open_asset
		// users since enrichment shadowed them. They're gone.
		//
		// remove_widget is kept here: _enrich_widget_blueprint intentionally does NOT override
		// remove_widget so the engine-helper-delegating implementation below is canonical.

		// bp:remove_widget(name) — remove a widget from widget tree
		// Delegates to FWidgetBlueprintEditorUtils::DeleteWidgets (public, UE_API, WidgetBlueprintEditorUtils.h:84),
		// which handles: binding cleanup, named-slot content clearing, variable node removal, child recursion,
		// transient-package rename, OnVariableRemoved notification, and desired-focus replacement.
		// The old-signature variant taking FWidgetBlueprintEditor+FWidgetReference requires an editor session
		// and was deprecated in 5.6 — we only support the non-interactive (BP, Widgets, WarningType) form.
		BP.set_function("remove_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& WidgetName, sol::this_state S) -> sol::object
		{
			FString FWidgetName = NeoLuaStr::ToFString(WidgetName);

			UWidgetBlueprint* WidgetBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] remove_widget -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveWidget", "Remove Widget"));

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			TSet<UWidget*> ToDelete;
			ToDelete.Add(Widget);
			FWidgetBlueprintEditorUtils::DeleteWidgets(
				WidgetBP, ToDelete,
				FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
#else
			// Pre-5.6: DeleteWidgets(BP, TSet<UWidget*>, EDeleteWidgetWarningType) did not exist;
			// the only public variant required an editor session. Fall back to the manual flow.
			WidgetBP->WidgetTree->SetFlags(RF_Transactional);
			WidgetBP->WidgetTree->Modify();
			WidgetBP->Modify();
			Widget->SetFlags(RF_Transactional);
			const FName WName = Widget->GetFName();
			const FString WidgetNameStr = WName.ToString();
			for (int32 i = WidgetBP->Bindings.Num() - 1; i >= 0; --i)
			{
				if (WidgetBP->Bindings[i].ObjectName == WidgetNameStr)
					WidgetBP->Bindings.RemoveAt(i);
			}
			if (UPanelWidget* Parent = Widget->GetParent())
			{
				Parent->SetFlags(RF_Transactional);
				Parent->Modify();
			}
			Widget->Modify();
			if (Widget == WidgetBP->WidgetTree->RootWidget)
			{
				WidgetBP->WidgetTree->RootWidget = nullptr;
			}
			else
			{
				WidgetBP->WidgetTree->RemoveWidget(Widget);
				if (Widget->GetParent() == nullptr)
				{
					WidgetBP->WidgetTree->ForEachWidget([&](UWidget* W)
					{
						if (INamedSlotInterface* SlotHost = Cast<INamedSlotInterface>(W))
						{
							TArray<FName> SlotNames;
							SlotHost->GetSlotNames(SlotNames);
							for (const FName& SlotName : SlotNames)
							{
								if (SlotHost->GetContentForSlot(SlotName) == Widget)
									SlotHost->SetContentForSlot(SlotName, nullptr);
							}
						}
					});
				}
			}
			FBlueprintEditorUtils::RemoveVariableNodes(WidgetBP, WName);
			Widget->Rename(nullptr, GetTransientPackage());
			TArray<UWidget*> ChildWidgets;
			UWidgetTree::GetChildWidgets(Widget, ChildWidgets);
			for (UWidget* ChildWidget : ChildWidgets)
			{
				ChildWidget->SetFlags(RF_Transactional);
				ChildWidget->Modify();
				FBlueprintEditorUtils::RemoveVariableNodes(WidgetBP, ChildWidget->GetFName());
				ChildWidget->Rename(nullptr, GetTransientPackage());
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
#endif

			Session.Log(FString::Printf(TEXT("[OK] remove_widget(\"%s\")"), *FWidgetName));
			return sol::make_object(S, true);
		});

		// configure_widget is provided by _enrich_widget_blueprint — see comment above.

		// rename_widget is provided by _enrich_widget_blueprint — see comment above.

		// bp:wrap_widgets(wrapper_class, widget_names) — wrap widgets with a container
		BP.set_function("wrap_widgets", [&Session, FPath](sol::table /*self*/,
			const std::string& WrapperType, sol::table WidgetNames, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWrapperType = NeoLuaStr::ToFString(WrapperType);

			UWidgetBlueprint* WidgetBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] wrap_widgets -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			// Wrapper must be a UPanelWidget subclass (needs AddChild support).
			UClass* WrapperClass = FindWidgetClassByName(FWrapperType, UPanelWidget::StaticClass());
			if (!WrapperClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] wrap_widgets -> wrapper class '%s' not found or not a panel widget"), *FWrapperType));
				return sol::lua_nil;
			}

			// Collect widgets to wrap
			TArray<UWidget*> WidgetsToWrap;
			for (auto& Pair : WidgetNames)
			{
				std::string Name = Pair.second.as<std::string>();
				FString FName_Str = NeoLuaStr::ToFString(Name);
				UWidget* W = WidgetBP->WidgetTree->FindWidget(FName(*FName_Str));
				if (!W)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] wrap_widgets -> widget '%s' not found"), *FName_Str));
					return sol::lua_nil;
				}
				WidgetsToWrap.Add(W);
			}

			if (WidgetsToWrap.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] wrap_widgets -> no widgets specified"));
				return sol::lua_nil;
			}

			// All widgets must share the same parent
			UPanelWidget* CommonParent = nullptr;
			int32 MinIndex = INT_MAX;
			UWidget* MinWidget = nullptr;
			bool bIsRoot = false;

			for (UWidget* W : WidgetsToWrap)
			{
				if (W == WidgetBP->WidgetTree->RootWidget)
				{
					bIsRoot = true;
					if (WidgetsToWrap.Num() > 1)
					{
						Session.Log(TEXT("[FAIL] wrap_widgets -> cannot wrap root widget together with other widgets"));
						return sol::lua_nil;
					}
					break;
				}
				int32 Idx;
				UPanelWidget* Parent = UWidgetTree::FindWidgetParent(W, Idx);
				if (!Parent)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] wrap_widgets -> widget '%s' has no parent panel"), *W->GetName()));
					return sol::lua_nil;
				}
				if (!CommonParent)
				{
					CommonParent = Parent;
				}
				else if (CommonParent != Parent)
				{
					Session.Log(TEXT("[FAIL] wrap_widgets -> all widgets must share the same parent"));
					return sol::lua_nil;
				}
				if (Idx < MinIndex)
				{
					MinIndex = Idx;
					MinWidget = W;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaWrapWidgets", "Wrap Widgets"));
			WidgetBP->WidgetTree->SetFlags(RF_Transactional);
			WidgetBP->WidgetTree->Modify();

			// Create the wrapper widget
			UPanelWidget* Wrapper = Cast<UPanelWidget>(WidgetBP->WidgetTree->ConstructWidget<UWidget>(WrapperClass));
			if (!Wrapper)
			{
				Session.Log(TEXT("[FAIL] wrap_widgets -> failed to construct wrapper widget"));
				return sol::lua_nil;
			}
			Wrapper->CreatedFromPalette();

			if (bIsRoot)
			{
				// Wrapping the root: new wrapper becomes root, old root becomes child
				UWidget* OldRoot = WidgetBP->WidgetTree->RootWidget;
				WidgetBP->WidgetTree->RootWidget = Wrapper;
				Wrapper->AddChild(OldRoot);
			}
			else
			{
				CommonParent->SetFlags(RF_Transactional);
				CommonParent->Modify();

				// Replace the lowest-index widget with the wrapper
				CommonParent->ReplaceChildAt(MinIndex, Wrapper);

				// Add the replaced widget first to maintain order
				Wrapper->AddChild(MinWidget);

				// Add remaining widgets (AddChild auto-removes from old parent)
				for (UWidget* W : WidgetsToWrap)
				{
					if (W == MinWidget) continue;
					W->SetFlags(RF_Transactional);
					W->Modify();
					Wrapper->AddChild(W);
				}
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetBP->OnVariableAdded(Wrapper->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

			Session.Log(FString::Printf(TEXT("[OK] wrap_widgets(\"%s\") -> wrapped %d widgets, wrapper name = \"%s\""),
				*FWrapperType, WidgetsToWrap.Num(), *Wrapper->GetName()));

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Wrapper->GetName());
			Result["type"] = TCHAR_TO_UTF8(*WrapperClass->GetName());
			Result["child_count"] = WidgetsToWrap.Num();
			return Result;
		});

		// bp:replace_widget(old_name, new_class) — replace a widget with a different class
		BP.set_function("replace_widget", [&Session, FPath](sol::table /*self*/,
			const std::string& WidgetName, const std::string& NewType, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = NeoLuaStr::ToFString(WidgetName);
			FString FNewType = NeoLuaStr::ToFString(NewType);

			UWidgetBlueprint* WidgetBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WidgetBP || !WidgetBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] replace_widget -> not a Widget Blueprint or no widget tree"));
				return sol::lua_nil;
			}

			UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] replace_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			UClass* NewClass = FindWidgetClassByName(FNewType, UWidget::StaticClass());
			if (!NewClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] replace_widget -> class '%s' not found"), *FNewType));
				return sol::lua_nil;
			}

			// If widget has children and replacement is not a panel, fail
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				if (Panel->GetChildrenCount() > 0 && !NewClass->IsChildOf(UPanelWidget::StaticClass()))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] replace_widget(\"%s\") -> has children but '%s' is not a panel widget"),
						*FWidgetName, *FNewType));
					return sol::lua_nil;
				}
			}

			// FWidgetBlueprintEditorUtils::ReplaceWidgets (public, UE_API, WidgetBlueprintEditorUtils.h:72)
			// handles variable-reference rewiring, name preservation, and slot data transfer.
			// MaintainNameAndReferences keeps the old widget's name on the replacement (when class-compatible)
			// so graph nodes referencing it stay valid; otherwise falls back to a generated name.
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReplaceWidget", "Replace Widget"));
			TSet<UWidget*> ToReplace;
			ToReplace.Add(Widget);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			FWidgetBlueprintEditorUtils::ReplaceWidgets(
				WidgetBP, ToReplace, NewClass,
				FWidgetBlueprintEditorUtils::EReplaceWidgetNamingMethod::MaintainNameAndReferences);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
			Session.Log(FString::Printf(TEXT("[OK] replace_widget(\"%s\") -> \"%s\""), *FWidgetName, *FNewType));
			return sol::make_object(Lua, true);
#else
			// Pre-5.7: ReplaceWidgets signature differs and isn't a clean fallback. Skip
			// with a Lua-side fail so the caller knows to use UE 5.7+ for this op.
			(void)WidgetBP; (void)ToReplace; (void)NewClass; (void)FNewType;
			Session.Log(FString::Printf(TEXT("[FAIL] replace_widget(\"%s\") requires UE 5.7+"), *FWidgetName));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// Event binding operations
		// ================================================================

		// bp:list_events(source?) — discover delegate events on a component, widget, or all components
		BP.set_function("list_events", [&Session, FPath](sol::table /*self*/,
			sol::optional<std::string> OptSourceName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] list_events -> blueprint not found"));
				return sol::lua_nil;
			}

			// Helper: collect delegate events from a UClass into the result table
			auto CollectEvents = [&Lua](UClass* TargetClass, const FString& SourceLabel, sol::table& Result, int32& Idx)
			{
				for (TFieldIterator<FMulticastDelegateProperty> It(TargetClass); It; ++It)
				{
					FMulticastDelegateProperty* Prop = *It;
					if (!Prop || !Prop->HasAnyPropertyFlags(CPF_BlueprintAssignable)) continue;

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Prop->GetName());
					Entry["source"] = TCHAR_TO_UTF8(*SourceLabel);
					Entry["target"] = TCHAR_TO_UTF8(*SourceLabel);
					Entry["kind"] = "delegate";
					Entry["bindable"] = true;

					UFunction* SigFunc = Prop->SignatureFunction;
					if (SigFunc)
					{
						FString Sig;
						for (TFieldIterator<FProperty> PIt(SigFunc); PIt && PIt->HasAnyPropertyFlags(CPF_Parm) && !PIt->HasAnyPropertyFlags(CPF_ReturnParm); ++PIt)
						{
							if (!Sig.IsEmpty()) Sig += TEXT(", ");
							Sig += FString::Printf(TEXT("%s %s"), *PIt->GetCPPType(), *PIt->GetName());
						}
						Entry["signature"] = TCHAR_TO_UTF8(*Sig);
					}
					Result[Idx++] = Entry;
				}
			};
			auto CollectBlueprintEvents = [&Lua](UClass* TargetClass, const FString& SourceLabel, sol::table& Result, int32& Idx)
			{
				if (!TargetClass) return;
				for (TFieldIterator<UFunction> It(TargetClass); It; ++It)
				{
					UFunction* Func = *It;
					if (!Func || !Func->HasAnyFunctionFlags(FUNC_Event | FUNC_BlueprintEvent)) continue;
					if (Func->HasAnyFunctionFlags(FUNC_Delegate)) continue;
					if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent)) continue;

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Func->GetName());
					Entry["source"] = TCHAR_TO_UTF8(*SourceLabel);
					Entry["target"] = TCHAR_TO_UTF8(*SourceLabel);
					Entry["kind"] = "blueprint_event";
					Entry["bindable"] = false;

					FString Sig;
					for (TFieldIterator<FProperty> PIt(Func); PIt && PIt->HasAnyPropertyFlags(CPF_Parm) && !PIt->HasAnyPropertyFlags(CPF_ReturnParm); ++PIt)
					{
						if (!Sig.IsEmpty()) Sig += TEXT(", ");
						Sig += FString::Printf(TEXT("%s %s"), *PIt->GetCPPType(), *PIt->GetName());
					}
					if (!Sig.IsEmpty())
					{
						Entry["signature"] = TCHAR_TO_UTF8(*Sig);
					}
					Result[Idx++] = Entry;
				}
			};

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			if (OptSourceName.has_value())
			{
				FString FSource = NeoLuaStr::ToFString(*OptSourceName);
				UClass* TargetClass = nullptr;
				bool bSelfEvents = FSource.Equals(TEXT("self"), ESearchCase::IgnoreCase)
					|| FSource.Equals(TEXT("userwidget"), ESearchCase::IgnoreCase)
					|| FSource.Equals(TEXT("this"), ESearchCase::IgnoreCase);

				if (bSelfEvents)
				{
					TargetClass = Info.Blueprint->ParentClass ? Info.Blueprint->ParentClass : Info.Blueprint->GeneratedClass;
					CollectBlueprintEvents(TargetClass, TEXT("Self"), Result, Idx);
					Session.Log(FString::Printf(TEXT("[OK] list_events(\"%s\") -> %d inherited blueprint events"), *FSource, Idx - 1));
					return Result;
				}

				// Check widget
				UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Info.Blueprint);
				if (WidgetBP && WidgetBP->WidgetTree)
				{
					UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FSource));
					if (Widget) TargetClass = Widget->GetClass();
				}

				// Check component
				if (!TargetClass)
				{
					UActorComponent* Comp = NeoBlueprint::GetComponentTemplate(Info.Blueprint, FSource);
					if (Comp) TargetClass = Comp->GetClass();
				}

				if (!TargetClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list_events(\"%s\") -> not found as widget or component"), *FSource));
					return sol::lua_nil;
				}

				CollectEvents(TargetClass, FSource, Result, Idx);
				Session.Log(FString::Printf(TEXT("[OK] list_events(\"%s\") -> %d events"), *FSource, Idx - 1));
			}
			else
			{
				// No source specified — list events from all widget-tree widgets and components.
				UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Info.Blueprint);
				if (WidgetBP && WidgetBP->WidgetTree)
				{
					CollectBlueprintEvents(WidgetBP->ParentClass ? WidgetBP->ParentClass : WidgetBP->GeneratedClass, TEXT("Self"), Result, Idx);
					TArray<UWidget*> AllWidgets;
					WidgetBP->WidgetTree->GetAllWidgets(AllWidgets);
					for (UWidget* Widget : AllWidgets)
					{
						if (!Widget) continue;
						CollectEvents(Widget->GetClass(), Widget->GetName(), Result, Idx);
					}
				}

				if (Info.Blueprint->SimpleConstructionScript)
				{
					for (USCS_Node* SCSNode : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
					{
						if (!SCSNode || !SCSNode->ComponentTemplate) continue;
						FString CompName = SCSNode->GetVariableName().ToString();
						CollectEvents(SCSNode->ComponentTemplate->GetClass(), CompName, Result, Idx);
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list_events() -> %d events from widgets/components"), Idx - 1));
			}

			return Result;
		});

		// bp:bind_event(source, event) — bind a component/widget delegate event
		BP.set_function("bind_event", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& SourceName, const std::string& EventName,
			sol::optional<std::string> /*HandlerName*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSource = NeoLuaStr::ToFString(SourceName);
			FString FEvent = NeoLuaStr::ToFString(EventName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] bind_event -> blueprint not found"));
				return sol::lua_nil;
			}

			// Widget Blueprint path
			UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Info.Blueprint);
			if (WidgetBP && WidgetBP->WidgetTree)
			{
				UWidget* Widget = WidgetBP->WidgetTree->FindWidget(FName(*FSource));
				if (Widget)
				{
					// Widget must be a variable for the skeleton class to have an FObjectProperty for it.
					// Auto-promote and recompile if needed.
					if (!Widget->bIsVariable)
					{
						Widget->bIsVariable = true;
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						FKismetEditorUtilities::CompileBlueprint(Info.Blueprint);
						Session.Log(FString::Printf(TEXT("[INFO] bind_event -> auto-promoted widget '%s' to variable"), *FSource));
					}

					UClass* SkeletonClass = Info.Blueprint->SkeletonGeneratedClass;
					if (!SkeletonClass) SkeletonClass = Info.Blueprint->GeneratedClass;
					FObjectProperty* WidgetProp = SkeletonClass ?
						FindFProperty<FObjectProperty>(SkeletonClass, FName(*FSource)) : nullptr;

					// If property still not found, try recompiling (widget may have been added this session)
					if (!WidgetProp)
					{
						FKismetEditorUtilities::CompileBlueprint(Info.Blueprint);
						SkeletonClass = Info.Blueprint->SkeletonGeneratedClass;
						if (!SkeletonClass) SkeletonClass = Info.Blueprint->GeneratedClass;
						WidgetProp = SkeletonClass ?
							FindFProperty<FObjectProperty>(SkeletonClass, FName(*FSource)) : nullptr;
					}

					if (WidgetProp)
					{
						FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), FName(*FEvent));
						if (!DelegateProperty)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] bind_event(\"%s\", \"%s\") -> event not found on %s. Call list_events(\"%s\") to discover valid events."),
								*FSource, *FEvent, *Widget->GetClass()->GetName(), *FSource));
							return sol::lua_nil;
						}

						const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaBindEvent", "Bind Event"));
						FKismetEditorUtilities::CreateNewBoundEventForClass(
							Widget->GetClass(), FName(*FEvent), Info.Blueprint, WidgetProp);
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						RefreshSelf(Self, S);
						Session.Log(FString::Printf(TEXT("[OK] bind_event(\"%s\", \"%s\") -> bound (widget)"), *FSource, *FEvent));
						return sol::make_object(Lua, true);
					}

					Session.Log(FString::Printf(TEXT("[FAIL] bind_event -> could not find property for widget '%s' in skeleton class after compile"), *FSource));
					return sol::lua_nil;
				}
			}

			// Component path
			if (Info.Blueprint->SimpleConstructionScript)
			{
				USCS_Node* SCSNode = nullptr;
				for (USCS_Node* Node : Info.Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->GetVariableName().ToString().Equals(FSource, ESearchCase::IgnoreCase))
					{
						SCSNode = Node;
						break;
					}
				}

				if (SCSNode && SCSNode->ComponentClass)
				{
					UClass* SkeletonClass = Info.Blueprint->SkeletonGeneratedClass;
					if (!SkeletonClass) SkeletonClass = Info.Blueprint->GeneratedClass;
					FObjectProperty* CompProp = SkeletonClass ?
						FindFProperty<FObjectProperty>(SkeletonClass, SCSNode->GetVariableName()) : nullptr;

					if (CompProp)
					{
						FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(SCSNode->ComponentClass, FName(*FEvent));
						if (!DelegateProperty)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] bind_event(\"%s\", \"%s\") -> event not found on %s. Call list_events(\"%s\") to discover valid events."),
								*FSource, *FEvent, *SCSNode->ComponentClass->GetName(), *FSource));
							return sol::lua_nil;
						}

						const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaBindCompEvent", "Bind Component Event"));
						FKismetEditorUtilities::CreateNewBoundEventForClass(
							SCSNode->ComponentClass, FName(*FEvent), Info.Blueprint, CompProp);
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
						RefreshSelf(Self, S);
						Session.Log(FString::Printf(TEXT("[OK] bind_event(\"%s\", \"%s\") -> bound (component)"), *FSource, *FEvent));
						return sol::make_object(Lua, true);
					}
				}
			}

			Session.Log(FString::Printf(TEXT("[FAIL] bind_event(\"%s\", \"%s\") -> source not found"), *FSource, *FEvent));
			return sol::lua_nil;
		});

		// bp:unbind_event(source, event) — remove an event binding
		BP.set_function("unbind_event", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& SourceName, const std::string& EventName,
			sol::this_state S) -> sol::object
		{
			FString FSource = NeoLuaStr::ToFString(SourceName);
			FString FEvent = NeoLuaStr::ToFString(EventName);

			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint)
			{
				Session.Log(TEXT("[FAIL] unbind_event -> blueprint not found"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaUnbindEvent", "Unbind Event"));
			bool bRemoved = false;

			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (!Graph) continue;
				for (int32 i = Graph->Nodes.Num() - 1; i >= 0; --i)
				{
					UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Graph->Nodes[i]);
					if (BoundEvent &&
						BoundEvent->ComponentPropertyName.ToString().Equals(FSource, ESearchCase::IgnoreCase) &&
						BoundEvent->DelegatePropertyName.ToString().Equals(FEvent, ESearchCase::IgnoreCase))
					{
						FBlueprintEditorUtils::RemoveNode(Blueprint, BoundEvent, true);
						bRemoved = true;
					}
				}
			}

			if (bRemoved)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				RefreshSelf(Self, S);
				Session.Log(FString::Printf(TEXT("[OK] unbind_event(\"%s\", \"%s\")"), *FSource, *FEvent));
				return sol::make_object(S, true);
			}

			// Also check widget delegate bindings
			UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint);
			if (WidgetBP)
			{
				for (int32 i = WidgetBP->Bindings.Num() - 1; i >= 0; --i)
				{
					if (WidgetBP->Bindings[i].ObjectName.Equals(FSource, ESearchCase::IgnoreCase) &&
						WidgetBP->Bindings[i].PropertyName.ToString().Equals(FEvent, ESearchCase::IgnoreCase))
					{
						WidgetBP->Bindings.RemoveAt(i);
						bRemoved = true;
					}
				}
				if (bRemoved)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
					RefreshSelf(Self, S);
					Session.Log(FString::Printf(TEXT("[OK] unbind_event(\"%s\", \"%s\") (widget binding)"), *FSource, *FEvent));
					return sol::make_object(S, true);
				}
			}

			Session.Log(FString::Printf(TEXT("[FAIL] unbind_event(\"%s\", \"%s\") -> binding not found"), *FSource, *FEvent));
			return sol::lua_nil;
		});

		// ================================================================
		// Event Dispatcher (multicast delegate variable) operations
		// ================================================================

		// bp:add_event_dispatcher(name, params?)
		BP.set_function("add_event_dispatcher", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& DispName, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FDispName = NeoLuaStr::ToFString(DispName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] add_event_dispatcher -> blueprint not found"));
				return sol::lua_nil;
			}

			// Check existing
			for (const FBPVariableDescription& Var : Info.Blueprint->NewVariables)
			{
				if (Var.VarName == FName(*FDispName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_event_dispatcher(\"%s\") -> already exists"), *FDispName));
					return sol::lua_nil;
				}
			}

			TArray<FParamDesc> ParamDescs;
			if (Params.has_value())
			{
				sol::table ParamsTable = Params.value();
				if (sol::optional<sol::table> NestedParams = ParamsTable.get<sol::optional<sol::table>>("params"))
				{
					ParamsTable = NestedParams.value();
				}
				else if (sol::optional<sol::table> NestedInputs = ParamsTable.get<sol::optional<sol::table>>("inputs"))
				{
					ParamsTable = NestedInputs.value();
				}

				for (auto& Pair : ParamsTable)
				{
					if (Pair.second.is<sol::table>())
					{
						sol::table P = Pair.second.as<sol::table>();
						FParamDesc Desc;
						Desc.Name = NeoLuaStr::ToFString(P.get_or<std::string>("name", ""));
						Desc.Type = NeoLuaStr::ToFString(P.get_or<std::string>("type", ""));
						if (!Desc.Name.IsEmpty() && !Desc.Type.IsEmpty())
							ParamDescs.Add(MoveTemp(Desc));
					}
				}
			}

			UEdGraph* SigGraph = NeoBlueprint::AddEventDispatcher(Info.Blueprint, FDispName, ParamDescs);
			if (!SigGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_event_dispatcher(\"%s\") -> failed"), *FDispName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] add_event_dispatcher(\"%s\") -> %d params"), *FDispName, ParamDescs.Num()));
			RefreshSelf(Self, S);
			return sol::make_object(S, true);
		});

		// bp:remove_event_dispatcher(name)
		BP.set_function("remove_event_dispatcher", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& DispName, sol::this_state S) -> sol::object
		{
			FString FDispName = NeoLuaStr::ToFString(DispName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) { Session.Log(TEXT("[FAIL] remove_event_dispatcher -> blueprint not found")); return sol::lua_nil; }

			for (int32 i = Blueprint->NewVariables.Num() - 1; i >= 0; --i)
			{
				const FBPVariableDescription& Var = Blueprint->NewVariables[i];
				if (Var.VarName == FName(*FDispName) && Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Var.VarName);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					RefreshSelf(Self, S);
					Session.Log(FString::Printf(TEXT("[OK] remove_event_dispatcher(\"%s\")"), *FDispName));
					return sol::make_object(S, true);
				}
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_event_dispatcher(\"%s\") -> not found"), *FDispName));
			return sol::lua_nil;
		});

		// bp:remove_custom_event(name)
		BP.set_function("remove_custom_event", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& EventName, sol::this_state S) -> sol::object
		{
			FString FEventName = NeoLuaStr::ToFString(EventName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) { Session.Log(TEXT("[FAIL] remove_custom_event -> blueprint not found")); return sol::lua_nil; }

			if (Blueprint->UbergraphPages.Num() == 0) { Session.Log(TEXT("[FAIL] remove_custom_event -> no event graphs")); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemCE", "Remove Custom Event"));
			// Search all ubergraph pages, not just the first EventGraph
			for (UEdGraph* EventGraph : Blueprint->UbergraphPages)
			{
				if (!EventGraph) continue;
				for (int32 i = EventGraph->Nodes.Num() - 1; i >= 0; --i)
				{
					UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(EventGraph->Nodes[i]);
					if (EventNode && EventNode->CustomFunctionName.ToString().Equals(FEventName, ESearchCase::IgnoreCase))
					{
						FBlueprintEditorUtils::RemoveNode(Blueprint, EventNode, true);
						RefreshSelf(Self, S);
						Session.Log(FString::Printf(TEXT("[OK] remove_custom_event(\"%s\")"), *FEventName));
						return sol::make_object(S, true);
					}
				}
			}
			Session.Log(FString::Printf(TEXT("[FAIL] remove_custom_event(\"%s\") -> not found"), *FEventName));
			return sol::lua_nil;
		});

		// ================================================================
		// Override functions
		// ================================================================

		// bp:override_function(name) — create event or function override for parent class function
		BP.set_function("override_function", [&Session, FPath, PathStr, RefreshSelf](sol::table Self,
			const std::string& FuncName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FFuncName = NeoLuaStr::ToFString(FuncName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint) { Session.Log(TEXT("[FAIL] override_function -> blueprint not found")); return sol::lua_nil; }

			// Materialize any pending interface function graphs so the existing-graph
			// check below sees them (mirrors SMyBlueprint::ImplementFunction).
			FBlueprintEditorUtils::ConformImplementedInterfaces(Info.Blueprint);

			UFunction* OverrideFunc = nullptr;
			UClass* OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
				Info.Blueprint, FName(*FFuncName), &OverrideFunc);

			// If not found by C++ name, try matching by ScriptName or DisplayName metadata.
			// Many engine functions use K2_ prefix internally but expose friendly names
			// (e.g. K2_ActivateAbility has ScriptName="ActivateAbility").
			if (!OverrideFunc || !OverrideFuncClass)
			{
				UClass* ParentClass = Info.Blueprint->SkeletonGeneratedClass
					? Info.Blueprint->SkeletonGeneratedClass->GetSuperClass()
					: nullptr;
				if (ParentClass)
				{
					for (TFieldIterator<UFunction> FuncIt(ParentClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
					{
						UFunction* Func = *FuncIt;
						if (!Func) continue;
#if WITH_METADATA
						const FString& ScriptName = Func->GetMetaData(TEXT("ScriptName"));
						const FString& DisplayName = Func->GetMetaData(TEXT("DisplayName"));
						if (ScriptName.Equals(FFuncName, ESearchCase::IgnoreCase)
							|| DisplayName.Equals(FFuncName, ESearchCase::IgnoreCase))
						{
							// Retry with the real C++ function name
							FName RealName = Func->GetFName();
							OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
								Info.Blueprint, RealName, &OverrideFunc);
							if (OverrideFunc && OverrideFuncClass)
							{
								FFuncName = RealName.ToString();
								Session.Log(FString::Printf(TEXT("[INFO] override_function -> resolved \"%s\" to C++ name \"%s\""),
									UTF8_TO_TCHAR(FuncName.c_str()), *FFuncName));
								break;
							}
						}
#endif
					}
				}
			}

			if (!OverrideFunc || !OverrideFuncClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] override_function(\"%s\") -> not found in parent class"), *FFuncName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaOverride", "Override Function"));

			// FindObject(BP, name) catches graphs in FunctionGraphs/UbergraphPages/MacroGraphs
			// AND ImplementedInterfaces[].Graphs — they are all outered to the BP. Our
			// helper NeoBlueprint::FindGraph misses the interface list, so using it here
			// would let CreateNewGraph rename the interface graph aside and double-create.
			UEdGraph* ExistingGraphAtName = FindObject<UEdGraph>(Info.Blueprint, *FFuncName);
			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Info.Blueprint);

			const bool bWantEvent =
				UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunc)
				&& ExistingGraphAtName == nullptr
				&& EventGraph != nullptr;

			if (bWantEvent)
			{
				UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
					Info.Blueprint, OverrideFuncClass, FName(*FFuncName));

				// Also check for any event node matching this name (catches non-override events
				// like those placed from the default BP template)
				if (!Existing)
				{
					TArray<UK2Node_Event*> AllEvents;
					FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Info.Blueprint, AllEvents);
					for (UK2Node_Event* Evt : AllEvents)
					{
						if (Evt && Evt->EventReference.GetMemberName() == FName(*FFuncName))
						{
							Existing = Evt;
							break;
						}
					}
				}

				if (Existing)
				{
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> already exists (event)"), *FFuncName));
					sol::table Result = Lua.create_table();
					Result["handle"] = TCHAR_TO_UTF8(*Existing->NodeGuid.ToString());
					Result["type"] = "event";
					return Result;
				}

				FName EventFName = FName(*FFuncName);
				UK2Node_Event* NewEvent = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Event>(
					EventGraph, EventGraph->GetGoodPlaceForNewNode(), EK2NewNodeFlags::None,
					[EventFName, OverrideFuncClass](UK2Node_Event* NewInstance)
					{
						NewInstance->EventReference.SetExternalMember(EventFName, OverrideFuncClass);
						NewInstance->bOverrideFunction = true;
					}
				);

				if (NewEvent)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					RefreshSelf(Self, S);
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> created as event"), *FFuncName));
					sol::table Result = Lua.create_table();
					Result["handle"] = TCHAR_TO_UTF8(*NewEvent->NodeGuid.ToString());
					Result["type"] = "event";
					return Result;
				}
			}
			else
			{
				if (ExistingGraphAtName)
				{
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> already exists (function)"), *FFuncName));
					return BuildGraphObject(Lua, Session, ExistingGraphAtName, PathStr, FuncName);
				}

				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
					Info.Blueprint, FName(*FFuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (NewGraph)
				{
					FBlueprintEditorUtils::AddFunctionGraph(Info.Blueprint, NewGraph, false, OverrideFuncClass);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
					RefreshSelf(Self, S);
					Session.Log(FString::Printf(TEXT("[OK] override_function(\"%s\") -> created as function"), *FFuncName));
					return BuildGraphObject(Lua, Session, NewGraph, PathStr, FuncName);
				}
			}

			Session.Log(FString::Printf(TEXT("[FAIL] override_function(\"%s\") -> failed to create"), *FFuncName));
			return sol::lua_nil;
		});

		// bp:add_parent_call(function_or_event, opts?) — same node as editor "Add Call to Parent Function"
		BP.set_function("add_parent_call", [&Session, FPath, RefreshSelf](sol::table Self,
			sol::object TargetArg, sol::optional<sol::table> OptsArg, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint) { Session.Log(TEXT("[FAIL] add_parent_call -> blueprint not found")); return sol::lua_nil; }

			UEdGraphNode* SourceNode = nullptr;
			FString TargetName;

			if (TargetArg.is<sol::table>())
			{
				sol::table Target = TargetArg.as<sol::table>();
				if (sol::object HandleObj = Target["handle"]; HandleObj.valid() && HandleObj.is<std::string>())
				{
					SourceNode = Session.FindNode(NeoLuaStr::ToFString(HandleObj.as<std::string>()));
				}
				if (sol::object FunctionObj = Target["function"]; FunctionObj.valid() && FunctionObj.is<std::string>())
				{
					TargetName = NeoLuaStr::ToFString(FunctionObj.as<std::string>());
				}
				else if (sol::object NameObj = Target["name"]; NameObj.valid() && NameObj.is<std::string>())
				{
					TargetName = NeoLuaStr::ToFString(NameObj.as<std::string>());
				}
			}
			else if (TargetArg.is<std::string>())
			{
				TargetName = NeoLuaStr::ToFString(TargetArg.as<std::string>()).TrimStartAndEnd();
				SourceNode = Session.FindNode(TargetName);
			}
			else
			{
				Session.Log(TEXT("[FAIL] add_parent_call -> target must be an override function/event name, node handle, or table {handle=...}"));
				return sol::lua_nil;
			}

			UK2Node_Event* EventNode = Cast<UK2Node_Event>(SourceNode);
			UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(SourceNode);
			if (!EventNode && !EntryNode && !TargetName.IsEmpty())
			{
				FName FunctionName(*TargetName);
				EventNode = FindOverrideEventNode(Info.Blueprint, FunctionName);
				if (!EventNode)
				{
					EntryNode = FindFunctionEntryNode(Info.Blueprint, FunctionName);
				}
			}

			UEdGraphNode* AnchorNode = EventNode ? static_cast<UEdGraphNode*>(EventNode) : static_cast<UEdGraphNode*>(EntryNode);
			if (!AnchorNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_parent_call(\"%s\") -> override event/function not found. Call override_function(name) first, then add_parent_call(name)."),
					*TargetName));
				return sol::lua_nil;
			}

			UFunction* OverrideFunction = EventNode
				? EventNode->FindEventSignatureFunction()
				: EntryNode->FindSignatureFunction();
			UFunction* ParentFunction = ResolveCallableParentFunction(Info.Blueprint, OverrideFunction);
			if (!ParentFunction)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_parent_call(\"%s\") -> no callable parent function for override \"%s\""),
					*TargetName,
					OverrideFunction ? *OverrideFunction->GetName() : TEXT("(unknown)")));
				return sol::lua_nil;
			}

			UEdGraph* TargetGraph = AnchorNode->GetGraph();
			if (!TargetGraph)
			{
				Session.Log(TEXT("[FAIL] add_parent_call -> override node has no graph"));
				return sol::lua_nil;
			}

			bool bAutoConnect = false;
			double PosX = AnchorNode->NodePosX;
			double PosY = AnchorNode->NodePosY + 200.0;
			if (OptsArg.has_value())
			{
				sol::table Opts = OptsArg.value();
				bAutoConnect = Opts.get_or("auto_connect", false);
				PosX = Opts.get<sol::optional<double>>("x").value_or(PosX);
				PosY = Opts.get<sol::optional<double>>("y").value_or(PosY);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddParentCall", "Add Parent Function Call"));
			TargetGraph->SetFlags(RF_Transactional);
			TargetGraph->Modify();

			FGraphNodeCreator<UK2Node_CallParentFunction> NodeCreator(*TargetGraph);
			UK2Node_CallParentFunction* ParentCallNode = NodeCreator.CreateNode();
			if (!ParentCallNode)
			{
				Session.Log(TEXT("[FAIL] add_parent_call -> failed to create K2Node_CallParentFunction"));
				return sol::lua_nil;
			}
			ParentCallNode->SetFromFunction(ParentFunction);
			ParentCallNode->AllocateDefaultPins();
			ParentCallNode->NodePosX = static_cast<int32>(PosX);
			ParentCallNode->NodePosY = static_cast<int32>(PosY);
			UEdGraphSchema_K2::SetNodeMetaData(ParentCallNode, FNodeMetadata::DefaultGraphNode);
			NodeCreator.Finalize();

			if (bAutoConnect)
			{
				UEdGraphPin* FromPin = FindFirstExecOutputPin(AnchorNode);
				UEdGraphPin* ToPin = ParentCallNode->GetExecPin();
				if (FromPin && ToPin)
				{
					FromPin->MakeLinkTo(ToPin);
					AnchorNode->PinConnectionListChanged(FromPin);
					ParentCallNode->PinConnectionListChanged(ToPin);
				}
			}

			Session.RegisterGraphNodes(TargetGraph);
			Session.MarkGraphDirty(TargetGraph, Info.Blueprint);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
			RefreshSelf(Self, S);

			Session.Log(FString::Printf(TEXT("[OK] add_parent_call(\"%s\") -> %s handle=%s"),
				*ParentFunction->GetName(),
				bAutoConnect ? TEXT("created and auto-connected") : TEXT("created"),
				*ParentCallNode->NodeGuid.ToString()));
			return sol::make_object(Lua, BuildParentCallResult(Lua, ParentCallNode));
		});

		// bp:set_function_name_array_outputs(function_name, { OutputPin = {"NameA", "NameB"} })
		// Authors real MakeArray nodes wired into existing TArray<FName> function-result pins.
		BP.set_function("set_function_name_array_outputs", [&Session, FPath](sol::table /*Self*/,
			const std::string& FuncName, sol::table Outputs, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			const FString FFuncName = NeoLuaStr::ToFString(FuncName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] set_function_name_array_outputs -> blueprint not found"));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::ConformImplementedInterfaces(Info.Blueprint);
			UEdGraph* Graph = FindObject<UEdGraph>(Info.Blueprint, *FFuncName);
			if (!Graph)
			{
				Graph = NeoBlueprint::FindGraph(Info.Blueprint, FFuncName);
			}
			if (!Graph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_function_name_array_outputs(\"%s\") -> graph not found"), *FFuncName));
				return sol::lua_nil;
			}

			UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(Graph);
			if (!ResultNode)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_function_name_array_outputs(\"%s\") -> function result node not found"), *FFuncName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaSetNameArrayOutputs", "Set Function Name Array Outputs"));
			bool bChanged = false;
			int32 NodeIndex = 0;
			for (auto&& Entry : Outputs)
			{
				if (!Entry.first.is<std::string>() || !Entry.second.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] set_function_name_array_outputs -> expected table of OutputPin = {names...}"));
					return sol::lua_nil;
				}

				const FString PinName = NeoLuaStr::ToFString(Entry.first.as<std::string>());
				UEdGraphPin* TargetPin = NeoLuaPin::Find(ResultNode, PinName, EGPD_Input);
				if (!IsNameArrayFunctionResultPin(TargetPin))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_function_name_array_outputs(\"%s\") -> \"%s\" is not a TArray<Name> result pin"),
						*FFuncName, *PinName));
					return sol::lua_nil;
				}

				const TArray<FString> Values = LuaStringArrayToFStrings(Entry.second.as<sol::table>());
				if (Values.Num() == 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_function_name_array_outputs(\"%s\") -> \"%s\" requires at least one non-empty name"),
						*FFuncName, *PinName));
					return sol::lua_nil;
				}

				if (!CreateNameMakeArrayNode(Graph, TargetPin, Values, NodeIndex++))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_function_name_array_outputs(\"%s\") -> failed to author \"%s\""),
						*FFuncName, *PinName));
					return sol::lua_nil;
				}
				bChanged = true;
			}

			if (bChanged)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Info.Blueprint);
				Session.MarkGraphDirty(Graph);
			}

			Session.Log(FString::Printf(TEXT("[OK] set_function_name_array_outputs(\"%s\")"), *FFuncName));
			return sol::make_object(Lua, InspectFunctionNameArrayOutputs(Lua, Graph));
		});

		BP.set_function("list_function_name_array_outputs", [&Session, FPath](sol::table /*Self*/,
			const std::string& FuncName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			const FString FFuncName = NeoLuaStr::ToFString(FuncName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint)
			{
				Session.Log(TEXT("[FAIL] list_function_name_array_outputs -> blueprint not found"));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::ConformImplementedInterfaces(Info.Blueprint);
			UEdGraph* Graph = FindObject<UEdGraph>(Info.Blueprint, *FFuncName);
			if (!Graph)
			{
				Graph = NeoBlueprint::FindGraph(Info.Blueprint, FFuncName);
			}
			if (!Graph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list_function_name_array_outputs(\"%s\") -> graph not found"), *FFuncName));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_function_name_array_outputs(\"%s\")"), *FFuncName));
			return sol::make_object(Lua, InspectFunctionNameArrayOutputs(Lua, Graph));
		});

		// ================================================================
		// Anim Layer graph support (AnimBlueprint only)
		// ================================================================

		// bp:add_anim_layer(name) -> graph
		BP.set_function("add_anim_layer", [&Session, FPath, PathStr, RefreshSelf](sol::table Self,
			const std::string& LayerName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FLayerName = NeoLuaStr::ToFString(LayerName).TrimStartAndEnd();
			if (FLayerName.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] add_anim_layer -> layer name required"));
				return sol::lua_nil;
			}

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint) { Session.Log(TEXT("[FAIL] add_anim_layer -> blueprint not found")); return sol::lua_nil; }

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP) { Session.Log(TEXT("[FAIL] add_anim_layer -> not an Animation Blueprint")); return sol::lua_nil; }

			if (UEdGraph* ExistingGraph = FindObject<UEdGraph>(AnimBP, *FLayerName))
			{
				if (!ExistingGraph->IsA<UAnimationGraph>())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_anim_layer(\"%s\") -> graph already exists but is not an AnimationGraph"), *FLayerName));
					return sol::lua_nil;
				}

				Session.Log(FString::Printf(TEXT("[OK] add_anim_layer(\"%s\") -> already exists"), *FLayerName));
				return BuildGraphObject(Lua, Session, ExistingGraph, PathStr, LayerName);
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddAnimLayer", "Add Animation Layer Graph"));
			AnimBP->Modify();

			FName UniqueName = FBlueprintEditorUtils::FindUniqueKismetName(AnimBP, FLayerName);
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				AnimBP, UniqueName, UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
			if (!NewGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_anim_layer(\"%s\") -> graph creation failed"), *FLayerName));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::AddDomainSpecificGraph(AnimBP, NewGraph);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			RefreshSelf(Self, S);

			Session.Log(FString::Printf(TEXT("[OK] add_anim_layer(\"%s\") -> graph=\"%s\""), *FLayerName, *NewGraph->GetName()));
			return BuildGraphObject(Lua, Session, NewGraph, PathStr, std::string(TCHAR_TO_UTF8(*NewGraph->GetName())));
		});

		// bp:add_linked_anim_layer(layer_name, interface?)
		BP.set_function("add_linked_anim_layer", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& LayerName, sol::optional<std::string> InterfaceStr,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FLayerName = NeoLuaStr::ToFString(LayerName);

			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FPath);
			if (!Info.Blueprint) { Session.Log(TEXT("[FAIL] add_linked_anim_layer -> blueprint not found")); return sol::lua_nil; }

			UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Info.Blueprint);
			if (!AnimBP) { Session.Log(TEXT("[FAIL] add_linked_anim_layer -> not an Animation Blueprint")); return sol::lua_nil; }

			UEdGraph* AnimGraph = NeoBlueprint::FindAnimGraph(AnimBP);
			if (!AnimGraph) { Session.Log(TEXT("[FAIL] add_linked_anim_layer -> AnimGraph not found")); return sol::lua_nil; }

			auto GetImplementedDesc = [AnimBP](int32 Index) -> const FBPInterfaceDescription*
			{
				return AnimBP->ImplementedInterfaces.IsValidIndex(Index) ? &AnimBP->ImplementedInterfaces[Index] : nullptr;
			};

			auto EnsureInterfaceImplemented = [&Session, AnimBP](const FString& InterfaceInput, UClass*& InterfaceClass) -> const FBPInterfaceDescription*
			{
				int32 DescIndex = FindImplementedAnimLayerInterfaceIndex(AnimBP, InterfaceClass, InterfaceInput);
				if (DescIndex != INDEX_NONE)
				{
					if (!InterfaceClass)
					{
						InterfaceClass = AnimBP->ImplementedInterfaces[DescIndex].Interface;
					}
					return &AnimBP->ImplementedInterfaces[DescIndex];
				}

				const int32 BeforeCount = AnimBP->ImplementedInterfaces.Num();
				Session.Log(FString::Printf(TEXT("[INFO] add_linked_anim_layer -> interface '%s' not implemented, attempting auto-implement"), *InterfaceInput));
				if (NeoBlueprint::AddInterface(AnimBP, InterfaceInput))
				{
					if (InterfaceClass)
					{
						DescIndex = FindImplementedAnimLayerInterfaceIndex(AnimBP, InterfaceClass, InterfaceInput);
					}
					if (DescIndex == INDEX_NONE)
					{
						for (int32 Index = BeforeCount; Index < AnimBP->ImplementedInterfaces.Num(); ++Index)
						{
							const FBPInterfaceDescription& Desc = AnimBP->ImplementedInterfaces[Index];
							if (IsAnimLayerInterfaceClass(Desc.Interface))
							{
								DescIndex = Index;
								break;
							}
						}
					}
					if (DescIndex != INDEX_NONE)
					{
						InterfaceClass = AnimBP->ImplementedInterfaces[DescIndex].Interface;
						return &AnimBP->ImplementedInterfaces[DescIndex];
					}
				}

				return nullptr;
			};

			auto FormatLayerNames = [](const TArray<FName>& Names) -> FString
			{
				TArray<FString> Strings;
				for (FName Name : Names)
				{
					Strings.Add(Name.ToString());
				}
				return FString::Join(Strings, TEXT(", "));
			};

			UClass* InterfaceClass = nullptr;
			const FBPInterfaceDescription* InterfaceDesc = nullptr;
			TArray<FName> LayersToAdd;

			if (InterfaceStr.has_value())
			{
				FString FInterface = NeoLuaStr::ToFStringOpt(InterfaceStr);
				InterfaceClass = ResolveAnimLayerInterfaceClass(FInterface);
				InterfaceDesc = EnsureInterfaceImplemented(FInterface, InterfaceClass);
				if (!InterfaceDesc)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> AnimLayerInterface '%s' could not be implemented"), *FInterface));
					return sol::lua_nil;
				}
				LayersToAdd.Add(FName(*FLayerName));
			}
			else
			{
				const FString FInterfaceOrLayer = FLayerName;
				InterfaceClass = ResolveAnimLayerInterfaceClass(FInterfaceOrLayer);
				int32 DescIndex = FindImplementedAnimLayerInterfaceIndex(AnimBP, InterfaceClass, FInterfaceOrLayer);
				if (DescIndex != INDEX_NONE)
				{
					InterfaceDesc = GetImplementedDesc(DescIndex);
					if (!InterfaceClass && InterfaceDesc)
					{
						InterfaceClass = InterfaceDesc->Interface;
					}
				}
				else if (InterfaceClass)
				{
					InterfaceDesc = EnsureInterfaceImplemented(FInterfaceOrLayer, InterfaceClass);
					if (!InterfaceDesc)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> AnimLayerInterface '%s' could not be implemented"), *FInterfaceOrLayer));
						return sol::lua_nil;
					}
				}
				else
				{
					const int32 BeforeCount = AnimBP->ImplementedInterfaces.Num();
					if (NeoBlueprint::AddInterface(AnimBP, FInterfaceOrLayer))
					{
						for (int32 Index = BeforeCount; Index < AnimBP->ImplementedInterfaces.Num(); ++Index)
						{
							const FBPInterfaceDescription& Desc = AnimBP->ImplementedInterfaces[Index];
							if (IsAnimLayerInterfaceClass(Desc.Interface))
							{
								InterfaceClass = Desc.Interface;
								InterfaceDesc = &Desc;
								break;
							}
						}
					}
				}

				if (InterfaceClass)
				{
					CollectAnimLayerFunctionNames(InterfaceClass, InterfaceDesc, LayersToAdd);
				}
				else
				{
					for (const FBPInterfaceDescription& Desc : AnimBP->ImplementedInterfaces)
					{
						if (IsAnimLayerInterfaceClass(Desc.Interface))
						{
							InterfaceClass = Desc.Interface;
							InterfaceDesc = &Desc;
							LayersToAdd.Add(FName(*FLayerName));
							break;
						}
					}
				}
			}

			if (!InterfaceClass)
			{
				Session.Log(TEXT("[FAIL] add_linked_anim_layer -> AnimLayerInterface not found. Use add_interface() first, or pass the interface asset path."));
				return sol::lua_nil;
			}

			if (!IsAnimLayerInterfaceClass(InterfaceClass))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> '%s' is not an AnimLayerInterface"),
					*InterfaceClass->GetName()));
				return sol::lua_nil;
			}

			if (LayersToAdd.Num() == 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> no anim layer functions found on interface '%s'"),
					*InterfaceClass->GetName()));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddLayer", "Add Linked Anim Layer"));
			AnimGraph->Modify();

			TArray<FString> CreatedHandles;
			auto CreateLinkedLayerNode = [&](FName LayerName) -> sol::object
			{
				TArray<FName> AvailableLayers;
				CollectAnimLayerFunctionNames(InterfaceClass, InterfaceDesc, AvailableLayers);
				if (!FindAnimLayerFunctionByName(InterfaceClass, LayerName) && !AvailableLayers.Contains(LayerName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_linked_anim_layer -> function '%s' not found on interface '%s'. Available layers: %s"),
						*LayerName.ToString(), *InterfaceClass->GetName(), *FormatLayerNames(AvailableLayers)));
					return sol::lua_nil;
				}

				UAnimGraphNode_LinkedAnimLayer* LayerNode = NewObject<UAnimGraphNode_LinkedAnimLayer>(AnimGraph);
				LayerNode->Node.Interface = InterfaceClass;  // BEFORE PostPlacedNewNode
				LayerNode->CreateNewGuid();
				AnimGraph->AddNode(LayerNode, false, false);
				LayerNode->SetFlags(RF_Transactional);
				LayerNode->PostPlacedNewNode();
				LayerNode->AllocateDefaultPins();

				if (!ConfigureLinkedAnimLayerNode(AnimBP, LayerNode, InterfaceClass, LayerName))
				{
					Session.Log(TEXT("[FAIL] add_linked_anim_layer -> unable to initialize linked anim layer node"));
					return sol::lua_nil;
				}
				LayerNode->ReconstructNode();

				int32 MaxX = 200;
				for (UEdGraphNode* Node : AnimGraph->Nodes)
				{
					if (Node && Node != LayerNode)
						MaxX = FMath::Max(MaxX, static_cast<int32>(Node->NodePosX) + 300);
				}
				LayerNode->NodePosX = MaxX;
				LayerNode->NodePosY = 0;

				FString NodeGuid = LayerNode->NodeGuid.ToString();
				Session.Nodes.Add(NodeGuid, LayerNode);
				CreatedHandles.Add(NodeGuid);

				sol::table Result = Lua.create_table();
				Result["handle"] = TCHAR_TO_UTF8(*NodeGuid);
				Result["layer"] = TCHAR_TO_UTF8(*LayerName.ToString());
				Result["interface"] = TCHAR_TO_UTF8(*InterfaceClass->GetName());
				return Result;
			};

			sol::table CreatedNodes = Lua.create_table();
			for (FName LayerToCreate : LayersToAdd)
			{
				sol::object NodeResult = CreateLinkedLayerNode(LayerToCreate);
				if (!NodeResult.valid() || !NodeResult.is<sol::table>())
				{
					return sol::lua_nil;
				}
				CreatedNodes.add(NodeResult);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			RefreshSelf(Self, S);

			if (LayersToAdd.Num() == 1)
			{
				sol::table Result = CreatedNodes[1].get<sol::table>();
				Session.Log(FString::Printf(TEXT("[OK] add_linked_anim_layer(\"%s\", interface=\"%s\") -> handle=\"%s\""),
					*LayersToAdd[0].ToString(), *InterfaceClass->GetName(),
					CreatedHandles.Num() > 0 ? *CreatedHandles[0] : TEXT("")));
				return Result;
			}

			sol::table Result = Lua.create_table();
			Result["interface"] = TCHAR_TO_UTF8(*InterfaceClass->GetName());
			Result["count"] = LayersToAdd.Num();
			Result["nodes"] = CreatedNodes;
			Session.Log(FString::Printf(TEXT("[OK] add_linked_anim_layer(interface=\"%s\") -> %d linked layer nodes"),
				*InterfaceClass->GetName(), LayersToAdd.Num()));
			return Result;
		});

		// ================================================================
		// Additional component operations
		// ================================================================

		// bp:rename_component(name, new_name)
		BP.set_function("rename_component", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			FString FOld = NeoLuaStr::ToFString(OldName);
			FString FNew = NeoLuaStr::ToFString(NewName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] rename_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			USCS_Node* Node = nullptr;
			for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
				if (N && N->GetVariableName().ToString().Equals(FOld, ESearchCase::IgnoreCase)) { Node = N; break; }
			if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] rename_component(\"%s\") -> not found"), *FOld)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenComp", "Rename Component"));
			FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, FName(*FNew));
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] rename_component(\"%s\" -> \"%s\")"), *FOld, *FNew));
			return sol::make_object(S, true);
		});

		// bp:duplicate_component(name, new_name?)
		BP.set_function("duplicate_component", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& SourceName, sol::optional<std::string> NewNameOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSource = NeoLuaStr::ToFString(SourceName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] duplicate_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			USCS_Node* SourceNode = nullptr;
			for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
				if (N && N->GetVariableName().ToString().Equals(FSource, ESearchCase::IgnoreCase)) { SourceNode = N; break; }
			if (!SourceNode || !SourceNode->ComponentTemplate)
			{ Session.Log(FString::Printf(TEXT("[FAIL] duplicate_component(\"%s\") -> not found"), *FSource)); return sol::lua_nil; }

			FString FNew = NewNameOpt.has_value() ? NeoLuaStr::ToFString(NewNameOpt.value()) :
				FString::Printf(TEXT("%s_Copy"), *FSource);

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaDupComp", "Duplicate Component"));
			USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(SourceNode->ComponentTemplate->GetClass(), FName(*FNew));
			if (!NewNode) { Session.Log(TEXT("[FAIL] duplicate_component -> CreateNode failed")); return sol::lua_nil; }

			UEngine::CopyPropertiesForUnrelatedObjects(SourceNode->ComponentTemplate, NewNode->ComponentTemplate);

			USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(SourceNode);
			if (ParentNode) ParentNode->AddChildNode(NewNode);
			else Blueprint->SimpleConstructionScript->AddNode(NewNode);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Session.Log(FString::Printf(TEXT("[OK] duplicate_component(\"%s\" -> \"%s\")"), *FSource, *FNew));
			RefreshSelf(Self, S);

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*NewNode->GetVariableName().ToString());
			Result["class"] = NewNode->ComponentClass ? TCHAR_TO_UTF8(*NewNode->ComponentClass->GetName()) : "None";
			return Result;
		});

		// bp:reparent_component(name, parent?) — move to new parent (empty = root)
		BP.set_function("reparent_component", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& CompName, sol::optional<std::string> ParentName, sol::this_state S) -> sol::object
		{
			FString FComp = NeoLuaStr::ToFString(CompName);
			FString FParent = NeoLuaStr::ToFStringOpt(ParentName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] reparent_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			auto FindSCSNode = [&](const FString& Name) -> USCS_Node* {
				for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
					if (N && N->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase)) return N;
				return nullptr;
			};

			USCS_Node* Node = FindSCSNode(FComp);
			if (!Node) { Session.Log(FString::Printf(TEXT("[FAIL] reparent_component(\"%s\") -> not found"), *FComp)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReparentComp", "Reparent Component"));
			USCS_Node* CurrentParent = Blueprint->SimpleConstructionScript->FindParentNode(Node);
			if (CurrentParent) CurrentParent->RemoveChildNode(Node, false);
			else Blueprint->SimpleConstructionScript->RemoveNode(Node, false);

			bool bToRoot = FParent.IsEmpty() || FParent.Equals(TEXT("root"), ESearchCase::IgnoreCase);
			if (bToRoot)
			{
				Blueprint->SimpleConstructionScript->AddNode(Node);
			}
			else
			{
				USCS_Node* NewParent = FindSCSNode(FParent);
				if (!NewParent) { Blueprint->SimpleConstructionScript->AddNode(Node);
					Session.Log(FString::Printf(TEXT("[FAIL] reparent_component -> parent '%s' not found"), *FParent)); return sol::lua_nil; }
				// Circular check
				USCS_Node* Check = NewParent;
				while (Check) {
					if (Check == Node) { Blueprint->SimpleConstructionScript->AddNode(Node);
						Session.Log(TEXT("[FAIL] reparent_component -> circular reference")); return sol::lua_nil; }
					Check = Blueprint->SimpleConstructionScript->FindParentNode(Check);
				}
				NewParent->AddChildNode(Node, true);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] reparent_component(\"%s\" -> \"%s\")"), *FComp, bToRoot ? TEXT("root") : *FParent));
			return sol::make_object(S, true);
		});

		// bp:set_root_component(name) — promote a scene component to root
		BP.set_function("set_root_component", [&Session, FPath, RefreshSelf](sol::table Self,
			const std::string& CompName, sol::this_state S) -> sol::object
		{
			FString FComp = NeoLuaStr::ToFString(CompName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{ Session.Log(TEXT("[FAIL] set_root_component -> blueprint not found or no SCS")); return sol::lua_nil; }

			USCS_Node* NewRootNode = nullptr;
			for (USCS_Node* N : Blueprint->SimpleConstructionScript->GetAllNodes())
				if (N && N->GetVariableName().ToString().Equals(FComp, ESearchCase::IgnoreCase)) { NewRootNode = N; break; }
			if (!NewRootNode || !Cast<USceneComponent>(NewRootNode->ComponentTemplate))
			{ Session.Log(FString::Printf(TEXT("[FAIL] set_root_component(\"%s\") -> not found or not SceneComponent"), *FComp)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaSetRoot", "Set Root Component"));

			USCS_Node* CurrentRoot = nullptr;
			for (USCS_Node* RN : Blueprint->SimpleConstructionScript->GetRootNodes())
				if (RN && Cast<USceneComponent>(RN->ComponentTemplate)) { CurrentRoot = RN; break; }

			if (CurrentRoot == NewRootNode) { Session.Log(FString::Printf(TEXT("[OK] set_root_component(\"%s\") -> already root"), *FComp)); return sol::make_object(S, true); }

			USCS_Node* OldParent = Blueprint->SimpleConstructionScript->FindParentNode(NewRootNode);
			if (OldParent) OldParent->RemoveChildNode(NewRootNode, false);
			else Blueprint->SimpleConstructionScript->RemoveNode(NewRootNode, false);

			if (CurrentRoot)
			{
				TArray<USCS_Node*> Children = CurrentRoot->GetChildNodes();
				for (USCS_Node* Child : Children)
					if (Child && Child != NewRootNode) { CurrentRoot->RemoveChildNode(Child, false); NewRootNode->AddChildNode(Child, false); }
				Blueprint->SimpleConstructionScript->RemoveNode(CurrentRoot, false);
				NewRootNode->AddChildNode(CurrentRoot, true);
			}
			Blueprint->SimpleConstructionScript->AddNode(NewRootNode);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] set_root_component(\"%s\")"), *FComp));
			return sol::make_object(S, true);
		});

		// bp:add_event_graph(name)
		BP.set_function("add_event_graph", [&Session, FPath, PathStr, RefreshSelf](sol::table Self,
			const std::string& GraphName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FGraphName = NeoLuaStr::ToFString(GraphName);
			UBlueprint* Blueprint = NeoLuaAsset::Resolve<UBlueprint>(FPath);
			if (!Blueprint) { Session.Log(TEXT("[FAIL] add_event_graph -> blueprint not found")); return sol::lua_nil; }

			for (UEdGraph* G : Blueprint->UbergraphPages)
				if (G && G->GetName().Equals(FGraphName, ESearchCase::IgnoreCase))
				{ Session.Log(FString::Printf(TEXT("[FAIL] add_event_graph(\"%s\") -> already exists"), *FGraphName)); return sol::lua_nil; }

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddEG", "Add Event Graph"));
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*FGraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph) { Session.Log(FString::Printf(TEXT("[FAIL] add_event_graph(\"%s\") -> failed"), *FGraphName)); return sol::lua_nil; }

			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			RefreshSelf(Self, S);
			Session.Log(FString::Printf(TEXT("[OK] add_event_graph(\"%s\")"), *FGraphName));
			return BuildGraphObject(Lua, Session, NewGraph, PathStr, GraphName);
		});

		// bp:compile()
		// bp:info() -> table with blueprint metadata
		BP.set_function("info", [&Session, FPath](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FBlueprintInfo BPInfo = NeoBlueprint::LoadBlueprint(FPath);
			if (!BPInfo.Blueprint)
			{
				Session.Log(TEXT("[FAIL] info() -> blueprint not found"));
				return sol::lua_nil;
			}
			UBlueprint* Blueprint = BPInfo.Blueprint;

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*BPInfo.Name);
			Result["path"] = TCHAR_TO_UTF8(*BPInfo.AssetPath);
			Result["parent_class"] = TCHAR_TO_UTF8(*BPInfo.ParentClass);
			Result["type"] = TCHAR_TO_UTF8(*Blueprint->GetClass()->GetName());

			// Counts — match UE's complete Blueprint graph set, including collapsed,
			// delegate, interface, and extension graphs.
			int32 TotalNodes = 0;
			TSet<UEdGraph*> CountedGraphs;
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* G : AllGraphs)
			{
				if (!G || CountedGraphs.Contains(G)) continue;
				CountedGraphs.Add(G);
				TotalNodes += G->Nodes.Num();
			}
			int32 NumGraphs = CountedGraphs.Num();

			// AnimBP: count SM sub-graphs (state machines, states, transitions, layers)
			if (Blueprint->IsA<UAnimBlueprint>())
			{
				TArray<TPair<FString, UEdGraph*>> AnimGraphs;
				NeoBlueprint::CollectAnimBPGraphs(Blueprint, AnimGraphs);
				for (const auto& Pair : AnimGraphs)
				{
					if (!Pair.Value || CountedGraphs.Contains(Pair.Value)) continue;
					CountedGraphs.Add(Pair.Value);
					NumGraphs++;
					TotalNodes += Pair.Value->Nodes.Num();
				}
			}

			Result["num_graphs"] = NumGraphs;
			Result["num_nodes"] = TotalNodes;
			Result["num_variables"] = Blueprint->NewVariables.Num();
			Result["num_components"] = Blueprint->SimpleConstructionScript
				? Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0;
			Result["num_interfaces"] = Blueprint->ImplementedInterfaces.Num();
			Result["num_functions"] = Blueprint->FunctionGraphs.Num();
			Result["num_macros"] = Blueprint->MacroGraphs.Num();
			Result["num_event_graphs"] = Blueprint->UbergraphPages.Num();

			// Blueprint flags
			Result["is_actor"] = Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
			Result["is_widget"] = Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(UUserWidget::StaticClass());
			Result["is_anim_bp"] = Blueprint->IsA<UAnimBlueprint>();

			// Graphs array — list all graph names and types
			sol::table GraphsArray = Lua.create_table();
			int32 GIdx = 1;
			auto AddGraphInfo = [&](UEdGraph* G, const char* GraphType)
			{
				if (!G) return;
				sol::table GEntry = Lua.create_table();
				GEntry["name"] = TCHAR_TO_UTF8(*G->GetName());
				GEntry["type"] = GraphType;
				GEntry["num_nodes"] = G->Nodes.Num();
				GraphsArray[GIdx++] = GEntry;
			};
			TSet<UEdGraph*> ListedGraphs;

			auto AddGraphInfoUnique = [&](UEdGraph* G, const char* GraphType)
			{
				if (!G || ListedGraphs.Contains(G)) return;
				ListedGraphs.Add(G);
				AddGraphInfo(G, GraphType);
			};

			for (UEdGraph* G : Blueprint->UbergraphPages) AddGraphInfoUnique(G, "event_graph");
			for (UEdGraph* G : Blueprint->FunctionGraphs) AddGraphInfoUnique(G, "function");
			for (UEdGraph* G : Blueprint->MacroGraphs) AddGraphInfoUnique(G, "macro");

			// AnimBP state-machine graphs are also nested subgraphs. Classify them
			// before the generic subgraph walker so info() exposes semantic types.
			if (Blueprint->IsA<UAnimBlueprint>())
			{
				TArray<TPair<FString, UEdGraph*>> AnimGraphs;
				NeoBlueprint::CollectAnimBPGraphs(Blueprint, AnimGraphs);
				for (const auto& Pair : AnimGraphs)
				{
					if (!Pair.Value || ListedGraphs.Contains(Pair.Value)) continue;
					ListedGraphs.Add(Pair.Value);
					sol::table GEntry = Lua.create_table();
					GEntry["name"] = TCHAR_TO_UTF8(*Pair.Key);
					if (Pair.Key.Contains(TEXT("->")))
						GEntry["type"] = "transition";
					else if (Pair.Key.Contains(TEXT("/")))
						GEntry["type"] = "state";
					else
						GEntry["type"] = "state_machine";
					GEntry["num_nodes"] = Pair.Value->Nodes.Num();
					GraphsArray[GIdx++] = GEntry;
				}
			}

			// Include recursive SubGraphs
			TFunction<void(UEdGraph*)> ListSubGraphs = [&](UEdGraph* Parent)
			{
				for (UEdGraph* Sub : Parent->SubGraphs)
				{
					if (!Sub || ListedGraphs.Contains(Sub)) continue;
					ListedGraphs.Add(Sub);
					AddGraphInfo(Sub, "subgraph");
					ListSubGraphs(Sub);
				}
			};
			for (UEdGraph* G : Blueprint->UbergraphPages) if (G) ListSubGraphs(G);
			for (UEdGraph* G : Blueprint->FunctionGraphs) if (G) ListSubGraphs(G);
			for (UEdGraph* G : Blueprint->MacroGraphs) if (G) ListSubGraphs(G);

			for (UEdGraph* G : AllGraphs)
			{
				AddGraphInfoUnique(G, "additional");
			}

			Result["graphs"] = GraphsArray;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s, %d graphs, %d nodes, %d vars, %d components"),
				*BPInfo.Name, NumGraphs, TotalNodes, Blueprint->NewVariables.Num(),
				Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0));
			return Result;
		});

		// bp:help() -> string describing available methods
		BP.set_function("help", [&Session](sol::table self, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			std::string Out =
				"=== Blueprint methods ===\n"
				"  info() — structured metadata (graphs, variables, components, etc.)\n"
				"  help() — this help text\n"
				"\n--- Variables ---\n"
				"  add_variable(name, type, opts?) — add a member variable\n"
				"    opts: {default, category, tooltip, replicated, rep_notify, edit_anywhere, edit_defaults_only,\n"
				"      edit_instance_only, blueprint_read_only, save_game, transient, expose_on_spawn,\n"
				"      interp, advanced_display, deprecated, metadata={key=val}, duplicate_from=\"VarName\"}\n"
				"  rename_variable(old, new) — rename a variable\n"
				"  remove_variable(name) — remove a variable\n"
				"\n--- Components ---\n"
				"  add_component(name, class, parent?) — add a component\n"
				"  rename_component(old, new) — rename a component\n"
				"  duplicate_component(name) — duplicate a component\n"
				"  reparent_component(name, new_parent) — reparent a component\n"
				"  set_root_component(name) — set as root component\n"
				"  remove(\"component\", name) — remove a component\n"
				"\n--- Functions & Events ---\n"
				"  add_function(name, opts?) — add a function graph\n"
				"    opts: {params/inputs={{name,type},...}, returns/outputs={{name,type},...}, pure, const_func, category}\n"
				"  override_function(name) — override a parent function\n"
				"  add_parent_call(function_or_event, opts?) — add the editor's \"Call to Parent Function\" node; opts: {auto_connect, x, y}\n"
				"  add_macro(name) — add a macro graph\n"
				"  add_custom_event(name, opts?) — add a custom event\n"
				"    opts: {params={{name,type},...}} or direct {{name,type},...} for params only\n"
				"  remove_custom_event(name) — remove a custom event\n"
				"  add_event_dispatcher(name, params?) — add an event dispatcher; params may be {{name,type},...}, {params={{name,type},...}}, or {inputs={{name,type},...}}\n"
				"  remove_event_dispatcher(name) — remove an event dispatcher\n"
				"  add_event_graph(name) — add an event graph\n"
				"\n--- Events Binding ---\n"
				"  list_events(source?) — list delegate events on a component/widget, or all widget-tree delegates and components if omitted\n"
				"  bind_event(source, event) — bind a component/widget delegate event after validating the event name\n"
				"  unbind_event(event) — unbind an event\n"
				"\n--- Interfaces ---\n"
				"  add_interface(name) — implement an interface\n"
				"  remove_interface(name) — remove an interface\n"
				"\n--- State Machines (AnimBP) ---\n"
				"  add_state_machine(name, graph?) — add a state machine\n"
				"  add_state(state_machine, name) — add a state to a state machine\n"
				"  add_transition(state_machine, from, to) — add a transition between states\n"
				"  add_anim_layer(name) — add an AnimationGraph layer graph; use this for AnimGraph nodes, not override_function()\n"
				"  add_linked_anim_layer(layer, interface?) — add a linked anim layer; passing only an AnimLayerInterface adds its layer functions\n"
				"\n--- Widgets (UMG) ---\n"
				"  add_widget(class, name, parent?) — add a widget\n"
				"  remove_widget(name) — remove a widget\n"
				"  configure_widget(name, props) — configure widget properties\n"
				"  rename_widget(old_name, new_name) — rename a widget\n"
				"  wrap_widgets(wrapper_class, {names}) — wrap widgets in a container\n"
				"  replace_widget(name, new_class) — replace widget with different class\n"
				"\n--- Timelines ---\n"
				"  add_timeline(name, opts?) — add a timeline (upserts)\n"
				"    opts: {length, auto_play, loop, replicated, ignore_time_dilation,\n"
				"      tracks={{name, type=\"float\"|\"vector\"|\"color\"|\"event\",\n"
				"        keys={{time=0, value=0, interp_mode=\"Cubic\"}, ...}},...}}\n"
				"    Vector keys: {time=0, x=0, y=0, z=0}. Color keys: {time=0, r=1, g=0, b=0, a=1}.\n"
				"    Event keys: {1.0, 2.5} (bare numbers). Legacy {time, value} form still works.\n"
				"  add_timeline_track(name, track) — append one track to an existing timeline\n"
				"\n--- Graph Operations ---\n"
				"  find_nodes(query, max?) — search for node types\n"
				"  add_comment(text, x, y, w?, h?) — add a comment node\n"
				"  break_connection(node, pin) — break connections\n"
				"  align_nodes(handles, axis) — align nodes\n"
				"\n--- Properties ---\n"
				"  set_property(name, props) — set variable props (interp, advanced_display, deprecated, metadata={...})\n"
				"    For functions: description, keywords, compact_title, deprecated, deprecation_message, call_in_editor, thread_safe\n"
				"  set(target, property, value) — set property via reflection\n"
				"  get(target, property) — read a property\n"
				"  list_properties(target, filter?) — list properties\n"
				"\n--- Other ---\n"
				"  rename(type, old, new) — rename function/variable\n"
				"  remove(type, name) — remove function/variable/component\n"
				"  reparent(new_parent_class) — change parent class\n"
				"  compile() — compile the blueprint\n"
				"  save() — save to disk\n"
				"  refresh() — rebuild .graphs/.variables/.components/.interfaces on this same handle\n"
				"\n--- Data Access ---\n"
				"  .graphs — current graph objects (by name and index); structural mutations refresh this table\n"
				"  .variables — current variable info (by name); structural mutations refresh this table\n"
				"  .components — current component info (by name); structural mutations refresh this table\n"
				"  .interfaces — table of implemented interfaces\n"
				"  .name, .path, .parent_class — basic info\n"
				"\n--- Graph Object Methods (on .graphs[\"name\"]) ---\n"
				"  add_node(node, x?, y?) — add a node to this graph\n"
				"  connect(from_handle, from_pin, to_handle, to_pin) — connect pins\n"
				"  disconnect(handle, pin_name) — break all connections on a pin\n"
				"  disconnect_from(from_handle, from_pin, to_handle, to_pin) — break a specific connection\n"
				"  set_pin(handle, pin_name, value) — set pin default value\n"
				"  get_pin(handle, pin_name) — read pin default value\n"
				"  delete_node(handle) — delete a node\n"
				"  move_node(handle, x, y, relative?) — move node position\n"
				"  split_pin(handle, pin_name) — split a struct pin\n"
				"  recombine_pin(handle, pin_name) — recombine a split pin\n"
				"  add_exec_pin(handle) — add execution pin (AddPins)\n"
				"  remove_exec_pin(handle) — remove last execution pin\n"
				"  reset_pin(handle, pin_name) — reset pin to default\n"
				"  set_node_comment(handle, text, visible?) — set node comment\n"
				"  .nodes — table of all nodes in the graph\n";

			// Append enrichment-specific help (e.g., WidgetBlueprint, GameplayAbility)
			sol::optional<std::string> Extra = self.get<sol::optional<std::string>>("_help_text");
			if (Extra.has_value() && !Extra.value().empty())
			{
				Out += "\n" + Extra.value();
			}

			Session.Log(FString::Printf(TEXT("[OK] help()\n%s"), UTF8_TO_TCHAR(Out.c_str())));
			return sol::make_object(Lua, Out);
		});

		BP.set_function("compile", [&Session, FPath](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			FBlueprintInfo CompileInfo = NeoBlueprint::LoadBlueprint(FPath);
			if (!CompileInfo.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] compile(\"%s\") -> blueprint not found"), *FPath));
				return sol::lua_nil;
			}

			FCompileResult Result = NeoBlueprint::CompileBlueprint(CompileInfo.Blueprint);

			if (Result.bSuccess)
			{
				Session.Log(FString::Printf(TEXT("[OK] compile(\"%s\") -> success"), *FPath));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] compile(\"%s\") -> errors"), *FPath));
			}

			return sol::make_object(S, Result.bSuccess);
		});

		// bp:save()
		BP.set_function("save", [&Session, FPath](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			FBlueprintInfo SaveInfo = NeoBlueprint::LoadBlueprint(FPath);
			if (!SaveInfo.Blueprint)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> blueprint not found"), *FPath));
				return sol::lua_nil;
			}

			bool bSaved = NeoBlueprint::SaveBlueprint(SaveInfo.Blueprint);
			if (bSaved)
			{
				Session.Log(FString::Printf(TEXT("[OK] save(\"%s\") -> saved"), *FPath));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] save(\"%s\") -> save failed"), *FPath));
			}

			return sol::make_object(S, bSaved);
		});

		int32 TotalNodes = 0;
		for (UEdGraph* Graph : Info.Blueprint->UbergraphPages)
			if (Graph) TotalNodes += Graph->Nodes.Num();
		for (UEdGraph* Graph : Info.Blueprint->FunctionGraphs)
			if (Graph) TotalNodes += Graph->Nodes.Num();
		for (UEdGraph* Graph : Info.Blueprint->MacroGraphs)
			if (Graph) TotalNodes += Graph->Nodes.Num();

		int32 NumVars = Info.Blueprint->NewVariables.Num();
		int32 NumComps = Info.Blueprint->SimpleConstructionScript
			? Info.Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0;

		Session.Log(FString::Printf(TEXT("[OK] open_blueprint(\"%s\") -> %d graphs, %d nodes, %d variables, %d components"),
			*FPath, Info.Graphs.Num(), TotalNodes, NumVars, NumComps));

		return BP;
	});
}

REGISTER_LUA_BINDING(Blueprint, BlueprintDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindBlueprint(Lua, Session);
});
