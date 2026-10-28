# 小智音频链路与 Codec 适配指南

> 本文基于本仓库（`xiaozhi-esp32-ycsc`，板型 `ycsc-esp32s3-8311-wgxl-cat`）的实际代码整理。
> 目标：① 看懂从麦克风到云端、再从云端到喇叭的完整链路；② 知道换一颗音频芯片时要动哪些地方。

---

## 目录

1. [一张图看懂](#1-一张图看懂)
2. [分层架构](#2-分层架构)
3. [ES8311 的工作流](#3-es8311-的工作流)
4. [I2C 与 I2S 的分工](#4-i2c-与-i2s-的分工)
5. [I2S DMA 到底实现在哪里](#5-i2s-dma-到底实现在哪里)
6. [框架是怎么对接 codec 的](#6-框架是怎么对接-codec-的)
7. [上行链路与音频格式](#7-上行链路与音频格式)
8. [下行链路与音频格式](#8-下行链路与音频格式)
9. [AudioProcessor 层：AFE 与空实现](#9-audioprocessor-层afe-与空实现)
10. [三条典型硬件路线](#10-三条典型硬件路线)
11. [适配新芯片：决策树与改动清单](#11-适配新芯片决策树与改动清单)
12. [验证顺序](#12-验证顺序)
13. [故障排查表](#13-故障排查表)
14. [代码地图](#14-代码地图)

---

## 1. 一张图看懂

```text
                        ┌──────────────── ESP32-S3 ────────────────┐
                        │                                          │
 麦克风 ──模拟/PDM──► ES8311 ADC ──I2S DIN──► I2S RX ──DMA──► PCM  │
                        │                                          │
                        │  AudioInputTask ─► AudioProcessor ─► Opus │──WiFi/MQTT──► 服务器
                        │                                          │
                        │  AudioOutputTask ◄─ Opus Decoder ◄─ 队列  │◄──WiFi/MQTT── 服务器
                        │        │                                 │
 喇叭 ◄──PA── ES8311 DAC ◄──I2S DOUT ◄── I2S TX ◄── DMA ◄── PCM    │
                        │                                          │
                        └──── I2C（仅配置寄存器，不传音频）────────┘
```

一句话：**I2S 负责“声音”，I2C 负责“配置”，Opus 负责“压缩”，云端负责“理解与生成”。**

---

## 2. 分层架构

| 层 | 代表文件 | 职责 |
|---|---|---|
| 应用层 | `main/application.cc` | 状态机（Idle / Listening / Speaking）、协议选择、上下行回调 |
| 音频服务层 | `main/audio/audio_service.cc` | 任务调度、编解码队列、重采样、省电 |
| 处理器层 | `main/audio/processors/*` | AEC / 降噪 / VAD / 分帧（或空实现） |
| 编解码层 | `managed_components/78__esp-opus-encoder` | Opus 编码/解码、重采样 |
| **板级适配层** | `main/audio/codecs/*_audio_codec.cc` | **把 I2S / I2C / GPIO 组装成一颗“codec 设备”** |
| 芯片驱动层 | `managed_components/espressif__esp_codec_dev/device/es8311/es8311.c` | 寄存器序列、时钟分频、上下电 |
| 通用设备框架 | `esp_codec_dev.c` + `platform/*` | `data_if` / `ctrl_if` / `gpio_if` 的抽象与分派 |
| 硬件驱动层 | IDF `esp_driver_i2s`、`esp_driver_i2c` | DMA 环形缓冲、I2C 事务 |

**关键认知：`es8311_audio_codec.cc` 不是驱动，是“适配器”。** 它不定义任何寄存器位，只负责：选哪些引脚、用哪个 I2S 口、什么时候上电、把上层 4 个动作转给谁。同目录的 `es8374 / es8388 / es8389` 结构几乎一样，只是换了个芯片。

---

## 3. ES8311 的工作流

### 3.1 硬件连接（本板）

| 信号 | GPIO | 说明 |
|---|---|---|
| MCLK | 38 | 主时钟，ESP32 输出（codec 是 Slave） |
| BCLK | 39 | 位时钟 |
| WS/LRCK | 41 | 帧同步 |
| DIN | 40 | 麦克风数据 → ESP32 |
| DOUT | 42 | 播放数据 → codec |
| PA | 43 | 功放使能，普通 GPIO |
| SDA | 1 | I2C 数据 |
| SCL | 2 | I2C 时钟 |
| I2C 地址 | 0x30（7 位 0x18） | |

采样率：输入输出均 **16 kHz**（`config.h`）。

### 3.2 初始化时序

```text
Board::InitializeI2c()                建 I2C 主机总线（SDA/SCL）
Board::GetAudioCodec()                new Es8311AudioCodec(...)      ← 只建对象，不碰芯片
Application::Start()
  └ AudioService::Initialize(codec)
      └ codec->Start()
          ├ i2s_channel_enable(tx/rx)                                  ← 只跑时钟
          └ EnableInput(true) / EnableOutput(true)
              └ UpdateDeviceState()
                  ├ esp_codec_dev_new
                  ├ esp_codec_dev_open  → es8311_open()                ← I2C 写 ~20 个寄存器
                  │                     → es8311_set_fs()              ← 时钟分频 + 数据格式
                  │                     → es8311_enable(1)             ← 上下电 + 拉 PA
                  ├ set_in_gain(30 dB)
                  └ set_out_vol(70)
```

要点：

- **构造函数不碰硬件**，连一次 I2C 寻址都没有。
- 芯片真正上电发生在“第一次需要录音或放音”时，是**惰性**的。
- 先开 I2S 时钟，再配 codec，顺序不能反（codec 依赖 MCLK 才能锁存寄存器）。

### 3.3 运行期

- `Read()` → `esp_codec_dev_read` → I2S RX DMA，**全程不碰 I2C**。
- `Write()` → `esp_codec_dev_write` → I2S TX DMA，**全程不碰 I2C**。
- 唯一运行期 I2C 写入：用户调音量 → 写 `REG32`。

### 3.4 空闲省电

`AudioService` 每秒检查一次 `last_input_time_ / last_output_time_`：

```text
15 秒没有输入也没有输出
  → EnableInput(false) + EnableOutput(false)
  → UpdateDeviceState() → esp_codec_dev_close → es8311_suspend()   ← 全芯片下电 + PA 拉低
下次需要时再自动 open。
```

硅麦方案没有这个能力（见第 10 节）。

---

## 4. I2C 与 I2S 的分工

| | I2S | I2C |
|---|---|---|
| 传什么 | PCM 音频流 | 寄存器配置 |
| 数据量 | 32 KB/s 起 | 几十字节/次 |
| 时机 | 持续 | 初始化、调音量、上下电、调试 dump |
| 谁产生时钟 | ESP32（master） | ESP32 |
| 音频期间是否活动 | 是 | **静默** |

### I2C 控制的寄存器一览

| 控制内容 | 寄存器 | 驱动函数 |
|---|---|---|
| 复位、主从、上电 | 0x00 | `es8311_open / start` |
| MCLK 分频、BCLK/LRCK 分频 | 0x01–0x08 | `es8311_config_sample` |
| I2S 格式、位宽 | 0x09 / 0x0A | `es8311_config_fmt / set_bits_per_sample` |
| MIC PGA 增益 | 0x16 | `es8311_set_mic_gain` |
| ADC 音量 | 0x17 | `es8311_start` |
| DAC 音量 / 静音 | 0x32 / 0x31 | `es8311_set_vol / set_mute` |
| ADC/DAC 上下电、VMID | 0x0D / 0x0E / 0x12 | `es8311_start / suspend` |
| DMIC 选择、模拟参考 | 0x14 / 0x44 | `es8311_start / open` |

### I2C 不负责的事

- PCM 数据（走 I2S）
- 时钟 MCLK/BCLK/WS（走 I2S 外设）
- 功放开关（走 GPIO43）
- AEC / VAD / 编码（走 ESP32 软件）

### 两个已知实现细节

1. `es8311_open` 里把 `REG44 = 0x08` **连写两次**，注释说明“首次 I2C 写可能失败”——官方 workaround。
2. 初始化过程**不校验芯片 ID**（0xFD/0xFE）。I2C 不通时表现为 `esp_codec_dev_open` 失败，而不是“找不到芯片”。

---

## 5. I2S DMA 到底实现在哪里

**项目里只配置，不实现。** 真正的环形缓冲与搬运在 ESP-IDF 与 GDMA 硬件里。

```text
codec->Read()                      main/audio/codecs/es8311_audio_codec.cc
 └ esp_codec_dev_read()            esp_codec_dev.c
    └ _i2s_data_read()             platform/audio_codec_data_i2s.c
       └ i2s_channel_read()        esp_driver_i2s/i2s_common.c
          └ msg_queue（GDMA 中断投递描述符）
             └ GDMA 硬件写入内部 RAM
```

| 环节 | 位置 | 关键行为 |
|---|---|---|
| 配置 DMA 规模 | `audio_codec.h`：`AUDIO_CODEC_DMA_DESC_NUM=6`、`AUDIO_CODEC_DMA_FRAME_NUM=240` | 6 个描述符 × 240 帧 |
| 分配描述符 | IDF `i2s_common.c` `i2s_allocate_dma_buffer` | 内部 RAM，`STAILQ` 串成环形链表 |
| 挂接 GDMA | IDF `i2s_common.c` `i2s_install_dma_isr` | `gdma_new_ahb_channel` + `gdma_connect` |
| DMA 完成中断 | IDF `i2s_dma_rx_callback` | 只 `xQueueSendFromISR(&buf)`，**不拷贝数据** |
| 任务侧取数 | `i2s_channel_read` | `xQueueReceive` + `memcpy` 到调用者 buffer |

**关键点：中断里只投递指针，memcpy 在任务上下文完成。**

按 16 kHz / 16 bit 单声道估算：240 帧 ≈ 15 ms，6 个描述符 ≈ 90 ms 环形缓冲；应用层一次读 60 ms（960 点），约跨 4 个描述符。

---

## 6. 框架是怎么对接 codec 的

框架只认基类 `AudioCodec*`，不认识具体芯片。对接点共 4 处：

### 6.1 工厂约定

`Board` 的纯虚函数 `GetAudioCodec()` 是唯一入口。`Board::GetInstance()` → `Application::Start()` → `AudioService::Initialize(codec)`。

### 6.2 行为约定

`AudioCodec` 只有两个**纯虚**函数：

```cpp
virtual int Read(int16_t* dest, int samples) = 0;       // 录
virtual int Write(const int16_t* data, int samples) = 0; // 放
```

其余可覆写：`Start`、`EnableInput`、`EnableOutput`、`SetOutputVolume`、`OutputData`、`InputData`，以及能力查询 getter（采样率、通道数、是否双工、是否需要参考通道）。框架靠这些 getter 决定要不要加重采样、要不要开 AEC 参考。

### 6.3 生命周期约定

```text
AudioService::Initialize
  ├ codec->Start()                      开 I2S、上电
  ├ OpusDecoder(codec->output_sample_rate())
  ├ OpusEncoder(16000)
  ├ 输入采样率 != 16k → 建输入重采样器
  └ AFE / WakeWord 按通道数初始化

AudioService::Start
  ├ AudioInputTask   每 60ms：Read() → Processor → 编码 → 发送队列
  ├ AudioOutputTask  有数据时：Write()
  └ 省电定时器       每秒检查，15 秒空闲关闭输入输出
```

### 6.4 编译约定

- `main/CMakeLists.txt` 的 `SOURCES` 写死列出所有 codec 实现，新增文件必须加一行。
- 板子由 `Kconfig.projbuild` 的 `BOARD_TYPE` + CMake 的 `if/elseif` 分支决定。

---

## 7. 上行链路与音频格式

### 7.1 格式在握手中写死

WebSocket / MQTT 的 hello 消息：

```json
{
  "type": "hello",
  "version": 2,
  "transport": "websocket",
  "features": { "aec": true, "mcp": true },
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

**上行固定：Opus / 16 kHz / 单声道 / 60 ms 一包。**

### 7.2 各环节格式

| 环节 | 格式 |
|---|---|
| 麦克风 → codec | 模拟波形，或 PDM / 数字流 |
| codec → I2S | PCM，int16 小端，16 kHz，单声道 |
| 采样率统一 | codec 非 16k 时先重采样到 16k |
| 双通道拆分 | 2ch = 左 mic + 右 AEC 参考 |
| 分帧 | 960 采样 / 帧 = 60 ms |
| Opus 编码 | VOIP 模式、16k、mono、60ms、DTX 开、complexity 0、未显式设码率 |
| 网络封装 | v1 裸包 / v2 二进制头 / v3 二进制头 / UDP + AES-CTR |

### 7.3 三个协议版本

| 版本 | 结构 |
|---|---|
| v1 | 裸 Opus payload |
| v2 | `version(u16) + type=0(u16) + reserved(u32) + timestamp(u32) + payload_size(u32) + payload` |
| v3 | `type=0(u8) + reserved(u8) + payload_size(u16) + payload` |

MQTT/UDP 版额外加 `type(0x01)` 头和 AES-CTR 加密。

### 7.4 节流上限

`MAX_SEND_PACKETS_IN_QUEUE = 2400 / 60 = 40` 包 ≈ 2.4 秒；编码队列只有 2 个 task，超过就阻塞生产者，不丢帧。

> 若外接芯片自身能输出 Opus（自带编码），理论上可绕过 ESP32 编码；但本仓库的 `AudioCodec` 没有对应钩子，需要自行扩展基类。

---

## 8. 下行链路与音频格式

### 8.1 格式由服务器 hello 决定

设备侧默认值：`server_sample_rate = 24000`、`server_frame_duration = 60`，收到服务器 hello 后被覆盖。

**下行通常是：Opus / 24 kHz / 单声道 / 60 ms 一包。**

### 8.2 各环节格式

| 环节 | 格式 |
|---|---|
| 网络接收 | Opus 包（24k / mono / 60ms） |
| 入队条件 | 仅当状态为 `kDeviceStateSpeaking` |
| 解码 | PCM int16，24k，mono，1440 采样/帧 |
| 重采样 | 24k → codec 输出采样率（本板 16k），960 采样 |
| 播放队列 | 最多 2 个 task（约 120 ms 缓冲） |
| I2S TX | PCM int16，16k，mono |
| codec → 喇叭 | ES8311 DAC → PA → 扬声器 |

### 8.3 动态重建解码器

```cpp
void SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder 已匹配) return;
    opus_decoder_ = OpusDecoderWrapper(sample_rate, 1, frame_duration);
    if (sample_rate != codec->output_sample_rate())
        output_resampler_.Configure(sample_rate, codec->output_sample_rate());
}
```

服务器中途换采样率或帧长时解码器会重建——**所以帧长必须从包里读，不能写死**。

### 8.4 采样率不匹配会告警

```cpp
if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
    ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion");
}
```

本板 codec 16k、服务器 24k，每帧都要重采样。若能直接把 codec 配成 24k，可省掉这一步（但注意上行仍需要 16k）。

### 8.5 提示音也走同一条路

`PlaySound` 的 OGG 提示音解码后同样进入 `audio_playback_queue_`，所以喇叭侧只认一种输入：**int16 PCM @ codec 输出采样率**。

---

## 9. AudioProcessor 层：AFE 与空实现

`AudioService` 不直接消费 `Read()` 的数据，而是交给 `AudioProcessor`。

| 能力 | `AfeAudioProcessor`（AFE 开） | `NoAudioProcessor`（AFE 关，本仓库当前） |
|---|---|---|
| 设备端 AEC | 有 | 无 |
| 设备端 VAD | 有，实时回调 | 无，`voice_detected_` 恒 false |
| 降噪 / AGC | 有 | 无 |
| 说话时能否开麦 | 可以（realtime） | **不能**，必须停麦 |
| 双通道处理 | 左 mic + 右参考送 AEC | 只取左声道，右声道丢弃 |
| 分帧 | 60 ms | 60 ms（960 点） |
| PSRAM 占用 | 高 | 近零 |

编译期二选一：

```cpp
#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif
```

`Kconfig` 中 `USE_AUDIO_PROCESSOR` 依赖 `ESP32-S3/P4 && SPIRAM`；当前 `sdkconfig` 中该符号未启用，所以实际走的是 `NoAudioProcessor`。

**没有 AEC 的直接后果**：进入 Speaking 状态必须 `EnableVoiceProcessing(false)` 关麦，否则会把喇叭声录回去。

---

## 10. 三条典型硬件路线

| | 有 codec 芯片（ES8311） | 硅麦直连 | 双芯片（ES7210 + ES8311） |
|---|---|---|---|
| I2C | 有 | **无** | 有（两个地址） |
| esp_codec_dev | 用 | 不用 | 用两套 dev |
| 音量 | 写 `REG32` | **软件乘法** | 写 `REG32` |
| 麦克风增益 | 写 `REG16` | 代码位移量 | 写 ES7210 寄存器 |
| 下电省电 | 支持 suspend | **不支持** | 支持 |
| 采样率自由度 | 收发必须相同 | 可不同（两套时钟） | 必须相同（共用 BCLK/WS） |
| 代码 | `es8311_audio_codec.cc` | `no_audio_codec.cc` | `box_audio_codec.cc` |

### 10.1 硅麦（PDM / I2S 数字麦）

无需 codec 芯片，直接操作 I2S：

| 类型 | 引脚 | 类 |
|---|---|---|
| PDM 麦 | CLK + DATA | `NoAudioCodecSimplexPdm` |
| I2S 数字麦 | SCK + WS + SD | `NoAudioCodecSimplex` |
| 收发共用一组 I2S | BCLK/WS/DOUT/DIN | `NoAudioCodecDuplex` |

实现要点：

1. PDM 必须用 `i2s_channel_init_pdm_rx_mode`，硬件 CIC 解调后直接出 16 bit。
2. **PDM 位宽固定 16 bit**，且需要 `SOC_I2S_SUPPORTS_PDM_RX` 保护。
3. **音量靠软件**：`(volume/100)^2 * 65536`，int16 升到 int32 域再写 I2S。
4. I2S 数字麦读 32 bit 帧，`>>12` 取高 20 位再钳位到 int16（相当于 16 倍数字增益，灵敏度过高时改移位量）。
5. PDM 的 `Read` 必须覆写：硬件已经是 16 bit，再移位就没声音了。
6. 没有下电能力，麦克风一直工作；需要省电要自己加电源使能脚。

### 10.2 双芯片（ADC 与 DAC 分离）

核心思路：**一条 PCM 管道 + 两个寄存器域 + 两个 `esp_codec_dev` 句柄**。

```text
I2S_NUM_0 ─┬─ tx (STD, 1ch) ─┐
           └─ rx (TDM, 4ch) ─┴─ data_if_ ─┬─ output_dev_ ← es8311_codec_if ← out_ctrl_if_(I2C)
                                          └─ input_dev_  ← es7210_codec_if  ← in_ctrl_if_(I2C)
```

要点：

1. `codec_mode` 分别设为 `WORK_MODE_DAC` / `WORK_MODE_ADC`，`dev_type` 设为 OUT / IN。
2. 两个 dev **共用同一个 `data_if_`**。
3. 输出用 STD，输入用 TDM（四麦阵列），共用 MCLK/BCLK/WS。
4. `fs.channel` 填 **TDM 总 slot 数**，`channel_mask` 填**实际要的 slot**；硬件只把选中的 slot 写入 DMA。
5. `input_channels_ = input_reference ? 2 : 1`，框架据此拆 mic/参考通道。
6. 两个 dev 的采样率必须一致（共用时钟），否则 `assert` 或配置被拒。
7. 开通顺序会影响时钟：`esp_codec_dev` 有 `check_fs_compatible` 补丁，会把 TX 时钟同步重配。
8. PA 由 ES8311 驱动的 `gpio_if` 控制，这个类里没有 `pa_pin_` 成员。
9. `box_audio_codec.cc` 里 `i2c_cfg.port` 写死为 1，照抄时注意改成实际端口。

---

## 11. 适配新芯片：决策树与改动清单

### 11.1 先答这 8 个问题

| 问题 | 影响 |
|---|---|
| I2S、PCM/TDM 还是 UART？ | 是否需要自写 data_if |
| Master 还是 Slave？ | `i2s_chan_config_t.role` |
| 采样率？ | 非 16k 会自动重采样（费 CPU） |
| 位宽、通道数？ | `esp_codec_dev_sample_info_t` |
| 是否 2 通道（mic + 参考）？ | `input_reference_` → 是否开 AEC |
| 音量/增益是寄存器还是数字电位器？ | `SetOutputVolume` 实现方式 |
| 有无 PA 使能脚、极性？ | `pa_pin` / `pa_inverted` |
| 上电时序（MCLK 先给？复位脚？LDO？） | `Start()` 顺序 |

### 11.2 三种情况

**情况 A：`esp_codec_dev` 已支持该芯片（ES8374/8388/8389 等）**

最省事，照抄 `es8311_audio_codec.cc` 骨架：

1. 构造函数：设能力字段 → 建 I2S → 建 `data_if_ / ctrl_if_ / gpio_if_` → `xxx_codec_new`
2. `UpdateDeviceState()`：惰性 open/close + PA
3. `Read` / `Write`：一行转发
4. `EnableInput/EnableOutput`：加锁 + 去重 + `UpdateDeviceState`
5. 加进 `main/CMakeLists.txt`
6. 板级 `config.h` 加引脚宏
7. 板级 `<board>.cc` 的 `InitializeI2c()` 配 SCL/SDA，`GetAudioCodec()` 返回新对象

**情况 B：框架不支持，但仍是 I2S + I2C 寄存器芯片**

- 推荐：自己实现一份 `audio_codec_if_t`（`open/enable/set_fs/set_vol/set_mic_gain/mute/set_reg/get_reg/dump_reg/close`），继续复用 `esp_codec_dev` 的组合能力。
- 省事：跳过 `esp_codec_dev`，在适配类里直接持有 I2C 句柄 + `i2s_channel_read/write`（即 `NoAudioCodec` 的模式）。

**情况 C：非 I2S 设备（UART / 自带 DSP / 自带 Opus）**

本仓库的 `AudioCodec` 只提供 `Read / Write / Start / EnableInput / EnableOutput / SetOutputVolume` 等基础虚函数，**没有** Opus 直通或会话事件钩子。非 I2S 设备（UART / 自带 DSP）需要在 `Read / Write` 内自行实现协议收发，必要时扩展基类。

### 11.3 文件级改动清单

| # | 文件 | 动作 |
|---|---|---|
| 1 | `main/audio/codecs/xxx_audio_codec.h/.cc` | 新增适配类 |
| 2 | `main/CMakeLists.txt`（SOURCES） | 加一行 |
| 3 | `main/boards/<board>/config.h` | 引脚、采样率、I2C 地址 |
| 4 | `main/boards/<board>/<board>.cc` | `InitializeI2c()` + `GetAudioCodec()` |
| 5 | `main/Kconfig.projbuild` | 新板子：加 `config BOARD_TYPE_XXX` |
| 6 | `main/CMakeLists.txt`（if/elseif） | 新板子：加分支 |

### 11.4 最小骨架

```cpp
class Es8xxxAudioCodec : public AudioCodec {
public:
    Es8xxxAudioCodec(void* i2c_bus, i2c_port_t port, int in_rate, int out_rate,
                     gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                     gpio_num_t dout, gpio_num_t din,
                     gpio_num_t pa_pin, uint8_t addr) {
        duplex_ = true;
        input_channels_ = 1;
        input_reference_ = false;
        input_sample_rate_ = in_rate;
        output_sample_rate_ = out_rate;
        pa_pin_ = pa_pin;

        CreateDuplexChannels(mclk, bclk, ws, dout, din);

        audio_codec_i2s_cfg_t i2s_cfg = { I2S_NUM_0, rx_handle_, tx_handle_ };
        data_if_ = audio_codec_new_i2s_data(&i2s_cfg);

        audio_codec_i2c_cfg_t i2c_cfg = { port, addr, (i2c_master_bus_handle_t)i2c_bus };
        ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);

        gpio_if_ = audio_codec_new_gpio();

        es8xxx_codec_cfg_t cfg = {};
        cfg.ctrl_if = ctrl_if_;
        cfg.gpio_if = gpio_if_;
        cfg.pa_pin = pa_pin;
        codec_if_ = es8xxx_codec_new(&cfg);
    }

    int Read(int16_t* dest, int samples) override {
        if (input_enabled_) esp_codec_dev_read(dev_, dest, samples * 2);
        return samples;
    }
    int Write(const int16_t* src, int samples) override {
        if (output_enabled_) esp_codec_dev_write(dev_, (void*)src, samples * 2);
        return samples;
    }
    // EnableInput / EnableOutput / UpdateDeviceState：照抄 es8311 版本
};
```

---

## 12. 验证顺序

不要一上来就跑整机，按层验证：

1. **I2C 通不通**：读一次寄存器（类似 `es8311_dump`），确认 ACK。
2. **I2S 时钟通不通**：示波器看 MCLK / BCLK / WS。
3. **单放音**：`SetOutputVolume` + `Write` 一段固定 PCM，确认 PA 电平和极性。
4. **单录音**：进入回环测试（长按 BOOT），听自己的声音。
5. **跑整机**：唤醒 → 上行 → 云端识别 → 下行播放。

---

## 13. 故障排查表

| 现象 | 常见原因 |
|---|---|
| 完全没声音 | 采样率与 MCLK 分频不匹配（`coeff_div` 表未命中） |
| 只有杂音 / 全是噪声 | PDM 按 24 bit 读；或 I2S 数字麦移位量不对 |
| I2C 打不开 | 8 位/7 位地址搞混（0x30 vs 0x18）；SDA/SCL 接反 |
| 上电偶发失败 | ES8311 首次写失败，需重复写 `REG44`（官方 workaround） |
| 一放音就啸叫 | 没有 AEC 却同时开麦；检查 Speaking 时是否关麦 |
| 编译/启动 `assert` | 输入输出采样率不等（`assert(rate 相等)`） |
| 播放声音变调 | 服务器采样率与 codec 不一致，重采样未配置 |
| 谈话 15 秒后断 | 省电逻辑正常关闭 codec；检查是不是误判空闲 |
| 非 PCM 设备下 `Read` 一直失败 | 设备输出的不是 PCM，需自行解析或扩展基类 |
| 音量调不动 | 硅麦方案音量是软件乘法，不走寄存器 |

---

## 14. 代码地图

| 文件 | 看什么 |
|---|---|
| `main/application.cc` | 状态机、协议选择、上下行回调 |
| `main/audio/audio_service.cc` | 三个任务、队列、重采样、省电 |
| `main/audio/audio_codec.h` | 抽象接口、DMA 参数宏 |
| `main/audio/codecs/es8311_audio_codec.cc` | **单芯片适配范本** |
| `main/audio/codecs/no_audio_codec.cc` | **硅麦 / 无 codec 范本** |
| `main/audio/codecs/box_audio_codec.cc` | **双芯片范本** |
| `main/audio/wake_word.h` | 唤醒词抽象接口 |
| `main/audio/processors/afe_audio_processor.cc` | AEC / VAD / 分帧 |
| `main/audio/processors/no_audio_processor.cc` | 透传与 60 ms 分帧 |
| `main/protocols/websocket_protocol.cc` | hello 协商、二进制封装 |
| `main/protocols/mqtt_protocol.cc` | UDP + AES-CTR 封装 |
| `managed_components/.../device/es8311/es8311.c` | ES8311 寄存器序列 |
| `managed_components/.../platform/audio_codec_data_i2s.c` | I2S 数据接口 |
| `managed_components/.../platform/audio_codec_ctrl_i2c.c` | I2C 控制接口 |
| `D:\Espressif\frameworks\esp-idf-v5.5.3\components\esp_driver_i2s\i2s_common.c` | DMA 环形缓冲、中断、`i2s_channel_read` |

---

## 附：三句话总结

1. **有 codec 芯片时，你是在“配置”声音；用硅麦时，你是在“直接搬运”声音。**
2. **I2C 只决定芯片怎么工作，I2S 才是声音本身。** 音频传输期间 I2C 全程静默。
3. **适配一颗新芯片 = 选对路线（单芯片 / 硅麦 / 双芯片 / 非 I2S）+ 填对 7 个文件 + 按 5 步验证。**
