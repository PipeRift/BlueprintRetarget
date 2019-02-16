// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintRetarget : ModuleRules
{
	public BlueprintRetarget(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Projects",
			"InputCore",
			"UnrealEd",
			"CoreUObject",
			"Engine",
			"EditorStyle",
			"SlateCore",
			"Slate",
			"BlueprintGraph"
		});
	}
}
