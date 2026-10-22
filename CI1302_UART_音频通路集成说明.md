# CI1302 替换 ES8311 —— UART 音频通路集成说明

> 目标：在 ESP32-S3 小智工程（xiaozhi-esp32-ycsc）中用 CI1302 离线语音芯片替换 ES8311 codec，
> 1302 经**串口1（921600）**完成全部音频收发：
> 上行 Opus（1302 内部编码 → ESP32 → 云端），下行 MP3（云端 → ESP32 → 1302 播放）。
> 旧实现全部作废，本说明基于重写后的代码。

---

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          CI1302（离线侧）                                 │
│  ASR + AEC + NN降噪 + NN_VAD → Opus编码（40ms/16kHz/单声道/CBR16kbps）   │
│  SAP 精简播放器（MP3 解码输出）                                           │
└───────────────┬───────────────────────────────▲─────────────────────────┘
                │ UART1 @ 921600               │
                │ 上行：WAKE_UP/VAD_START/PCM_MIDDLE(Opus帧)/VAD_END       │
                │ 下行：NET_PLAY_START/PLAY_DATA_GET/PLAY_DATA_RECV/END    │
┌───────────────▼───────────────────────────────┴─────────────────────────┐
│                          ESP32-S3 小智工程                               │
│  UartAudioCodec（重写）                                                   │
│    上行：PCM_MIDDLE 帧 → 直通队列 → AudioService::OpusCodecTask          │
│          （跳过 ESP32 二次 Opus 编码）→ send_queue → websocket → 云端     │
│    下行：云端 Opus → 解码 PCM → shine 编码 MP3（24kHz/单声道/48kbps）      │
│          → 按 PLAY_DATA_GET 定量回包（≤1024B，绝不回空包）                 │
│  ES8311 实例仅 Start() 保留，为 1302 麦克风提供 MICBIAS 偏压（PA=NC）      │
└─────────────────────────────────────────────────────────────────────────┘
```

## 2. 小智侧改动清单

### 2.1 `main/audio/audio_codec.h` — 基类新增直通/会话接口（默认 no-op，不影响普通 I2S codec）
- `HasUplinkOpusPacket()` / `PopUplinkOpusPacket()`：codec 直出已编码 Opus 帧时返回 true 并弹出。
- `OnListeningStarted()` / `OnListeningStopped()`：Application 进出 Listening 状态回调。
- `NotifyStreamEnd()`：服务器 tts/stop 回调，codec 结束下行数据流（发 PLAY_DATA_END）。
- `AbortPlayback()`：打断播放回调（发 NET_PLAY_STOP）。

### 2.2 `main/audio/audio_service.{h,cc}` — 上行直通管道
- `OpusCodecTask` 的等待条件与循环体新增**直通分支**：
  - 条件：`codec_->HasUplinkOpusPacket()` 且 `Application` 处于 `kDeviceStateListening` 且发送队列未满。
  - 动作：`PopUplinkOpusPacket()` → 直接推入 `audio_send_queue_` → 触发 `on_send_queue_available` 上云。
  - **只在 Listening 状态消费**：VAD 会话外的帧不会误发；VAD_START→Listening 建立期间的帧留在 codec 队列，Listening 建立后补发（不丢帧）。
- 新增 `NotifyCodecTask()`：codec 收帧 / Listening 建立时唤醒编码任务。
- 新增 `NotifyStreamEnd()` / `AbortPlayback()`：转发到 codec。

### 2.3 `main/application.cc` — 会话联动钩子
- **tts/stop**：先 `audio_service_.NotifyStreamEnd()`（1302 收尾发 END），再切状态。
- **AbortSpeaking**：追加 `audio_service_.AbortPlayback()`（1302 立即 NET_PLAY_STOP）。
- **StartListening**（Idle 与 Speaking 分支）：`GetAudioCodec()->OnListeningStarted()`（按键触发时 1302 强制录音）。
- **StopListening / ToggleChatState(Listening→Close) / OnAudioChannelClosed**：`OnListeningStopped()`（1302 停止录音、清残留帧）。

### 2.4 `main/audio/codecs/uart_audio_codec.{h,cc}` — 完全重写（核心）
上行：
- `PCM_MIDDLE(0x0105)` 负载即 1302 的 **Opus 帧**（40ms，通常 80B），入直通队列（64 帧 = 2.56s），满时覆盖最旧。
- `WAKE_UP(0x0102)/VAD_START(0x0108)` → `Application::StartListening()`（Idle 打开通道；Speaking 先 Abort 再强制进 Listening，不依赖服务器回包）。
- `VAD_END(0x0103)/PCM_FINISH(0x0106)` → `Application::StopListening()`。
- 按键/外部进入 Listening 且非 1302 主动唤醒（2s 窗口外）→ 发 `SET_AUDIO_START_RECORD(0x011A)`；停止时发 `0x011B`（两命令必须配对）。

下行（拉流状态机，独立播放任务）：
```
kIdle ── Write() 首包 ──► kPrebuffer（攒够 4096B 或 1.5s 超时）
      ── NET_PLAY_START(0x0201) ──► kPlaying
