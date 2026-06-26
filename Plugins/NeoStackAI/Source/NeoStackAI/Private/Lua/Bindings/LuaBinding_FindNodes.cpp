#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphCapabilityRegistry.h"
#include "Lua/LuaGraphResolver.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Blueprint/NodeUtils.h"
#include "Blueprint/BlueprintUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "Lua/LuaPinHelper.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// Generic node discovery via schema context actions — works for Material, BT, SoundCue, EQS, etc.
// Uses the engine's standard GetGraphContextActions API (same system the editor context menu uses).
static void FindNodesViaSchema(UEdGraph* Graph, const FString& Query, int32 MaxResults,
	TArray<FNodeSearchResult>& OutResults)
{
	if (!Graph) return;

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema) return;

	// Some schemas need the editor open for class discovery
	NeoNodes::EnsureEditorOpenForSchema(Graph);

	// Construct the context menu builder — same as the editor does for right-click menu
	FGraphContextMenuBuilder ContextMenuBuilder(Graph);
	Schema->GetGraphContextActions(ContextMenuBuilder);

	FString QueryLower = Query.ToLower();

	TArray<FNodeSearchResult> ScoredActions;

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
		FString Category = Action->GetCategory().ToString();
		FString Tooltip = Action->GetTooltipDescription().ToString();
		FString NameLower = Name.ToLower();

		// Build combined keywords string from engine search keywords
		FString Keywords;
		const TArray<FString>& KWArray = Action->GetSearchKeywordsArray();
		if (KWArray.Num() > 0)
		{
			Keywords = FString::Join(KWArray, TEXT(" "));
		}

		// Score: exact > starts with > contains > category > keywords > tooltip
		int32 Score = 0;
		if (NameLower == QueryLower) Score = 100;
		else if (NameLower.StartsWith(QueryLower)) Score = 80;
		else if (NameLower.Contains(QueryLower)) Score = 60;
		else if (Category.ToLower().Contains(QueryLower)) Score = 40;
		else if (!Keywords.IsEmpty() && Keywords.ToLower().Contains(QueryLower)) Score = 30;
		else if (Tooltip.ToLower().Contains(QueryLower)) Score = 20;
		else continue;

		FNodeSearchResult R;
		R.Name = Name;
		R.Category = Category;
		R.Tooltip = Tooltip;
		R.Keywords = Keywords;
		R.Score = Score;
		R.SchemaAction = Action;
		R.SchemaGraph = Graph;
		ScoredActions.Add(MoveTemp(R));
	}

	// Sort by score descending
	ScoredActions.Sort([](const FNodeSearchResult& A, const FNodeSearchResult& B) { return A.Score > B.Score; });

	int32 Count = FMath::Min(ScoredActions.Num(), MaxResults);
	for (int32 i = 0; i < Count; i++)
	{
		OutResults.Add(MoveTemp(ScoredActions[i]));
	}
}

static TArray<FLuaFunctionDoc> FindNodesDocs = {
	{
		TEXT("find_nodes(query, asset_path?, graph_name?, max_results?)"),
		TEXT("Search for available node types to add to a graph.\n"
			"  IMPORTANT: First argument is the search QUERY string (e.g. \"Delay\", \"Print String\"), NOT the asset path.\n"
			"  Example: find_nodes(\"Delay\", \"/Game/MyBP\", \"EventGraph\", 5)\n"
			"  Pin-context form: find_nodes({query=\"Add to Viewport\", asset_path=\"/Game/MyBP\", graph_name=\"EventGraph\", from_handle=\"...\", from_pin=\"Return Value\"})\n"
			"  For Blueprint graphs: uses the Blueprint action database (rich results with _spawner_id).\n"
			"  For schema-driven graphs: uses schema context actions.\n"
			"  Extension-owned graphs may provide custom find_nodes behavior.\n"
			"  asset_path and graph_name help narrow results to compatible nodes.\n"
			"  max_results defaults to 10, clamped to 1-100.\n"
			"  Searches name, category, keywords, and tooltip (in priority order).\n"
			"  Returns results sorted by relevance score."),
		TEXT("table of {name, category, tooltip, keywords?, score, _spawner_id?, _action_id?, owning_class?, pins_in?, pins_out?}")
	}
};

