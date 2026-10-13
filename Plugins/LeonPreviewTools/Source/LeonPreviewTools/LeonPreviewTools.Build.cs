using UnrealBuildTool;
using System.IO;
public class LeonPreviewTools : ModuleRules
{
    public LeonPreviewTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new string[] {
            "UnrealEd", "Slate", "SlateCore", "InputCore", "Json", "PhysicsCore", "Projects",
            "LevelEditor", "ToolMenus", "AssetRegistry", "ContentBrowser", "HairStrandsCore",
            "Renderer", "RHI", "RenderCore", "ComputeFramework", "OptimusCore", "HairStrandsDeformer"
        });
        PrivateIncludePaths.AddRange(new string[] {
            Path.Combine(EngineDirectory, "Plugins/Animation/DeformerGraph/Source/OptimusCore/Public"),
            Path.Combine(EngineDirectory, "Plugins/Animation/DeformerGraph/Source/OptimusCore/Private"),
            Path.Combine(EngineDirectory, "Plugins/Runtime/HairStrands/Source/HairStrandsDeformer/Private")
        });
    }
}
