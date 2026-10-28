# ESP32 对接 CI130X（IIS 音频工程）适配提示词

> 本文档由 CI130X 固件工程 `YCSC-CI13XX_SDK_LLM_AIOT_V3.0.19 / projects/offline_asr_llm_aiot_iis_sample` 源码逐行核实整理，可直接作为提示词交给 ESP32 侧的 AI / 开发者使用。所有数值均为源码中的真实配置。

---

## 0. 角色与任务（给 ESP32 侧 AI 的指令）

你是一名 ESP32（ESP-IDF）固件工程师。你的 MCU 通过 **IIS（I2S）+ UART** 与启英泰伦 **CI1302** 语音芯片对接。CI1302 被当作一颗**由 ESP32 全权控制的音频 Codec**（类比 ES8311 之于主控的关系）：**录音使能、播放使能、音量、静音全部由 ESP32 通过 UART 指令控制**，芯片本身不自主决定录/播；同时它附带唤醒/离线识别能力，识别结果作为事件上报给 ESP32。架构分工：

- CI130X 负责：MIC 采集、唤醒词识别、离线命令词识别（ASR）、VAD、AEC 回声消除、NN 降噪，并把处理后的 PCM 通过 IIS 输出；同时接收 ESP32 通过 IIS 送入的 PCM 并用喇叭播放。
- ESP32 负责：联网、云端 ASR/LLM/TTS、音频流的收发调度、整机主控逻辑。

你的任务是按本文档实现 ESP32 侧的：① I2S 从机收发驱动；② UART 协议栈（帧解析/组帧/校验/应答）；③ 对话状态机。不要修改 CI130X 侧的任何行为约定。

---

## 1. 系统架构与数据流

```
                ┌──────────────────────────── CI130X ────────────────────────────┐
  MIC ──► 内部Codec ADC ──► AEC/NN降噪/VAD/唤醒/ASR ──► IIS0 SDO ────────────────┼──► ESP32 (I2S RX)   【上传流：16kHz/16bit/立体声PCM】
 ESP32 (I2S TX) ──► IIS0 SDI ──► 播放任务(16ms/帧) ──► 内部Codec DAC ──► 功放 ──► 喇叭  【下行流：16kHz/16bit/立体声PCM】
                └── UART1(1306) / UART0(1302) @921600 ──► 控制协议（双向指令）◄──┘
```

三条独立数据流：
1. **上传流（CI→ESP32）**：算法前处理后的音频经 IIS SDO 输出。默认上传**降噪后**音频。
2. **下行流（ESP32→CI）**：ESP32 把 TTS/提示音 PCM 经 IIS SDI 送入，CI 内部任务每 16ms 搬运一次到喇叭。
3. **控制流（双向 UART）**：唤醒/VAD/识别结果等事件上报；录音开关、播放开关、音量等指令下发。

---

## 2. 硬件连接

### 2.1 IIS 引脚（CI1306 板级 CI-D06GT01D；CI1302 板级 CI-D02GS01J 引脚相同）

| 信号 | CI130X 引脚 | 方向 | 说明 |
|---|---|---|---|
| IIS0_SDO | PA4 | CI → ESP32 | 上传音频数据（MIC 前处理结果） |
| IIS0_SDI | PA2 | ESP32 → CI | 下行播放音频数据 |
| IIS0_SCLK(BCLK) | PA5 | CI → ESP32 | 位时钟，CI 输出 |
| IIS0_LRCLK(WS) | PA3 | CI → ESP32 | 声道时钟，CI 输出 |
| IIS0_MCLK | PA6 | CI → ESP32 | 主时钟，CI 输出 |

### 2.2 控制串口

| 芯片 | 串口 | TX（CI→ESP32） | RX（ESP32→CI） | 波特率 |
|---|---|---|---|---|
| **CI1302（当前目标）** | **UART0** | **PB5** | **PB6** | **921600, 8N1** |
| CI1306（备选） | UART1 | PB7 | PC0 | 921600, 8N1 |

> CI1302 引脚少，IIS 占用 PA2~PA6 后 UART1/UART2 不可再用，协议口只能是 UART0，且与日志口冲突（固件已设 `USE_1302_UART0_MODE=2`：UART0 给协议通信，日志关闭）。

---

## 3. IIS 音频接口规范（ESP32 I2S 配置依据）

以下全部来自固件源码的真实配置（`audio_pre_tslt_out_info` / `pre_rslt_out_sound_info` / `play_sound_info`）：

