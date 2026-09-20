#include "pn532.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "PN532"

namespace {

constexpr uint8_t kTfiHost  = 0xD4;   // 主机 -> PN532
constexpr uint8_t kTfiPn532 = 0xD5;   // PN532 -> 主机

constexpr uint8_t kCmdGetFirmwareVersion  = 0x02;
constexpr uint8_t kCmdSamConfig           = 0x14;
constexpr uint8_t kCmdInDataExchange      = 0x40;
constexpr uint8_t kCmdInListPassiveTarget = 0x4A;

// 主机发出命令后 PN532 返回的固定 ACK 帧
const uint8_t kAckFrame[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// PN532 校验和：累加后取反加一
inline uint8_t Checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(~sum + 1);
}

}  // namespace

Pn532::Pn532(uart_port_t uart_port, gpio_num_t tx, gpio_num_t rx, uint32_t baud_rate)
    : uart_port_(uart_port), tx_(tx), rx_(rx), baud_rate_(baud_rate) {
    uart_config_t uart_config = {};
    uart_config.baud_rate  = static_cast<int>(baud_rate_);
    uart_config.data_bits  = UART_DATA_8_BITS;
    uart_config.parity     = UART_PARITY_DISABLE;
    uart_config.stop_bits  = UART_STOP_BITS_1;
    uart_config.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(uart_port_, 1024, 1024, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return;
    }
    err = uart_param_config(uart_port_, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return;
    }
    err = uart_set_pin(uart_port_, static_cast<int>(tx_), static_cast<int>(rx_),
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return;
    }
    initialized_ = true;
    ESP_LOGI(TAG, "UART ready (TX=%d RX=%d %u bps)",
             static_cast<int>(tx_), static_cast<int>(rx_), baud_rate_);
}

Pn532::~Pn532() {
    Stop();
    if (initialized_) {
        uart_driver_delete(uart_port_);
        initialized_ = false;
    }
}

