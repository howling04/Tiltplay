# Luna × TiltPlay

在 Windows 资源管理器双击 **Play-LunaTiltPlay.cmd**。它编译当前目录的 Luna 工程、启动游戏，并接入 COM7 上的 Nano RP2040 Connect。工程使用 UE 5.8.4，网关要求 Python 3.10 或更新版本。

从 GitHub 克隆前请安装 Git LFS，克隆后运行 `git lfs pull` 获取 UE 地图和其他二进制资产。真板演示使用 USB 串口和本机 UDP。独立 MQTT 测试如需密码，通过本机环境变量 `TILT_MQTT_PASSWORD` 提供。

## 搬到其他路径或电脑

运行入口从当前工程目录读取地图、资产、网关、固件源码和 Python 网关依赖。迁移时保留 `Luna_v1.uproject`、根目录的 CMD 文件、`Config`、`Content`、`Source`、`Scripts`、`Integration`。需要重新导入素材时同时保留 `SourceArt`。新副本可直接让启动脚本重建 `Binaries`、`Intermediate` 和缓存。若要保留旧记录和本地设置，可另带 `Saved`。

目标电脑需安装 Windows 版 UE 5.8（工程已用 5.8.4 构建）、兼容的 Visual Studio C++ 工具链和 Windows SDK、Python 3.10 或以上。板子需烧录交付的 `USB_Live.ino`。启动脚本会从本机注册表或 Epic Launcher 安装记录查找引擎。对于手动安装的引擎，可以在 PowerShell 指定位置和串口。

```powershell
$env:LUNA_UE_ROOT = 'C:\Program Files\Epic Games\UE_5.8'
.\Play-LunaTiltPlay.cmd -Port COM7
```

旧版 BCD 调试脚本 `Integration/test_bcd_flow.py` 可通过 `--editor` 和 `--gateway` 显式指定历史测试夹具。日常启动、回放和 `Scripts/Test-Luna.ps1` 使用本目录中的工程与交付文件。

## 首次实板测试

1. 把板子元件面朝上平放，USB 口朝向自己。启动脚本后，在控制台等网关显示「回中成功」。
2. 在 Luna 里按 **2** 进入侧向练习。启动脚本已选中 Nano 模式。板子平放时按 **C**，等到控制恢复。**F1** 可查看输入状态、采样序号和数据年龄。
3. 分别前倾、右倾，然后放平。倾斜带来水平推力，放平使推力回零，Luna 按原有阻尼继续滑行。
4. 要验证失联锁定，可先建立控制，再短暂中断板子输入，看 Luna 是否进入飞行锁定。板子重新连接后，重新运行网关，平放并按 **C** 恢复。
5. 测完在网关控制台按 **Ctrl+C**，记录写入 `Saved/Verification/TiltPlay-*`。其中 `runtime.log` 有 UE 的回中与锁定事件，`summary.json` 有网关计数，`raw.jsonl` 和 `input_udp.jsonl` 有串口采样与网关帧。

控制器链路为 **USB CDC COM7 → Python 网关 → 本机 UDP 7001 → Luna**。回中指令走本机 UDP 7002。Nano 固件、网关和插件交付源码在 `Integration/TiltPlay_ForD_20261125`。Luna 的 C++ 接收器直接接收这套网关的 schema 2 数据，并使用网关已经映射好的 `move`。

## 第三关升降

在主菜单按 **3** 进入极轨训练。板子平放时按 **C** 回中，随后让板面保持大致水平，短促上推或下推整块板，每次手势改变一层轨道。两次动作之间保持平稳片刻。**Q / E** 可作为键盘升降操作。

工程副本的 `Integration/TiltPlay_ForD_20261125/02_gateway/gateway/config/bridge.json` 已在 `devices.nano-a1b2c3d4e5f6` 下启用 `vertical_step_enabled`。改动后重启网关即可加载。右侧仪表显示目标轨道层和当前高度。实际发生的手势换层会记录为 `LUNA_ORBIT_STEP`。

网关打不开串口或收不到 IMU 帧时，双击 **Check-LunaTiltPlay.cmd** 可单独检查串口。网关等待回中超时后也会自动诊断。诊断会在 `Saved/Verification/TiltPlay-*` 记录 8 秒的串口样本到 `serial_probe.json`。

## 不接板子时

运行 `Play-LunaTiltPlay.cmd -Replay`。在 Luna 中按 **2**，再按 **C**，开始约 40 秒的实录数据回放。回放结束后可观察失联锁定。继续使用键盘时，可从 Luna 菜单选择 Keyboard simulation，或用 **F2** 切换输入。

## 验证

- UE 自动化入口为 `Scripts/Test-Luna.ps1`。其中 `LunaMVP.Input.TiltPlayHandoff` 覆盖上电时未知年龄、网关回包格式、回中因果顺序与失效锁定。
- `Saved/Verification/tiltplay-offline.log` 记录交付文件完整性、网关单元测试及实录帧检查。
