#ifndef _CI130X_AUDIO_CODEC_H
#define _CI130X_AUDIO_CODEC_H

#include "audio_codec.h"
#include "ci130x_protocol.h"

#include <driver/gpio.h>

#include <atomic>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

/**
 * 把 CI1302/CI1306 当作一颗"智能 Codec"接入小智框架。
 *
 * 音频：I2S 从机（CI 是主机，输出 MCLK/SCLK/LRCK），单端口全双工
 *       16 kHz / 16 bit / 双声道 / 32 bit slot（SCK:LRCK = 64）
 * 控制：UART 指令（开始/停止录音、播放窗口、音量）
 */
class Ci130xAudioCodec : public AudioCodec {
public:
    Ci130xAudioCodec(uart_port_t uart_port, gpio_num_t uart_tx, gpio_num_t uart_rx, int uart_baud,
                     int input_sample_rate, int output_sample_rate,
                     gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    virtual ~Ci130xAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

    /**
     * CI130X 自己做离线唤醒，检测到后通过 UART 上报 WAKE_UP(0x0102)。
     * 板级把该事件桥接到 Application::WakeWordInvoke，这样即便 ESP32 本地
     * 唤醒词模型因为音频通道不理想而失效，也能被 CI 唤醒。
     */
    static void SetWakeWordHandler(std::function<void(const std::string&)> handler);

    // 由设备状态回调触发：ESP32 回到待机时，让 CI130X 也退出唤醒态。
    // 只是置标志，真正的串口下发在 SyncTask 里做，避免阻塞调用者。
    void RequestExitWakeUp();

protected:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

private:
    void CreateDuplexChannels(gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    // 把"期望状态"同步到 CI：录音开关、播放窗口、音量。全部已一致时返回 true
    // force=true 时无视"已同步"缓存，强制全部重发（CI 重启后必须这样做）
    bool SyncWithChip(bool force = false);
    // 后台重试任务：CI 与 ESP32 各自独立上电，指令可能丢，需要收敛
    void SyncTask();

    Ci130xProtocol* protocol_ = nullptr;

    static std::function<void(const std::string&)> wake_word_handler_;

    std::mutex state_mutex_;   // 保护下面 3 个期望值
    std::mutex sync_mutex_;    // 串行化同步过程，避免两次同步交叉

    bool input_requested_ = false;
    bool output_requested_ = false;
    uint8_t volume_level_ = 5;     // CI130X 音量档位 1~7

    bool applied_input_ = false;
    bool applied_output_ = false;
    uint8_t applied_volume_ = 0;

    // CI 的"唤醒态超时自动退出"改成极长（uint16 秒，65535s ≈ 18 小时），
    // 录音开关改由 ESP32 的 0x011A / 0x011B 掌握，长对话不会被它自己掐断。
    uint16_t wake_timeout_s_ = 65535;
    uint16_t applied_wake_timeout_ = 0;

    // CI 当前是否处于唤醒态（由 0x0102 / 0x0109 事件维护）
    std::atomic<bool> ci_awake_{false};
    std::atomic<bool> exit_wakeup_requested_{false};

    TaskHandle_t sync_task_ = nullptr;

    std::vector<int16_t> read_buffer_;
    std::vector<int16_t> write_buffer_;
    int read_timeout_count_ = 0;
    int write_timeout_count_ = 0;

    // 上行数据看门狗：CI 有时会在"该录音"的时候悄悄停止上传（例如它自己的
    // 唤醒态超时退出），此时 Read 只能全返回静音，云端就会"听不到"。
    // 连续静音到阈值后置位，由 SyncTask 重新下发一次开始录音指令。
    int silent_read_count_ = 0;
    std::atomic<bool> force_record_resync_{false};
    // CI 重启（再次上报 SYS_READY）后它内部所有状态都被清零，必须整组重发
    std::atomic<bool> force_full_resync_{false};
};

#endif  // _CI130X_AUDIO_CODEC_H
