# MANIFEST — TiltPlay_ForD_20261125

生成日期：2026-11-25
面向：D（玩法/集成）
生成者：C（UE 输入服务）

## 包内有什么

| 路径 | 是什么 | 谁维护 |
|---|---|---|
| `00_先读我_接入总览.md` | 入口文档：链路、最快路径、目录、环境、验收口径 | C |
| `01_firmware/USB_Live.ino` | 板子固件（与 11-25 复测同哈希） | A |
| `01_firmware/烧录说明.md` | 烧录与自检步骤 | C |
| `02_gateway/gateway/python/tiltplay/` | 网关：校验、融合、回中、60 Hz 输出 | B（本体）/ A（补丁） |
| `02_gateway/gateway/.vendor/` | paho-mqtt + pyserial，免 pip 安装 | 上游 |
| `02_gateway/gateway/config/bridge.json` | 唯一配置：端口/频率/门限/取反/设备 | A 提供，C 改了两处环境值 |
| `02_gateway/record_live.py` | USB 板 → 校验 → 7001，同时落盘 | A 原版，C 改写演示模式 |
| `02_gateway/check_env.py` + `CHECK_ENV.cmd` | 起跑前自检（Python/依赖/端口/串口/配置） | C |
| `02_gateway/START_USB_LIVE.cmd` | 一键起：`START_USB_LIVE.cmd COM7` | C |
| `03_ue_plugin/TiltPlayInput/` | UE 插件源码（Runtime 模块） | C |
| `03_ue_plugin/接入说明.md` | 装法、C++/蓝图取数、回中、参数、排障 | C |
| `04_无板自测/replay_to_ue.py` | 把录好的帧按 60 Hz 回放到 7001 | C |
| `04_无板自测/sample_input_40s.jsonl` | 40 秒真实网关输出（2393 帧，回放会话标识已替换） | 实测数据 |
| `04_无板自测/START_NO_BOARD.cmd` | 一键跑无板自测 | C |
| `05_已知缺口.md` | 演示前必读：现在就是没有的东西 | C |
| `SHA256SUMS.txt` | 全部文件校验和 | — |

## 包里没有什么（重要）

- **没有 UE 打包 exe**。581 MB（去调试符号后），微信传不了；明天单独发网盘/U 盘链接。今天先用插件在你自己的工程里跑通。
- **没有 A 的自用脚本**（`C_START.py`、`*_REAL.cmd`、`C_SIMULATOR.cmd` 等），也没有 `gateway/python/tests`——这些不是 D 要用的。
- **没有旧固件、旧配置**。全包只有一份固件、一份 `bridge.json`。

## 与上游的差别（C 侧的改动，逐条可查）

1. `bridge.json`：`mqtt.host` 原局域网地址 → `127.0.0.1`；`log.dir` `tests/logs` → `runtime/logs`。
2. `record_live.py`：去掉硬编码 40 秒五段复测脚本与蜂鸣；`--seconds 0` 表示一直录；回中失败改为中文提示后正常退出；默认双轴（原默认单轴），要单轴加 `--single-axis`。
3. UE 侧：把 `InputServiceSubsystem` 从游戏模块拆成独立插件 `TiltPlayInput`，类与 API 未变，只加了 `TILTPLAYINPUT_API` 导出宏、日志类别改 `LogTiltPlayInput`。

网关的逻辑、协议、参数口径一字未改；固件原样。

## 版本与哈希（前 16 位）

| 文件 | SHA256 前 16 |
|---|---|
| `01_firmware/USB_Live.ino` | `1B14A0FBD8545A06` |
| `02_gateway/gateway/config/bridge.json` | 见 `SHA256SUMS.txt` |
| `02_gateway/record_live.py` | 见 `SHA256SUMS.txt` |
| `03_ue_plugin/TiltPlayInput/TiltPlayInput.uplugin` | 见 `SHA256SUMS.txt` |

完整校验：在包根目录执行
`Get-FileHash -Algorithm SHA256 (Get-ChildItem -Recurse -File | ? Name -ne 'SHA256SUMS.txt').FullName`
