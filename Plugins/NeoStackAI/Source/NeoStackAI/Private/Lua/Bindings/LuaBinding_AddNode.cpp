#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphCapabilityRegistry.h"
#include "Lua/LuaPinHelper.h"
#include "Lua/LuaGraphResolver.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Blueprint/NodeUtils.h"
#include "Blueprint/BlueprintUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"

static const FLuaGraphCapability* FindAddNodeGraphCapability(UEdGraph* Graph);
static bool RequiresGraphCapabilityAddNode(UEdGraph* Graph);

// Common shorthand aliases for material expression nodes.
// Maps user-friendly short names to their actual UE expression names.
static const TMap<FString, FString> MaterialNodeAliases = {
	{ TEXT("lerp"), TEXT("LinearInterpolate") },
	{ TEXT("objectposition"), TEXT("ObjectPositionWS") },
	{ TEXT("actorposition"), TEXT("ActorPositionWS") },
};

static bool IsStateTreeAsset(UObject* Asset)
{
	return Asset && Asset->GetClass()->GetName() == TEXT("StateTree");
}

// Spawn a node on a non-Blueprint graph using schema context actions.
// This is the same mechanism the engine editor uses when user right-clicks and selects a node.
// FromPin is optional — if provided, PerformAction auto-connects the new node to this pin.
static UEdGraphNode* SpawnNodeViaSchema(UEdGraph* Graph, const FString& NodeName, double X, double Y, FString& OutError, UEdGraphPin* FromPin = nullptr)
{
	if (!Graph)
	{
		OutError = TEXT("null graph");
		return nullptr;
	}

	// RigVM/ControlRig graphs crash in PerformAction due to check(NewNode) in
	// RigVMEdGraphUnitNodeSpawner/FunctionRefNodeSpawner. Block early.
	if (RequiresGraphCapabilityAddNode(Graph))
	{
		OutError = TEXT("graph requires an extension-owned add_node bridge");
		return nullptr;
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		OutError = TEXT("graph has no schema");
		return nullptr;
	}

	// Some schemas need the editor open for class discovery
	NeoNodes::EnsureEditorOpenForSchema(Graph);

	// Build the context menu to discover all available actions
	FGraphContextMenuBuilder ContextMenuBuilder(Graph);
	Schema->GetGraphContextActions(ContextMenuBuilder);

	// Check if the user-supplied name has a common alias (e.g. "Lerp" -> "LinearInterpolate")
	FString ResolvedName = NodeName;
	if (const FString* Alias = MaterialNodeAliases.Find(NodeName.ToLower()))
	{
		ResolvedName = *Alias;
	}
	FString NodeNameLower = ResolvedName.ToLower();

	// Find the best matching action
	TSharedPtr<FEdGraphSchemaAction> BestAction;
	int32 BestScore = 0;
	int32 AmbiguousCount = 0;
	FString AmbiguousOptions;

	const int32 NumActions = ContextMenuBuilder.GetNumActions();
	for (int32 i = 0; i < NumActions; i++)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		TSharedPtr<FEdGraphSchemaAction> Action = ContextMenuBuilder.GetSchemaAction(i);
#else
		FGraphActionListBuilderBase::ActionGroup& Group = ContextMenuBuilder.GetAction(i);
		TSharedPtr<FEdGraphSchemaAction> Action = Group.Actions.Num() > 0 ? Group.Actions[0] : nullptr;
#endif
		if (!Action.IsValid()) continue;

		FString Name = Action->GetMenuDescription().ToString();
		FString NameLower = Name.ToLower();

		int32 Score = 0;
		if (NameLower == NodeNameLower) Score = 100;
		else if (NameLower.StartsWith(NodeNameLower)) Score = 80;
		else if (NameLower.Contains(NodeNameLower)) Score = 60;
		else continue;

		if (Score > BestScore)
		{
			BestScore = Score;
			BestAction = Action;
			AmbiguousCount = 1;
			AmbiguousOptions = FString::Printf(TEXT("  [1] \"%s\" (%s)"),
				*Name, *Action->GetCategory().ToString());
		}
		else if (Score == BestScore)
		{
			AmbiguousCount++;
			AmbiguousOptions += FString::Printf(TEXT("\n  [%d] \"%s\" (%s)"),
				AmbiguousCount, *Name, *Action->GetCategory().ToString());
		}
	}

	if (!BestAction.IsValid())
	{
		OutError = FString::Printf(TEXT("no node type matching \"%s\" found in this graph"), *NodeName);
		return nullptr;
	}

	if (AmbiguousCount > 1)
	{
		if (BestScore < 100)
		{
			OutError = FString::Printf(TEXT("ambiguous, %d nodes match \"%s\" equally. Use the exact name:\n%s"),
				AmbiguousCount, *NodeName, *AmbiguousOptions);
			return nullptr;
		}
		// Multiple exact matches — use the first but warn
		OutError = FString::Printf(TEXT("[WARN] %d nodes have the exact name \"%s\", using first match. Alternatives:\n%s"),
			AmbiguousCount, *NodeName, *AmbiguousOptions);
		// Don't return nullptr — continue with BestAction, OutError is used as warning below
	}

	// Spawn the node via PerformAction — same as the engine editor does
	const FScopedTransaction Transaction(NSLOCTEXT("NeoStackAI", "AddNode", "Add Node"));
	Graph->Modify();
	if (FromPin) FromPin->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	UEdGraphNode* NewNode = BestAction->PerformAction(Graph, FromPin, FVector2f(static_cast<float>(X), static_cast<float>(Y)), true);
