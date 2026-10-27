#include "uart_audio_codec.h"

#include "application.h"
#include "protocol.h"

#include <esp_log.h>
#include <cstring>

#define TAG "UartAudioCodec"

/* CI1302 上行 Opus 帧队列元素：定长缓冲 + 实际长度（兼容不同帧长配置）。
 * 注意：这是全局类型，不能引用类内 private 常量，故用文件级宏保持一致。 */
#define UPLINK_OPUS_ITEM_BYTES 256  // 与 UartAudioCodec::UPLINK_OPUS_FRAME_MAX 一致
typedef struct {
    uint16_t len;
    uint8_t data[UPLINK_OPUS_ITEM_BYTES];
} uplink_opus_item_t;

/* ------------------------------------------------------------------ */
/* 构造 / 析构                                                          */
/* ------------------------------------------------------------------ */

UartAudioCodec::UartAudioCodec(uart_port_t uart_port, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate)
    : uart_port_(uart_port), tx_pin_(tx_pin), rx_pin_(rx_pin), baud_rate_(baud_rate) {
}

UartAudioCodec::~UartAudioCodec() {
    if (rx_task_handle_ != nullptr) {
        vTaskDelete(rx_task_handle_);
        rx_task_handle_ = nullptr;
    }
    if (play_task_handle_ != nullptr) {
        vTaskDelete(play_task_handle_);
        play_task_handle_ = nullptr;
    }
    SpeexDeinit();
    if (shine_enc_ != nullptr) {
        shine_close(shine_enc_);
        shine_enc_ = nullptr;
    }
    if (downlink_mp3_sb_ != nullptr) {
        vStreamBufferDelete(downlink_mp3_sb_);
        downlink_mp3_sb_ = nullptr;
    }
    if (uplink_opus_queue_ != nullptr) {
        vQueueDelete(uplink_opus_queue_);
        uplink_opus_queue_ = nullptr;
    }
    if (play_req_queue_ != nullptr) {
        vQueueDelete(play_req_queue_);
        play_req_queue_ = nullptr;
    }
    if (tx_mutex_ != nullptr) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
    if (state_mutex_ != nullptr) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    if (mp3_mutex_ != nullptr) {
        vSemaphoreDelete(mp3_mutex_);
        mp3_mutex_ = nullptr;
    }
    uart_driver_delete(uart_port_);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

void UartAudioCodec::Start() {
    InitUart();

    tx_mutex_ = xSemaphoreCreateMutex();
    state_mutex_ = xSemaphoreCreateMutex();
    mp3_mutex_ = xSemaphoreCreateMutex();
    uplink_opus_queue_ = xQueueCreate(UPLINK_OPUS_QUEUE_LEN, sizeof(uplink_opus_item_t));
    downlink_mp3_sb_ = xStreamBufferCreate(DOWNLINK_MP3_BUF_SIZE, 1);
    play_req_queue_ = xQueueCreate(8, sizeof(uint32_t));
    configASSERT(tx_mutex_ != nullptr && state_mutex_ != nullptr && mp3_mutex_ != nullptr);
    configASSERT(uplink_opus_queue_ != nullptr && downlink_mp3_sb_ != nullptr && play_req_queue_ != nullptr);

    if (!Mp3EncoderInit()) {
        ESP_LOGE(TAG, "MP3 encoder init failed");
    }
    if (!SpeexInit()) {
        ESP_LOGE(TAG, "Speex decoder init failed");
    }

    /* RX 任务栈 32KB：该任务除收包解析外还要做 Speex 解码 + Opus 编码。
     * esp-opus 以 -DUSE_ALLOCA 编译，opus_encode() 的临时区全部走 alloca，
     * 16kHz/60ms（960 点）单帧实测需要 20KB 以上；8KB 会直接触发栈溢出，
     * 16KB 仍不够，溢出会向下写穿相邻堆块（覆盖 OpusEncoderWrapper 里的
     * std::mutex，表现为 Encode 返回时 pthread_mutex_unlock 崩溃）。
     * 参考：主线小智给做 Opus 编解码的 opus_codec 任务 2048*13=26KB。 */
    xTaskCreate(RxTaskEntry, "ci1302_rx", 32768, this, 4, &rx_task_handle_);
    xTaskCreate(PlayTaskEntry, "ci1302_play", 8192, this, 4, &play_task_handle_);

    /* 下发 VAD 结束判定参数：静音 500ms 判为说完（0x0110 参数 20）。
     * 1302 该参数为运行期 RAM 值，每次上电必须重发，否则回落到出厂默认
     * （默认较长，导致用户说完话要等好几秒才 VAD_END/上报）。 */
    {
        uint8_t vad_param = VAD_END_SILENCE_INDEX;
        SendFrame(CI_CMD_SET_VAD_SENSITIVITY_ACTIVATE_LENTH, &vad_param, 1);
        ESP_LOGI(TAG, "Set CI1302 VAD end silence index=%u (500ms)", vad_param);
    }

    /* 基类标志：本 codec 无本地输入/输出硬件，恒使能，
     * 防止 AudioService 音频电源看门狗（15s 无活动关闭 IO）停掉直通链路 */
    input_enabled_ = true;
    output_enabled_ = true;
    duplex_ = true;
    input_sample_rate_ = 16000;
    output_sample_rate_ = MP3_SAMPLE_RATE;
    input_channels_ = 1;
    output_channels_ = 1;

    ESP_LOGI(TAG, "UartAudioCodec started: UART%d @%d baud, TX=GPIO%d RX=GPIO%d",
             uart_port_, baud_rate_, tx_pin_, rx_pin_);
}

void UartAudioCodec::SetOutputVolume(int volume) {
    AudioCodec::SetOutputVolume(volume);
    /* 映射 0-100 -> CI1302 音量 1-7 */
    int v = 1 + (volume * 6) / 100;
    if (v < 1) v = 1;
    if (v > 7) v = 7;
    uint8_t payload = (uint8_t)v;
    SendFrame(CI_CMD_SET_AUDIO_VOLUME, &payload, 1);
    ESP_LOGI(TAG, "Set CI1302 volume: %d (raw %d)", volume, v);
}

/* ------------------------------------------------------------------ */
/* UART 初始化与收发                                                     */
/* ------------------------------------------------------------------ */

void UartAudioCodec::InitUart() {
    uart_config_t cfg = {};
    cfg.baud_rate = baud_rate_;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(uart_port_, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_port_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* RX 16KB：921600 下 1302 峰值（每 5ms 一包 96B）持续 800ms 不丢；TX 4KB 缓冲防短时背压丢包 */
    ESP_ERROR_CHECK(uart_driver_install(uart_port_, 16384, 4096, 0, nullptr, 0));
}

void UartAudioCodec::SendFrame(uint16_t cmd, const uint8_t* payload, uint16_t len) {
    if (len > CIAS_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "SendFrame payload too long: %u", len);
        return;
    }
    /* 帧缓冲放堆上（vector）：栈上 16+2048B 数组在 8KB 任务栈内仍挤占深调用链，
     * 且 SendFrame 可能被任意任务调用，堆分配彻底消除栈溢出风险 */
    std::vector<uint8_t> frame(CIAS_HEADER_SIZE + len);
    /* magic 0x5A5AA5A5，小端字节序：a5 a5 5a 5a */
    frame[0] = 0xa5;
    frame[1] = 0xa5;
    frame[2] = 0x5a;
    frame[3] = 0x5a;
    uint16_t checksum = 0;
    for (uint16_t i = 0; i < len; i++) {
        checksum += payload[i];
    }
    frame[4] = (uint8_t)(checksum & 0xff);
    frame[5] = (uint8_t)(checksum >> 8);
    frame[6] = (uint8_t)(cmd & 0xff);
    frame[7] = (uint8_t)(cmd >> 8);
    frame[8] = (uint8_t)(len & 0xff);
    frame[9] = (uint8_t)(len >> 8);
    frame[10] = (uint8_t)(CIAS_VERSION_DEF & 0xff);
    frame[11] = (uint8_t)(CIAS_VERSION_DEF >> 8);
    /* fill_data 0x12345678，小端 */
    frame[12] = 0x78;
    frame[13] = 0x56;
    frame[14] = 0x34;
    frame[15] = 0x12;
    if (len > 0) {
        memcpy(frame.data() + CIAS_HEADER_SIZE, payload, len);
    }
    xSemaphoreTake(tx_mutex_, portMAX_DELAY);
    uart_write_bytes(uart_port_, frame.data(), frame.size());
    xSemaphoreGive(tx_mutex_);
}

void UartAudioCodec::SendCmd(uint16_t cmd) {
    SendFrame(cmd, nullptr, 0);
}

void UartAudioCodec::RxTaskEntry(void* arg) {
    UartAudioCodec* self = (UartAudioCodec*)arg;
    self->RxTaskLoop();
    vTaskDelete(NULL);
}

void UartAudioCodec::RxTaskLoop() {
    uint8_t tmp[512];
    uint32_t rx_total = 0;
    TickType_t last_beat = xTaskGetTickCount();
    while (true) {
        int n = uart_read_bytes(uart_port_, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (n > 0) {
            rx_total += (uint32_t)n;
            rx_parse_buf_.insert(rx_parse_buf_.end(), tmp, tmp + n);
            ParseAndDispatch();
        }
        /* 心跳：10s 一条，用于区分「1302 未上报」与「ESP32 未收到」 */
        TickType_t now = xTaskGetTickCount();
        if (now - last_beat >= pdMS_TO_TICKS(10000)) {
            last_beat = now;
            ESP_LOGI(TAG, "rx alive: bytes=%u parse_buf=%u stack_free=%u",
                     (unsigned)rx_total, (unsigned)rx_parse_buf_.size(),
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    }
}

void UartAudioCodec::ParseAndDispatch() {
    while (true) {
        if (rx_parse_buf_.size() < 4) {
            break;
        }
        /* 同步 magic：a5 a5 5a 5a */
        size_t off = rx_parse_buf_.size();
        for (size_t i = 0; i + 3 < rx_parse_buf_.size(); i++) {
            if (rx_parse_buf_[i] == 0xa5 && rx_parse_buf_[i + 1] == 0xa5 &&
                rx_parse_buf_[i + 2] == 0x5a && rx_parse_buf_[i + 3] == 0x5a) {
                off = i;
                break;
            }
        }
        if (off == rx_parse_buf_.size()) {
            /* 未找到 magic：保留末尾最多 3 字节（可能跨包），其余丢弃 */
            if (rx_parse_buf_.size() > 3) {
                rx_parse_buf_.erase(rx_parse_buf_.begin(), rx_parse_buf_.end() - 3);
            }
            break;
        }
        if (off > 0) {
            rx_parse_buf_.erase(rx_parse_buf_.begin(), rx_parse_buf_.begin() + off);
        }
        if (rx_parse_buf_.size() < CIAS_HEADER_SIZE) {
            break;
        }
        uint16_t cmd = rx_parse_buf_[6] | (uint16_t)(rx_parse_buf_[7] << 8);
        uint16_t len = rx_parse_buf_[8] | (uint16_t)(rx_parse_buf_[9] << 8);
        if (len > CIAS_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "Invalid frame len=%u, resync", len);
            rx_parse_buf_.erase(rx_parse_buf_.begin(), rx_parse_buf_.begin() + 1);
            continue;
        }
        if (rx_parse_buf_.size() < CIAS_HEADER_SIZE + len) {
            break;  /* 等待完整帧 */
        }
        /* 校验 checksum：负载逐字节累加，uint16 小端 */
        uint16_t sum = rx_parse_buf_[4] | (uint16_t)(rx_parse_buf_[5] << 8);
        uint16_t calc = 0;
        for (size_t i = 0; i < len; i++) {
            calc += rx_parse_buf_[CIAS_HEADER_SIZE + i];
        }
        if (sum != calc) {
            ESP_LOGW(TAG, "Checksum mismatch cmd=0x%04x len=%u, drop", cmd, len);
            rx_parse_buf_.erase(rx_parse_buf_.begin(), rx_parse_buf_.begin() + 1);
            continue;
        }
        DispatchFrame(cmd, rx_parse_buf_.data() + CIAS_HEADER_SIZE, len);
        rx_parse_buf_.erase(rx_parse_buf_.begin(), rx_parse_buf_.begin() + CIAS_HEADER_SIZE + len);
    }
}

void UartAudioCodec::DispatchFrame(uint16_t cmd, const uint8_t* payload, uint16_t len) {
    switch (cmd) {
    case CI_CMD_WAKE_UP:
        ESP_LOGI(TAG, "CI1302 WAKE_UP");
        OnWakeUpEvent();
        break;
    case CI_CMD_VAD_START:
        /* 1302 自己 VAD 检测到人声开始，已自动开始推 Speex。仅日志。 */
        ESP_LOGI(TAG, "CI1302 VAD_START");
        break;
    case CI_CMD_LOCAL_ASR_NOTIFY: {
        /* 本地命令词：Listening 结束聆听；Speaking 打断 TTS。不做"不上云"特判。 */
        uint16_t cmd_id = (len >= 2) ? (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8)) : 0;
        ESP_LOGI(TAG, "CI1302 LOCAL_ASR_NOTIFY cmd_id=0x%04x", cmd_id);
        auto& app = Application::GetInstance();
        DeviceState st = app.GetDeviceState();
        if (st == kDeviceStateListening) {
            app.StopListening();
        } else if (st == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
        break;
    }
    case CI_CMD_VAD_END:
        //日志，不干预
        ESP_LOGE(TAG, "CI1302 VAD_END");
        break;
    case CI_CMD_PCM_FINISH:
        /* 1302 VAD 判定说完了，已停止上传。补齐最后一帧，再补一段静音。
         * 云端 listen mode=auto 的 VAD 靠「收到静音」判句尾，而 1302 在
         * VAD_END 之后完全不上传（连静音帧都没有），服务器就永远等不到
         * 句尾，会把后面几轮话攒成一大段最后一次性提交（表现为一直不结束、
         * 然后突然识别出一长串重复文本）。 */
        ESP_LOGI(TAG, "CI1302 PCM_FINISH");
        FlushUplinkPcm();
        AppendUplinkSilence();
        break;
    case CI_CMD_PCM_MIDDLE:
        /* 负载为 Speex 帧：首字节为码流长度（典型 0x2A=42），其后为 42 字节码流。
         * ESP32 解码为 PCM 后再 Opus 编码上云（小智服务端只接受 Opus）。 */
        DecodeSpeexAndForward(payload, len);
        break;
    case CI_CMD_EXIT_WAKE_UP:
        ESP_LOGD(TAG, "CI1302 EXIT_WAKE_UP");
        break;
    case CI_CMD_PLAY_DATA_GET: {
        if (len >= 4) {
            uint32_t request = payload[0] | ((uint32_t)payload[1] << 8) |
                               ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
            if (request == 0 || request > PLAY_DATA_MAX_CHUNK) {
                request = PLAY_DATA_MAX_CHUNK;
            }
            /* 拉流请求交给播放任务串行处理；队列满则丢弃（1302 每 ~200ms 会再请求） */
            xQueueSend(play_req_queue_, &request, 0);
        }
        break;
    }
    case CI_CMD_PLAY_TTS_END:
        ESP_LOGI(TAG, "CI1302 PLAY_TTS_END (playback done)");
        xSemaphoreTake(state_mutex_, portMAX_DELAY);
        if (play_state_ != PlayState::kIdle) {
            AbortPlaySessionInternal("1302 reported play end");
        }
        xSemaphoreGive(state_mutex_);
        break;
    case CI_CMD_AUDIO_SYS_READY:
        ESP_LOGI(TAG, "CI1302 audio system ready");
        break;
    case CI_CMD_CMD_EXEC_STATE:
        ESP_LOGD(TAG, "CI1302 cmd exec state: %02x %02x %02x",
                 len > 0 ? payload[0] : 0, len > 1 ? payload[1] : 0, len > 2 ? payload[2] : 0);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled CI1302 cmd 0x%04x len=%u", cmd, len);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 上行：1302 Speex 解码 -> Opus 编码                                    */
/* ------------------------------------------------------------------ */

void UartAudioCodec::OnWakeUpEvent() {
    /* 1302 主动唤醒：WAKE_UP 到达时触发回调，由板级决定怎么处理。
     * codec 层不依赖 Application，零耦合——对齐 VB6824 模式。 */
    if (wake_up_callback_) {
        wake_up_callback_();
    }
}

bool UartAudioCodec::SpeexInit() {
    if (!speex_bits_inited_) {
        speex_bits_init(&speex_bits_);
        speex_bits_inited_ = true;
    }
    if (speex_dec_ == nullptr) {
        /* Speex 宽带模式：16kHz，20ms/帧(320 点)，对应 CI1302 的 43 字节上行包 */
        speex_dec_ = speex_decoder_init(&speex_wb_mode);
        if (speex_dec_ == nullptr) {
            return false;
        }
        int enh = 1;
        speex_decoder_ctl(speex_dec_, SPEEX_SET_ENH, &enh);
        int frame_size = 0;
        if (speex_decoder_ctl(speex_dec_, SPEEX_GET_FRAME_SIZE, &frame_size) == 0 && frame_size > 0) {
            speex_frame_samples_ = frame_size;
        }
    }
    if (uplink_opus_encoder_ == nullptr) {
        uplink_opus_encoder_ = std::make_unique<OpusEncoderWrapper>(
            UPLINK_SAMPLE_RATE, 1, UPLINK_OPUS_FRAME_MS);
        uplink_opus_encoder_->SetComplexity(0);
    }
    ESP_LOGI(TAG, "Speex WB decoder ready, frame=%d samples", speex_frame_samples_);
    return true;
}

void UartAudioCodec::SpeexDeinit() {
    if (speex_dec_ != nullptr) {
        speex_decoder_destroy(speex_dec_);
        speex_dec_ = nullptr;
    }
    if (speex_bits_inited_) {
        speex_bits_destroy(&speex_bits_);
        speex_bits_inited_ = false;
    }
    uplink_opus_encoder_.reset();
}

void UartAudioCodec::DecodeSpeexAndForward(const uint8_t* payload, uint16_t len) {
    if (speex_dec_ == nullptr || payload == nullptr || len == 0) {
        return;
    }

    /* 启英泰伦协议（cias_speex_compressed_data）：负载首字节为码流长度
     * （典型 0x2A = 42），其后为 Speex 码流；1302 按 43 字节定长上传，
     * 码流不足时尾部补 0，因此必须按首字节长度截取，不能直接整包解码。
     * 若首字节大于 len-1（不可能是长度），则按无长度字节固件兼容处理。 */
    const uint8_t* data = payload;
    int data_len = len;
    if (data_len >= 2 && payload[0] > 1 && payload[0] <= data_len - 1) {
        data_len = payload[0];
        ++data;
    }
    if (data_len <= 1) {
        return;
    }

    /* 每个 PCM_MIDDLE 包恰好携带一帧 Speex（20ms / 320 点），解码一次 */
    speex_bits_read_from(&speex_bits_, reinterpret_cast<const char*>(data), data_len);
    std::vector<int16_t> pcm(speex_frame_samples_);
    int ret = speex_decode_int(speex_dec_, &speex_bits_, pcm.data());
    if (ret != 0) {
        ESP_LOGW(TAG, "Speex decode error %d (len=%d)", ret, data_len);
        return;
    }
    EncodeUplinkPcm(std::move(pcm));
}

void UartAudioCodec::EncodeUplinkPcm(std::vector<int16_t>&& pcm) {
    if (uplink_opus_encoder_ == nullptr || pcm.empty()) {
        return;
    }
    const int frame_samples = UPLINK_SAMPLE_RATE * UPLINK_OPUS_FRAME_MS / 1000;
    uplink_pcm_residual_ = (uplink_pcm_residual_ + (int)pcm.size()) % frame_samples;
    uplink_opus_encoder_->Encode(std::move(pcm), [this](std::vector<uint8_t>&& opus) {
        QueueUplinkOpus(opus.data(), (uint16_t)opus.size());
    });
    /* 首次编码成功后打印一次栈余量（字节）：opus_encode 走 alloca，
     * 栈消耗随帧长变化，用这个值确认 32KB 是否仍有足够余量。 */
    if (!uplink_stack_reported_) {
        uplink_stack_reported_ = true;
        ESP_LOGI(TAG, "uplink encode ok, rx stack high-water free=%u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}

void UartAudioCodec::FlushUplinkPcm() {
    if (uplink_opus_encoder_ == nullptr || uplink_pcm_residual_ == 0) {
        return;
    }
    const int frame_samples = UPLINK_SAMPLE_RATE * UPLINK_OPUS_FRAME_MS / 1000;
    std::vector<int16_t> pad(frame_samples - uplink_pcm_residual_, 0);
    /* 补零凑满 60ms Opus 帧，避免丢失最后不足一帧的语音 */
    EncodeUplinkPcm(std::move(pad));
}

void UartAudioCodec::AppendUplinkSilence() {
    if (uplink_opus_encoder_ == nullptr) {
        return;
    }
    /* 按 60ms 一帧逐步喂，避免一次性分配大块内存；
     * 采用 1.5s 静音：云端 auto VAD 的静音判决门限各版本不同（经验 0.8~1.2s），
     * 之前 780ms 时偶尔判不到句尾，会和下一句合并成一段提交。 */
    const int frame_samples = UPLINK_SAMPLE_RATE * UPLINK_OPUS_FRAME_MS / 1000;
    const int frames = UPLINK_TAIL_SILENCE_MS / UPLINK_OPUS_FRAME_MS;
    for (int i = 0; i < frames; i++) {
        EncodeUplinkPcm(std::vector<int16_t>(frame_samples, 0));
    }
    ESP_LOGD(TAG, "Append %d ms silence after VAD end", frames * UPLINK_OPUS_FRAME_MS);
}

void UartAudioCodec::QueueUplinkOpus(const uint8_t* payload, uint16_t len) {
    if (len == 0 || len > UPLINK_OPUS_FRAME_MAX) {
        ESP_LOGW(TAG, "Invalid uplink opus frame len=%u", len);
        return;
    }
    uplink_opus_item_t item;
    item.len = len;
    memcpy(item.data, payload, len);
    if (xQueueSend(uplink_opus_queue_, &item, 0) != pdTRUE) {
        /* 队列满：丢弃最旧帧，保证实时性 */
        uplink_opus_item_t dummy;
        xQueueReceive(uplink_opus_queue_, &dummy, 0);
        xQueueSend(uplink_opus_queue_, &item, 0);
    }
    /* 唤醒 OpusCodecTask 消费 */
    Application::GetInstance().GetAudioService().NotifyCodecTask();
}

bool UartAudioCodec::HasUplinkOpusPacket() {
    return uplink_opus_queue_ != nullptr && uxQueueMessagesWaiting(uplink_opus_queue_) > 0;
}

bool UartAudioCodec::PopUplinkOpusPacket(std::unique_ptr<AudioStreamPacket>& packet) {
    uplink_opus_item_t item;
    if (xQueueReceive(uplink_opus_queue_, &item, 0) != pdTRUE) {
        return false;
    }
    auto pkt = std::make_unique<AudioStreamPacket>();
    pkt->frame_duration = UPLINK_OPUS_FRAME_MS;  /* ESP32 重新编码后的 Opus 帧 60ms */
    pkt->sample_rate = UPLINK_SAMPLE_RATE;
    pkt->timestamp = 0;          /* 转码链路无本地采集时间戳，服务器 AEC 时间戳缺省 */
    pkt->payload.assign(item.data, item.data + item.len);
    packet = std::move(pkt);
    return true;
}

/* 上行由 codec 内部完成 Speex 解码与 Opus 编码，不向 AudioService 提供 PCM */
int UartAudioCodec::Read(int16_t* dest, int samples) {
    return 0;
}

/* ------------------------------------------------------------------ */
/* 下行：PCM -> shine MP3 -> 1302 拉流                                  */
/* ------------------------------------------------------------------ */

int UartAudioCodec::Write(const int16_t* data, int samples) {
    last_write_tick_ = xTaskGetTickCount();

    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (play_state_ == PlayState::kIdle) {
        /* 新一路 TTS 音频到达：进入预缓冲，凑够 PLAY_PREBUFFER_BYTES 再发 NET_PLAY_START */
        play_state_ = PlayState::kPrebuffer;
        session_start_tick_ = last_write_tick_;
        stream_end_requested_ = false;
        stream_end_reported_ = false;
        xStreamBufferReset(downlink_mp3_sb_);
    }
    xSemaphoreGive(state_mutex_);

    xSemaphoreTake(mp3_mutex_, portMAX_DELAY);
    Mp3Encode(data, samples);
    xSemaphoreGive(mp3_mutex_);

    StartPlaySessionIfReady();
    return samples;
}

void UartAudioCodec::StartPlaySessionIfReady() {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (play_state_ == PlayState::kPrebuffer) {
        size_t avail = xStreamBufferBytesAvailable(downlink_mp3_sb_);
        TickType_t now = xTaskGetTickCount();
        if (avail >= PLAY_PREBUFFER_BYTES ||
            (now - session_start_tick_) > pdMS_TO_TICKS(PLAY_PREBUFFER_TIMEOUT_MS)) {
            SendCmd(CI_CMD_NET_PLAY_START);
            play_state_ = PlayState::kPlaying;
            last_get_tick_ = now;
            stream_end_requested_ = false;
            stream_end_reported_ = false;
            ESP_LOGI(TAG, "NET_PLAY_START sent, prebuffer=%u bytes", (unsigned)avail);
        }
    }
    xSemaphoreGive(state_mutex_);
}

void UartAudioCodec::PlayTaskEntry(void* arg) {
    UartAudioCodec* self = (UartAudioCodec*)arg;
    self->PlayTaskLoop();
    vTaskDelete(NULL);
}

void UartAudioCodec::PlayTaskLoop() {
    uint32_t request;
    while (true) {
        if (xQueueReceive(play_req_queue_, &request, pdMS_TO_TICKS(50)) == pdTRUE) {
            last_get_tick_ = xTaskGetTickCount();
            play_restart_count_ = 0;   /* 1302 恢复拉流，清零看门狗重发计数 */
            HandlePlayDataGet(request);
        } else {
            MaybeAutoEndOrAbort();
        }
    }
}

void UartAudioCodec::HandlePlayDataGet(uint32_t request) {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (play_state_ != PlayState::kPlaying) {
        /* 未开始播放（1302 提前拉流）或已结束：忽略 */
        xSemaphoreGive(state_mutex_);
        return;
    }

    uint8_t buf[PLAY_DATA_MAX_CHUNK];
    /* 攒满 1302 请求的长度再回包：它一次要 1024B，若只回几十字节的碎包，
     * 芯片立刻再请求、再等一次，等于给播放器喂带空洞的碎流，4KB 预取垫不住会卡顿。
     * 缓冲暂时不够时最多等 PLAY_DATA_WAIT_MS，超时才退回短包（保持拉流活跃）。 */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PLAY_DATA_WAIT_MS);
    size_t got = 0;
    while (got < request) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            break;
        }
        size_t n = xStreamBufferReceive(downlink_mp3_sb_, buf + got, request - got, deadline - now);
        if (n == 0) {
            break;  /* 超时，缓冲仍未补上数据 */
        }
        got += n;
    }
    if (got > 0) {
        /* 有数据就回包（即使短包也回，保持 1302 拉流活跃）；绝不回空包 */
        SendFrame(CI_CMD_PLAY_DATA_RECV, buf, got);
        xSemaphoreGive(state_mutex_);
        return;
    }

    /* 缓冲空 */
    if (stream_end_requested_) {
        /* TTS 流已结束且缓冲拉空：正常收尾（END 携带尾部数据，绝不为空负载） */
        FinishPlaySession();
        xSemaphoreGive(state_mutex_);
        return;
    }

    /* 无数据且流未结束：跳过本次 GET（1302 每 ~200ms 会再请求，等待编码器补数） */
    xSemaphoreGive(state_mutex_);
}

void UartAudioCodec::MaybeAutoEndOrAbort() {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (play_state_ != PlayState::kPlaying) {
        xSemaphoreGive(state_mutex_);
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (stream_end_requested_) {
        /* 流已结束：缓冲已空且 1302 停止拉流超过宽限期 -> 强制收尾，避免悬挂 */
        if (xStreamBufferBytesAvailable(downlink_mp3_sb_) == 0 &&
            (now - last_get_tick_) > pdMS_TO_TICKS(PLAY_AUTO_END_GRACE_MS)) {
            FinishPlaySession();
        }
    } else if (play_restart_count_ < PLAY_RESTART_MAX &&
               (now - last_get_tick_) > pdMS_TO_TICKS(PLAY_RESTART_TIMEOUT_MS)) {
        /* 播放中长时间无 GET：1302 播放器可能未启动/卡住（其播放结束会关 DAC，
         * 启动失败则保持静音——"有反应但没声音"根因）。重发 NET_PLAY_START
         * 强制其重新走 sap_play_stream 并重开 DAC；重发达到上限仍无 GET 时，
         * 由下方 PLAY_NO_GET_TIMEOUT_MS 判死复位（last_get_tick_ 不再被重置）。 */
        ESP_LOGW(TAG, "No GET for %lums, resend NET_PLAY_START #%d to restart 1302 player",
                 (unsigned long)pdTICKS_TO_MS(now - last_get_tick_), play_restart_count_ + 1);
        SendCmd(CI_CMD_NET_PLAY_START);
        play_restart_count_++;
        last_get_tick_ = now;
    } else if ((now - last_get_tick_) > pdMS_TO_TICKS(PLAY_NO_GET_TIMEOUT_MS)) {
        /* 播放中长时间无 GET：1302 侧已自行结束（异常路径），复位会话 */
        ESP_LOGW(TAG, "No GET for %ums, abort play session", (unsigned)(now - last_get_tick_));
        AbortPlaySessionInternal("no GET timeout");
    }
    xSemaphoreGive(state_mutex_);
}

void UartAudioCodec::NotifyStreamEnd() {
    if (play_state_ == PlayState::kIdle) {
        return;  /* 无播放会话 */
    }
    /* 1) 将 shine 编码器剩余样本 flush 成尾部 MP3，随后续 GET/END 下发 */
    std::vector<uint8_t> tail;
    xSemaphoreTake(mp3_mutex_, portMAX_DELAY);
    Mp3Flush(tail);
    /* 2) 追加静音尾巴（默认 1s，覆盖 1302 播放器缓冲 4096B≈683ms + 解码余量）：
     *    1302 最后一次拉流会带走剩余真实音频到它自己的缓冲里慢慢播，
     *    若 ESP32 此时发 END/STOP，播放器缓冲中未播完的真实音频会被吞掉（"最后几个字听不到"）。
     *    追加静音后，1302 播完真实音频还有静音垫底，缓冲耗尽才自然结束，真实语音 100% 播完。
     *    同时保证 PLAY_DATA_END 一定有非空负载（绝不降级为 NET_PLAY_STOP）。 */
    std::vector<int16_t> silence(MP3_SAMPLE_RATE * PLAY_TAIL_SILENCE_MS / 1000, 0);
    Mp3Encode(silence.data(), (int)silence.size());
    std::vector<uint8_t> tail2;
    Mp3Flush(tail2);   /* 静音样本非 samples_per_pass 整数倍时残留半帧，需再 flush */
    xSemaphoreGive(mp3_mutex_);

    if (!tail.empty()) {
        xStreamBufferSend(downlink_mp3_sb_, tail.data(), tail.size(), pdMS_TO_TICKS(100));
    }
    if (!tail2.empty()) {
        xStreamBufferSend(downlink_mp3_sb_, tail2.data(), tail2.size(), pdMS_TO_TICKS(100));
    }
    stream_end_requested_ = true;
    ESP_LOGI(TAG, "NotifyStreamEnd: TTS stream end, real_tail=%u silence_tail=%u bytes",
             (unsigned)tail.size(), (unsigned)tail2.size());
    /* 播放任务在 50ms 轮询里看到 stream_end_requested_，随 1302 下一次 GET 收尾 */
}

void UartAudioCodec::FinishPlaySession() {
    /* 调用方须已持有 state_mutex_。仅在 stream_end_requested_ 且缓冲拉空后调用。 */
    std::vector<uint8_t> end_payload;
    uint8_t tmp[PLAY_DATA_MAX_CHUNK];
    /* 收走缓冲剩余数据（正常已空，仅剩尾部） */
    size_t n = xStreamBufferReceive(downlink_mp3_sb_, tmp, sizeof(tmp), 0);
    if (n > 0) {
        end_payload.insert(end_payload.end(), tmp, tmp + n);
    }
    /* shine 尾部兜底（NotifyStreamEnd 后若编码器又有产出） */
    xSemaphoreTake(mp3_mutex_, portMAX_DELAY);
    std::vector<uint8_t> tail;
    Mp3Flush(tail);
    if (!tail.empty()) {
        end_payload.insert(end_payload.end(), tail.begin(), tail.end());
    }
    Mp3EncoderInit();  /* 复位编码器，为下一路播放做准备 */
    xSemaphoreGive(mp3_mutex_);

    if (end_payload.empty()) {
        /* 兜底：不能发空负载 END（1302 收尾会卡死），改为直接停止播放 */
        ESP_LOGW(TAG, "Play end payload empty, send NET_PLAY_STOP instead");
        SendCmd(CI_CMD_NET_PLAY_STOP);
    } else {
        SendFrame(CI_CMD_PLAY_DATA_END, end_payload.data(), end_payload.size());
        ESP_LOGI(TAG, "Play session finished, PLAY_DATA_END payload=%u bytes", (unsigned)end_payload.size());
    }
    play_state_ = PlayState::kIdle;
    stream_end_requested_ = false;
    stream_end_reported_ = true;
    play_restart_count_ = 0;
    xStreamBufferReset(downlink_mp3_sb_);
}

void UartAudioCodec::AbortPlayback() {
    if (play_state_ == PlayState::kIdle && !stream_end_requested_) {
        return;  /* 无播放会话 */
    }
    /* 通知 1302 立即停止（1302 回 PLAY_TTS_END，本端幂等忽略） */
    SendCmd(CI_CMD_NET_PLAY_STOP);
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    AbortPlaySessionInternal("abort");
    xSemaphoreGive(state_mutex_);
}

void UartAudioCodec::AbortPlaySessionInternal(const char* reason) {
    /* 调用方须已持有 state_mutex_ */
    play_state_ = PlayState::kIdle;
    stream_end_requested_ = false;
    stream_end_reported_ = false;
    play_restart_count_ = 0;
    xStreamBufferReset(downlink_mp3_sb_);
    xSemaphoreTake(mp3_mutex_, portMAX_DELAY);
    Mp3EncoderInit();
    xSemaphoreGive(mp3_mutex_);
    ESP_LOGI(TAG, "Play session aborted: %s", reason);
}

/* ------------------------------------------------------------------ */
/* shine MP3 编码器（24kHz/单声道/48kbps）                                */
/* ------------------------------------------------------------------ */

bool UartAudioCodec::Mp3EncoderInit() {
    if (shine_enc_ != nullptr) {
        shine_close(shine_enc_);
        shine_enc_ = nullptr;
    }
    shine_config_t config;
    shine_set_config_mpeg_defaults(&config.mpeg);
    config.wave.channels = PCM_MONO;
    config.wave.samplerate = MP3_SAMPLE_RATE;
    config.mpeg.mode = MONO;
    config.mpeg.bitr = MP3_BITRATE;
    if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0) {
        ESP_LOGE(TAG, "Unsupported MP3 config: %dHz %dkbps", MP3_SAMPLE_RATE, MP3_BITRATE);
        return false;
    }
    shine_enc_ = shine_initialise(&config);
    if (shine_enc_ == nullptr) {
        ESP_LOGE(TAG, "shine_initialise failed");
        return false;
    }
    shine_samples_per_pass_ = shine_samples_per_pass(shine_enc_);
    mp3_accum_.clear();
    mp3_accum_.reserve(SHINE_MAX_SAMPLES);
    return true;
}

void UartAudioCodec::Mp3Encode(const int16_t* data, int samples) {
    if (shine_enc_ == nullptr) {
        return;
    }
    mp3_accum_.insert(mp3_accum_.end(), data, data + samples);
    while ((int)mp3_accum_.size() >= shine_samples_per_pass_) {
        int16_t* in = mp3_accum_.data();
        int16_t* channels[1] = { in };
        int written = 0;
        unsigned char* mp3 = shine_encode_buffer(shine_enc_, channels, &written);
        if (mp3 != nullptr && written > 0) {
            /* 空间不足时整帧丢弃（绝不分包写入）：半帧 MP3 会破坏 1302 解码器状态，
             * 即便恢复拉流也会持续破音。正常拉流（每 ~200ms 1024B）下空间充足，
             * 空间不足即消费端已停，交由 Write 防死锁 + 播放看门狗复位会话。 */
            if (xStreamBufferSpacesAvailable(downlink_mp3_sb_) < (size_t)written) {
                ESP_LOGW(TAG, "Downlink MP3 buffer full, drop whole frame %d bytes", written);
            } else {
                xStreamBufferSend(downlink_mp3_sb_, mp3, written, 0);
            }
        }
        mp3_accum_.erase(mp3_accum_.begin(), mp3_accum_.begin() + shine_samples_per_pass_);
    }
}

void UartAudioCodec::Mp3Flush(std::vector<uint8_t>& tail) {
    tail.clear();
    if (shine_enc_ != nullptr) {
        int written = 0;
        unsigned char* p = shine_flush(shine_enc_, &written);
        if (p != nullptr && written > 0) {
            tail.assign(p, p + written);
        }
    }
}