kPlaying ── 收到 PLAY_DATA_GET(0x020A, 请求N) ──► 回 PLAY_DATA_RECV(0x020B, ≤N 字节，有数据就回，绝不回空包)
          ── tts/stop（NotifyStreamEnd）→ shine flush 尾部入缓冲 ──►
             缓冲拉空后发 PLAY_DATA_END(0x020C，携带尾部数据，绝不为空负载) ──► kIdle
          ── AbortPlayback / 10s 无 GET ──► NET_PLAY_STOP(0x0204) ──► kIdle
```
- 1302 收包缓冲上限 **1043B**，本端 `PLAY_DATA_MAX_CHUNK=1024`，每包 ≤ 请求量。
- `PLAY_DATA_END` 携带 shine flush 尾部（>0 字节），让 SAP 播放器正常收尾回 IDLE（旧版卡死的根因之一）。
- 下行 MP3：shine 编码器（components/shine），24kHz/单声道/48kbps（MPEG-II L3，帧 144B/24ms，与云端 TTS 原生 24kHz 对齐，避免重采样）。
- 下行缓冲 64KB（≈10.7s @48kbps），预缓冲 4096B 后启动播放。

### 2.5 板级（未改，已确认）
`main/boards/ycsc-esp32s3-8311-wgxl-cat/`：
- `config.h`：`AUDIO_UART_PORT=UART_NUM_1`、`AUDIO_UART_BAUDRATE=921600`、`TX=GPIO17`、`RX=GPIO18`（占位值，按实际硬件核对）。
- board .cc：`CONFIG_AUDIO_CODEC_UART_CI1302` 分支保留 ES8311 实例仅 `Start()` 供 MICBIAS（PA=NC）——**硬件硬约束，不可移除**。

## 3. CI1302 侧改动清单

| 文件 | 改动 |
|---|---|
| `system/user_alg_config.mk` | `AIOT_AUDIO_COMPRESS_TYPE := 2`（speex→opus） |
| `app/app_main/user_config.h` | `AUDIO_COMPRESS_SPEEX_ENABLE 1→0`、`AUDIO_COMPRESS_OPUS_ENABLE 0→1` |

切到 opus 后 SDK 自动生效的宏（**勿手改**）：
`OPUS_BUILD=1`、`OPUS_MUILT_FRAME=1`、`COMPRESS_NEED_PCM_LEN=640`（40ms）、
`COMPRESS_UPLOAD_QUEUE_TIEM_SIZE=80`（每帧 80B）、`PCM_UPLOAD_QUEUE_NUM=30`；
组合条件 `AUDIO_COMPRESS_OPUS_ENABLE && NET_AUDIO_PLAY_BY_MP3` 自动关闭重采样
（`INNER_CODEC_AUDIO_IN_USE_RESAMPLE=0`，省 24KB，满足内存）。

已核对无需改：`NET_AUDIO_PLAY_BY_MP3=1`（下行 MP3）、`UART_NUM_SEND_PLAY_AUDIO_NUMBER=HAL_UART1_BASE`、
`UART_NUM_SEND_PLAY_AUDIO_BAUDRATE=921600`、`UPLOAD_PCM_DATA_ENABLE=0`（带完整协议帧）、
`AUDIO_SEND_WITH_PROTOCOL_HEADER=1`、`SYS_HEAP_SIZE=1024*120`（≥ opus+mp3 推荐 110KB）。

## 4. 三大验收点对应的设计

1. **上行 Opus 可行**：1302 侧 opus 编码（40ms CBR16kbps）→ PCM_MIDDLE 帧（含 16B 帧头 + 80B 负载）
   → ESP32 直通队列 → OpusCodecTask 只在 Listening 消费 → websocket 原样上云。
   不经过 ESP32 二次编码，码流零改造。
2. **下行 MP3 正确流畅**：每包 ≤ 1302 收包上限（1024 ≤ 1043）、有数据才回包、END 带尾包、
   预缓冲 4096B + 64KB 缓冲扛抖动、失效兜底（10s 无 GET 复位）防悬挂。
3. **高波特率串口不干扰其他逻辑**：独立 RX 任务（优先级 4）+ 独立播放任务；帧解析状态机
   校验 magic/checksum，错帧单字节重同步；上行 40ms/帧、下行 200ms/拉流，921600 下负载极低；
   16KB RX 缓冲防突发丢帧；`SendFrame` 持发送互斥，杜绝并发写串口。

## 5. 与旧实现失败根因的对照

| 旧实现问题（CI1302临时日志.log） | 新实现处理 |
|---|---|
| 回包长度失控（2048/1408/432/448/976/0） | 每包 ≤ 请求量（1024 上限），请求值防御性钳制 |
| 空包/0 长度回包 → "rcv play null data" | 有数据才回；无数据且流未结束 → 跳过本次 GET（1302 200ms 后重请） |
| 空负载 PLAY_DATA_END → SAP "wait audio play over" 卡死 | END 必带 shine flush 尾部；兜底改发 NET_PLAY_STOP |
| 播完不结束、状态悬挂 | tts/stop 驱动 END + 10s 无 GET 看门狗复位 + 1302 PLAY_TTS_END 异常复位 |

## 6. 联调检查清单

**构建**：
- [ ] 小智侧：`idf.py build`（需激活 ESP-IDF 环境，本机 IDF 在 `D:\Espressif\frameworks\esp-idf-v5.5.3`，
      `export.bat` 需先 `set IDF_TOOLS_PATH=D:\Espressif`）。
- [ ] 1302 侧：`project_file` 目录 `make clean && make -j8`（VS Code tasks 封装），
      检查 `user_code.bin` ≤ 分区上限（`config.ini` user_size=0x3F000）；opus 库比 speex 大，
      若超分区需 `makefile` 调整或裁剪算法。

**上行**：
- [ ] 说"小智/你好小智"唤醒 → 日志出现 WAKE_UP/VAD_START，ESP32 进入 Listening 并上报 hello。
- [ ] 说话期间持续收到 PCM_MIDDLE（80B），ESP32 日志 encode_count 递增，云端收到 Opus 语音。
- [ ] 静音 → VAD_END/PCM_FINISH → ESP32 StopListening → 云端返回结果。
- [ ] **风险点**：hello 声明 frame_duration=60ms（`OPUS_FRAME_DURATION_MS`），上行实际 40ms 帧。
      若云端拒绝/解析异常，备选：`audio_service.h` 中 `OPUS_FRAME_DURATION_MS 60→40` 全链路对齐（1302 侧不动）。

**下行**：
- [ ] 云端 TTS 开始 → ESP32 日志 "NET_PLAY_START sent"，1302 开始拉流（GET 每 ~200ms）。
- [ ] 播放流畅无卡顿；日志无 "rcv play null data" / "wait audio play over"。
- [ ] TTS 结束 → 日志 "PLAY_DATA_END payload=N"，1302 播放器正常回 IDLE，可继续下一轮对话。
- [ ] 打断（再唤醒/按键）→ 日志 "Play session aborted"，1302 立即停声。

**串口/稳定性**：
- [ ] 长时间（≥10 分钟）连续多轮对话，无串口丢帧、无任务卡死、无内存告警（`PrintHeapStats`）。
- [ ] 唤醒后立即说话（竞态窗口）首帧不丢（队列缓冲 + Listening 建立后补发）。

## 7. 已知限制
- 1302 的拉流超时机制：fetcher 每 200ms 请求一次，累计 15 次（约 3s）无数据即判定超时结束播放。
  ESP32 侧 64KB 缓冲可扛约 5.3s 停顿，但**服务器 TTS 流中断 >5s 时播放会提前结束**（1302 固件行为，无法绕过）。
- 上行时间戳（服务器 AEC 用）在直通链路缺省为 0，不影响功能，仅影响服务器侧回声消除时延对齐。
- 1302 启动提示音 `PLAY_WELCOME_EN=1` 会走本地播放，不影响 UART 链路。