#else
	UEdGraphNode* NewNode = BestAction->PerformAction(Graph, FromPin, FVector2D(X, Y), true);
#endif
	if (!NewNode)
	{
		OutError = TEXT("PerformAction returned null");
		return nullptr;
	}

	return NewNode;
}

// Helper to call PerformAction with the correct vector type per engine version
// (cannot use #if inside macro arguments — undefined behavior)
static UEdGraphNode* PerformSchemaAction(FEdGraphSchemaAction* Action, UEdGraph* Graph, double X, double Y, UEdGraphPin* FromPin = nullptr)
{
	if (RequiresGraphCapabilityAddNode(Graph))
	{
		return nullptr;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Action->PerformAction(Graph, FromPin, FVector2f(static_cast<float>(X), static_cast<float>(Y)), true);
#else
	return Action->PerformAction(Graph, FromPin, FVector2D(X, Y), true);
#endif
}

static TArray<FLuaFunctionDoc> AddNodeDocs = {
	{
		TEXT("add_node(asset_path, graph_name?, node, x?, y?)"),
		TEXT("Add a node to any asset's graph.\n"
			"  For Blueprint graphs: 'node' can be a find_nodes result table (with _spawner_id) or a name string.\n"
			"  For schema-driven graphs: 'node' is a name string matched against schema actions.\n"
			"  Extension-owned graphs may provide custom add_node behavior.\n"
			"  graph_name is optional (nil) for single-graph assets (Material, BehaviorTree, PCG, SoundCue, EQS).\n"
			"  Auto-connect: if 'node' is a table, set from_handle and from_pin to auto-connect on spawn.\n"
			"    e.g. add_node(path, {name=\"Print String\", from_handle=\"abc\", from_pin=\"then\"})\n"
			"  Also available as graph:add_node(node, x?, y?) on blueprint graph objects."),
		TEXT("{handle, name, pins_in, pins_out} or nil")
	}
};

static const FLuaGraphCapability* FindAddNodeGraphCapability(UEdGraph* Graph)
{
	const FLuaGraphCapability* Capability = FLuaGraphCapabilityRegistry::Get().FindOwner(Graph);
	if (!Capability || Capability->Bridges.AddNodeFunctionName.IsEmpty())
	{
		return nullptr;
	}

	return Capability;
}

static bool RequiresGraphCapabilityAddNode(UEdGraph* Graph)
{
	return FindAddNodeGraphCapability(Graph) != nullptr;
}

static sol::object AddNode_HandleGraphCapability(FLuaSessionData& Session, UEdGraph* Graph, sol::object& NodeArg,
	double PosX, double PosY, sol::this_state S)
{
	sol::state_view LuaView(S);
	const FLuaGraphCapability* Capability = FindAddNodeGraphCapability(Graph);
	if (!Capability)
	{
		Session.Log(TEXT("[FAIL] add_node -> graph capability bridge not registered"));
		return sol::lua_nil;
	}

	sol::protected_function AddNodeFn = LuaView[TCHAR_TO_UTF8(*Capability->Bridges.AddNodeFunctionName)];
	if (!AddNodeFn.valid())
	{
		Session.Log(FString::Printf(TEXT("[FAIL] add_node on %s graph -> bridge \"%s\" is not available"),
			*Capability->Name,
			*Capability->Bridges.AddNodeFunctionName));
		return sol::lua_nil;
	}

	sol::protected_function_result Result = AddNodeFn(sol::lightuserdata_value(Graph), NodeArg, PosX, PosY);
	if (!Result.valid())
	{
		sol::error Error = Result;
		Session.Log(FString::Printf(TEXT("[FAIL] add_node on %s graph -> %s"),
			*Capability->Name,
			UTF8_TO_TCHAR(Error.what())));
		return sol::lua_nil;
	}

	return Result.get<sol::object>();
}

REGISTER_LUA_BINDING(AddNode, AddNodeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	// Signature: add_node(asset_path, graph_name_or_node, node_or_x?, x_or_y?, y?)
	// graph_name can be nil for single-graph assets, so we detect the overload:
	//   add_node(path, nil, node, x?, y?)   — nil graph name
	//   add_node(path, "name", node, x?, y?) — explicit graph name
	//   add_node(path, node, x?, y?)         — omitted graph name (node is 2nd arg)
	Lua.set_function("add_node", [&Session](const std::string& AssetPath, sol::object Arg2,
		sol::object Arg3, sol::optional<double> Arg4, sol::optional<double> Arg5,
		sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);

		FString FAssetPath = NeoLuaStr::ToFString(AssetPath);

		// Detect overload: is Arg2 a graph name string, nil, or the node arg?
		FString FGraphName;
		sol::object NodeArg;
		double PosX, PosY;

		if (Arg2.is<std::string>() && (Arg3.is<std::string>() || Arg3.is<sol::table>()))
		{
			// add_node(path, "graph_name", node, x?, y?)
			FGraphName = NeoLuaStr::ToFString(Arg2.as<std::string>());
			NodeArg = Arg3;
			PosX = Arg4.value_or(0.0);
			PosY = Arg5.value_or(0.0);
		}
		else if (Arg2.get_type() == sol::type::lua_nil && (Arg3.is<std::string>() || Arg3.is<sol::table>()))
		{
			// add_node(path, nil, node, x?, y?)
			FGraphName = TEXT("");
			NodeArg = Arg3;
			PosX = Arg4.value_or(0.0);
			PosY = Arg5.value_or(0.0);
		}
		else
		{
			// add_node(path, node, x?, y?) — graph_name omitted
			// node can be a string name, a table from find_nodes(), or nil
			FGraphName = TEXT("");
			NodeArg = Arg2;
			PosX = Arg3.is<double>() ? Arg3.as<double>() : 0.0;
			PosY = Arg4.value_or(0.0);
		}

		FAssetPath = NeoLuaAsset::NormalizePath(FAssetPath);

		// Load asset
		UObject* Asset = NeoLuaAsset::Resolve<UObject>(FAssetPath);
		if (!Asset)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_node -> asset \"%s\" not found"), *FAssetPath));
			return sol::lua_nil;
		}

		// Check if this is a Blueprint
		UBlueprint* BP = Cast<UBlueprint>(Asset);

		// Find the graph
		UEdGraph* Graph = nullptr;
		if (BP)
		{
			FBlueprintInfo Info = NeoBlueprint::LoadBlueprint(FAssetPath);
			if (Info.Blueprint)
			{
				Graph = NeoBlueprint::FindGraph(Info.Blueprint, FGraphName);
			}
			if (!Graph)
			{
				Graph = LuaGraphResolver::FindGraph(Asset, FGraphName);
			}
		}
		else
		{
			Graph = LuaGraphResolver::FindGraph(Asset, FGraphName);
		}

		if (!Graph)
		{
			if (IsStateTreeAsset(Asset))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_node -> StateTree asset \"%s\" is not a UEdGraph asset. Use open_asset(path):add(\"state\", ...), add(\"task\", {state=\"...\", type=\"...\"}), add(\"condition\", ...), add(\"transition\", ...), then compile()."),
					*FAssetPath));
				return sol::lua_nil;
			}

			FString Available = BP
				? TEXT("(check graph name)")
				: LuaGraphResolver::ListGraphNames(Asset);
			Session.Log(FString::Printf(TEXT("[FAIL] add_node -> graph \"%s\" not found. Available: %s"),
				*FGraphName, *Available));
			return sol::lua_nil;
		}

		Session.RegisterGraphNodes(Graph);
		Session.MarkGraphDirty(Graph, Asset);

		if (RequiresGraphCapabilityAddNode(Graph))
		{
			return AddNode_HandleGraphCapability(Session, Graph, NodeArg, PosX, PosY, S);
		}

		// ---- Standard path (Blueprint / Schema) ----
		UEdGraphNode* Node = nullptr;
		FString DisplayName;

		if (NodeArg.is<sol::table>())
		{
			// Table from find_nodes — try Blueprint spawner first, then schema action
			sol::table NodeTable = NodeArg.as<sol::table>();
			std::string SpawnerIdStr = NodeTable.get_or<std::string>("_spawner_id", "");
			std::string ActionIdStr = NodeTable.get_or<std::string>("_action_id", "");
			DisplayName = NeoLuaStr::ToFString(NodeTable.get_or<std::string>("name", "unknown"));

			// Auto-connect / wildcard-promotion source pin. Wildcard K2 nodes
			// (Add|Math, Multiply, Equal(byte) — UK2Node_PromotableOperator) need
			// this to resolve their pin types; without it they spawn as Wildcard
			// and the BP compiler rejects them with "spawn failed".
			UEdGraphPin* FromPin = nullptr;
			{
				std::string FromHandleStr = NodeTable.get_or<std::string>("from_handle", "");
				std::string FromPinStr = NodeTable.get_or<std::string>("from_pin", "");
				if (!FromHandleStr.empty() && !FromPinStr.empty())
				{
					FString FFromHandle = NeoLuaStr::ToFString(FromHandleStr);
					FString FFromPin = NeoLuaStr::ToFString(FromPinStr);
					if (UEdGraphNode* FromNode = Session.FindNode(FFromHandle))
					{
						FromPin = NeoLuaPin::Find(FromNode, FFromPin);
						if (!FromPin)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> from_pin \"%s\" not found on node \"%s\". Available: %s"),
								*DisplayName, *FFromPin, *FFromHandle, *NeoLuaPin::ListAvailable(FromNode)));
							return sol::lua_nil;
						}
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> from_handle \"%s\" not found. Call read_graph() or add_node() first."),
							*DisplayName, *FFromHandle));
						return sol::lua_nil;
					}
				}
			}

			if (!SpawnerIdStr.empty())
			{
				// Blueprint spawner path
				FString SpawnerId = NeoLuaStr::ToFString(SpawnerIdStr);
				UBlueprintNodeSpawner* Spawner = Session.FindCachedSpawner(SpawnerId);
				if (!Spawner)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> cached spawner expired. Re-run find_nodes()."), *DisplayName));
					return sol::lua_nil;
				}
				Node = NeoNodes::SpawnNode(Graph, Spawner, PosX, PosY, FromPin);
			}
			else if (!ActionIdStr.empty())
			{
				// Schema action path (BehaviorTree, Material, SoundCue, EQS, etc.)
				FString ActionId = NeoLuaStr::ToFString(ActionIdStr);
				FLuaSessionData::FCachedSchemaAction* Cached = Session.FindCachedSchemaAction(ActionId);
				if (!Cached || !Cached->Action.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> cached schema action expired. Re-run find_nodes()."), *DisplayName));
					return sol::lua_nil;
				}

				// Hold a local shared_ptr copy to keep the action alive during PerformAction
				TSharedPtr<FEdGraphSchemaAction> ActionCopy = Cached->Action;

				// Ensure editor is open for schemas that need it
				NeoNodes::EnsureEditorOpenForSchema(Graph);

				const FScopedTransaction Transaction(NSLOCTEXT("NeoStackAI", "AddNodeSchema", "Add Node via Schema Action"));
				Graph->Modify();
				if (FromPin) FromPin->Modify();

				Node = PerformSchemaAction(ActionCopy.Get(), Graph, PosX, PosY, FromPin);
				if (!Node)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> PerformAction returned null"), *DisplayName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> table missing _spawner_id or _action_id. Use a result from find_nodes()."), *DisplayName));
				return sol::lua_nil;
			}
		}
		else if (NodeArg.is<std::string>())
		{
			DisplayName = NeoLuaStr::ToFString(NodeArg.as<std::string>());

			if (BP)
			{
				// Blueprint: use context-aware search when targeting a specific sub-graph
				// (e.g. AnimBP transition/state graphs), otherwise global action DB
				TArray<FNodeSearchResult> Results = (!FGraphName.IsEmpty() && Graph)
					? NeoNodes::FindNodesForGraph(DisplayName, BP, Graph, 10)
					: NeoNodes::FindNodes(DisplayName, BP, 10);

				if (Results.Num() == 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> no matching node type found"), *DisplayName));
					return sol::lua_nil;
				}

				int32 TopScore = Results[0].Score;
				int32 AmbiguousCount = 0;
				for (const FNodeSearchResult& R : Results)
				{
					if (R.Score == TopScore) AmbiguousCount++;
					else break;
				}

				if (AmbiguousCount > 1)
				{
					FString Options;
					for (int32 i = 0; i < AmbiguousCount; i++)
					{
						if (i > 0) Options += TEXT("\n");
						Options += FString::Printf(TEXT("  [%d] \"%s\" (category: %s)"),
							i + 1, *Results[i].Name, *Results[i].Category);
					}
					Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> ambiguous, %d nodes match equally. Use find_nodes() and pass the result table:\n%s"),
						*DisplayName, AmbiguousCount, *Options));
					return sol::lua_nil;
				}

				// Try results in order — supports both spawner-based (global DB) and
				// schema action-based (context-aware MakeContextMenu) results
				for (int32 ResultIdx = 0; ResultIdx < Results.Num() && !Node; ResultIdx++)
				{
					if (ResultIdx == 0 && Results[0].Name != DisplayName)
					{
						Session.Log(FString::Printf(TEXT("[INFO] add_node(\"%s\") -> matched \"%s\" (score: %d)"),
							*DisplayName, *Results[0].Name, Results[0].Score));
					}

					if (Results[ResultIdx].SchemaAction.IsValid())
					{
						// Context-aware schema action (from FindNodesForGraph / MakeContextMenu)
						const FScopedTransaction Transaction(NSLOCTEXT("NeoStackAI", "AddNodeCtx", "Add Node via Context Menu"));
						Graph->Modify();
						Node = PerformSchemaAction(Results[ResultIdx].SchemaAction.Get(), Graph, PosX, PosY);
					}
					else if (UBlueprintNodeSpawner* Spawner = Results[ResultIdx].Spawner.Get())
					{
						Node = NeoNodes::SpawnNode(Graph, Spawner, PosX, PosY);
					}
				}
				if (!Node)
				{
					// Fallback: try schema context actions (works for macro instances like FlipFlop)
					FString SchemaError;
					Node = SpawnNodeViaSchema(Graph, DisplayName, PosX, PosY, SchemaError);
					if (!Node)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> all %d results failed, schema fallback: %s"),
							*DisplayName, Results.Num(), *SchemaError));
						return sol::lua_nil;
					}
				}
			}
			else
			{
				// Non-Blueprint: use schema context actions (same as editor right-click menu)
				FString Error;
				Node = SpawnNodeViaSchema(Graph, DisplayName, PosX, PosY, Error);
				if (!Node)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> %s"), *DisplayName, *Error));
					return sol::lua_nil;
				}
				// Log warning if set (e.g., multiple exact name matches)
				if (!Error.IsEmpty())
				{
					Session.Log(FString::Printf(TEXT("[WARN] add_node(\"%s\") -> %s"), *DisplayName, *Error));
				}
			}
		}
		else
		{
			Session.Log(TEXT("[FAIL] add_node -> 'node' must be a string name or a result table from find_nodes()"));
			return sol::lua_nil;
		}

		if (!Node)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] add_node(\"%s\") -> spawn failed"), *DisplayName));
			return sol::lua_nil;
		}

		Node->NodePosX = FMath::RoundToInt(PosX);
		Node->NodePosY = FMath::RoundToInt(PosY);

		FString NodeGuid = Node->NodeGuid.ToString();
		Session.Nodes.Add(NodeGuid, Node);

		// Auto-connect: if the node table has from_handle + from_pin, connect after spawn
		if (NodeArg.is<sol::table>())
		{
			sol::table NodeTable = NodeArg.as<sol::table>();
			std::string FromHandleStr = NodeTable.get_or<std::string>("from_handle", "");
			std::string FromPinStr = NodeTable.get_or<std::string>("from_pin", "");
			if (!FromHandleStr.empty() && !FromPinStr.empty())
			{
				FString FromHandle = NeoLuaStr::ToFString(FromHandleStr);
				FString FromPinName = NeoLuaStr::ToFString(FromPinStr);
				UEdGraphNode* FromNode = Session.FindNode(FromHandle);
				if (FromNode)
				{
					// Latent nodes like Delay have output exec pin named "then" but displayed as "Completed"
					// — NeoLuaPin::Find handles exec-pin aliases + display-name / case-insensitive fallbacks.
					UEdGraphPin* SrcPin = NeoLuaPin::Find(FromNode, FromPinName, EGPD_Output);
					if (!SrcPin)
					{
						// Last fallback: any direction
						SrcPin = NeoLuaPin::Find(FromNode, FromPinName);
					}
					if (SrcPin)
					{
						// Find a compatible pin on the new node to connect to
						const UEdGraphSchema* Schema = Graph->GetSchema();
						if (Schema)
						{
							EEdGraphPinDirection TargetDir = (SrcPin->Direction == EGPD_Output) ? EGPD_Input : EGPD_Output;
							bool bConnected = false;
							bool bSawCandidatePin = false;
							for (UEdGraphPin* TargetPin : Node->Pins)
							{
								if (!TargetPin || TargetPin->bHidden || TargetPin->Direction != TargetDir) continue;
								bSawCandidatePin = true;
								if (Schema->CanCreateConnection(SrcPin, TargetPin).Response != CONNECT_RESPONSE_DISALLOW)
								{
									if (Schema->TryCreateConnection(SrcPin, TargetPin))
									{
										Session.Log(FString::Printf(TEXT("[OK] auto-connected %s.%s -> %s.%s"),
											*FromHandle, *FromPinName, *NodeGuid, *TargetPin->PinName.ToString()));
										bConnected = true;
										break;
									}
								}
							}
							if (!bConnected)
							{
								Session.Log(FString::Printf(TEXT("[WARN] add_node auto-connect skipped: no compatible %s pin on new node %s for %s.%s%s"),
									TargetDir == EGPD_Input ? TEXT("input") : TEXT("output"),
									*NodeGuid, *FromHandle, *FromPinName,
									bSawCandidatePin ? TEXT("") : TEXT(" (new node has no pins in the required direction)")));
							}
						}
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] from_pin \"%s\" not found on node %s"), *FromPinName, *FromHandle));
					}
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] from_handle \"%s\" not found"), *FromHandle));
				}
			}
		}

		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
		Session.Log(FString::Printf(TEXT("[OK] add_node(\"%s\") -> placed \"%s\" at (%.0f, %.0f) handle=%s"),
			*DisplayName, *NodeTitle, PosX, PosY, *NodeGuid));

		sol::table T = LuaView.create_table();
		T["handle"] = TCHAR_TO_UTF8(*NodeGuid);
		T["name"] = TCHAR_TO_UTF8(*NodeTitle);

		sol::table InPins = LuaView.create_table();
		sol::table OutPins = LuaView.create_table();
		int32 InIdx = 1, OutIdx = 1;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden) continue;
			sol::table PinInfo = NeoLuaPin::BuildPinTable(LuaView, Pin);
			if (Pin->Direction == EGPD_Input)
				InPins[InIdx++] = PinInfo;
			else
				OutPins[OutIdx++] = PinInfo;
		}
		T["pins_in"] = InPins;
		T["pins_out"] = OutPins;

		return T;
	});
});
