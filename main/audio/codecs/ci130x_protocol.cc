#include "ci130x_protocol.h"

#include <esp_log.h>
#include <cstring>

#define TAG "Ci130xProtocol"

static constexpr uint32_t kMagic = 0x5A5AA5A5;
static constexpr uint16_t kVersion = 0x0100;
static constexpr uint32_t kFillData = 0x12345678;

static inline void WriteU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void WriteU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

Ci130xProtocol::Ci130xProtocol(uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate)
    : port_(port) {
    uart_config_t cfg = {};
    cfg.baud_rate = baud_rate;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(port_, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port_, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port_, 1024, 0, 0, nullptr, 0));

    ack_sem_ = xSemaphoreCreateBinary();

    xTaskCreate([](void* arg) {
        static_cast<Ci130xProtocol*>(arg)->RxTask();
        vTaskDelete(NULL);
    }, "ci130x_uart", 3072, this, 5, &rx_task_);

    ESP_LOGI(TAG, "UART%d initialized: TX=%d RX=%d baud=%d", (int)port_, (int)tx_pin, (int)rx_pin, baud_rate);
}

Ci130xProtocol::~Ci130xProtocol() {
    if (rx_task_ != nullptr) {
        vTaskDelete(rx_task_);
        rx_task_ = nullptr;
    }
    if (ack_sem_ != nullptr) {
        vSemaphoreDelete(ack_sem_);
        ack_sem_ = nullptr;
    }
    uart_driver_delete(port_);
}

void Ci130xProtocol::Reset() {
    SendCommand(CIAS_AUDIO_RST, nullptr, 0, 0);
}

bool Ci130xProtocol::SendCommand(uint16_t type, const uint8_t* payload, uint16_t len, int timeout_ms) {
    if (len > kMaxPayload) {
        ESP_LOGE(TAG, "payload too long: %u", len);
        return false;
    }

    // 在接收任务里不能等应答：应答正是由这个任务派发的，等下去必然超时
    if (xTaskGetCurrentTaskHandle() == rx_task_) {
        timeout_ms = 0;
    }

    std::lock_guard<std::mutex> lock(send_mutex_);

    uint8_t frame[kHeaderSize + kMaxPayload];
    memset(frame, 0, kHeaderSize + len);

    uint16_t checksum = 0;
    for (int i = 0; i < len; i++) {
        checksum = (uint16_t)(checksum + payload[i]);
    }

    WriteU32(frame + 0, kMagic);
    WriteU16(frame + 4, checksum);
    WriteU16(frame + 6, type);
    WriteU16(frame + 8, len);
    WriteU16(frame + 10, kVersion);
    WriteU32(frame + 12, kFillData);
    if (len > 0) {
        memcpy(frame + kHeaderSize, payload, len);
    }

    pending_ack_type_.store(type);
    pending_ack_result_.store(0);
    xSemaphoreTake(ack_sem_, 0);  // 清掉历史残留

    int written = uart_write_bytes(port_, (const char*)frame, kHeaderSize + len);
    if (written != kHeaderSize + len) {
        ESP_LOGE(TAG, "uart write failed: %d", written);
        pending_ack_type_.store(0xFFFF);
        return false;
    }

    if (timeout_ms <= 0) {
        pending_ack_type_.store(0xFFFF);
        return true;
    }

    if (xSemaphoreTake(ack_sem_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "no ack for cmd 0x%04X in %d ms", type, timeout_ms);
        pending_ack_type_.store(0xFFFF);
        return false;
    }

    pending_ack_type_.store(0xFFFF);
    bool ok = pending_ack_result_.load() == 0x01;
    if (!ok) {
        ESP_LOGW(TAG, "cmd 0x%04X rejected by CI130X", type);
    }
    return ok;
}

void Ci130xProtocol::RxTask() {
    uint8_t buf[512];
    size_t size = 0;
    uint8_t chunk[64];

    while (true) {
        int n = uart_read_bytes(port_, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        if (size + n > sizeof(buf)) {
            size = 0;  // 严重异常，丢弃已有数据重新同步
        }
        memcpy(buf + size, chunk, n);
        size += n;

        size_t offset = 0;
        while (size - offset >= 4) {
            size_t i = offset;
            bool found = false;
            for (; i + 4 <= size; i++) {
                if (buf[i] == 0xA5 && buf[i + 1] == 0xA5 && buf[i + 2] == 0x5A && buf[i + 3] == 0x5A) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // 没找到帧头，丢弃噪声，但保留末尾 3 字节以防 magic 跨包
                offset = size > 3 ? size - 3 : 0;
                break;
            }
            if (size - i < (size_t)kHeaderSize) {
                offset = i;
                break;
            }

            uint16_t checksum = ReadU16(buf + i + 4);
            uint16_t type = ReadU16(buf + i + 6);
            uint16_t len = ReadU16(buf + i + 8);
            if (len > kMaxPayload) {
                offset = i + 4;
                continue;
            }
            if (size - i < (size_t)(kHeaderSize + len)) {
                offset = i;  // 半包，等下一批
                break;
            }

            uint16_t sum = 0;
            for (int k = 0; k < len; k++) {
                sum = (uint16_t)(sum + buf[i + kHeaderSize + k]);
            }
            if (sum != checksum) {
                ESP_LOGW(TAG, "checksum mismatch for type 0x%04X", type);
                offset = i + 4;
                continue;
            }

            HandleFrame(type, buf + i + kHeaderSize, len);
            offset = i + kHeaderSize + len;
        }

        if (offset > 0) {
            size_t remain = size - offset;
            memmove(buf, buf + offset, remain);
            size = remain;
        }
    }
}

void Ci130xProtocol::HandleFrame(uint16_t type, const uint8_t* payload, uint16_t len) {
    if (type == CIAS_CMD_EXEC_STATE) {
        if (len >= 3) {
            uint16_t acked = (uint16_t)(payload[0] | (payload[1] << 8));
            if (acked == pending_ack_type_.load()) {
                pending_ack_result_.store(payload[2]);
                xSemaphoreGive(ack_sem_);
            }
        }
        return;
    }

    if (type == CIAS_AUDIO_SYS_READY) {
        if (!ready_.exchange(true)) {
            ESP_LOGI(TAG, "CI130X is ready");
        }
    }

    if (event_callback_) {
        event_callback_(type, payload, len);
    }
}
