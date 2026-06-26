#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphCapabilityRegistry.h"
#include "Lua/LuaStr.h"
#include "Modules/ModuleManager.h"
#if WITH_CONTROLRIG
#include "Lua/LuaControlRigHelper.h"
#endif
#include "Blueprint/BlueprintUtils.h"
#include "K2Node_Event.h"
#include "MaterialGraph/MaterialGraph.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Helper function extracted outside REGISTER_LUA_BINDING to avoid #if inside macro arguments (MSVC C5101)
#if WITH_CONTROLRIG
// Returns true if handled (success or failure), false if not a RigVM node
static bool DeleteNode_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle,
	const FString& NodeName, sol::this_state S, sol::object& OutResult)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ControlRig"))) return false;
	if (!LuaControlRig::IsRigVMNode(Node)) return false;

	UEdGraph* Graph = Node->GetGraph();
	FString Error;
	if (!LuaControlRig::DeleteNode(Graph, Node, Error))
	{
		Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> %s"), *FHandle, *Error));
		OutResult = sol::object(sol::lua_nil);
		return true;
	}
	Session.Nodes.Remove(FHandle);
	Session.Log(FString::Printf(TEXT("[OK] delete_node(\"%s\") -> removed \"%s\" via RigVM controller"), *FHandle, *NodeName));
	OutResult = sol::make_object(S, true);
	return true;
}
#else
static bool DeleteNode_HandleControlRig(FLuaSessionData& Session, UEdGraphNode* Node, const FString& FHandle,
	const FString& NodeName, sol::this_state S, sol::object& OutResult)
{
	// Runtime RigVM detection without ControlRig headers
	if (!Node || !Node->GetClass()->GetName().Contains(TEXT("RigVM"))) return false;

	// Without ControlRig module linkage, we cannot use URigVMController for proper deletion.
	// Return an error rather than crash with K2 deletion on a RigVM node.
	Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" is a ControlRig/RigVM node. "
		"Ensure the NSAI_ControlRig extension module is loaded."), *FHandle, *NodeName));
	OutResult = sol::object(sol::lua_nil);
	return true;
}
#endif

static TArray<FLuaFunctionDoc> DeleteNodeDocs = {
	{
		TEXT("delete_node(handle_or_node)"),
		TEXT("Remove a node from its graph. Accepts a raw handle string or an add_node/read_graph node table. Refuses undeletable nodes (entry/result/tunnel). Also available as graph:delete_node(handle)."),
		TEXT("true on success, nil on failure")
	}
};

