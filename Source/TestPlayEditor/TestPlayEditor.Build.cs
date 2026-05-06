using UnrealBuildTool;

public class TestPlayEditor : ModuleRules
{
	public TestPlayEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"TestPlay"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"DesktopPlatform",
			"InputCore",
			"Json",
			"LevelEditor",
			"Projects",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd"
		});
	}
}
