# CI1302 ↔ ESP32 串口语音集成说明

> **注意**：本文记录早期“上行裸 PCM”方案，已废弃。
> 当前实现为上行 **Speex（1302 编码）→ ESP32 解码 → Opus 上云**、下行 MP3 拉流，
> 请以仓库根目录 `CI1302_UART_音频通路集成说明.md` 为准。

本文档说明小智 ESP32 端与 CI1302 离线语音芯片之间的 UART 交互协议处理，
对应实现文件 `main/audio/codecs/uart_audio_codec.h/.cc`。

## 1. 总体架构

```
            唤醒/AEC/降噪/VAD（CI1302 完成）
                                    │
 麦克风 ──► CI1302 ──PCM 16kHz 单声道──► ESP32(UartAudioCodec::Read)
                                              │ 现有 Opus 编码管线（60ms 帧）
                                              ▼
                                           云端 ASR/LLM
                                              │ Opus 下发
                                              ▼
                        ESP32 解码 PCM ──► shine MP3 编码 ──► UART ──► CI1302 播放 ──► 喇叭
```

- 上行：CI1302 上传裸 PCM（`AUDIO_COMPRESS_RECORD_DISABLE`），ESP32 不做任何前处理，
  直接进现有 Opus 编码管线（hello 帧长等云端协议参数完全不变）。
- 下行：CI1302 只能播 MP3，ESP32 用 shine 定点编码器把云端 PCM 实时转 MP3。
- 交互：CI1302 本地唤醒词/VAD 驱动小智聆听状态，触摸键 `ToggleChatState` 保持原有功能。

## 2. UART 参数

| 参数 | 值 |
|---|---|
| 波特率 | 921600，8N1，无流控 |
| ESP32 端口 | `AUDIO_UART_PORT`（默认 UART_NUM_1） |
| 引脚 | `AUDIO_UART_TX_PIN / AUDIO_UART_RX_PIN`（config.h 中为占位值，按实际硬件修改） |

## 3. 帧格式（小端）

16 字节帧头 + 负载：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | magic u32 | `0x5A5AA5A5` |
| 4 | checksum u16 | 负载逐字节累加（不含帧头） |
| 6 | type u16 | 命令字 |
| 8 | len u16 | 负载长度（无负载为 0） |
| 10 | version u16 | 普通命令 `0x0100`；PCM_MIDDLE 用作包序号（ESP32 端忽略） |
| 12 | fill_data u32 | `0x12345678` |

ESP32 接收时校验 magic 与 checksum，失败逐字节滑动重新同步（见 `ParseRxBuffer`）。

## 4. 上行命令处理（CI1302 → ESP32）

| 命令字 | 名称 | ESP32 处理 |
|---|---|---|
| 0x0102 | WAKE_UP | `Application::StartListening()`（idle 时打开音频通道并进入聆听） |
| 0x0108 | VAD_START | `Application::StartListening()`（说话中则打断播报并聆听，即 barge-in） |
| 0x0103 | VAD_END | `Application::StopListening()` |
| 0x0106 | PCM_FINISH | `Application::StopListening()`（与 VAD_END 幂等） |
| 0x0109 | EXIT_WAKE_UP | `Application::StopListening()`（兜底） |
| 0x0105 | PCM_MIDDLE | 负载为 16kHz 单声道 16bit PCM（每包 512 字节 = 16ms，`PCM_ALG_FRAME_LEN=512`），写入上行流缓冲供 `Read()` 消费；Speex 模式下此处是 43 字节压缩码流，与本 codec 不兼容 |
| 0x0101 | LOCAL_ASR_RESULT_NOTIFY | 仅记录日志（本地命令词结果，可扩展为本地指令） |
| 0x020A | PLAY_DATA_GET | 负载 u32 为请求长度，ESP32 从下行 MP3 缓冲取 ≤请求长度数据，回 PLAY_DATA_RECV |
| 0x020D | PLAY_TTS_END | CI1302 播完通知，记录日志 |
| 0x0601 | CIAS_AUDIO_SYS_READY | CI1302 就绪，ESP32 重新下发当前音量 |
| 0x0801 | CIAS_CMD_EXEC_STATE | 命令应答（2 字节原命令 + 1 字节结果），记录日志 |

`StartListening`/`StopListening` 内部均通过 `Schedule` 切到主任务执行，UART 接收任务直接调用是安全的；
重复调用幂等（已聆听时再 Start 为空操作）。

## 5. 下行播放流程（ESP32 → CI1302）

```
ESP32 首帧 PCM 到达 Write()
   │  发送 NET_PLAY_START(0x0201)      ──► CI1302 启动 MP3 播放器
   ▼
CI1302 ── PLAY_DATA_GET(0x020A, 请求长度, MP3 模式单次 1024B) ──► ESP32
   ▼
ESP32 ── PLAY_DATA_RECV(0x020B, ≤请求长度的 MP3 数据) ──► CI1302
   │        （循环直至本路音频发完）
   ▼
Write() 空闲 > 800ms 且下行缓冲已被拉空
   │  发送 PLAY_DATA_END(0x020C, 负载为 shine flush 尾包)
   ▼
CI1302 播完残余数据 ── PLAY_TTS_END(0x020D) ──► ESP32（日志）
```

