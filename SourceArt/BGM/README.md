# Quiet Meridian · 静谧子午线

《The Wandering Luna》的第一首背景音乐。**80 BPM、4/4 拍、16 小节，单周 48.000 秒。** 曲子用 Strudel 的合成器、滤波器、延迟和混响生成，所有声部均由乐谱定义。

## 编曲与混音

- **第 1–8 小节**从 Dmaj9 开始。低音经过 C♯、B、G 下行，高声部保留共同音。三音主旋律和间断琶音给轻缓的鼓机律动留出空间。
- **第 9–16 小节**扩大音域，高声部向上，合成圆号应答。最后一小节的 A(sus4) 转向 Aadd9，回环时落回 Dmaj9。
- **四组铺底**包括低弦、左右微失谐的锯齿波弦垫、FM 合唱垫和慢起的合成圆号。短琶音与三角波主旋律提供清晰的前景，低音、底鼓、军鼓和踩镲维持轻量的 synthwave 脉冲。长包络和延迟保持空间感，回环处保留上一周的尾音。

## 制作与接入

1. 用 `Quiet_Meridian.strudel.js` 编曲。可将脚本全文粘贴到 Strudel 编辑器试听及修改。在本目录以 Node.js 22.12 或更新版运行 `npm ci`，再运行 `npm run render`。渲染调用本机 Chrome、Edge 或 `LUNA_CHROMIUM` 指定的 Chromium 浏览器。[Strudel 编辑器](https://strudel.cc/)
2. 在 Strudel 离线渲染的 16 小节之前预卷 4 小节，让上一周的包络与混响进入循环。剪掉 12 秒预卷，将结尾 60 ms 平滑接到开头之前的真实预卷尾音，保持循环长度为 **2,304,000 帧**。交付 `SW_Luna_QuietMeridian.wav`，格式为 **48 kHz、16-bit PCM、立体声**。`loop-verification.json` 记录周期、声部和双通道接缝测量。
3. 母带导入为 `/Game/LunaArt/Audio/SW_Luna_QuietMeridian`，使用 **PCM、Force Inline、Looping**。通过 `Play-Luna.cmd` 或在编辑器内试玩时，`ALunaGame` 在开场启动一次 2D 播放，播放持续跨过菜单、飞行和暂停。固定增益为 **0.30（−10.46 dB）**，给开场旁白留余量，关卡结束时停止。从 Strudel 重渲染后，在内容浏览器对此资产执行 Reimport，或用 `import_into_unreal.py` 重新导入并检查循环设置。

## 验证

UE 5.8 编辑器构建通过，重新读取已保存的 Sound Wave 时确认了 48.000 秒、双声道、48 kHz 以及 PCM 循环设置。在编辑器内录制了 101.781 秒主混音，只有一个 BGM 实例，从头到尾使用同一个引擎声音实例。第 48 秒和第 96 秒两处接缝均与母带在采样点上对齐，接缝两侧各 0.5 秒的左右声道相关系数均大于 0.9999997。[UE 播放验证记录](../../Saved/Verification/LunaBgm/playback-verification.json)

参考 [Strudel 合成器文档](https://strudel.cc/learn/synths/)和 [Unreal Engine 5.8 Sound Wave 文档](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/USoundWave?application_version=5.8)。