| 参数 | 值 |
|---|---|
| 外设 | IIS0 |
| **主从模式** | **CI130X 为主机（MASTER）**，输出 BCLK/LRCLK/MCLK；**ESP32 必须配为从机（SLAVE）** |
| 数据格式 | 标准 I2S（Philips，`IIS_DF_IIS`） |
| 采样率 | 16000 Hz |
| 位宽 | 16 bit |
| 声道 | 立体声（2 声道） |
| BCLK : LRCK | 64（BCLK = 1.024 MHz） |
| MCLK | 256 × FS = 4.096 MHz，由 CI 输出 |
| 帧长 | 每帧 256 个采样点 = **16 ms**；立体声下每帧 = 256×2×2 = **1024 字节** |

### 3.1 上传流细节（ESP32 I2S RX）
- **左右两个声道的数据完全相同**（固件把单声道数据复制到 L/R），ESP32 只取任一声道即可得到单声道音频。
- 数据内容可配置为降噪后或降噪前音频（默认降噪后）。
- 未处于有效录音状态时，IIS 上无有效音频（静音/全 0），**是否输出由固件时机控制，见第 6 节**。

### 3.2 下行流细节（ESP32 I2S TX）
- 格式同上：16kHz/16bit/立体声。单声道 TTS 数据请复制到两个声道后发送。
- CI 内部播放任务每 16ms 从 IIS 输入缓冲搬 1024 字节到喇叭，**ESP32 必须按 16kHz 实时速率连续供数**，断流会导致播放异常。
- 播放窗口由 `MASTER_PLAY_STATE` 指令控制（见第 6 节）。

### 3.3 ESP-IDF I2S 配置要点
- 使用同一个 I2S 端口同时做 RX（收 MIC）和 TX（送播放），角色均为 **slave**；MCLK 引脚配为输入。
- 时钟：`sample_rate_hz = 16000`，`mclk_multiple = I2S_MCLK_MULTIPLE_256`，`clk_src` 对 slave 不重要（时钟来自 CI）。
- slot：standard 模式、`I2S_STD_SLOT_PHILIPS`（标准 I2S 时序）、16bit、双声道。
- 建议按 1024 字节/帧（16ms）的整数倍做 DMA 缓冲，例如 4~8 帧。

---

## 4. UART 控制协议

### 4.1 帧格式（小端序，16 字节帧头 + N 字节 payload）

```c
typedef struct {
    uint32_t magic;      // 固定 0x5A5AA5A5，小端存储为 A5 A5 5A 5A
    uint16_t checksum;   // payload 所有字节的累加和（uint16 自然溢出）；无 payload 时为 0
    uint16_t type;       // 命令字，见 4.2
    uint16_t len;        // payload 字节数（不含帧头）
    uint16_t version;    // 固定 0x0100
    uint32_t fill_data;  // 填充字段，一般填 0x12345678（DEF_FILL）
} cias_standard_head_t;  // 共 16 字节，无 payload 的纯指令帧总长就是 16 字节
```

组帧参考（固件侧 `broadlink_frame_create`）：`checksum = Σ payload[i]`；`version = 0x0100`；`magic = 0x5A5AA5A5`。

### 4.2 命令字总表

**A. CI → ESP32（事件上报，ESP32 需要解析）**

| 命令字 | 值 | payload | 含义 |
|---|---|---|---|
| CIAS_AUDIO_SYS_READY | 0x0601 | 无 | **上电就绪**，语音系统初始化完成，ESP32 应以此作为握手起点 |
| WAKE_UP | 0x0102 | 无 | 唤醒成功，进入对话状态，IIS 开始输出有效音频 |
| VAD_START | 0x0108 | 无 | 检测到人声开始（此时起 IIS 上的音频是有效语音） |
| VAD_END | 0x0103 | 无 | VAD 结束 |
| PCM_FINISH | 0x0106 | 无 | **一句话说完**，本段上传结束，可送云端识别 |
| SKIP_INVAILD_SPEAK | 0x0104 | 无 | 无效语音（太短），本段忽略 |
| EXIT_WAKE_UP | 0x0109 | 无 | 退出唤醒态（超时或指令触发），IIS 停止有效输出 |
| LOCAL_ASR_RESULT_NOTIFY | 0x0101 | 2 字节：命令词 ID（uint16 小端） | 离线命令词识别结果 |
| CIAS_CMD_EXEC_STATE | 0x0801 | 3 字节：[指令低字节, 指令高字节, 结果] | **指令应答**：前 2 字节=被应答的指令 type，第 3 字节=0x01 成功 / 0x02 失败 |
| CIAS_AUDIO_SYS_ERR | 0x0602 | - | 语音系统异常 |
| DOA_ANGLE | 0x0901 | - | DOA 角度（如启用） |

