#ifndef PN532_H_
#define PN532_H_

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// PN532 NFC 模块驱动（UART 通信）
//
// 功能：后台持续寻卡，读到卡片后解析其 NDEF 中的文本（角色信息），
//       卡片离开时再次回调。回调在后台任务上下文执行。
class Pn532 {
public:
    struct CardInfo {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        std::string ndef_text;   // NDEF 中解析出的文本（角色信息）
    };

    using CardDetectedCallback = std::function<void(const CardInfo&)>;
    using CardRemovedCallback  = std::function<void()>;

    // uart_port: UART 端口；tx: ESP32 -> PN532；rx: PN532 -> ESP32
    Pn532(uart_port_t uart_port, gpio_num_t tx, gpio_num_t rx,
          uint32_t baud_rate = 115200);
    ~Pn532();

    // 初始化模块并启动后台寻卡任务
    void Start();
    void Stop();

    void OnCardDetected(CardDetectedCallback cb) { on_detected_ = std::move(cb); }
    void OnCardRemoved(CardRemovedCallback cb)   { on_removed_  = std::move(cb); }

private:
    // 串口收发
    esp_err_t UartWrite(const uint8_t* data, size_t len);
    size_t    UartRead(uint8_t* buf, size_t len, int timeout_ms);
    void      UartFlush();
    // 打印接收缓冲中的原始数据，用于排查硬件问题
    void      DumpRxBuffer();

    // PN532 协议帧
    // wakeup=true 时会在命令帧前拼接 HSU 唤醒序列 55 55 00 ...
    bool SendCommand(const uint8_t* data, size_t len, uint32_t timeout_ms = 200,
                     bool wakeup = false);
    bool WaitAck(uint32_t timeout_ms);
    bool ReadFrame(std::vector<uint8_t>& out_payload, uint32_t timeout_ms);

    // PN532 命令
    bool SamConfig(bool wakeup = false);
    bool GetFirmwareVersion(uint32_t& version);
    bool InListPassiveTarget(uint8_t* uid, uint8_t& uid_len);
    bool InDataExchange(const uint8_t* tx, size_t tx_len, std::vector<uint8_t>& rx);

    // 读取卡片 NDEF 文本
    bool ReadNdefText(std::string& out_text);

    static void PollTaskEntry(void* arg);
    void PollLoop();

    uart_port_t uart_port_;
    gpio_num_t  tx_;
    gpio_num_t  rx_;
    uint32_t    baud_rate_;
    bool        initialized_ = false;

    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};

    CardDetectedCallback on_detected_;
    CardRemovedCallback  on_removed_;

    bool    card_present_ = false;
    uint8_t current_uid_[10] = {0};
    uint8_t current_uid_len_ = 0;
};

#endif // PN532_H_
