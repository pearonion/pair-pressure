#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphCapabilityRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Lua/LuaStr.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "MaterialEditorUtilities.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Pin finder + listing moved to NeoLuaPin::Find / NeoLuaPin::ListAvailable (LuaPinHelper.h).

static const FLuaGraphCapability* Connect_FindCapability(UEdGraph* Graph)
{
	return FLuaGraphCapabilityRegistry::Get().FindOwner(Graph);
}

template <typename... ArgTypes>
static bool Connect_InvokeBridge(FLuaSessionData& Session, const FLuaGraphCapability* Capability,
	const FString& FunctionName, const TCHAR* OperationName, sol::this_state S, sol::object& OutResult, ArgTypes&&... Args)
{
	if (!Capability || FunctionName.IsEmpty())
	{
		return false;
	}

	sol::state_view LuaView(S);
	sol::protected_function Function = LuaView[TCHAR_TO_UTF8(*FunctionName)];
	if (!Function.valid())
	{
		Session.Log(FString::Printf(TEXT("[FAIL] %s on %s graph -> bridge \"%s\" is not available"),
			OperationName,
			*Capability->Name,
			*FunctionName));
		OutResult = sol::object(sol::lua_nil);
		return true;
	}

	sol::protected_function_result Result = Function(Forward<ArgTypes>(Args)...);
	if (!Result.valid())
	{
		sol::error Error = Result;
		Session.Log(FString::Printf(TEXT("[FAIL] %s on %s graph -> %s"),
			OperationName,
			*Capability->Name,
			UTF8_TO_TCHAR(Error.what())));
		OutResult = sol::object(sol::lua_nil);
		return true;
	}

	OutResult = Result.get<sol::object>();
	return true;
}

static TArray<FLuaFunctionDoc> ConnectDocs = {
	{
		TEXT("connect(from_handle, from_pin, to_handle, to_pin)"),
		TEXT("Connect two node pins. Uses node handles from add_node. Pin names are case-insensitive. The engine will auto-create conversion nodes if needed."),
		TEXT("true on success, nil on failure")
	},
	{
		TEXT("disconnect(handle, pin_name)"),
		TEXT("Break all connections on a pin. For specific disconnect use disconnect_from(from_handle, from_pin, to_handle, to_pin)."),
		TEXT("true on success, nil on failure")
	},
	{
		TEXT("disconnect_all(handle)"),
		TEXT("Break all connections on all pins of a node at once."),
		TEXT("true on success, nil on failure")
	}
};

