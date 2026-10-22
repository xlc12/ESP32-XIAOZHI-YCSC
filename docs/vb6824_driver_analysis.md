# VB6824 外置音频芯片驱动架构分析

> 参考工程：`RoPet_ESPS3_AI_EYE-blufi-SY01`（doit-esp32s3-eye-6824 版型）
> 目的：对照参考实现，找出我们 CI1302 驱动的问题，对齐正确的设计模式

---

## 一、VB6824 整体架构

```
┌─────────────┐     UART 2Mbps      ┌─────────────┐
│   ESP32      │ ◄──────────────────► │   VB6824    │
│  xiaozhi    │   Opus帧(上行)       │  离线语音芯片 │
│  框架       │   PCM帧(下行)        │  唤醒词/VAD/  │
└─────────────┤                      │  AEC/编码    │
              │                      └─────────────┘
              │
   ┌──────────┴──────────┐
   │  VbAduioCodec       │  ← xiaozhi AudioCodec 子类
   │  ├─ Read()          │  ← 框架调，取 Opus 帧
   │  ├─ Write()         │  ← 框架调，推 PCM 帧
   │  ├─ EnableInput()   │  ← 开 mic
   │  ├─ EnableOutput()  │  ← 开 speaker
   │  └─ OnWakeUp()      │  ← 唤醒词回调
   └──────────┬──────────┘
              │
   ┌──────────┴──────────┐
   │  vb6824.c (C驱动)   │  ← 纯 C 层，UART 收发 + ringbuffer
   │  ├─ g_rx_ringbuffer │  ← 上行：芯片→ESP32
   │  ├─ g_tx_ringbuffer │  ← 下行：ESP32→芯片
   │  └─ __send_task     │  ← 定时把 tx ringbuffer 推给芯片
   └─────────────────────┘
```

---

## 二、关键设计点（跟我们 CI1302 的对比）

### 2.1 接口极简 —— 只有 6 个函数

| 函数 | VB6824 | CI1302（我们） |
|---|---|---|
| `Start()` | 初始化 + EnableInput(true) + EnableOutput(true) | 初始化 + 发配置参数 |
| `Read(dest, samples)` | 从 rx ringbuffer 读 Opus 帧（阻塞） | 返回 0（不提供 PCM） |
| `Write(data, samples)` | PCM 写入 tx ringbuffer，芯片自己播 | PCM → shine MP3 → 拉流播放 |
| `EnableInput(enable)` | 控制芯片 mic 开关 | 空操作 |
| `EnableOutput(enable)` | 控制芯片 speaker 开关 | 空操作 |
| `SetOutputVolume(v)` | 发 UART 命令调音量 | 发 UART 命令调音量 |

**没有的东西**：
- ❌ 没有 `OnListeningStarted()` / `OnListeningStopped()`
- ❌ 没有 0x011A / 0x011B 强制录音命令
- ❌ 没有"开始录/停止录"的概念
- ❌ 没有拉流播放（NET_PLAY_START / PLAY_DATA_GET）
- ❌ 没有 `HasUplinkOpusPacket()` / `PopUplinkOpusPacket()` 直通接口

### 2.2 上行：芯片自己 VAD，ESP32 只管读

```
芯片检测人声 → 自动开始发 Opus 帧 → ESP32 UART 收到 → 入 rx ringbuffer
                                                          ↓
                                          xiaozhi AFE/编码任务调 Read() → 取走帧 → 发到服务器
```

**关键**：
- 芯片自己做唤醒词检测、VAD、Opus 编码
- ESP32 不干预"什么时候开始录/什么时候停"
- `Read()` 是阻塞的——有数据就返回，没数据就等
- 只要设备在 Listening 状态，框架就一直调 `Read()`，自然就把数据读走了

### 2.3 下行：推模式，芯片自己播

```
xiaozhi 解码 TTS → PCM → Write() → 入 tx ringbuffer
                                        ↓
                            后台 send_task 定时把 ringbuffer 数据发给芯片
                                        ↓
                            VB6824 自己解码播放
```

**关键**：
- ESP32 只管推 PCM 数据，播放时序由芯片自己控制
- 没有拉流（芯片不来要数据），是 ESP32 主动推
- TTS 结束后芯片自己播完，不需要 ESP32 确认

### 2.4 唤醒词：芯片上报文本，板级调 WakeWordInvoke

```
芯片检测到唤醒词 → 发 UART 命令上报唤醒词文本
                        ↓
            VbAduioCodec 的回调收到 → 板级 OnWakeUp 处理
                        ↓
            Application::WakeWordInvoke("你好小智") → 进入 Listening
```

---

## 三、我们 CI1302 哪里不一样