**B. ESP32 → CI（指令下发，CI 收到后回 `CIAS_CMD_EXEC_STATE` 应答）**

| 命令字 | 值 | payload | 含义 |
|---|---|---|---|
| SET_AUDIO_START_RECORD | 0x011A | 无 | 强制开始录音（不依赖唤醒，IIS 持续输出） |
| SET_AUDIO_STOP_RECORD | 0x011B | 无 | 停止录音（与 0x011A 配对） |
| MASTER_PLAY_STATE | 0x0220 | 1 字节：1=开始 / 0=停止 | **控制 IIS 下行播放窗口**：发 1 后 CI 才开始消费 IIS 输入并播放；播完发 0 |
| SET_PLAY_VOICE_ID | 0x0112 | 3 字节：音频ID(uint16 LE) + 是否打断(uint8) | 播放 CI 本地提示音 |
| SET_AUDIO_VOLUME | 0x0117 | 1 字节：1~7 | 设置音量（默认 5，范围 1-7） |
| SET_VOLUME_MUTE_STATE | 0x0119 | 1 字节：1=mute / 0=取消 | 静音控制 |
| SET_ENTER_WAKE_UP | 0x0114 | 1 字节：1=带进入提示音 / 0=不带 | 指令直接进入唤醒态（免唤醒词） |
| SET_AUDIO_EXIT_WAKE_UP | 0x010D | 1 字节：1=带退出提示音 / 0=不带 | 指令退出唤醒态 |
| PCM_DENOISE_ENABLE | 0x010E | 1 字节：1=上传降噪音频(默认) / 0=不降噪 | 切换上传音频是否带降噪 |
| SET_WAKE_UP_CONTINUE_TIME | 0x0113 | 2 字节(秒) | 持续唤醒时间，超时强制退出（默认 15s，固件另配 30s） |
| SET_VAD_START_MAX_TIMEOUT | 0x0111 | 2 字节(秒) | 单次说话最长时间，超时强制 VAD END（默认 5s） |
| SET_VAD_SENSITIVITY_ACTIVATE_LENTH | 0x0110 | 1 字节 | 多长静音触发 VAD END |
| SET_VAD_FILTER_FRAME | 0x010F | 2 字节 | 小于该帧数(16ms/帧)判为无效语音不上传 |
| SET_FORCE_VAD_END | 0x011D | 无 | 强制 VAD 结束 |
| SET_INTERACTION_NULTI_ROUND_ENABLE | 0x0115 | 1 字节：0=单轮 / 1=多轮(默认) | 对话轮次模式 |
| UPLOAD_PLAY_FULL_DUPLEX_EANBLE | 0x0116 | 1 字节：0=播放时不上传(默认) / 1=上传 | 全双工开关 |
| VAD_START_STOP_PLAY | 0x0216 | 1 字节：1=停止播放 / 0=不停止 | 全双工下 VAD_START 是否打断当前播放 |
| SET_AUDIO_PLAY_MODE | 0x0215 | 1 字节：1=打断当前播报 / 0=顺序播放 | 播放打断模式 |
| CIAS_AUDIO_RST | 0x0603 | 无 | 复位语音芯片 |
| GET_SW_VERSION | 0x030D | 无 | 获取固件版本 |

> 用户自定义指令请从 **0x4xxx** 段分配，避免与系统指令冲突。

### 4.3 应答与可靠性
- ESP32 下发的每条指令，CI 都会回 `CIAS_CMD_EXEC_STATE(0x0801)`。建议 ESP32 做 **超时重发**（如 200~500ms 无应答重发，最多 3 次）。
- 接收状态机：按字节流搜索 `A5 A5 5A 5A`（小端 magic）→ 读满 16 字节帧头 → 按 `len` 收 payload → 校验 `checksum == Σpayload` → 分发。注意处理粘包/断包。

### 4.4 ES8311 式 Codec 控制模型（ESP32 为主机，必须实现）

把 CI1302 当成一颗"智能 Codec"，ESP32 像主机控制 ES8311 一样控制它，控制接口映射如下：

