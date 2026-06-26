#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Lua/LuaGraphResolver.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"

// Populate editor-only graph metadata (guid, subgraphs) when available
static void PopulateEditorOnlyGraphData(sol::state_view& LuaView, sol::table& Result, UEdGraph* Graph)
{
#if WITH_EDITORONLY_DATA
	Result["graph_guid"] = TCHAR_TO_UTF8(*Graph->GraphGuid.ToString());

	if (Graph->SubGraphs.Num() > 0)
	{
		sol::table SubGraphsTable = LuaView.create_table();
		int32 SubIdx = 1;
		for (UEdGraph* SubGraph : Graph->SubGraphs)
		{
			if (SubGraph)
			{
				SubGraphsTable[SubIdx++] = TCHAR_TO_UTF8(*SubGraph->GetName());
			}
		}
		Result["subgraphs"] = SubGraphsTable;
	}
#endif
}

// Look up the resolver-assigned friendly name for a graph (e.g. "MaterialGraph" not "MaterialGraph_0")
static FString GetResolverGraphName(UObject* Asset, UEdGraph* Graph)
{
	TArray<FResolvedGraphInfo> AllGraphs = LuaGraphResolver::GetGraphs(Asset);
	for (const FResolvedGraphInfo& G : AllGraphs)
	{
		if (G.Graph == Graph) return G.Name;
	}
	// Fallback to UObject name if not found in resolver
	return Graph->GetName();
}

static bool IsStateTreeAsset(UObject* Asset)
{
	return Asset && Asset->GetClass()->GetName() == TEXT("StateTree");
}

static TArray<FLuaFunctionDoc> ReadGraphDocs = {
	{
		TEXT("read_graph(asset_path, graph_name?, opts?)"),
		TEXT("Read nodes and visible-pin connections in any asset's graph.\n"
			"  Works on: Blueprint, Material, BehaviorTree, NiagaraSystem, NiagaraScript, PCG, ControlRig.\n"
			"  graph_name is optional for single-graph assets (Material, BehaviorTree, PCG).\n"
			"  opts.include_hidden=true includes hidden pins and connections; pins report is_hidden.\n"
			"  For Blueprints: use graph names like \"EventGraph\", \"MyFunction\", etc.\n"
			"  For NiagaraSystems: \"SystemSpawn\", \"EmitterName/Spawn\", \"EmitterName/ParticleUpdate\", etc.\n"
			"  Once read, all nodes are registered — use connect(), set_pin(), delete_node() on them.\n"
			"  Tip: call read_graph(path) with no graph_name to list available graphs."),
		TEXT("{graph_name, graph_guid, nodes = [{handle, name, type, tooltip, keywords, x, y, enabled_state, comment, error_type, pins_in, pins_out, connections, connections_in, connections_out}], connections=[...], subgraphs = [name]}")
	}
};