- MP3 参数：MPEG-II Layer3，16kHz 单声道，48kbps（`MP3_BITRATE` 宏可调），CI1302 ROM 解码器原生支持该格式。
- 下行播放采用预缓冲机制：`Write()` 开始编码后先攒够约 2KB MP3（或 400ms 超时）才发送 `NET_PLAY_START`，
  且每次响应 `PLAY_DATA_GET` 时尽量凑满整包再回，避免开局零头短包导致 CI1302 播放器状态机异常。
- 每路播放结束时 shine 编码器 flush 并重新初始化，保证下一路 MP3 流有干净的帧头。
- 播放中途有新 PCM 到达（barge-in 后的新回复）时不会触发 END，数据连续编码下发；
  800ms 空闲才判定一路结束。

## 6. 音量

ESP32 侧 `SetOutputVolume(0-100)` → 映射为 1-7 档，发送 SET_AUDIO_VOLUME(0x0117)，
负载 1 字节。CI1302 上电就绪（0x0601）后会自动重发当前音量。

## 7. ESP32 端文件与启用方式

启用方式：`idf.py menuconfig` → Xiaozhi Assistant → **Use CI1302 UART Audio Codec**
（`CONFIG_AUDIO_CODEC_UART_CI1302`，仅在 BOARD_TYPE_YCSC_ESP32S3_8311_WGXL_CAT 下可见）。
选中后 Kconfig 依赖关系自动关闭 ESP32 侧 AFE 唤醒词与音频处理器（NoAudioProcessor 直通），
无需手动修改 sdkconfig；不选则固件行为与原项目完全一致（ES8311 + 麦克风）。

| 文件 | 说明 |
|---|---|
| `main/audio/codecs/uart_audio_codec.h/.cc` | 全部新增逻辑：UART 初始化、协议解析、上行 PCM、下行 MP3 编码与拉流、聆听状态驱动 |
| `components/shine/` | vendor 的 shine MP3 编码器（toots/shine，含自有 CMakeLists） |
| `main/Kconfig.projbuild` | 新增 `AUDIO_CODEC_UART_CI1302` 开关，并给 `USE_AFE_WAKE_WORD` / `USE_CUSTOM_WAKE_WORD` / `USE_AUDIO_PROCESSOR` 的 depends 追加 `!AUDIO_CODEC_UART_CI1302` |
| `main/CMakeLists.txt` | 仅新增一行源文件 |
| `main/boards/ycsc-esp32s3-8311-wgxl-cat/config.h` | 新增 UART 端口/波特率/引脚宏（占位引脚，按硬件修改） |
| `main/boards/ycsc-esp32s3-8311-wgxl-cat/ycsc_esp32s3_8311_wgxl_cat.cc` | `GetAudioCodec()` 按 `CONFIG_AUDIO_CODEC_UART_CI1302` 二选一（#if/#else） |

`sdkconfig`、`sdkconfig.defaults`、`audio_service`、`application` 等原有代码零修改。

## 8. CI1302 端需要的配置（在 CI1302 工程中修改，非本仓库）

`projects/offline_asr_llm_aiot_uart_sample/app/app_main/user_config.h`：

```c
#define AUDIO_COMPRESS_RECORD_DISABLE 1   // PCM 上传
#define AUDIO_COMPRESS_SPEEX_ENABLE   0   // 必须关掉，三个压缩宏只能三选一（有 #error 约束）
#define AUDIO_COMPRESS_OPUS_ENABLE    0
#define NET_AUDIO_PLAY_BY_MP3         1   // MP3 播放
#define NET_AUDIO_PLAY_BY_PCM         0   // 必须显式置 0，与 MP3 互斥
```

`sdkconfig` 之外的压缩类型开关在 **`system/user_alg_config.mk`**（不是项目 `project_file/makefile`，
项目 makefile 第 4 行 include 它，并在第 87 行按此值决定链接 `libspeex.a` / `libopus.a`）：

```makefile
AIOT_AUDIO_COMPRESS_TYPE       := 0   # 0:不压缩(PCM) 1:speex 2:opus 3:g722
```

改完这一项后，项目 makefile 会自动与 `components/bnpu_core_alg_pro/project_file/config.a`
里的宏快照比对，不一致就同步快照并重建 BNPU 算法库（首次构建会慢一截，属正常）。

链接脚本里的堆大小（`lds/ci130x_asr_alg_mcu.lds` 及其 `_temp.lds` 模板）：
`SYS_HEAP_SIZE = (1024*130)`（PCM 上传/播放缓冲都比 Speex/MP3 组合更大）。

该组合（PCM 上传 + MP3 播放 + 全双工）是 SDK 显式支持的满血配置，
命令词自学习（CWSL）、重采样、单麦 AEC、NN 降噪/VAD 均不受影响。

## 9. 可调参数（uart_audio_codec.h 内宏）

| 宏 | 默认 | 说明 |
|---|---|---|
| `MP3_BITRATE` | 48 | 下行 MP3 码率 kbps |
| `PLAY_END_IDLE_TIMEOUT_MS` | 800 | 判定一路播放结束的 Write 空闲时长 |
| `READ_TIMEOUT_MS` | 250 | 上行 PCM 等待超时，超时补静音保持管线存活 |
| `PLAY_DATA_MAX_CHUNK` | 2048 | 单次 PLAY_DATA_RECV 最大负载 |