| Codec 概念（ES8311 类比） | CI1302 对应 UART 指令 | 说明 |
|---|---|---|
| ADC 使能（开始录音） | `SET_AUDIO_START_RECORD` (0x011A)，无 payload | 发后 IIS SDO 开始输出有效 PCM |
| ADC 禁用（停止录音） | `SET_AUDIO_STOP_RECORD` (0x011B)，无 payload | 与 0x011A 配对；发后 IIS 无有效数据 |
| DAC 使能（开始播放） | `MASTER_PLAY_STATE` (0x0220)，payload=1 | 发后 CI 才消费 IIS SDI 数据并送喇叭 |
| DAC 禁用（停止播放） | `MASTER_PLAY_STATE` (0x0220)，payload=0 | 播放结束必须发，复位播放状态 |
| 音量设置 | `SET_AUDIO_VOLUME` (0x0117)，payload=1~7 | 默认 5 |
| 静音/取消静音 | `SET_VOLUME_MUTE_STATE` (0x0119)，payload=1/0 | 1=mute |
| 芯片复位 | `CIAS_AUDIO_RST` (0x0603) | 异常恢复用 |

控制原则：
1. **上电默认录、播均关闭**（`IIS_UPLOAD_IS_WAKEUP=1`、`IIS_DOWNLOAD_BY_CMD=1`），一切由 ESP32 的上述指令开启，语义与 Codec 的使能位一致。
2. 唤醒事件（`WAKE_UP` 上报）也会顺带开启录音输出——这是芯片附带的语音触发能力；若 ESP32 不需要，收到后可立即发 `SET_AUDIO_STOP_RECORD` 收回控制权，或直接忽略该事件流。
3. 每条控制指令都会收到 `CIAS_CMD_EXEC_STATE` 应答，ESP32 应确认后再进行下一步（尤其是播放：先确认 0x0220=1 应答成功，再开始喂 I2S 数据）。

---

## 5. 交互时序（状态机）

### 5.1 ESP32 主控模式（推荐，ES8311 式）

```
[上电]
  ESP32 等待 CIAS_AUDIO_SYS_READY(0x0601)              ← 握手完成，此时录/播均关闭
[录音（ESP32 决定何时录）]
  发 SET_AUDIO_START_RECORD(0x011A) → 等应答 → I2S RX 开始收 16kHz PCM
  （可选：收到 VAD_START/PCM_FINISH 事件辅助切分语音段；也可纯靠 ESP32 自己定时/定长）
  发 SET_AUDIO_STOP_RECORD(0x011B) → 录音关闭
[播放（ESP32 决定何时播）]
  发 MASTER_PLAY_STATE(0x0220) payload=1 → 等应答
  I2S TX 按 16kHz 实时速率连续写入 PCM（16kHz/16bit/双声道）
  数据发完 → 发 MASTER_PLAY_STATE payload=0
[音量/静音] 任意时刻发 SET_AUDIO_VOLUME / SET_VOLUME_MUTE_STATE
[异常] 无应答或状态异常 → CIAS_AUDIO_RST(0x0603) 复位后重新握手
```

### 5.2 语音事件驱动模式（可选，利用芯片唤醒能力）

```
[待唤醒态]  IIS 无有效音频
  收到 WAKE_UP(0x0102)（或主动发 SET_ENTER_WAKE_UP） → 进入[唤醒态]，IIS 自动开始输出
  收到 VAD_START(0x0108)  → 本段语音开始，送云端/LLM
  收到 PCM_FINISH(0x0106) → 一句话说完
  （收到 LOCAL_ASR_RESULT_NOTIFY → 离线命令词，ESP32 自行决定执行/忽略）
  收到 EXIT_WAKE_UP(0x0109) → 回到[待唤醒态]
[打断]
  播放期间用户再次说话：CI 的 AEC 生效，收到 VAD_START 即视为打断，
  是否停止播放由 VAD_START_STOP_PLAY / SET_AUDIO_PLAY_MODE 配置决定
```

两种模式可混用：平时由 ESP32 主控，需要低功耗语音触发时让唤醒事件自动开录音。

---

## 6. 音频时机模式（固件侧已按"ESP32 主控"配置）

| 配置项 | 当前值 | 含义 |
|---|---|---|
| `IIS_UPLOAD_IS_WAKEUP` | **1** | 录音默认关闭，由 ESP32 发 `SET_AUDIO_START_RECORD` 使能（唤醒事件也会自动开启，ESP32 可发 STOP 收回）。置 0 则为上电常开 |
| `IIS_DOWNLOAD_BY_CMD` | **1** | 播放默认关闭，由 ESP32 发 `MASTER_PLAY_STATE=1` 使能。置 0 则为上电持续消费（流模式） |
| `UPLOAD_NNDENOISE_AUDIO_DATA_ENABLE` | 1 | 上传降噪后音频 |
| `VAD_TIMEOUT_VALUE` | 5s | 单次说话最长 5 秒强制结束 |
| `NN_VAD_FRONT_PORCH` | 30 帧(300ms) | VAD 后沿，说话停顿不会立即结束 |
| `EXIT_WAKEUP_TIME` | 30s | 唤醒后 30s 无任何识别自动退回待唤醒 |
| `AEC_INTERRUPT_TYPE` | 2 | 播放中唤醒词/命令词均可打断 |