REGISTER_LUA_BINDING(Connect, ConnectDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("connect", [&Session](sol::object FromHandleObj, sol::object FromPinObj,
		sol::object ToHandleObj, sol::object ToPinObj, sol::this_state S) -> sol::object
	{
		if (!FromHandleObj.is<std::string>() || !FromPinObj.is<std::string>() ||
			!ToHandleObj.is<std::string>() || !ToPinObj.is<std::string>())
		{
			Session.Log(TEXT("[FAIL] connect(from_node, from_pin, to_node, to_pin) -> all 4 arguments must be strings (got nil — check that add_node/find_nodes returned valid handles)"));
			return sol::lua_nil;
		}
		FString FFromHandle = NeoLuaStr::ToFString(FromHandleObj.as<std::string>());
		FString FToHandle = NeoLuaStr::ToFString(ToHandleObj.as<std::string>());
		FString FFromPin = NeoLuaStr::ToFString(FromPinObj.as<std::string>());
		FString FToPin = NeoLuaStr::ToFString(ToPinObj.as<std::string>());

		UEdGraphNode* FromNode = Session.FindNode(FFromHandle);
		UEdGraphNode* ToNode = Session.FindNode(FToHandle);

		if (!FromNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> source node \"%s\" not found. Call read_graph() or add_node() first."), *FFromHandle));
			return sol::lua_nil;
		}
		if (!ToNode)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> target node \"%s\" not found. Call read_graph() or add_node() first."), *FToHandle));
			return sol::lua_nil;
		}

		// Validate both nodes belong to the same graph — cross-graph connections corrupt
		// blueprint state (pins get different outers, causing ensure failures and crash loops
		// during compile/save).
		UEdGraph* FromGraph = FromNode->GetGraph();
		UEdGraph* ToGraph = ToNode->GetGraph();
		if (!FromGraph || !ToGraph)
		{
			Session.Log(TEXT("[FAIL] connect -> one or both nodes have no owning graph (possibly orphaned in /Engine/Transient)"));
			return sol::lua_nil;
		}
		if (FromGraph != ToGraph)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect(%s -> %s) -> nodes belong to different graphs (\"%s\" vs \"%s\"). Cannot connect across graphs."),
				*FFromHandle, *FToHandle,
				*FromGraph->GetName(), *ToGraph->GetName()));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(FromGraph);

		{
			const FLuaGraphCapability* Capability = Connect_FindCapability(FromGraph);
			sol::object BridgeResult;
			if (Connect_InvokeBridge(Session, Capability,
				Capability ? Capability->Bridges.ConnectFunctionName : FString(),
				TEXT("connect"), S, BridgeResult,
				TCHAR_TO_UTF8(*FFromHandle), TCHAR_TO_UTF8(*FFromPin), TCHAR_TO_UTF8(*FToHandle), TCHAR_TO_UTF8(*FToPin)))
			{
				return BridgeResult;
			}
		}

		UEdGraphPin* OutPin = NeoLuaPin::Find(FromNode, FFromPin, EGPD_Output);
		if (!OutPin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> pin \"%s\" not found on source node. Available output pins: %s"),
				*FFromPin, *NeoLuaPin::ListAvailable(FromNode, EGPD_Output, /*bShowDirection=*/true, /*bShowType=*/false)));
			return sol::lua_nil;
		}

		UEdGraphPin* InPin = NeoLuaPin::Find(ToNode, FToPin, EGPD_Input);
		if (!InPin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect -> pin \"%s\" not found on target node. Available input pins: %s"),
				*FToPin, *NeoLuaPin::ListAvailable(ToNode, EGPD_Input, /*bShowDirection=*/true, /*bShowType=*/false)));
			return sol::lua_nil;
		}

		// FromGraph already validated above
		const UEdGraphSchema* Schema = FromGraph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] connect -> could not get graph schema"));
			return sol::lua_nil;
		}

		// Check compatibility first for a better error message
		FPinConnectionResponse Response = Schema->CanCreateConnection(OutPin, InPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] connect(%s:%s -> %s:%s) -> %s"),
				*FFromHandle, *FFromPin, *FToHandle, *FToPin, *Response.Message.ToString()));
			return sol::lua_nil;
		}

		// TryCreateConnection handles auto-conversion nodes and calls PinConnectionListChanged
		bool bSuccess = Schema->TryCreateConnection(OutPin, InPin);
		if (bSuccess)
		{
			// Reconstruct nodes with wildcard pins so types propagate correctly
			// (e.g. ForEachLoop's Array Element pin infers type from connected array)
			auto ReconstructIfHasWildcard = [](UEdGraphNode* Node)
			{
				if (!Node) return;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					{
						Node->ReconstructNode();
						return;
					}
				}
			};
			ReconstructIfHasWildcard(FromNode);
			ReconstructIfHasWildcard(ToNode);
			// After ReconstructNode, OutPin and InPin may be dangling — do NOT dereference
			// either below this point. Re-find via Node->FindPin(name) if new use is needed.

			// Material graphs need the full Apply sequence in-line. Schema->TryCreateConnection
			// only updates the EdGraph pin LinkedTo arrays. The shader compiler reads
			// FExpressionInput.Expression on the UMaterial / UMaterialExpression objects, and
			// those are populated by LinkMaterialExpressionsFromGraph. Without the rest of
			// the sequence, the cached FMaterialResource isn't invalidated, so a subsequent
			// compile() in the same or next script call returns the stale (disconnected)
			// shader and the material renders black even though read_graph confirms the
			// EdGraph link. This sequence mirrors UpdateOriginalMaterial in the Material
			// Editor's Apply path (MaterialEditor.cpp:2736-2744) and the LuaGraphFinalizer
			// material branch — the engine relies on all five calls firing together.
			//   Repro: create Material, add_node "Custom", set Code = "return float3(1,0,0);",
			//   connect Output -> EmissiveColor, apply to a static mesh, screenshot.
			//   Pre-fix: solid black. Post-fix: red.
			if (UMaterialGraph* MatGraph = Cast<UMaterialGraph>(FromGraph))
			{
				if (UMaterial* Mat = MatGraph->Material)
				{
					Mat->Modify();
					MatGraph->LinkMaterialExpressionsFromGraph();
					FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(MatGraph);
					MatGraph->NotifyGraphChanged();
					Mat->MarkPackageDirty();
					Mat->ForceRecompileForRendering();
				}
			}

			FString Method = (Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
				? TEXT("with conversion") : TEXT("direct");
			Session.Log(FString::Printf(TEXT("[OK] connect(%s:%s -> %s:%s) -> %s"),
				*FFromHandle, *FFromPin, *FToHandle, *FToPin, *Method));
			return sol::make_object(S, true);
		}

		Session.Log(FString::Printf(TEXT("[FAIL] connect(%s:%s -> %s:%s) -> connection failed"),
			*FFromHandle, *FFromPin, *FToHandle, *FToPin));
		return sol::lua_nil;
	});

	Lua.set_function("disconnect", [&Session](sol::object HandleObj, sol::object PinNameObj, sol::this_state S) -> sol::object
	{
		if (!HandleObj.is<std::string>() || !PinNameObj.is<std::string>())
		{
			Session.Log(TEXT("[FAIL] disconnect(node_handle, pin_name) -> both arguments must be strings (got nil)"));
			return sol::lua_nil;
		}
		FString FHandle = NeoLuaStr::ToFString(HandleObj.as<std::string>());
		FString FPinName = NeoLuaStr::ToFString(PinNameObj.as<std::string>());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] disconnect -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		UEdGraph* Graph = Node->GetGraph();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] disconnect -> node has no owning graph"));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(Graph);

		{
			const FLuaGraphCapability* Capability = Connect_FindCapability(Graph);
			sol::object BridgeResult;
			if (Connect_InvokeBridge(Session, Capability,
				Capability ? Capability->Bridges.DisconnectFunctionName : FString(),
				TEXT("disconnect"), S, BridgeResult,
				TCHAR_TO_UTF8(*FHandle), TCHAR_TO_UTF8(*FPinName)))
			{
				return BridgeResult;
			}
		}

		UEdGraphPin* Pin = NeoLuaPin::Find(Node, FPinName);
		if (!Pin)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] disconnect -> pin \"%s\" not found. Available: %s"),
				*FPinName, *NeoLuaPin::ListAvailable(Node, EGPD_MAX, /*bShowDirection=*/true, /*bShowType=*/false)));
			return sol::lua_nil;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] disconnect -> could not get graph schema"));
			return sol::lua_nil;
		}

		Schema->BreakPinLinks(*Pin, true);
		// Full Apply sequence — see comment in connect() for why all five calls are needed.
		// Without them the EdGraph link is gone but FExpressionInput.Expression on the
		// UMaterial still references the source expression, AND the shader cache isn't
		// invalidated, so disconnect renders identical to before-disconnect.
		if (UMaterialGraph* MatGraph = Cast<UMaterialGraph>(Graph))
		{
			if (UMaterial* Mat = MatGraph->Material)
			{
				Mat->Modify();
				MatGraph->LinkMaterialExpressionsFromGraph();
				FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(MatGraph);
				MatGraph->NotifyGraphChanged();
				Mat->MarkPackageDirty();
				Mat->ForceRecompileForRendering();
			}
		}
		Session.Log(FString::Printf(TEXT("[OK] disconnect(%s:%s) -> all connections broken"), *FHandle, *FPinName));
		return sol::make_object(S, true);
	});

	Lua.set_function("disconnect_all", [&Session](sol::object HandleObj, sol::this_state S) -> sol::object
	{
		if (!HandleObj.is<std::string>())
		{
			Session.Log(TEXT("[FAIL] disconnect_all(node_handle) -> argument must be a string (got nil)"));
			return sol::lua_nil;
		}
		FString FHandle = NeoLuaStr::ToFString(HandleObj.as<std::string>());

		UEdGraphNode* Node = Session.FindNode(FHandle);
		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] disconnect_all -> node \"%s\" not found. Call read_graph() or add_node() first."), *FHandle));
			return sol::lua_nil;
		}

		UEdGraph* Graph = Node->GetGraph();
		if (!Graph)
		{
			Session.Log(TEXT("[FAIL] disconnect_all -> node has no owning graph"));
			return sol::lua_nil;
		}

		Session.MarkGraphDirty(Graph);

		{
			const FLuaGraphCapability* Capability = Connect_FindCapability(Graph);
			sol::object BridgeResult;
			if (Connect_InvokeBridge(Session, Capability,
				Capability ? Capability->Bridges.DisconnectAllFunctionName : FString(),
				TEXT("disconnect_all"), S, BridgeResult,
				TCHAR_TO_UTF8(*FHandle)))
			{
				return BridgeResult;
			}
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			Session.Log(TEXT("[FAIL] disconnect_all -> could not get graph schema"));
			return sol::lua_nil;
		}

		// BreakNodeLinks breaks all pins and sends NodeConnectionListChanged to all affected nodes
		Schema->BreakNodeLinks(*Node);
		// Full Apply sequence — see comment in connect() for why all five calls are needed.
		if (UMaterialGraph* MatGraph = Cast<UMaterialGraph>(Graph))
		{
			if (UMaterial* Mat = MatGraph->Material)
			{
				Mat->Modify();
				MatGraph->LinkMaterialExpressionsFromGraph();
				FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(MatGraph);
				MatGraph->NotifyGraphChanged();
				Mat->MarkPackageDirty();
				Mat->ForceRecompileForRendering();
			}
		}
		Session.Log(FString::Printf(TEXT("[OK] disconnect_all(%s) -> all connections broken"), *FHandle));
		return sol::make_object(S, true);
	});
});
