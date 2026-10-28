#ifndef _CI130X_PROTOCOL_H
#define _CI130X_PROTOCOL_H

#include <driver/uart.h>
#include <driver/gpio.h>

#include <atomic>
#include <functional>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/**
 * CI130X（启英泰伦）语音芯片 UART 控制协议。
 *
 * 帧格式（小端，16 字节帧头 + N 字节 payload）：
 *   magic(4) = 0x5A5AA5A5 | checksum(2) = Σpayload | type(2) | len(2)
 *   | version(2) = 0x0100 | fill_data(4) = 0x12345678
 *
 * 每条下发指令，CI 都会回 CIAS_CMD_EXEC_STATE(0x0801)，
 * payload 3 字节：[type 低字节, type 高字节, 结果(1=成功 2=失败)]。
 */
class Ci130xProtocol {
public:
    // 常用命令字
    enum : uint16_t {
        CIAS_AUDIO_SYS_READY      = 0x0601,  // CI -> ESP32：上电就绪
        CIAS_AUDIO_SYS_ERR        = 0x0602,  // CI -> ESP32：语音系统异常
        CIAS_AUDIO_RST            = 0x0603,  // ESP32 -> CI：复位语音芯片
        CIAS_CMD_EXEC_STATE       = 0x0801,  // CI -> ESP32：指令执行结果

        WAKE_UP                   = 0x0102,  // 唤醒成功
        VAD_END                   = 0x0103,  //
        PCM_FINISH                = 0x0106,  // 一句话说完
        VAD_START                 = 0x0108,  // 检测到人声开始
        EXIT_WAKE_UP              = 0x0109,  // 退出唤醒态

        SET_AUDIO_EXIT_WAKE_UP    = 0x010D,  // payload: 1 字节，1=带退出提示音 0=不带
        SET_WAKE_UP_CONTINUE_TIME = 0x0113,  // payload: uint16 秒，唤醒态持续超时
        SET_AUDIO_VOLUME          = 0x0117,  // payload: 1~7
        SET_VOLUME_MUTE_STATE     = 0x0119,  // payload: 1=mute 0=unmute
        SET_AUDIO_START_RECORD    = 0x011A,  // 开始录音，IIS 输出有效 PCM
        SET_AUDIO_STOP_RECORD     = 0x011B,  // 停止录音
        MASTER_PLAY_STATE         = 0x0220,  // payload: 1=开始消费 IIS 下行 0=停止
    };

    using EventCallback = std::function<void(uint16_t type, const uint8_t* payload, uint16_t len)>;

    Ci130xProtocol(uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate);
    ~Ci130xProtocol();

    /**
     * 下发一条指令并等待 CIAS_CMD_EXEC_STATE 应答。
     * @param timeout_ms 0 表示不等待应答（火速发出即可）
     * @return 收到应答且结果为成功返回 true
     */
    bool SendCommand(uint16_t type, const uint8_t* payload = nullptr, uint16_t len = 0,
                     int timeout_ms = 300);

    void SetEventCallback(EventCallback cb) { event_callback_ = std::move(cb); }
    bool ready() const { return ready_.load(); }

    /** 复位语音芯片（异常恢复用），不等待应答 */
    void Reset();

private:
    static constexpr int kHeaderSize = 16;
    static constexpr int kMaxPayload = 256;

    uart_port_t port_;
    EventCallback event_callback_;
    TaskHandle_t rx_task_ = nullptr;
    SemaphoreHandle_t ack_sem_ = nullptr;
    std::mutex send_mutex_;

    std::atomic<bool> ready_{false};
    std::atomic<uint16_t> pending_ack_type_{0xFFFF};
    std::atomic<uint8_t> pending_ack_result_{0};

    void RxTask();
    void HandleFrame(uint16_t type, const uint8_t* payload, uint16_t len);
};

#endif  // _CI130X_PROTOCOL_H