// Runtime RigVM graph detection — works without ControlRig module linkage.
// Leaf class name check only. UControlRigGraph won't match — that's OK,
// find_nodes falls through to schema context actions which work fine.
static const FLuaGraphCapability* FindFindNodesGraphCapability(UEdGraph* Graph)
{
	const FLuaGraphCapability* Capability = FLuaGraphCapabilityRegistry::Get().FindOwner(Graph);
	if (!Capability || Capability->Bridges.FindNodesFunctionName.IsEmpty())
	{
		return nullptr;
	}

	return Capability;
}

static sol::object FindNodes_QueryGraphCapability(FLuaSessionData& Session, UEdGraph* Graph,
	const FString& Query, int32 MaxResults, sol::this_state S)
{
	sol::state_view LuaView(S);
	const FLuaGraphCapability* Capability = FindFindNodesGraphCapability(Graph);
	if (!Capability)
	{
		return LuaView.create_table();
	}

	sol::protected_function FindNodesFn = LuaView[TCHAR_TO_UTF8(*Capability->Bridges.FindNodesFunctionName)];
	if (!FindNodesFn.valid())
	{
		Session.Log(FString::Printf(TEXT("[FAIL] find_nodes on %s graph -> bridge \"%s\" is not available"),
			*Capability->Name,
			*Capability->Bridges.FindNodesFunctionName));
		return LuaView.create_table();
	}

	sol::protected_function_result Result = FindNodesFn(sol::lightuserdata_value(Graph), TCHAR_TO_UTF8(*Query), MaxResults);
	if (!Result.valid())
	{
		sol::error Error = Result;
		Session.Log(FString::Printf(TEXT("[FAIL] find_nodes on %s graph -> %s"),
			*Capability->Name,
			UTF8_TO_TCHAR(Error.what())));
		return LuaView.create_table();
	}

	return Result.get<sol::object>();
}

