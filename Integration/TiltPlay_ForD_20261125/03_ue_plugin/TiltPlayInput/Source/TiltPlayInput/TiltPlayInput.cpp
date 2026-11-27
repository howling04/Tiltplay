// TiltPlayInput 插件模块入口：本插件没有自定义 Module 逻辑，
// 只负责让 InputServiceSubsystem 在引擎里注册（子系统由 GameInstance 自动实例化）。
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, TiltPlayInput);
