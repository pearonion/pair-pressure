#include "Lua/LuaBindingRegistry.h"
#include "NeoStackAIModule.h"
#include "Extensions/NeoStackExtensionRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectIterator.h"

FLuaBindingRegistry& FLuaBindingRegistry::Get()
{
	static FLuaBindingRegistry Instance;
	return Instance;
}

void FLuaBindingRegistry::Register(const FString& Name, TArray<FLuaFunctionDoc> Functions, FLuaBindingFunc BindFunc)
{
	RegisterOwned(TEXT("neostack.core"), Name, MoveTemp(Functions), MoveTemp(BindFunc));
}

FNeoStackExtensionHandle FLuaBindingRegistry::RegisterOwned(
	const FString& OwnerExtensionId,
	const FString& Name,
	TArray<FLuaFunctionDoc> Functions,
	FLuaBindingFunc BindFunc)
{
	FLuaBinding Binding;
	Binding.Name = Name;
	Binding.Functions = MoveTemp(Functions);
	Binding.Bind = MoveTemp(BindFunc);
	Binding.OwnerExtensionId = OwnerExtensionId.IsEmpty() ? TEXT("neostack.core") : OwnerExtensionId;
	Binding.RegistrationId = FGuid::NewGuid();
	Bindings.Add(MoveTemp(Binding));

	FNeoStackExtensionHandle Handle;
	Handle.RegistrationId = Bindings.Last().RegistrationId;
	Handle.Kind = TEXT("lua_binding");
	Handle.OwnerExtensionId = Bindings.Last().OwnerExtensionId;
	return Handle;
}

bool FLuaBindingRegistry::Unregister(const FGuid& RegistrationId)
{
	for (int32 Index = Bindings.Num() - 1; Index >= 0; --Index)
	{
		if (Bindings[Index].RegistrationId == RegistrationId)
		{
			Bindings.RemoveAt(Index);
			return true;
		}
	}

	return false;
}

int32 FLuaBindingRegistry::UnregisterAllForOwner(const FString& OwnerExtensionId)
{
	int32 Removed = 0;
	for (int32 Index = Bindings.Num() - 1; Index >= 0; --Index)
	{
		if (Bindings[Index].OwnerExtensionId.Equals(OwnerExtensionId, ESearchCase::CaseSensitive))
		{
			Bindings.RemoveAt(Index);
			++Removed;
		}
	}

	return Removed;
}

FString FLuaBindingRegistry::BuildDescription() const
{
	// Core bindings — always included in prompt
	static const TSet<FString> CoreBindings = {
		TEXT("OpenAsset"), TEXT("CreateBlueprint"), TEXT("Explore"),
		TEXT("AddNode"), TEXT("DeleteNode"), TEXT("Connect"), TEXT("SetPin"),
		TEXT("FindNodes"), TEXT("ReadGraph"), TEXT("GraphOps"), TEXT("ControlRigGraphOps"),
		TEXT("AddVariable"), TEXT("ReadFile"), TEXT("ReadLog"), TEXT("Profile"),
		TEXT("ExecutePython"),
		// report_issue() is the agent's escape hatch when something is broken
		// or missing — agents need to discover it without first calling help().
		TEXT("ReportIssue")
	};

	FString Desc = TEXT("Execute a Lua script that interacts with Unreal Editor.\n\nIMPORTANT: Before attempting any operation, call help() or help('domain') to discover the correct function names and parameter syntax.\n\nAvailable functions:\n");
	TArray<FString> DiscoverableDomains;

	for (const FLuaBinding& Binding : Bindings)
	{
		if (Binding.Functions.Num() == 0)
		{
			continue; // Skip enrichments (empty docs)
		}

		if (CoreBindings.Contains(Binding.Name))
		{
			// Emit full docs for core bindings
			for (const FLuaFunctionDoc& Func : Binding.Functions)
			{
				Desc += FString::Printf(TEXT("\n  %s\n    %s\n"), *Func.Signature, *Func.Description);
				if (!Func.Returns.IsEmpty())
				{
					Desc += FString::Printf(TEXT("    Returns: %s\n"), *Func.Returns);
				}
			}
		}
		else
		{
			// Non-core with docs — collect for discoverable list
			DiscoverableDomains.Add(Binding.Name);
		}
	}

	Desc += TEXT("\n  help(domain?) — list functions for a domain, or all available domains\n    Returns: string\n");
	Desc += TEXT("\n  log(msg) — output to trace\n  print(...) — output to trace\n");

	if (DiscoverableDomains.Num() > 0)
	{
		DiscoverableDomains.Sort();
		Desc += TEXT("\nAdditional domains available (call help('domain_name') for functions):\n  ");
		Desc += FString::Join(DiscoverableDomains, TEXT(", "));
		Desc += TEXT("\n");
	}

	// Append extension inventory status so agents know what capabilities are active vs unavailable.
	const TArray<FNeoStackExtensionDescriptor> Extensions = FNeoStackExtensionRegistry::Get().GetAllExtensions();
	if (Extensions.Num() > 0)
	{
		TArray<FString> Active, Unavailable, Incompatible, Failed;
		for (const FNeoStackExtensionDescriptor& Extension : Extensions)
		{
			switch (Extension.State)
			{
			case ENeoStackExtensionState::Active:
				Active.Add(Extension.DisplayName);
				break;
			case ENeoStackExtensionState::Unavailable:
				Unavailable.Add(Extension.DisplayName);
				break;
			case ENeoStackExtensionState::Incompatible:
				Incompatible.Add(Extension.DisplayName);
				break;
			case ENeoStackExtensionState::Failed:
				Failed.Add(Extension.DisplayName);
				break;
			default:
				break;
			}
		}
		Active.Sort();
		Unavailable.Sort();
		Incompatible.Sort();
		Failed.Sort();

		if (Active.Num() > 0)
		{
			Desc += TEXT("\nActive extensions: ");
			Desc += FString::Join(Active, TEXT(", "));
			Desc += TEXT("\n");
		}
		if (Unavailable.Num() > 0)
		{
			Desc += TEXT("Unavailable extensions: ");
			Desc += FString::Join(Unavailable, TEXT(", "));
			Desc += TEXT("\n");
		}
		if (Incompatible.Num() > 0)
		{
			Desc += TEXT("Incompatible extensions: ");
			Desc += FString::Join(Incompatible, TEXT(", "));
			Desc += TEXT("\n");
		}
		if (Failed.Num() > 0)
		{
			Desc += TEXT("Failed extensions: ");
			Desc += FString::Join(Failed, TEXT(", "));
			Desc += TEXT("\n");
		}
	}

	return Desc;
}

