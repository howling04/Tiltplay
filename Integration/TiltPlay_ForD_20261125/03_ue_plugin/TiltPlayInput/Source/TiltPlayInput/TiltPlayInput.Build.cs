using UnrealBuildTool;

public class TiltPlayInput : ModuleRules
{
	public TiltPlayInput(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 只依赖引擎模块，不依赖 TiltPlay 游戏模块 —— 任何工程都能直接挂上
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"Sockets",
			"Networking",
			"Json"
		});
	}
}
