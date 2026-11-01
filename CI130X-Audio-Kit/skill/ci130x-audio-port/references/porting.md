# CI130X 移植 Checklist

按顺序执行；每完成一组就做一次验证，不要等到最后一起调。

## 1. 硬件与接线

| 检查项 | 说明 |
|---|---|
| CI SCLK → ESP32 BCLK | CI 是 I2S 主机 |
| CI LRCLK → ESP32 WS | 同上 |
| CI SDO → ESP32 DIN | 上行（麦克风） |
| CI SDI ← ESP32 DOUT | 下行（TTS） |
| CI MCLK | 不接 ESP32，ESP32 不驱动 |
| UART TX/RX | 交叉连接，默认 921600 8N1 |
| UART 引脚 | 避开 UART0 控制台、Flash/PSRAM 占用脚 |
| 功放使能 | 确认是 CI 的 PC4 + `0x0119`，还是 ESP32 GPIO + 极性 |
| 结构 | 麦克风与扬声器避免强反馈 |

## 2. CI 侧固件

- [ ] `IIS_UPLOAD_IS_WAKEUP`、`IIS_DOWNLOAD_BY_CMD` 已启用；
- [ ] 命令表与 `ci130x_protocol.h` 一致（`0x011A/0x011B/0x0119/0x0117/0x0220/0x0113/0x010D`）；
- [ ] 唤醒词与提示音配置符合产品要求；
- [ ] 上电会主动上报 `0x0601`，但 ESP32 逻辑不依赖它。

## 3. 板级配置

```c
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_39
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_41
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_42
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_40

#define CI130X_UART_PORT      UART_NUM_1
#define CI130X_UART_TX_PIN    GPIO_NUM_48
#define CI130X_UART_RX_PIN    GPIO_NUM_47
#define CI130X_UART_BAUD_RATE 921600

#define CI130X_PA_PIN          GPIO_NUM_NC
#define CI130X_PA_ACTIVE_HIGH  1
```

- [ ] 两个采样率必须相等（构造函数有 `assert`）；
- [ ] UART 引脚与板级其他外设不冲突。

## 4. 工程接入

- [ ] 放置 `components/ci130x_voice/`（或加入 `EXTRA_COMPONENT_DIRS`）；
- [ ] `main/CMakeLists.txt` 登记适配器源文件（组件源文件不用登记）；
- [ ] `Kconfig.projbuild` 增加板型符号（新建板时）；
- [ ] `sdkconfig.defaults` 与 `config.json` 的板型符号一致；
- [ ] `main` 能包含 `ci130x_audio.h`（main 自动依赖所有组件）。

## 5. 行为配置

| 选项 | 建议 |
|---|---|
| `CONFIG_USE_AUDIO_PROCESSOR` | `y`（AFE 需要 16k 单声道） |
| `CONFIG_USE_SERVER_AEC` | 与 `USE_DEVICE_AEC` 二选一；CI 无参考通道时优先 server AEC |
| `CONFIG_USE_DEVICE_AEC` | 需要硬件回采参考；CI 方案默认不具备 |
| `CONFIG_USE_AFE_WAKE_WORD` | 与 CI 自带唤醒二选一或明确分工，避免重复唤醒 |
| 日志等级 | 全局 ERROR，放行 `Application` / `Ci130xAudioCodec` / `Ci130xProtocol` |

> 只复制代码、不带这些配置，常见表现是“能编译、能出声，但唤醒/打断不对”。

## 6. 验证顺序

1. **UART**：收到 `0x0601` 与事件帧；
2. **上行**：下发 `0x011A` 后 `[AUDIO-IN]` 持续有非零 PCM；
3. **唤醒**：说唤醒词出现 `WAKE_UP`，设备进入 listening；
4. **下行**：云端回复能听到声音，`0x0220` 播放窗口正常；
5. **功放**：空口 15 秒后 `PA OFF`；
6. **稳定性**：连续对话/播报期间无重启、无断流；
7. **打断**：确认 AEC 模式与半双工策略一致。

## 7. 故障速查

| 现象 | 优先检查 |
|---|---|
| 无 `[AUDIO-IN]` 非零数据 | `0x011A`、I2S 方向、DMA 描述符能否整除单次读长度 |
| `Ringbuffer of AFE(FEED) is full` | `Read()` 是否按采样率补齐节奏 |
| 再喊唤醒词会话被掐断 | 唤醒回调是否只在 idle / speaking 放行 |
| 播报无声音、文字正常 | 设备状态是否被提前切到 listening |
| 播报期间重启 | 上行并发、AFE、供电；看 `0x0601` 是否重复上报 |
| 回声 / 误打断 | server AEC 与半双工策略是否冲突 |
| 音量无效 | `0x0117` 是否下发成功，映射是否为 1~7 |

详细日志判读见 `troubleshooting.md`；协议帧格式见 `protocol.md`。