REGISTER_LUA_BINDING(ReadGraph, ReadGraphDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("read_graph", [&Session](const std::string& AssetPath, sol::variadic_args Args, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FAssetPath = NeoLuaStr::ToFString(AssetPath);
		FString FGraphName;
		bool bIncludeHiddenPins = false;
		for (int ArgIdx = 0; ArgIdx < Args.size(); ++ArgIdx)
		{
			sol::object Arg = Args[ArgIdx];
			if (!Arg.valid() || Arg.get_type() == sol::type::lua_nil)
			{
				continue;
			}

			if (Arg.is<std::string>())
			{
				FGraphName = NeoLuaStr::ToFString(Arg.as<std::string>());
			}
			else if (Arg.is<sol::table>())
			{
				sol::table Opts = Arg.as<sol::table>();
				bIncludeHiddenPins = Opts.get_or("include_hidden", false);
			}
			else
			{
				Session.Log(TEXT("[FAIL] read_graph -> expected graph name string and/or options table"));
				return sol::lua_nil;
			}
		}

		FAssetPath = NeoLuaAsset::NormalizePath(FAssetPath);

		// Load the asset
		UObject* Asset = NeoLuaAsset::Resolve<UObject>(FAssetPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\") -> asset not found"), *FAssetPath));
			return sol::lua_nil;
		}

		// Find the graph
		UEdGraph* Graph = LuaGraphResolver::FindGraph(Asset, FGraphName);
		if (!Graph)
		{
			if (IsStateTreeAsset(Asset))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\") -> StateTree assets do not expose a UEdGraph. Use open_asset(path):list(\"states\"), list(\"tasks\", {state=\"...\"}), list(\"transitions\", {state=\"...\"}), add(\"state\"|\"task\"|\"condition\"|\"transition\", ...), and compile()."),
					*FAssetPath));
				return sol::lua_nil;
			}

			FString Available = LuaGraphResolver::ListGraphNames(Asset);
			if (FGraphName.IsEmpty())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\") -> multiple graphs, specify a name. Available: %s"),
					*FAssetPath, *Available));
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] read_graph(\"%s\", \"%s\") -> graph not found. Available: %s"),
					*FAssetPath, *FGraphName, *Available));
			}
			return sol::lua_nil;
		}

		Session.RegisterGraphNodes(Graph);

		sol::table Result = LuaView.create_table();
		sol::table NodesTable = LuaView.create_table();
		sol::table GraphConnections = LuaView.create_table();
		int32 NodeIdx = 1;
		int32 GraphConnIdx = 1;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			FString NodeGuid = Node->NodeGuid.ToString();

			sol::table NodeEntry = LuaView.create_table();
			NodeEntry["handle"] = TCHAR_TO_UTF8(*NodeGuid);
			NodeEntry["name"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
			NodeEntry["type"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
			NodeEntry["class"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
			NodeEntry["x"] = Node->NodePosX;
			NodeEntry["y"] = Node->NodePosY;

			// Tooltip and keywords help agents understand unfamiliar node types
			FString Tooltip = Node->GetTooltipText().ToString();
			if (!Tooltip.IsEmpty())
			{
				NodeEntry["tooltip"] = TCHAR_TO_UTF8(*Tooltip);
			}
			FString Keywords = Node->GetKeywords().ToString();
			if (!Keywords.IsEmpty())
			{
				NodeEntry["keywords"] = TCHAR_TO_UTF8(*Keywords);
			}

			if (!Node->NodeComment.IsEmpty())
			{
				NodeEntry["comment"] = TCHAR_TO_UTF8(*Node->NodeComment);
				NodeEntry["comment_visible"] = (bool)Node->bCommentBubbleVisible;
			}

			switch (Node->GetDesiredEnabledState())
			{
			case ENodeEnabledState::Enabled: NodeEntry["enabled_state"] = "enabled"; break;
			case ENodeEnabledState::Disabled: NodeEntry["enabled_state"] = "disabled"; break;
			case ENodeEnabledState::DevelopmentOnly: NodeEntry["enabled_state"] = "development_only"; break;
			}

			if (Node->bHasCompilerMessage)
			{
				NodeEntry["error_type"] = Node->ErrorType;
			}

			sol::table InPins = LuaView.create_table();
			sol::table OutPins = LuaView.create_table();
			sol::table ConnectionsOut = LuaView.create_table();
			sol::table ConnectionsIn = LuaView.create_table();
			int32 InIdx = 1, OutIdx = 1, ConnOutIdx = 1, ConnInIdx = 1;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || (!bIncludeHiddenPins && Pin->bHidden)) continue;

				sol::table PinInfo = NeoLuaPin::BuildPinTable(LuaView, Pin, bIncludeHiddenPins);

				if (Pin->Direction == EGPD_Input)
					InPins[InIdx++] = PinInfo;
				else
					OutPins[OutIdx++] = PinInfo;

				if (Pin->Direction == EGPD_Input)
				{
					FString PinName = NeoLuaPin::GetUsablePinName(Pin);
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin || (!bIncludeHiddenPins && LinkedPin->bHidden)) continue;
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNodeUnchecked();
						if (!LinkedNode) continue;

						sol::table Conn = LuaView.create_table();
						Conn["from_node"] = TCHAR_TO_UTF8(*LinkedNode->NodeGuid.ToString());
						Conn["from_node_name"] = TCHAR_TO_UTF8(*LinkedNode->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
						Conn["from_pin"] = TCHAR_TO_UTF8(*NeoLuaPin::GetUsablePinName(LinkedPin));
						Conn["from_pin_id"] = TCHAR_TO_UTF8(*LinkedPin->PinId.ToString());
						Conn["to_node"] = TCHAR_TO_UTF8(*NodeGuid);
						Conn["to_node_name"] = TCHAR_TO_UTF8(*Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
						Conn["to_pin"] = TCHAR_TO_UTF8(*PinName);
						Conn["to_pin_id"] = TCHAR_TO_UTF8(*Pin->PinId.ToString());

						ConnectionsIn[ConnInIdx++] = Conn;
					}
				}

				// Only add top-level connections from output pins to avoid duplicates.
				if (Pin->Direction == EGPD_Output)
				{
					FString PinName = NeoLuaPin::GetUsablePinName(Pin);
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin || (!bIncludeHiddenPins && LinkedPin->bHidden)) continue;
						// Use GetOwningNodeUnchecked — GetOwningNode() has check() that crashes on null
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNodeUnchecked();
						if (!LinkedNode) continue;

						const FString FromNode = NodeGuid;
						const FString FromNodeName = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
						const FString FromPin = PinName;
						const FString FromPinId = Pin->PinId.ToString();
						const FString ToNode = LinkedNode->NodeGuid.ToString();
						const FString ToNodeName = LinkedNode->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
						const FString ToPin = NeoLuaPin::GetUsablePinName(LinkedPin);
						const FString ToPinId = LinkedPin->PinId.ToString();

						sol::table Conn = LuaView.create_table();
						Conn["from_node"] = TCHAR_TO_UTF8(*FromNode);
						Conn["from_node_name"] = TCHAR_TO_UTF8(*FromNodeName);
						Conn["from_pin"] = TCHAR_TO_UTF8(*FromPin);
						Conn["from_pin_id"] = TCHAR_TO_UTF8(*FromPinId);
						Conn["to_node"] = TCHAR_TO_UTF8(*ToNode);
						Conn["to_node_name"] = TCHAR_TO_UTF8(*ToNodeName);
						Conn["to_pin"] = TCHAR_TO_UTF8(*ToPin);
						Conn["to_pin_id"] = TCHAR_TO_UTF8(*ToPinId);

						ConnectionsOut[ConnOutIdx++] = Conn;

						sol::table GraphConn = LuaView.create_table();
						GraphConn["from_node"] = TCHAR_TO_UTF8(*FromNode);
						GraphConn["from_node_name"] = TCHAR_TO_UTF8(*FromNodeName);
						GraphConn["from_pin"] = TCHAR_TO_UTF8(*FromPin);
						GraphConn["from_pin_id"] = TCHAR_TO_UTF8(*FromPinId);
						GraphConn["to_node"] = TCHAR_TO_UTF8(*ToNode);
						GraphConn["to_node_name"] = TCHAR_TO_UTF8(*ToNodeName);
						GraphConn["to_pin"] = TCHAR_TO_UTF8(*ToPin);
						GraphConn["to_pin_id"] = TCHAR_TO_UTF8(*ToPinId);
						GraphConnections[GraphConnIdx++] = GraphConn;
					}
				}
			}

			NodeEntry["pins_in"] = InPins;
			NodeEntry["pins_out"] = OutPins;
			NodeEntry["connections"] = ConnectionsOut;
			NodeEntry["connections_out"] = ConnectionsOut;
			NodeEntry["connections_in"] = ConnectionsIn;

			NodesTable[NodeIdx++] = NodeEntry;
		}

		Result["nodes"] = NodesTable;
		Result["connections"] = GraphConnections;
		Result["connection_count"] = GraphConnIdx - 1;
		Result["include_hidden"] = bIncludeHiddenPins;
		FString FriendlyName = GetResolverGraphName(Asset, Graph);
		Result["graph_name"] = TCHAR_TO_UTF8(*FriendlyName);

		PopulateEditorOnlyGraphData(LuaView, Result, Graph);

		Session.Log(FString::Printf(TEXT("[OK] read_graph(\"%s\", \"%s\") -> %d nodes"),
			*FAssetPath, *FriendlyName, Graph->Nodes.Num()));

		return Result;
	});
});
