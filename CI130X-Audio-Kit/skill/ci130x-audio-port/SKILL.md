---
name: ci130x-audio-port
description: Port a CI130X (CI1302 or CI1306) voice chip into xiaozhi-esp32 as an I2S slave codec controlled over UART. Use when integrating CI130X audio, fixing silence, wake-word or duplex I2S problems. Do not use for ES8311 or other register-based codecs.
---

# CI130X 音频移植

把 CI130X 当成一颗「智能 Codec」接进小智框架：音频走 I2S（CI 是主机，ESP32 是从机），控制走 UART。

## 开始前先确认

1. CI 侧工程是否启用 IIS 输出：`IIS_UPLOAD_IS_WAKEUP`、`IIS_DOWNLOAD_BY_CMD` 两个宏决定它什么时候出音频、什么时候收音频。
2. 拿到 CI 的 IIS 引脚定义（`ci130x_scu.h` 或板级文件），确认 SCLK/LRCLK/SDO/SDI/MCLK 五根线的方向。
3. 确认 ESP32 板上的功放使能脚与有效电平。

## 移植步骤

1. **配置引脚**：在板级 `config.h` 增加 `AUDIO_CODEC_CI130X_ENABLE`、UART 端口/TX/RX/波特率、`CI130X_PA_PIN`、`CI130X_PA_ACTIVE_HIGH`。I2S 复用原 ES8311 的四根线，但方向相反。
2. **实现协议层**：独立的 `ci130x_protocol`，负责找 magic、处理粘包断包、校验 checksum、组帧下发、匹配 `0x0801` 应答、分发事件。详细帧格式与命令表见 `references/protocol.md`。
3. **实现 Codec**：继承 `AudioCodec`，实现 `Read/Write/EnableInput/EnableOutput/SetOutputVolume`。I2S 配置见下方「I2S 参数」。
4. **状态同步**：维护期望值/已生效值，`SyncWithChip()` 逐项下发；后台任务周期收敛，失败指数退避。**不要以「收到过就绪事件」为前提**。
5. **桥接唤醒**：CI 的 `WAKE_UP` 事件 → `Application::WakeWordInvoke()`。必须按设备状态过滤，见「必踩的坑」第 3 条。
6. **接功放**：构造函数接收 `pa_pin` 与极性，`EnableOutput()` 里跟随 `output_enabled_` 驱动。
7. **收敛日志**：把全局日志压到 ERROR，只放行应用、Codec、协议三个 TAG，否则 AFE 与 LVGL 的刷屏会淹没音频调试信息。

## I2S 参数（从机侧）

```cpp
.role           = I2S_ROLE_SLAVE,
.dma_frame_num  = 256,                   // 1024 字节/描述符
.auto_clear_after_cb = true,             // TX 欠载补 0，避免爆音
.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
.slot_mode      = I2S_SLOT_MODE_STEREO,
.slot_mask      = I2S_STD_SLOT_BOTH,
.ws_width       = 32,
.bit_shift      = true,
.left_align     = true,
```

## 必踩的坑

1. **DMA 描述符必须整除单次读长度**。一次读 512 点 × 2 声道 = 2048 字节，`dma_frame_num` 取 256 才能整块返回；取 240 会每次短读 128 字节并丢掉尾巴。
2. **`Read()` 必须按采样率补齐节奏**。CI 没在供数时会立刻返回，输入任务会以几十倍实时速度灌 AFE，直接把 feed 环形缓冲灌爆（表现为 `Ringbuffer of AFE(FEED) is full`）。
3. **CI 的唤醒词是常开的，会重复上报**。设备已经在 listening 时再来一个 `WAKE_UP`，框架的 `WakeWordInvoke` 会走「关闭音频通道」分支，把正在进行的会话掐掉，表现就是「再喊一次唤醒词反而彻底不识别」。回调里只允许 idle 与 speaking 放行。
4. **不要在音频任务里同步等串口应答**。命令超时动辄几百毫秒，而 RX DMA 只有约 96 ms 缓冲，一次阻塞就会丢开头。状态同步放后台任务，前台只置标志。
5. **CI 只在自身启动时上报一次就绪**。ESP32 往往晚于它上电，这条消息必然错过；同步逻辑不能依赖它。运行中再次收到就绪，说明 CI 重启过，内部标志已清零，必须整组重发。
6. **退出唤醒态不会停录音**。录音门控是 `is_wake_up_flag || is_always_iis_flag`，后者只由 `0x011A/0x011B` 控制。想让它「上电就一直出音频」，发一次 `0x011A` 即可。
7. **唤醒态超时不能设 0**。内部用 `xTimerChangePeriod(秒*1000)`，周期 0 会触发 FreeRTOS 断言；想要「不自动退出」用 65535 秒。
8. **看门狗要覆盖「满字节全 0」**。CI 在门控关闭时是循环输出 zero buffer 而不是停 DMA，只看 `bytes_read == 0` 会漏判。

## 验证顺序

1. 串口能否收到 CI 的 `0x0601` 与事件帧 → 验证 UART 与校验。
2. 下发 `0x011A` 后是否持续有非零 PCM → 验证 I2S 方向与时序。
3. 说唤醒词是否出现 `WAKE_UP` 且 ESP32 进入 listening → 验证唤醒桥接。
4. 云端回复后是否听到声音 → 验证下行与 `0x0220` 播放窗口。
5. 空口 15 秒后功放是否自动关闭（`PA OFF`）→ 验证电源管理。

日志判读与故障对照见 `references/troubleshooting.md`。