REGISTER_LUA_BINDING(DeleteNode, DeleteNodeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("delete_node", [&Session](sol::object HandleArg, sol::this_state S) -> sol::object
	{
		if (!HandleArg.valid() || HandleArg.is<sol::lua_nil_t>())
		{
			Session.Log(TEXT("[FAIL] delete_node -> handle required"));
			return sol::lua_nil;
		}

		std::string Handle;
		if (HandleArg.is<std::string>())
		{
			Handle = HandleArg.as<std::string>();
		}
		else if (HandleArg.is<sol::table>())
		{
			sol::table NodeTable = HandleArg.as<sol::table>();
			Handle = NodeTable.get_or<std::string>("handle", "");
		}
		else
		{
			Session.Log(TEXT("[FAIL] delete_node -> expected handle string or node table"));
			return sol::lua_nil;
		}

		if (Handle.empty())
		{
			Session.Log(TEXT("[FAIL] delete_node -> node table missing 'handle'"));
			return sol::lua_nil;
		}

		FString FHandle = NeoLuaStr::ToFString(Handle);

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node -> node \"%s\" not found"), *FHandle));
			return sol::lua_nil;
		}

		FString NodeName = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
		UEdGraph* Graph = Node->GetGraph(); // Cache before any deletion (Node is destroyed after)

		// Guard against orphaned nodes (e.g. in /Engine/Transient with no blueprint owner).
		// These arise from cross-graph corruption or stale state. Calling BreakNodeLinks or
		// FindBlueprintForNodeChecked on them triggers a fatal assert crash loop.
		if (!Graph)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" has no owning graph (orphaned node)"), *FHandle, *NodeName));
			Session.Nodes.Remove(FHandle); // Clean up stale handle so subsequent calls don't hit the same node
			return sol::lua_nil;
		}
		// Reject truly orphaned nodes in the transient package, but allow Material Editor
		// preview graphs — those legitimately live in the transient package (the Material Editor
		// works on a transient duplicate of the original material).
		if (Graph->GetOutermost() == GetTransientPackage())
		{
			bool bIsMaterialEditorGraph = Graph->IsA<UMaterialGraph>();
			if (!bIsMaterialEditorGraph)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" is in /Engine/Transient (orphaned). Removing stale handle."), *FHandle, *NodeName));
				Session.Nodes.Remove(FHandle);
				return sol::lua_nil;
			}
		}

		// Guard against undeletable nodes (function entry/result, tunnel endpoints, etc.)
		if (!Node->CanUserDeleteNode())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" cannot be deleted (entry/result/tunnel node)"), *FHandle, *NodeName));
			return sol::lua_nil;
		}

		// Guard against override event nodes (BeginPlay, Tick, etc.) — these are technically
		// deletable by the engine but should be protected from agent deletion to prevent
		// accidentally breaking blueprints
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			if (EventNode->bOverrideFunction)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> \"%s\" is an override event node and cannot be deleted. Use override_function() to manage overrides."), *FHandle, *NodeName));
				return sol::lua_nil;
			}
		}

		// ControlRig/RigVM: use URigVMController::RemoveNode
		{
			sol::object CRResult;
			if (DeleteNode_HandleControlRig(Session, Node, FHandle, NodeName, S, CRResult))
			{
				// Mark dirty only after successful deletion
				if (CRResult.is<bool>() && CRResult.as<bool>())
					Session.MarkGraphDirty(Graph);
				return CRResult;
			}
		}

		// Extension-owned graphs (e.g. PCG, Dataflow): dispatch via the registered
		// capability bridge so domain-specific structural cleanup runs. For PCG
		// this is the only path that removes the underlying UPCGNode from UPCGGraph;
		// the generic BreakNodeLinks+DestroyNode fallback only cleans the editor node.
		if (const FLuaGraphCapability* Capability = FLuaGraphCapabilityRegistry::Get().FindOwner(Graph))
		{
			const FString& BridgeName = Capability->Bridges.DeleteNodeFunctionName;
			if (!BridgeName.IsEmpty())
			{
				sol::state_view LuaView(S);
				sol::protected_function BridgeFn = LuaView[TCHAR_TO_UTF8(*BridgeName)];
				if (!BridgeFn.valid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") on %s graph -> bridge \"%s\" not available"),
						*FHandle, *Capability->Name, *BridgeName));
					return sol::lua_nil;
				}

				sol::protected_function_result BridgeResult = BridgeFn(TCHAR_TO_UTF8(*FHandle));
				if (!BridgeResult.valid())
				{
					sol::error Err = BridgeResult;
					Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") on %s graph -> %s"),
						*FHandle, *Capability->Name, UTF8_TO_TCHAR(Err.what())));
					return sol::lua_nil;
				}

				sol::object Ret = BridgeResult.get<sol::object>();
				if (Ret.get_type() == sol::type::lua_nil)
				{
					return sol::lua_nil;
				}

				Session.MarkGraphDirty(Graph);
				Session.Nodes.Remove(FHandle);
				Session.Log(FString::Printf(TEXT("[OK] delete_node(\"%s\") -> removed \"%s\" via %s bridge"), *FHandle, *NodeName, *Capability->Name));
				return sol::make_object(S, true);
			}
		}

		if (!NeoBlueprint::DeleteNode(Node))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] delete_node(\"%s\") -> removal of \"%s\" failed"), *FHandle, *NodeName));
			return sol::lua_nil;
		}

		// Mark dirty only after successful deletion (Node is destroyed, use cached Graph)
		Session.MarkGraphDirty(Graph);
		Session.Nodes.Remove(FHandle);
		Session.Log(FString::Printf(TEXT("[OK] delete_node(\"%s\") -> removed \"%s\""), *FHandle, *NodeName));
		return sol::make_object(S, true);
	});
});
