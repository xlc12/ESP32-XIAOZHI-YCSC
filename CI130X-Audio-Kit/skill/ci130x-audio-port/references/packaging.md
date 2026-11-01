# CI130X 音频组件化打包

目标：把 CI1302/CI1306 方案从「某个板子的 codec」变成「可跨工程复用的 ESP-IDF 组件」，同时不破坏小智框架。

## 1. 分层原则

| 层 | 位置 | 允许依赖 | 职责 |
|---|---|---|---|
| L1 驱动 | `components/ci130x_voice/` | 仅 ESP-IDF | UART 协议、I2S 从机、PA、音量、状态同步、看门狗 |
| L2 适配器 | `main/audio/codecs/ci130x_audio_codec.*` | L1 + `AudioCodec` | 继承 `AudioCodec`，转发 `Read/Write/Enable*/SetOutputVolume` |
| L3 胶水 | `main/boards/common/ci130x_board_glue.*` | L2 + `Application` | 唤醒桥接、设备状态桥接、EXP-004 开关 |

**硬性规则**

1. L1 **不得**包含 `audio_codec.h`、`board.h`、`application.h`。
2. L1 **不得**用 Kconfig 定义引脚；引脚走运行时 `Config`。
3. L2 **不得**再写业务逻辑（同步、看门狗、协议解析一律在 L1）。
4. L3 **不得**包含引脚宏；引脚由板级 `config.h` 传给 L2。

> `audio_codec.h` 里 `#include "board.h"`，而 ESP-IDF 组件不能依赖 `main`，所以 L2 只能留在 `main`。

## 2. 目录结构

```
components/ci130x_voice/
├── CMakeLists.txt
├── idf_component.yml
├── Kconfig
├── README.md
├── include/
│   ├── ci130x_protocol.h
│   └── ci130x_audio.h
└── src/
    ├── ci130x_protocol.cc
    └── ci130x_audio.cc
```

## 3. 运行时配置

```cpp
struct Ci130xConfig {
    uart_port_t uart_port = UART_NUM_1;
    gpio_num_t  uart_tx   = GPIO_NUM_NC;
    gpio_num_t  uart_rx   = GPIO_NUM_NC;
    int         uart_baud = 921600;

    gpio_num_t  bclk = GPIO_NUM_NC;   // CI 输出
    gpio_num_t  ws   = GPIO_NUM_NC;   // CI 输出
    gpio_num_t  dout = GPIO_NUM_NC;   // ESP32 -> CI
    gpio_num_t  din  = GPIO_NUM_NC;   // CI -> ESP32

    gpio_num_t  pa_pin         = GPIO_NUM_NC;
    bool        pa_active_high = true;
    int         sample_rate    = 16000;
};
```

MCLK 固定 `I2S_GPIO_UNUSED`，不放进配置。

## 4. 驱动 API

```cpp
enum class Ci130xEvent {
    kSysReady, kSysError, kWakeUp, kExitWakeUp, kVadStart, kPcmFinish,
};

class Ci130xAudio {
public:
    using EventCallback = std::function<void(Ci130xEvent, const uint8_t*, uint16_t)>;

    explicit Ci130xAudio(const Ci130xConfig& cfg);
    ~Ci130xAudio();

    int  Read(int16_t* dest, int samples);
    int  Write(const int16_t* data, int samples);

    void SetOutputVolume(int volume);
    void EnableInput(bool enable);
    void EnableOutput(bool enable);
    void SetUplinkEnabled(bool enable);
    void RequestExitWakeUp();
    void SetEventCallback(EventCallback cb);

    i2s_chan_handle_t tx_handle() const;
    i2s_chan_handle_t rx_handle() const;
};
```

迁移映射：

| 原成员 | 新归属 | 说明 |
|---|---|---|
| `Ci130xProtocol::SendCommand` | L1 | 原样 |
| `Read` / `Write` | L1 | 去 `override` |
| `EnableInput` / `EnableOutput` / `SetOutputVolume` | L1 | 原样 |
| `SyncWithChip` / `SyncTask` | L1 private | 原样 |
| `SetChipUplinkEnabled` / `RequestExitWakeUp` | L1 | 原样 |
| `SetWakeWordHandler`（static） | 删除 | 改 `SetEventCallback` 实例回调 |
| `CreateDuplexChannels` | L1 `Init` | 原样 |
| `duplex_` / `input_reference_` / 采样率 | L2 | 适配器设置基类字段 |
| `pa_pin_` / `pa_active_high_` | `Ci130xConfig` | 不再写死在构造函数 |

## 5. 组件构建文件

```cmake
idf_component_register(
    SRCS "src/ci130x_protocol.cc" "src/ci130x_audio.cc"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_i2s esp_driver_gpio
    PRIV_REQUIRES freertos esp_timer
)
```

```yaml
version: "0.1.0"
description: "CI1302/CI1306 I2S-slave audio front-end with UART control"
targets:
  - esp32s3
  - esp32p4
dependencies:
  idf: ">=5.0"
```