REGISTER_LUA_BINDING(FindNodes, FindNodesDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("find_nodes", [&Session](sol::object QueryArg,
		sol::optional<std::string> AssetPath, sol::optional<std::string> GraphNameOpt,
		sol::optional<int> Max, sol::this_state S) -> sol::object
	{
		sol::state_view LuaView(S);
		FString FQuery;
		FString FromHandleName;
		FString FromPinName;

		if (QueryArg.is<std::string>())
		{
			FQuery = NeoLuaStr::ToFString(QueryArg.as<std::string>());
		}
		else if (QueryArg.is<sol::table>())
		{
			sol::table QueryTable = QueryArg.as<sol::table>();
			FQuery = NeoLuaStr::ToFString(QueryTable.get_or<std::string>("query", ""));

			std::string TableAssetPath = QueryTable.get_or<std::string>("asset_path", "");
			if (!TableAssetPath.empty())
			{
				AssetPath = TableAssetPath;
			}

			std::string TableGraphName = QueryTable.get_or<std::string>("graph_name", "");
			if (!TableGraphName.empty())
			{
				GraphNameOpt = TableGraphName;
			}

			sol::object MaxObj = QueryTable["max_results"];
			if (MaxObj.valid() && MaxObj.get_type() != sol::type::lua_nil)
			{
				if (MaxObj.is<int>())
				{
					Max = MaxObj.as<int>();
				}
				else if (MaxObj.is<double>())
				{
					Max = static_cast<int>(MaxObj.as<double>());
				}
			}

			FromHandleName = NeoLuaStr::ToFString(QueryTable.get_or<std::string>("from_handle", ""));
			FromPinName = NeoLuaStr::ToFString(QueryTable.get_or<std::string>("from_pin", ""));
		}
		else
		{
			Session.Log(TEXT("[FAIL] find_nodes -> first argument must be a query string or options table"));
			return LuaView.create_table();
		}

		if (FQuery.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] find_nodes -> query is empty"));
			return LuaView.create_table();
		}

		int32 MaxResults = FMath::Clamp(Max.value_or(10), 1, 100);

		UBlueprint* ContextBP = nullptr;
		UEdGraph* ContextGraph = nullptr;
		UEdGraphPin* ContextPin = nullptr;
		const FLuaGraphCapability* GraphCapability = nullptr;

		if (AssetPath.has_value())
		{
			FString FPath = NeoLuaAsset::NormalizePath(NeoLuaStr::ToFStringOpt(AssetPath));
			UObject* Asset = NeoLuaAsset::Resolve<UObject>(FPath);
			if (Asset)
			{
				ContextBP = Cast<UBlueprint>(Asset);
				if (!ContextBP)
				{
					if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Asset))
					{
						ContextBP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
					}
					else if (UClass* ClassAsset = Cast<UClass>(Asset))
					{
						if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(ClassAsset))
						{
							ContextBP = Cast<UBlueprint>(GeneratedClass->ClassGeneratedBy);
						}
					}
				}
				FString FGraphName = NeoLuaStr::ToFStringOpt(GraphNameOpt);

				UEdGraph* CapabilityGraph = LuaGraphResolver::FindGraph(Asset, FGraphName);
				GraphCapability = FindFindNodesGraphCapability(CapabilityGraph);
				if (GraphCapability)
				{
					ContextGraph = CapabilityGraph;
				}
				if (ContextBP)
				{
					if (!GraphCapability && !FGraphName.IsEmpty())
					{
						ContextGraph = NeoBlueprint::FindGraph(ContextBP, FGraphName);
					}
				}
				else
				{
					ContextGraph = CapabilityGraph;
				}
			}
		}

		if (!FromHandleName.IsEmpty() || !FromPinName.IsEmpty())
		{
			if (FromHandleName.IsEmpty() || FromPinName.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] find_nodes pin-context form requires both from_handle and from_pin"));
				return LuaView.create_table();
			}

			if (ContextGraph)
			{
				Session.RegisterGraphNodes(ContextGraph);
			}

			if (UEdGraphNode* FromNode = Session.FindNode(FromHandleName))
			{
				ContextPin = NeoLuaPin::Find(FromNode, FromPinName);
				if (!ContextPin)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] find_nodes(\"%s\") -> from_pin \"%s\" not found on node \"%s\". Available: %s"),
						*FQuery, *FromPinName, *FromHandleName, *NeoLuaPin::ListAvailable(FromNode)));
					return LuaView.create_table();
				}
				if (!ContextGraph)
				{
					ContextGraph = FromNode->GetGraph();
				}
				if (!ContextBP && ContextGraph)
				{
					ContextBP = FBlueprintEditorUtils::FindBlueprintForGraph(ContextGraph);
				}
			}
			else
			{
				Session.Log(FString::Printf(TEXT("[FAIL] find_nodes(\"%s\") -> from_handle \"%s\" not found. Call read_graph() or add_node() first."),
					*FQuery, *FromHandleName));
				return LuaView.create_table();
			}
		}

		if (GraphCapability && ContextGraph)
		{
			return FindNodes_QueryGraphCapability(Session, ContextGraph, FQuery, MaxResults, S);
		}

		TArray<FNodeSearchResult> Results;

		if (ContextBP || !AssetPath.has_value())
		{
			if (ContextGraph && ContextBP)
			{
				Results = NeoNodes::FindNodesForGraph(FQuery, ContextBP, ContextGraph, MaxResults, ContextPin);
			}
			else
			{
				Results = NeoNodes::FindNodes(FQuery, ContextBP, MaxResults);
			}
		}
		else if (ContextGraph)
		{
			FindNodesViaSchema(ContextGraph, FQuery, MaxResults, Results);
		}
		else
		{
			// Fallback
			Results = NeoNodes::FindNodes(FQuery, nullptr, MaxResults);
		}

		if (Results.Num() == 0)
		{
			Session.Log(FString::Printf(TEXT("[OK] find_nodes(\"%s\") -> 0 results"), *FQuery));
			return LuaView.create_table();
		}

		Session.Log(FString::Printf(TEXT("[OK] find_nodes(\"%s\") -> %d results"), *FQuery, Results.Num()));

		sol::table ResultTable = LuaView.create_table();
		for (int32 i = 0; i < Results.Num(); i++)
		{
			const FNodeSearchResult& R = Results[i];

			sol::table Entry = LuaView.create_table();
			Entry["name"] = TCHAR_TO_UTF8(*R.Name);
			Entry["category"] = TCHAR_TO_UTF8(*R.Category);
			Entry["tooltip"] = TCHAR_TO_UTF8(*R.Tooltip);
			Entry["score"] = R.Score;

			if (!R.Keywords.IsEmpty())
			{
				Entry["keywords"] = TCHAR_TO_UTF8(*R.Keywords);
			}

			// Blueprint spawner
			if (R.Spawner.IsValid())
			{
				FString SpawnerId = Session.CacheSpawner(R.Spawner.Get());
				Entry["_spawner_id"] = TCHAR_TO_UTF8(*SpawnerId);

				// Extract owning class for disambiguation (e.g., Pawn::GetControlRotation vs Controller::GetControlRotation)
				UClass const* OwnerClass = nullptr;
				if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(R.Spawner.Get()))
				{
					if (UFunction const* Func = FuncSpawner->GetFunction())
						OwnerClass = Func->GetOwnerClass();
				}
				else if (const UBlueprintVariableNodeSpawner* VarSpawner = Cast<UBlueprintVariableNodeSpawner>(R.Spawner.Get()))
				{
					if (const FProperty* VarProp = VarSpawner->GetVarProperty())
						OwnerClass = VarProp->GetOwnerClass();
				}
				if (OwnerClass)
				{
					Entry["owning_class"] = TCHAR_TO_UTF8(*OwnerClass->GetName());
					// Log owning_class so agent can disambiguate without extra iteration
					Session.Log(FString::Printf(TEXT("  %d: %s | %s | class=%s | %s"),
						i + 1, *R.Name, *R.Category, *OwnerClass->GetName(), *SpawnerId));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("  %d: %s | %s | %s"),
						i + 1, *R.Name, *R.Category, *SpawnerId));
				}
			}
			// Schema action (non-Blueprint graphs: BT, Material, SoundCue, EQS, etc.)
			else if (R.SchemaAction.IsValid() && R.SchemaGraph.IsValid())
			{
				FString ActionId = Session.CacheSchemaAction(R.SchemaAction, R.SchemaGraph.Get());
				Entry["_action_id"] = TCHAR_TO_UTF8(*ActionId);
				Session.Log(FString::Printf(TEXT("  %d: %s | %s | %s"),
					i + 1, *R.Name, *R.Category, *ActionId));
			}

			// Pin info (only when available — Blueprint spawners populate these)
			if (R.InputPins.Num() > 0)
			{
				sol::table InPins = LuaView.create_table();
				for (int32 p = 0; p < R.InputPins.Num(); p++)
				{
					InPins[p + 1] = TCHAR_TO_UTF8(*R.InputPins[p]);
				}
				Entry["pins_in"] = InPins;
			}

			if (R.OutputPins.Num() > 0)
			{
				sol::table OutPins = LuaView.create_table();
				for (int32 p = 0; p < R.OutputPins.Num(); p++)
				{
					OutPins[p + 1] = TCHAR_TO_UTF8(*R.OutputPins[p]);
				}
				Entry["pins_out"] = OutPins;
			}

			ResultTable[i + 1] = Entry;
		}

		return ResultTable;
	});
});
