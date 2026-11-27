using UnrealBuildTool;
using System.Collections.Generic;
public class LunaMVPTarget : TargetRules
{
    public LunaMVPTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("LunaMVP");
    }
}