```kconfig
menu "CI130X Audio"
    config CI130X_VOICE_ENABLE
        bool "Enable CI130X audio front-end"
        default n
endmenu
```

## 6. 适配器

```cpp
class Ci130xAudioCodec : public AudioCodec {
public:
    explicit Ci130xAudioCodec(const Ci130xConfig& cfg);
    void SetEventCallback(Ci130xAudio::EventCallback cb);
    void SetUplinkEnabled(bool enable);
    void RequestExitWakeUp();

    void SetOutputVolume(int volume) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

protected:
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

private:
    Ci130xAudio audio_;
};
```

构造函数：

```cpp
Ci130xAudioCodec::Ci130xAudioCodec(const Ci130xConfig& cfg) : audio_(cfg) {
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;
    output_channels_ = 1;
    input_sample_rate_ = cfg.sample_rate;
    output_sample_rate_ = cfg.sample_rate;
    tx_handle_ = audio_.tx_handle();
    rx_handle_ = audio_.rx_handle();
}
```

不要覆盖 `Start()`，继续复用基类的 NVS 音量逻辑。

## 7. 板级胶水

```cpp
struct Ci130xGlueOptions {
    bool wake_word_enabled          = true;
    bool exit_wakeup_on_idle        = true;
    bool stop_uplink_while_speaking = false;  // 原 EXP-004，默认关
};

class Ci130xBoardGlue {
public:
    static void Attach(Ci130xAudioCodec& codec, const Ci130xGlueOptions& opt = {});
};
```

实现要点：

1. `static Ci130xAudioCodec* s_codec` 保存实例，状态回调里直接用，避免再次 `Board::GetInstance().GetAudioCodec()`。
2. `kWakeUp` 只在 `kDeviceStateIdle` / `kDeviceStateSpeaking` 调 `WakeWordInvoke`。
3. `kDeviceStateIdle` → `RequestExitWakeUp()`。
4. `stop_uplink_while_speaking`：`Speaking` 关、`Listening` 开；默认关闭，避免破坏实时打断。
5. 只注册一次。

板级最终形态：

```cpp
virtual AudioCodec* GetAudioCodec() override {
    static Ci130xAudioCodec codec({
        .uart_port = CI130X_UART_PORT,
        .uart_tx   = CI130X_UART_TX_PIN,
        .uart_rx   = CI130X_UART_RX_PIN,
        .uart_baud = CI130X_UART_BAUD_RATE,
        .bclk = AUDIO_I2S_GPIO_BCLK,
        .ws   = AUDIO_I2S_GPIO_WS,
        .dout = AUDIO_I2S_GPIO_DOUT,
        .din  = AUDIO_I2S_GPIO_DIN,
        .pa_pin = CI130X_PA_PIN,
        .pa_active_high = CI130X_PA_ACTIVE_HIGH,
        .sample_rate = AUDIO_INPUT_SAMPLE_RATE,
    });
    Ci130xBoardGlue::Attach(codec);
    return &codec;
}
```

## 8. 重构顺序（每步可编译）

1. **移协议层**：`ci130x_protocol.*` → 组件；`main/CMakeLists.txt` 删掉该源文件。
2. **抽纯驱动**：把 I2S/PA/音量/同步逻辑搬进 `Ci130xAudio`，只改归属与命名。
3. **适配器瘦身**：`ci130x_audio_codec.cc` 缩到约 60 行。
4. **胶水收口**：新增 `ci130x_board_glue.*`，EXP-004 改成配置项。
5. **配置收敛**：`config.h` 只留引脚；修 `sdkconfig.defaults` / `config.json` 板型符号。
6. **打包**：`idf_component.yml` + tag + CHANGELOG。

每步验证：唤醒 → 对话 → 音量 → PA 待机 → 播报期间不断流。

## 9. 分发

| 方式 | 做法 |
|---|---|
| 本仓库 | 放入 `components/ci130x_voice/`，自动编译 |
| 多工程共享 | 顶层 `CMakeLists.txt` 加 `EXTRA_COMPONENT_DIRS` |
| Git 依赖 | `main/idf_component.yml`：`ci130x_voice: {git: "...", path: "components/ci130x_voice", version: "v0.1.0"}` |
| 组件注册表 | `compote component upload` |

## 10. 风险

| 风险 | 缓解 |
|---|---|
| 组件化后 `AudioCodec` 引用断裂 | 适配器留在 `main` |
| 静态回调改实例回调引入时序问题 | 保留原状态过滤条件 |
| EXP-004 默认关闭后播报期上行恢复 | 先保持默认 `true` 验证，再评估 |
| `sdkconfig` 漏带 | 随组件提供 `sdkconfig.defaults` 片段 |
| 其他芯片 I2S 不兼容 | `targets` 限定 `esp32s3` / `esp32p4` |
