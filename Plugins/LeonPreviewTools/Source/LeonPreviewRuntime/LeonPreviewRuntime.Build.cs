using UnrealBuildTool;

public class LeonPreviewRuntime : ModuleRules
{
    public LeonPreviewRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "Json", "Projects",
            "AssetRegistry", "HairStrandsCore"
            , "ComputeFramework", "OptimusCore"
        });
        PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "Renderer", "Projects", "RHI", "InputCore" });
    }
}
