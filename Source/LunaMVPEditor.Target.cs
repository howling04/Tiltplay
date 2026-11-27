using UnrealBuildTool;
public class LunaMVPEditorTarget : TargetRules
{
    public LunaMVPEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("LunaMVP");
    }
}