---

## 7. ESP32 侧适配清单（验收标准）

1. **I2S 从机驱动**：单端口全双工，RX 接 PA4、TX 接 PA2，BCLK/LRCLK/MCLK 为输入；16kHz/16bit/标准 I2S/双声道；不得自行产生时钟。
2. **上传解析**：取任一声道得到 16kHz 单声道 PCM；按 VAD_START→PCM_FINISH 窗口切段送云端。
3. **UART 协议栈**：921600；帧头 16 字节解析（magic 搜索 + checksum 校验）；组帧时 checksum=Σpayload、version=0x0100、fill_data=0x12345678；实现 `CIAS_CMD_EXEC_STATE` 应答匹配与超时重发。
4. **状态机**：按第 5 节实现，覆盖上电握手、录音使能/禁用（0x011A/0x011B）、播放使能/禁用（0x0220）、音量/静音、TTS 播放（先等 0x0220=1 应答 → 喂数 → =0）、异常复位（0x0603）。
5. **播放供数**：保证 16ms/1024 字节的实时速率，建议双缓冲 + 定时补充，避免断流。
6. **可选**：音量（0x0117）、本地提示音（0x0112）、全双工/打断策略（0x0116/0x0216/0x0215）等运行时配置。

---

## 8. 芯片差异（当前目标：CI1302）

| 项 | **CI1302（当前，已配置）** | CI1306（备选） |
|---|---|---|
| Flash/封装 | 2MB / SSOP24 | 4MB / QFN40 |
| 协议串口 | UART0：TX=PB5, RX=PB6（IIS 占用后唯一可用串口，日志已关闭） | UART1：TX=PB7, RX=PC0 |
| IIS 引脚 | PA2~PA6（同上表） | PA2~PA6（相同） |
| MIC | **仅模拟 MIC**（1302 不支持 DMIC + IIS 组合） | 模拟 MIC 或 DMIC |
| 其余协议/时序/音频格式 | 完全一致 | 完全一致 |

---

## 9. 常见坑位

1. **ESP32 配成 I2S 主机** → 时钟冲突，收不到正确数据。CI 永远是主机。
2. **未等 `CIAS_AUDIO_SYS_READY` 就发指令** → CI 未就绪，指令丢失。
3. **未唤醒就读 I2S RX 当有效音频** → 默认时机下只有唤醒后才有有效数据；需要常开录音请发 `SET_AUDIO_START_RECORD`。
4. **播放不发 `MASTER_PLAY_STATE=1`** → CI 不消费 IIS 输入，喇叭无声；播完忘记发 0 会影响后续状态。
5. **只发一个声道/采样率不符** → 必须 16kHz/16bit/双声道，单声道数据复制到两声道。
6. **checksum 算错** → 只累加 payload 字节，不含帧头；无 payload 指令 checksum=0。
7. **字节序** → 帧头所有多字节字段均为小端。
8. **供数不连续** → 播放卡顿/噪声；按 16ms/1024B 节拍持续喂数。

---

## 10. 源码出处（供交叉核对）

| 内容 | 文件 |
|---|---|
| 帧头结构 / magic | `app/app_common/cias_common.h` |
| 命令字总表 | `app/app_audio_handle/cias_network_msg_protocol.h` |
| 组帧 / checksum / 发送 | `app/app_audio_handle/cias_network_msg_send_task.c`（`broadlink_frame_create`、`cias_send_cmd`） |
| 指令处理（音量/录音/播放开关等） | `app/app_audio_handle/cias_audio_data_handle.c` |
| 唤醒/VAD/PCM_FINISH 上报 | `app/app_audio_handle/cias_voice_upload.c` |
| IIS 上传门控（唤醒/录音标志） | `components/audio_pre_rslt_iis_out/ci130x_audio_pre_rslt_out.c`（`audio_pre_rslt_upload_by_iis`） |
| IIS 硬件参数 / 引脚 | `driver/boards/CI-D06GT01D.c`（`audio_pre_tslt_out_info`）、`driver/boards/CI-D02GS01J.c` |
| 时机/功能宏配置 | `app/app_main/user_config.h`（离在线参数宏配置段） |
| 每帧 256 点 | `system/sdk_default_config.h`（`AUDIO_CAP_POINT_NUM_PER_FRM`） |
