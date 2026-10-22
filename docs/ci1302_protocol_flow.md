# CI1302 语音识别与 UART 协议流程

> 板型：YcscEsp32S3Es8311WgxlCat
> 通信：UART 921600 baud
> 上行：1302 直接吐 Opus 帧（ESP32 不再二次编码）
> 下行：ESP32 将云端 PCM 用 shine 编码为 MP3，1302 拉流播放

---

## 一、整体时序图

```mermaid
sequenceDiagram
    participant U as 用户
    participant C as CI1302
    participant E as ESP32
    participant S as 云端

    Note over U,C: ① 唤醒
    U->>C: 说唤醒词
    C->>E: 0x0102 WAKE_UP
    E->>S: WakeupStartListening → 开通道/发 listen start
    E->>C: 0x011A SET_AUDIO_START_RECORD
    Note over C: 开始采集上传（VAD_START 自启已屏蔽）

    Note over U,C: ② 语音上行
    U->>C: 说话
    C->>E: 0x0105 PCM_MIDDLE × N（Opus 帧）
    E->>S: 转发 Opus 帧

    Note over U,C: ③ 说完了
    U->>C: 停止说话
    C->>E: 0x0106 PCM_FINISH
    Note over E: 只清上行队列，不调 StopListening
    S->>E: 云端自动 VAD 提交 → STT 文本 → LLM → TTS
    E->>E: PCM → shine MP3 → 缓冲

    Note over E,C: ④ TTS 下行播放
    E->>C: 0x0201 NET_PLAY_START（缓冲 ≥8KB 后）
    C->>E: 0x020A PLAY_DATA_GET（请求 N 字节）
    E->>C: 0x020B PLAY_DATA_RECV（MP3 数据）
    C->>E: 0x020A PLAY_DATA_GET（循环）
    Note over E,C: ...重复拉流直到 TTS 结束...

    Note over E,C: ⑤ 收尾
    E->>C: 0x020C PLAY_DATA_END（带尾部静音）
    C->>E: 0x020D PLAY_TTS_END
    E->>E: STATE → listening
    E->>C: 0x011A SET_AUDIO_START_RECORD
    Note over E,C: 回到 ②，等下一轮
```

---

## 二、命令字一览

### ESP32 → CI1302（下行控制）

| 命令字 | 值 | 含义 |
|---|---|---|
| SET_AUDIO_START_RECORD | 0x011A | 强制开始录音上传 |
| SET_AUDIO_STOP_RECORD | 0x011B | 停止录音上传 |
| NET_PLAY_START | 0x0201 | 启动播放会话（1302 开始拉流） |
| NET_PLAY_STOP | 0x0204 | 立即停止播放（打断） |
| PLAY_DATA_RECV | 0x020B | 响应 1302 的 GET，携带 MP3 数据 |
| PLAY_DATA_END | 0x020C | 播放数据发完（带尾部数据） |

### CI1302 → ESP32（上行事件）

| 命令字 | 值 | 含义 |
|---|---|---|
| LOCAL_ASR_NOTIFY | 0x0101 | 本地命令词命中 |
| WAKE_UP | 0x0102 | 唤醒词检测到 |
| VAD_END | 0x0103 | 人声结束（静音检测） |
| PCM_MIDDLE | 0x0105 | Opus 上行帧（负载为 Opus 数据） |
| PCM_FINISH | 0x0106 | 本句音频上传完毕 |
| VAD_START | 0x0108 | 人声开始（原厂自启上传已屏蔽） |
| PLAY_DATA_GET | 0x020A | 请求播放数据（负载 u32 请求长度） |
| PLAY_TTS_END | 0x020D | 播放完成上报 |

---

## 三、上行状态机（CI1302 侧）

```mermaid
stateDiagram-v2
    [*] --> Idle: 上电
    Idle --> Wake: 唤醒词命中
    Wake --> Recording: 收到 0x011A\n(ESP32 控制)
    Recording --> Sending: VAD_START\n开始推 Opus 帧
    Sending --> VADEnd: VAD_END 静音
    VADEnd --> Sending: 再次说话\n(多轮)
    VADEnd --> Finished: PCM_FINISH
    Finished --> Recording: 下一轮 0x011A
    Finished --> Idle: 超时/退出
```

**关键点：**
- 唤醒后 1302 **不会自己开始上传**（原厂 `cias_aiot_vad_start_handle(VAD_START)` 已屏蔽），等 ESP32 发 `0x011A`。
- 录音期间 VAD_END 检测**必须保留**，否则 1302 一直上传不发 PCM_FINISH，云端等不到句子边界。

---

## 四、下行播放状态机（ESP32 侧）

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Prebuffer: TTS PCM 到达
    Prebuffer --> Playing: 缓冲 ≥8KB\n(或超时 1.5s)
    Prebuffer --> Playing: 发 NET_PLAY_START
    Playing --> Playing: PLAY_DATA_GET → RECV\n循环拉流
    Playing --> Stall: 1302 不拉流\n缓冲 >32KB
    Stall --> Playing: 重发 NET_PLAY_START\n(不清缓冲)
    Playing --> Ending: NotifyStreamEnd\n(流结束+尾部静音)
    Ending --> Finished: 缓冲拉空\n发 PLAY_DATA_END
    Finished --> Idle: 收到 PLAY_TTS_END
```

**关键点：**
- 预缓冲 8KB 再发 NET_PLAY_START，防止起播丢字。
- 下行死锁时**不清空缓冲**，只重发 NET_PLAY_START 踢 1302。
- TTS 结束追加 1 秒静音尾巴，防止 1302 播放器缓冲吞尾字。

---

## 五、多轮对话循环

```mermaid
flowchart LR
    A[WAKE_UP] --> B[开通道/listen start]
    B --> C[0x011A 开始录音]
    C --> D[用户说话\nOpus 上行]
    D --> E[VAD_END / PCM_FINISH]
    E --> F[云端自动 VAD 提交]
    F --> G[STT → LLM → TTS]
    G --> H[MP3 下行播放]
    H --> I[PLAY_TTS_END]
    I --> J[回到 listening]
    J --> C
```

---

## 六、与 ES8311 的差异

| 方面 | ES8311（I2S） | CI1302（UART） |
|---|---|---|
| 唤醒检测 | ESP32 本地 AFE | 1302 外置芯片检测 |
| VAD | ESP32 本地 | 1302 外置芯片 |
| 上行编码 | ESP32 Opus 编码 | 1302 直接吐 Opus，ESP32 直通 |
| 下行播放 | ESP32 I2S DAC | ESP32 shine→MP3，1302 拉流播放 |
| 录音控制 | 无（I2S 常开） | 0x011A 开始 / 0x011B 停止 |
| 句子收尾 | ESP32 VAD 触发 stop | 云端自动 VAD 提交（PCM_FINISH 只清队列） |
