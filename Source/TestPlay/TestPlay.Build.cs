using UnrealBuildTool;

public class TestPlay : ModuleRules
{
	public TestPlay(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EnhancedInput",
			"InputCore",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"UMG"
		});
	}
}
