using UnrealBuildTool;
public class LunaMVP : ModuleRules
{
    public LunaMVP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "Sockets", "Networking", "Json", "Slate", "SlateCore", "UMG", "DeveloperSettings", "ProceduralMeshComponent" });
    }
}