| 方面 | VB6824 | CI1302（我们现在） | 问题 |
|---|---|---|---|
| 上行取数 | `Read()` 阻塞读，框架自动消费 | `HasUplinkOpusPacket()` 条件触发 | 多了一层判断，状态不对就丢帧 |
| 会话控制 | 无，芯片自己管 VAD | 之前加了 OnListeningStarted/Stopped（已删） | 删了之后应该对齐了 |
| 下行播放 | 推模式，ESP32 直接写 PCM | 拉流模式，芯片来要数据 | 复杂得多，有时序问题 |
| 唤醒处理 | 上报文本 → WakeWordInvoke | WAKE_UP → WakeupStartListening | 类似 |
| EnableInput/Output | 真正控制芯片开关 | 空操作 | 我们的芯片自管理 |

---

## 四、多轮对话间歇性失败的根因分析

### 现象
- 唤醒后第一句能聊
- 第二句开始，有时失败有时成功
- 说 3-4 句才有一句被服务器识别

### 根因猜测

我们的上行转发有个条件：`GetDeviceState() == kDeviceStateListening`。

```cpp
// audio_service.cc OpusCodecTask
if (codec_->HasUplinkOpusPacket() &&
    audio_send_queue_.size() < MAX &&
    Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
    // 转发
}
```

如果设备不在 Listening 状态，1302 发过来的 Opus 帧就积在队列里，等到 `PCM_FINISH` 来时被 `ClearUplinkOpusQueue()` 清掉，永远没发出去。

**为什么设备不在 Listening 状态？**

看 tts/stop 处理流程：
```cpp
// application.cc
} else if (strcmp(state->valuestring, "stop") == 0) {
    Schedule([this]() {
        audio_service_.NotifyStreamEnd();  // 告诉 codec 流结束
        if (device_state_ == kDeviceStateSpeaking) {
            SetDeviceState(kDeviceStateListening);  // 切回 Listening
        }
    });
}
```

问题：我们的 `NotifyStreamEnd()` 是发 `PLAY_DATA_END`，然后等芯片回 `PLAY_TTS_END`。这个过程有延迟。

但设备状态切换是在 `SetDeviceState(kDeviceStateListening)` 那里，跟 `NotifyStreamEnd()` 无关。

等等——那为什么前几句不工作？

### 跟 VB6824 对比后的发现

**VB6824 没有"设备状态不对就不读"的问题。**

因为 VB6824 的 `Read()` 是被 AFE 任务直接调用的。AFE 任务在 Listening 状态时一直运行，一直调 `Read()`。只要 rx ringbuffer 里有数据，就读走。

而我们的实现是：OpusCodecTask 检查 `HasUplinkOpusPacket()` + 设备状态。如果设备状态不对，就不读。

**这就是区别！**

VB6824 的模式：
- AFE 任务在 Listening → 一直调 Read() → 数据自动被消费
- 不需要额外判断"要不要转发"

我们的模式：
- OpusCodecTask 检查"有没有数据" + "状态对不对"
- 状态不对 → 不读 → 数据积在队列里 → 被 PCM_FINISH 清掉

---

## 五、建议的改进方向

### 方向 1：对齐 VB6824 的 Read() 模式（推荐）

把 CI1302 改成跟 VB6824 一样的模式：
- 不用 `HasUplinkOpusPacket()` / `PopUplinkOpusPacket()` 直通
- 改成 `Read(uint8_t* dest, int samples)` 模式——框架来调，有数据就返回，没数据就等
- 这样就跟 AFE 任务自然对接，不需要额外判断状态

但这样有个问题：xiaozhi 框架的 AFE 是做 PCM 处理的，需要 16kHz PCM 输入。而 CI1302 给的是 Opus 帧。

看 VB6824 的配置：`CONFIG_VB6824_TYPE_OPUS_16K_20MS` —— 芯片直接给 Opus，框架直接转发，不经过 AFE。

这说明 xiaozhi 框架支持"Opus 直通"模式。我们的 `HasUplinkOpusPacket()` / `PopUplinkOpusPacket()` 就是为了这个模式加的。

### 方向 2：去掉状态判断，有数据就转发

最简单的改法：把 OpusCodecTask 里的 `GetDeviceState() == kDeviceStateListening` 判断去掉。

有 Opus 帧就转发到服务器，服务器自己判断要不要处理。

这样跟 VB6824 的效果一样——芯片发什么，ESP32 就转什么。

### 方向 3：调查设备状态为什么不对

加日志确认：tts/stop 到达后，设备状态是不是真的切到了 Listening？

如果切到了，那为什么 Opus 帧没被转发？
如果没切到，那是哪里出了问题？

---

## 六、总结

VB6824 的设计哲学：**芯片做所有事（唤醒/VAD/编码/播放），ESP32 只管收发数据，不做会话控制。**

我们 CI1302 的问题：加了太多额外的会话控制逻辑，反而出问题。

下一步建议：
1. 先加日志确认前几句失败时设备状态是什么
2. 如果设备状态是 Listening，那是 OpusCodecTask 的条件判断有问题
3. 如果设备状态不是 Listening，那是状态机切换的问题
