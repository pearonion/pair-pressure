// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VNHSimulator : ModuleRules
{
	public VNHSimulator(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"GameplayTasks",
			"UMG",
			"AssetRegistry",
			"OnlineSubsystem",
			"OnlineSubsystemUtils",
			"AdvancedSessions",
			"AdvancedSteamSessions"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Required for local packaged Steam testing with AppID 480 when the executable is launched outside Steam.
		RuntimeDependencies.Add("$(TargetOutputDir)/steam_appid.txt", "$(ProjectDir)/steam_appid.txt", StagedFileType.NonUFS);

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