void FLuaSessionData::RegisterGraphNodes(UEdGraph* Graph)
{
	if (!Graph || LoadedGraphs.Contains(Graph)) return;
	LoadedGraphs.Add(Graph);

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		FString Guid = Node->NodeGuid.ToString();
		if (!Nodes.Contains(Guid))
		{
			Nodes.Add(Guid, Node);
		}
	}
}

UEdGraphNode* FLuaSessionData::FindNode(const FString& Handle)
{
	// Fast path: check session-local map
	if (TWeakObjectPtr<UEdGraphNode>* Found = Nodes.Find(Handle))
	{
		if (Found->IsValid())
		{
			return Found->Get();
		}
		// Node was deleted externally (undo, user action, GC) — remove stale entry
		Nodes.Remove(Handle);
	}

	// Fallback: scan all loaded UEdGraph objects for a node with matching NodeGuid.
	// Prefer editable graphs registered by this session. Blueprint compilation creates
	// generated-node clones that can retain source NodeGuids in ExecuteUbergraph_* graphs;
	// those are not safe targets for graph editing or pin connection.
	FGuid TargetGuid;
	if (!FGuid::Parse(Handle, TargetGuid))
	{
		return nullptr;
	}

	for (UEdGraph* Graph : LoadedGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == TargetGuid)
			{
				Nodes.Add(Handle, Node);
				return Node;
			}
		}
	}

	for (TObjectIterator<UEdGraphNode> It; It; ++It)
	{
		UEdGraphNode* Node = *It;
		if (!Node || Node->NodeGuid != TargetGuid)
		{
			continue;
		}

		UEdGraph* Graph = Node->GetGraph();
		if (!Graph)
		{
			continue;
		}

		UBlueprint* OwnerBP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (!OwnerBP)
		{
			continue;
		}

		TArray<UEdGraph*> EditableGraphs;
		OwnerBP->GetAllGraphs(EditableGraphs);
		if (!EditableGraphs.Contains(Graph))
		{
			continue;
		}

		Nodes.Add(Handle, Node);
		return Node;
	}

	return nullptr;
}

void FLuaSessionData::MarkGraphDirty(UEdGraph* Graph)
{
	if (!Graph) return;
	// Walk outer chain to find the owning asset
	UObject* Outer = Graph->GetOuter();
	while (Outer)
	{
		if (Outer->IsAsset())
		{
			MarkGraphDirty(Graph, Outer);
			return;
		}
		Outer = Outer->GetOuter();
	}
	// Fallback: use direct outer
	MarkGraphDirty(Graph, Graph->GetOuter());
}