void Pn532::Start() {
    if (!initialized_) {
        ESP_LOGE(TAG, "not initialized");
        return;
    }
    if (running_.exchange(true)) {
        return;
    }

    // 等待模块上电稳定
    vTaskDelay(pdMS_TO_TICKS(100));

    // HSU 模式下 PN532 上电后处于等待唤醒状态，必须发送 55 55 00... 唤醒序列，
    // 否则不会返回任何数据。这里带唤醒重试若干次。
    bool sam_ok = false;
    for (int i = 1; i <= 5 && !sam_ok; ++i) {
        sam_ok = SamConfig(/*wakeup=*/true);
        if (!sam_ok) {
            ESP_LOGW(TAG, "SAMConfiguration attempt %d failed", i);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!sam_ok) {
        ESP_LOGE(TAG, "SAMConfiguration failed, check PN532 wiring/mode/DIP switch");
        DumpRxBuffer();
        running_ = false;
        return;
    }
    ESP_LOGI(TAG, "SAMConfiguration ok");

    uint32_t ver = 0;
    if (GetFirmwareVersion(ver)) {
        ESP_LOGI(TAG, "firmware version %d.%d", (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
    } else {
        ESP_LOGW(TAG, "read firmware version failed");
    }

    xTaskCreate(PollTaskEntry, "pn532_poll", 4096, this, 4, &task_);
    ESP_LOGI(TAG, "polling task started");
}

void Pn532::Stop() {
    running_ = false;
    if (task_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(150));
        task_ = nullptr;
    }
}

// ---------- 串口 ----------

esp_err_t Pn532::UartWrite(const uint8_t* data, size_t len) {
    return uart_write_bytes(uart_port_, data, len) >= 0 ? ESP_OK : ESP_FAIL;
}

size_t Pn532::UartRead(uint8_t* buf, size_t len, int timeout_ms) {
    int n = uart_read_bytes(uart_port_, buf, len, pdMS_TO_TICKS(timeout_ms));
    return n > 0 ? static_cast<size_t>(n) : 0;
}

void Pn532::UartFlush() {
    uart_flush_input(uart_port_);
    uint8_t tmp;
    while (uart_read_bytes(uart_port_, &tmp, 1, 0) > 0) {
        // 丢弃残留数据
    }
}

void Pn532::DumpRxBuffer() {
    uint8_t buf[64] = {0};
    size_t n = UartRead(buf, sizeof(buf), 300);
    if (n == 0) {
        ESP_LOGE(TAG, "no response from PN532. check: 1) TX/RX 交叉接线 "
                      "2) 模块拨码是否切到 HSU(UART) 3) 共地/供电 4) 波特率");
        return;
    }
    char hex[3 * sizeof(buf) + 1] = {0};
    for (size_t i = 0; i < n; ++i) {
        snprintf(hex + i * 3, 4, "%02X ", buf[i]);
    }
    ESP_LOGE(TAG, "received %u raw bytes: %s", static_cast<unsigned>(n), hex);
}

// ---------- 协议帧 ----------

bool Pn532::SendCommand(const uint8_t* data, size_t len, uint32_t timeout_ms, bool wakeup) {
    // 帧格式: 00 00 FF LEN LCS TFI DATA DCS 00
    std::vector<uint8_t> frame;
    frame.reserve(len + 8 + 16);
    frame.push_back(0x00);                                // PREAMBLE
    frame.push_back(0x00);                                // START
    frame.push_back(0xFF);
    frame.push_back(static_cast<uint8_t>(len + 1));       // LEN = TFI + DATA
    frame.push_back(Checksum(&frame[3], 1));              // LCS

    std::vector<uint8_t> tfi_data;
    tfi_data.reserve(len + 1);
    tfi_data.push_back(kTfiHost);
    tfi_data.insert(tfi_data.end(), data, data + len);
    frame.insert(frame.end(), tfi_data.begin(), tfi_data.end());
    frame.push_back(Checksum(tfi_data.data(), tfi_data.size()));  // DCS
    frame.push_back(0x00);                                // POSTAMBLE

    // HSU 唤醒序列：0x55 0x55 + 14 个 0x00
    std::vector<uint8_t> stream;
    if (wakeup) {
        stream.assign({0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    }
    stream.insert(stream.end(), frame.begin(), frame.end());

    UartFlush();
    if (UartWrite(stream.data(), stream.size()) != ESP_OK) {
        return false;
    }
    // 唤醒后模块需要一点时间准备
    if (wakeup) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return WaitAck(timeout_ms);
}

bool Pn532::WaitAck(uint32_t timeout_ms) {
    uint8_t ack[6] = {0};
    size_t got = 0;
    int64_t end = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (got < sizeof(ack) && esp_timer_get_time() < end) {
        int remaining_ms = static_cast<int>((end - esp_timer_get_time()) / 1000);
        if (remaining_ms <= 0) {
            break;
        }
        size_t n = UartRead(ack + got, sizeof(ack) - got, std::max(5, remaining_ms));
        if (n > 0) {
            got += n;
        }
    }
    return got == sizeof(ack) && memcmp(ack, kAckFrame, sizeof(ack)) == 0;
}

bool Pn532::ReadFrame(std::vector<uint8_t>& out_payload, uint32_t timeout_ms) {
    auto read_byte = [&](uint8_t& b, int to) -> bool {
        return UartRead(&b, 1, to) == 1;
    };

    int64_t end = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;

    // 同步起始码 0x00 0xFF（部分响应前面带一个 0x00 前导）
    uint8_t b = 0;
    bool prev_zero = false;
    bool synced = false;
    while (esp_timer_get_time() < end) {
        int remaining_ms = static_cast<int>((end - esp_timer_get_time()) / 1000);
        if (remaining_ms <= 0) {
            break;
        }
        if (!read_byte(b, std::max(5, remaining_ms))) {
            return false;
        }
        if (prev_zero && b == 0xFF) {
            synced = true;
            break;
        }
        prev_zero = (b == 0x00);
    }
    if (!synced) {
        return false;
    }

    uint8_t len = 0;
    uint8_t lcs = 0;
    if (!read_byte(len, 100) || !read_byte(lcs, 100)) {
        return false;
    }
    if (static_cast<uint8_t>(len + lcs) != 0) {
        ESP_LOGW(TAG, "LEN/LCS mismatch: len=%02X lcs=%02X", len, lcs);
        return false;
    }
    if (len < 2) {
        return false;
    }

    // LEN 个字节(TFI+DATA) + 1 字节 DCS
    std::vector<uint8_t> body(len + 1);
    size_t got = 0;
    while (got < body.size() && esp_timer_get_time() < end) {
        size_t n = UartRead(body.data() + got, body.size() - got, 50);
        if (n > 0) {
            got += n;
        }
    }
    if (got < body.size()) {
        return false;
    }

    uint8_t dcs = body.back();
    body.pop_back();
    if (dcs != Checksum(body.data(), body.size())) {
        ESP_LOGW(TAG, "DCS mismatch: got=%02X", dcs);
        return false;
    }
    if (body.empty() || body[0] != kTfiPn532) {
        ESP_LOGW(TAG, "unexpected TFI: %02X", body.empty() ? 0 : body[0]);
        return false;
    }

    out_payload.assign(body.begin() + 1, body.end());

    uint8_t post = 0;
    read_byte(post, 5);   // 丢弃 POSTAMBLE，失败也无所谓
    return true;
}

// ---------- 命令 ----------

bool Pn532::SamConfig(bool wakeup) {
    // 正常模式，不使用 SAM，超时 0
    uint8_t cmd[] = {kCmdSamConfig, 0x01, 0x00, 0x00};
    if (!SendCommand(cmd, sizeof(cmd), 200, wakeup)) {
        return false;
    }
    std::vector<uint8_t> resp;
    return ReadFrame(resp, 300);
}

bool Pn532::GetFirmwareVersion(uint32_t& version) {
    uint8_t cmd[] = {kCmdGetFirmwareVersion};
    if (!SendCommand(cmd, sizeof(cmd), 500)) {
        return false;
    }
    std::vector<uint8_t> resp;
    if (!ReadFrame(resp, 500) || resp.size() < 5) {
        return false;
    }
    version = (static_cast<uint32_t>(resp[1]) << 24) |
              (static_cast<uint32_t>(resp[2]) << 16) |
              (static_cast<uint32_t>(resp[3]) << 8) | resp[4];
    return true;
}

bool Pn532::InListPassiveTarget(uint8_t* uid, uint8_t& uid_len) {
    // 一次轮询 1 张 106 kbps Type A 卡
    uint8_t cmd[] = {kCmdInListPassiveTarget, 0x01, 0x00};
    if (!SendCommand(cmd, sizeof(cmd), 200)) {
        return false;
    }
    std::vector<uint8_t> resp;
    if (!ReadFrame(resp, 300)) {
        return false;
    }
    // resp: 4B NbTg Tg SENS_RES(2) SEL_RES NFCIDLength NFCID...
    if (resp.size() < 7 || resp[0] != 0x4B || resp[1] == 0) {
        return false;   // 未发现卡片
    }
    uint8_t n = resp[6];
    if (n == 0 || n > 10 || resp.size() < static_cast<size_t>(7 + n)) {
        return false;
    }
    memcpy(uid, &resp[7], n);
    uid_len = n;
    return true;
}

bool Pn532::InDataExchange(const uint8_t* tx, size_t tx_len, std::vector<uint8_t>& rx) {
    std::vector<uint8_t> cmd;
    cmd.reserve(tx_len + 2);
    cmd.push_back(kCmdInDataExchange);
    cmd.push_back(0x01);    // Tg = 1
    cmd.insert(cmd.end(), tx, tx + tx_len);

    if (!SendCommand(cmd.data(), cmd.size(), 200)) {
        return false;
    }
    std::vector<uint8_t> resp;
    if (!ReadFrame(resp, 300)) {
        return false;
    }
    // resp: 41 Status Data...
    if (resp.size() < 2 || resp[0] != 0x41 || resp[1] != 0x00) {
        return false;
    }
    rx.assign(resp.begin() + 2, resp.end());
    return true;
}

// ---------- NDEF ----------

bool Pn532::ReadNdefText(std::string& out_text) {
    // NTAG21x / Mifare Ultralight：READ(0x30) 一次返回 4 页共 16 字节
    // 从第 3 页开始读，可覆盖 CC 与 NDEF TLV 头部
    std::vector<uint8_t> data;
    for (uint8_t page = 3; page <= 11; page += 4) {
        uint8_t read_cmd[2] = {0x30, page};
        std::vector<uint8_t> chunk;
        if (!InDataExchange(read_cmd, sizeof(read_cmd), chunk) || chunk.size() < 16) {
            break;
        }
        data.insert(data.end(), chunk.begin(), chunk.begin() + 16);
    }
    if (data.size() < 8) {
        return false;
    }
    // 第 3 页首字节为 Capability Container 魔术字节
    if (data[0] != 0xE1) {
        ESP_LOGW(TAG, "no NDEF (CC magic = %02X)", data[0]);
        return false;
    }

    // 在 TLV 区中查找 NDEF Message TLV (T = 0x03)
    size_t i = 4;              // 跳过 4 字节 CC
    size_t ndef_off = 0;
    size_t ndef_len = 0;
    bool found = false;
    while (i < data.size()) {
        uint8_t t = data[i];
        if (t == 0x00) {       // NULL TLV
            ++i;
            continue;
        }
        if (t == 0xFE) {       // Terminator TLV
            break;
        }
        if (t == 0x03) {       // NDEF Message TLV
            ++i;
            if (i >= data.size()) break;
            if (data[i] == 0xFF) {
                if (i + 2 >= data.size()) break;
                ndef_len = (static_cast<size_t>(data[i + 1]) << 8) | data[i + 2];
                i += 3;
            } else {
                ndef_len = data[i];
                i += 1;
            }
            ndef_off = i;
            found = (ndef_len > 0);
            break;
        }
        // 其他 TLV：跳过 Value
        if (i + 1 >= data.size()) break;
        i += 2 + data[i + 1];
    }
    if (!found) {
        ESP_LOGW(TAG, "NDEF TLV not found");
        return false;
    }

    // 若 NDEF 超出已读范围，继续向后读取
    std::vector<uint8_t> ndef_data;
    if (ndef_off + ndef_len > data.size()) {
        ndef_data.assign(data.begin() + ndef_off, data.end());
        uint8_t page = static_cast<uint8_t>(3 + data.size() / 4);
        while (ndef_data.size() < ndef_len && page <= 0xE0) {
            uint8_t read_cmd[2] = {0x30, page};
            std::vector<uint8_t> chunk;
            if (!InDataExchange(read_cmd, sizeof(read_cmd), chunk) || chunk.size() < 16) {
                break;
            }
            size_t need = std::min(ndef_len - ndef_data.size(), static_cast<size_t>(16));
            ndef_data.insert(ndef_data.end(), chunk.begin(), chunk.begin() + need);
            page += 4;
        }
    } else {
        ndef_data.assign(data.begin() + ndef_off, data.begin() + ndef_off + ndef_len);
    }
    if (ndef_data.empty()) {
        return false;
    }

    // 解析第一条 NDEF Record
    uint8_t header = ndef_data[0];
    bool short_record = (header & 0x10) != 0;
    bool has_id = (header & 0x08) != 0;
    uint8_t tnf = header & 0x07;
    size_t idx = 1;
    if (idx >= ndef_data.size()) return false;

    uint8_t type_len = ndef_data[idx++];
    size_t payload_len = 0;
    if (short_record) {
        if (idx >= ndef_data.size()) return false;
        payload_len = ndef_data[idx++];
    } else {
        if (idx + 4 > ndef_data.size()) return false;
        payload_len = (static_cast<size_t>(ndef_data[idx]) << 24) |
                      (static_cast<size_t>(ndef_data[idx + 1]) << 16) |
                      (static_cast<size_t>(ndef_data[idx + 2]) << 8) |
                      ndef_data[idx + 3];
        idx += 4;
    }
    if (has_id) {
        if (idx >= ndef_data.size()) return false;
        idx += 1 + ndef_data[idx];
    }
    if (idx + type_len > ndef_data.size()) return false;
    std::string type(reinterpret_cast<const char*>(ndef_data.data() + idx), type_len);
    idx += type_len;
    if (idx >= ndef_data.size()) return false;
    payload_len = std::min(payload_len, ndef_data.size() - idx);
    const uint8_t* payload = ndef_data.data() + idx;

    out_text.clear();
    if (tnf == 0x01 && type == "T") {
        // 文本记录：首字节为语言编码长度
        if (payload_len < 2) return false;
        size_t lang_len = payload[0] & 0x3F;
        if (1 + lang_len > payload_len) return false;
        out_text.assign(reinterpret_cast<const char*>(payload + 1 + lang_len),
                        payload_len - 1 - lang_len);
        return true;
    }
    if (tnf == 0x01 && type == "U") {
        // URI 记录
        if (payload_len < 2) return false;
        static const char* kPrefix[] = {
            "", "http://www.", "https://www.", "http://", "https://",
            "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.",
            "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://",
            "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:",
            "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
            "tcpobex://", "irdaobex://", "file://", "urn:epc:id:",
            "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
            "urn:nfc:"
        };
        uint8_t prefix_idx = payload[0];
        const char* prefix = (prefix_idx < sizeof(kPrefix) / sizeof(kPrefix[0]))
                                 ? kPrefix[prefix_idx] : "";
        out_text = std::string(prefix) +
                   std::string(reinterpret_cast<const char*>(payload + 1), payload_len - 1);
        return true;
    }
    // 其他类型按原样输出
    out_text.assign(reinterpret_cast<const char*>(payload), payload_len);
    return !out_text.empty();
}

// ---------- 后台轮询 ----------

void Pn532::PollTaskEntry(void* arg) {
    static_cast<Pn532*>(arg)->PollLoop();
}

void Pn532::PollLoop() {
    constexpr int kIdleIntervalMs    = 300;   // 无卡时轮询间隔
    constexpr int kPresentIntervalMs = 300;   // 卡在场时检测间隔

    while (running_.load()) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        bool found = InListPassiveTarget(uid, uid_len);

        if (!card_present_) {
            if (!found) {
                vTaskDelay(pdMS_TO_TICKS(kIdleIntervalMs));
                continue;
            }

            // 检测到新卡片
            memcpy(current_uid_, uid, uid_len);
            current_uid_len_ = uid_len;
            card_present_ = true;

            CardInfo info;
            memcpy(info.uid, uid, uid_len);
            info.uid_len = uid_len;
            ReadNdefText(info.ndef_text);

            char uid_str[32] = {0};
            for (uint8_t i = 0; i < uid_len; ++i) {
                snprintf(uid_str + i * 3, sizeof(uid_str) - i * 3, "%02X ", uid[i]);
            }
            ESP_LOGI(TAG, "card in, UID=%sNDEF=\"%s\"", uid_str, info.ndef_text.c_str());

            if (on_detected_) {
                on_detected_(info);
            }
        } else {
            // 卡片仍在场判断：UID 一致视为同一张卡
            if (found && uid_len == current_uid_len_ &&
                memcmp(uid, current_uid_, current_uid_len_) == 0) {
                vTaskDelay(pdMS_TO_TICKS(kPresentIntervalMs));
                continue;
            }

            ESP_LOGI(TAG, "card out");
            card_present_ = false;
            current_uid_len_ = 0;
            memset(current_uid_, 0, sizeof(current_uid_));
            if (on_removed_) {
                on_removed_();
            }
            vTaskDelay(pdMS_TO_TICKS(kIdleIntervalMs));
        }
    }

    task_ = nullptr;
    vTaskDelete(nullptr);
}
