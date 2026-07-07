// Copyright Dev.GaeMyo 2024. All Rights Reserved.

using UnrealBuildTool;

public class GmRapidThumbnailCreator : ModuleRules
{
	public GmRapidThumbnailCreator(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Projects",
				"HairStrandsCore",
				"Niagara",
				"DesktopPlatform", "UMGEditor", "ScriptableEditorWidgets"
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Projects",
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"ToolMenus",
				"EditorFramework",
				"UMG",
				"UMGEditor",
				"EditorScriptingUtilities",
				"EditorSubsystem",
				"Blutility"
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
			);
	}
}
