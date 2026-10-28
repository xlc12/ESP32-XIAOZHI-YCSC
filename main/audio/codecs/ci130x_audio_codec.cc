#include "ci130x_audio_codec.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/i2s_std.h>
#include <cmath>
#include <cstring>

#define TAG "Ci130xAudioCodec"

std::function<void(const std::string&)> Ci130xAudioCodec::wake_word_handler_;

void Ci130xAudioCodec::SetWakeWordHandler(std::function<void(const std::string&)> handler) {
    wake_word_handler_ = std::move(handler);
}

Ci130xAudioCodec::Ci130xAudioCodec(uart_port_t uart_port, gpio_num_t uart_tx, gpio_num_t uart_rx,
                                   int uart_baud, int input_sample_rate, int output_sample_rate,
                                   gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;   // 框架侧按单声道处理，双声道拆解在 Read/Write 里完成
    output_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    assert(input_sample_rate_ == output_sample_rate_);

    protocol_ = new Ci130xProtocol(uart_port, uart_tx, uart_rx, uart_baud);
    protocol_->SetEventCallback([this](uint16_t type, const uint8_t* payload, uint16_t len) {
        switch (type) {
            case Ci130xProtocol::CIAS_AUDIO_SYS_READY:
                // 这里跑在串口接收任务里，绝不能就地发指令等应答（会把这个任务堵死），
                // 交给 SyncTask 在后台收敛即可。
                ESP_LOGI(TAG, ">>>>> [CI-EVT] SYS_READY 语音芯片就绪（若为重启，状态已清零）");
                // CI 重启后 is_always_iis_flag / 播放窗口 / 音量全部回到默认，
                // 而 ESP32 的 applied_* 还认为"已经设过了"，不强制重发就永远收不到音频。
                force_full_resync_.store(true);
                break;
            case Ci130xProtocol::WAKE_UP:
                ESP_LOGI(TAG, ">>>>> [CI-EVT] WAKE_UP 唤醒");
                // 唤醒瞬间让后台立刻补一次"开始录音"，避免 CI 已经唤醒、
                // ESP32 却因为状态机还没走到 listening 而收不到上行数据。
                force_record_resync_.store(true);
                if (wake_word_handler_) {
                    wake_word_handler_("你好小智");
                }
                break;
            case Ci130xProtocol::VAD_START:
                ESP_LOGI(TAG, ">>>>> [CI-EVT] VAD_START 检测到人声");
                break;
            case Ci130xProtocol::PCM_FINISH:
                ESP_LOGI(TAG, ">>>>> [CI-EVT] PCM_FINISH 一句话结束");
                break;
            case Ci130xProtocol::EXIT_WAKE_UP:
                ESP_LOGI(TAG, ">>>>> [CI-EVT] EXIT_WAKE_UP 退出唤醒态");
                // 退出唤醒态会把 is_wake_up_flag 清掉；如果此时会话仍在进行，
                // 必须靠 0x011A 的常开标志兜住，这里补一次保险。
                force_record_resync_.store(true);
                break;
            case Ci130xProtocol::CIAS_AUDIO_SYS_ERR:
                ESP_LOGE(TAG, ">>>>> [CI-EVT] SYS_ERR 语音芯片异常");
                break;
            default:
                break;
        }
    });

    CreateDuplexChannels(bclk, ws, dout, din);

    xTaskCreate([](void* arg) {
        static_cast<Ci130xAudioCodec*>(arg)->SyncTask();
        vTaskDelete(NULL);
    }, "ci130x_sync", 3072, this, 4, &sync_task_);

    ESP_LOGI(TAG, "===== Ci130xAudioCodec initialized (I2S slave 16k/16bit/stereo, UART ctrl) =====");
}

Ci130xAudioCodec::~Ci130xAudioCodec() {
    if (sync_task_ != nullptr) {
        vTaskDelete(sync_task_);
        sync_task_ = nullptr;
    }
    delete protocol_;
}

void Ci130xAudioCodec::CreateDuplexChannels(gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,          // CI130X 是 I2S 主机，ESP32 只做从机
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        // 取 256 而不是默认的 240：一次 Read 请求 512 点 × 2 声道 = 2048 字节，
        // 只有描述符大小能整除 2048 时 i2s_channel_read 才会整块返回；
        // 240 帧（960/1920 字节）除不尽，会每次都短读并丢掉尾巴上的采样。
        .dma_frame_num = 256,
        .auto_clear_after_cb = true,     // TX 欠载时自动补 0，避免残留数据爆音
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,               // 仅从机生效，保持默认 8
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,  // CI: SCK:LRCK = 64，即 32bit slot
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = 32,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            // 16bit 有效数据左对齐放在 32bit slot 的高 16 位（标准 I2S 时序）
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,     // MCLK 由 CI130X 输出，ESP32 绝不能驱动
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex I2S slave channels created");
}

bool Ci130xAudioCodec::SyncWithChip(bool force) {
    std::lock_guard<std::mutex> sync_lock(sync_mutex_);
    std::lock_guard<std::mutex> state_lock(state_mutex_);

    // 注意：不能以"收到过 CIAS_AUDIO_SYS_READY"为前提。CI1302 只在自身启动时
    // 主动上报一次就绪，而 ESP32 往往晚于 CI 启动，那条消息必然错过。
    bool ok = true;

    if (force) {
        ESP_LOGW(TAG, ">>>>> [CI-CMD] CI 重启过，强制重发全部状态");
        applied_input_ = !input_requested_;
        applied_output_ = !output_requested_;
        applied_volume_ = 0;
    }

    if (input_requested_ != applied_input_) {
        uint16_t cmd = input_requested_ ? Ci130xProtocol::SET_AUDIO_START_RECORD
                                        : Ci130xProtocol::SET_AUDIO_STOP_RECORD;
        if (protocol_->SendCommand(cmd)) {
            applied_input_ = input_requested_;
            ESP_LOGI(TAG, ">>>>> [CI-CMD] record %s", input_requested_ ? "START" : "STOP");
        } else {
            ESP_LOGW(TAG, ">>>>> [CI-CMD] record %s 失败，稍后重试",
                     input_requested_ ? "START" : "STOP");
            ok = false;
        }
    }

    if (output_requested_ != applied_output_) {
        uint8_t value = output_requested_ ? 1 : 0;
        if (protocol_->SendCommand(Ci130xProtocol::MASTER_PLAY_STATE, &value, 1)) {
            applied_output_ = output_requested_;
            ESP_LOGI(TAG, "<<<<< [CI-CMD] play window %s", output_requested_ ? "OPEN" : "CLOSE");
        } else {
            ESP_LOGW(TAG, "<<<<< [CI-CMD] play window %s 失败，稍后重试",
                     output_requested_ ? "OPEN" : "CLOSE");
            ok = false;
        }
    }

    if (volume_level_ != applied_volume_) {
        uint8_t value = volume_level_;
        if (protocol_->SendCommand(Ci130xProtocol::SET_AUDIO_VOLUME, &value, 1)) {
            applied_volume_ = volume_level_;
            ESP_LOGI(TAG, ">>>>> [CI-CMD] volume = %u", value);
        } else {
            ESP_LOGW(TAG, ">>>>> [CI-CMD] volume = %u 失败，稍后重试", value);
            ok = false;
        }
    }

    return ok;
}

void Ci130xAudioCodec::SyncTask() {
    int backoff_ms = 500;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));

        // 上行看门狗：Read 连续拿不到数据时置位，这里补发一次开始录音。
        // 放在本任务里发，避免在音频输入任务里阻塞等应答。
        if (force_record_resync_.exchange(false)) {
            bool need_record = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                need_record = input_requested_;
            }
            if (need_record) {
                ESP_LOGW(TAG, ">>>>> [CI-CMD] 上行持续无数据，重新下发 START_RECORD");
                std::lock_guard<std::mutex> sync_lock(sync_mutex_);
                if (protocol_->SendCommand(Ci130xProtocol::SET_AUDIO_START_RECORD)) {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    applied_input_ = true;
                    ESP_LOGW(TAG, ">>>>> [CI-CMD] START_RECORD 重发成功");
                }
            }
        }

        if (SyncWithChip(force_full_resync_.exchange(false))) {
            backoff_ms = 500;
        } else {
            // 指令没被确认，退避重试，避免刷屏也避免把串口打满
            if (backoff_ms < 5000) {
                backoff_ms *= 2;
            }
        }
    }
}

void Ci130xAudioCodec::SetOutputVolume(int volume) {
    AudioCodec::SetOutputVolume(volume);

    // 框架音量 0~100 映射到 CI130X 的 1~7 档
    int level = 1;
    if (volume > 0) {
        level = (volume * 7 + 50) / 100;
    }
    if (level < 1) {
        level = 1;
    } else if (level > 7) {
        level = 7;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        volume_level_ = (uint8_t)level;
    }
    SyncWithChip();
}

void Ci130xAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        input_requested_ = enable;
    }
    SyncWithChip();
}

void Ci130xAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        output_requested_ = enable;
    }
    SyncWithChip();
}

int Ci130xAudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || samples <= 0) {
        return 0;
    }

    int64_t start_us = esp_timer_get_time();

    size_t wanted = (size_t)samples * 2;  // 立体声交织
    if (read_buffer_.size() < wanted) {
        read_buffer_.resize(wanted);
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_handle_, read_buffer_.data(), wanted * sizeof(int16_t),
                                     &bytes_read, pdMS_TO_TICKS(60));

    // 只取真正收到的部分，剩余补静音。CI 未处于"持续录音"时 IIS 上的数据是断续的，
    // 把零散的有效帧丢掉反而会让唤醒词永远听不到声音。
    size_t frames = bytes_read / (2 * sizeof(int16_t));
    if (frames > (size_t)samples) {
        frames = (size_t)samples;
    }

    const int16_t* p = read_buffer_.data();
    for (size_t i = 0; i < frames; i++) {
        dest[i] = p[2 * i];  // 左声道 = MICL
    }
    for (size_t i = frames; i < (size_t)samples; i++) {
        dest[i] = 0;
    }

    if (err != ESP_OK && ++read_timeout_count_ % 100 == 1) {
        ESP_LOGW(TAG, "[AUDIO-IN ] I2S RX short read: %u/%u bytes (%s)",
                 (unsigned)bytes_read, (unsigned)(wanted * sizeof(int16_t)), esp_err_to_name(err));
    }

    // 上行看门狗：CI 停流时有两种表现——完全没数据，或满字节全是 0（它把
    // zero_buffer 顶上来了）。两种都等于云端听不到人说话，都要能自愈。
    // 触发一次后先"解除武装"，等真正收到声音再重新计数，避免空闲时反复刷屏。
    if (silent_read_count_ >= 0) {
        bool all_zero = (bytes_read == 0);
        if (!all_zero) {
            all_zero = true;
            for (size_t i = 0; i < frames; i++) {
                if (dest[i] != 0) { all_zero = false; break; }
            }
        }
        if (all_zero) {
            if (++silent_read_count_ >= 150) {   // 约 5~9 秒（取决于读是否超时）
                ESP_LOGW(TAG, ">>>>> [AUDIO-IN ] 连续 %d 次上行全静音（无数据/全 0），请求重发 START_RECORD",
                         silent_read_count_);
                force_record_resync_.store(true);
                silent_read_count_ = -1;         // 解除武装，等有声音再重新计数
            }
        } else {
            silent_read_count_ = 0;
        }
    }

    // CI 没在持续供数时上面会立刻返回，必须按采样率补齐节奏，
    // 否则输入任务会以几十倍实时速度往 AFE 里灌数据，把 feed 环形缓冲灌爆。
    int64_t target_us = (int64_t)samples * 1000000 / input_sample_rate_;
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (elapsed_us < target_us) {
        vTaskDelay(pdMS_TO_TICKS((target_us - elapsed_us) / 1000));
    }

    // ===== 音频输入调试：约每 1.6 秒打印一次能量，静音和有声一眼能看出 =====
    static uint32_t in_log_cnt = 0;
    if ((++in_log_cnt % 50) == 0) {
        int64_t sum_sq = 0;   // 512 点 × 32767² 会溢出 int32
        int32_t peak = 0;
        for (size_t i = 0; i < frames; i++) {
            int32_t v = dest[i];
            sum_sq += v * v;
            int32_t a = (v < 0) ? -v : v;
            if (a > peak) peak = a;
        }
        int rms = frames ? (int)sqrtf((float)(sum_sq / (int64_t)frames)) : 0;
        ESP_LOGI(TAG, "[AUDIO-IN ] %4u/%-4u B %3u frm rms=%-5d peak=%-5d silent=%d %s",
                 (unsigned)bytes_read, (unsigned)(wanted * sizeof(int16_t)),
                 (unsigned)frames, rms, (int)peak, silent_read_count_,
                 (err == ESP_OK) ? "OK" : esp_err_to_name(err));
    }

    return samples;
}

int Ci130xAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || samples <= 0) {
        return samples;
    }

    size_t wanted = (size_t)samples * 2;
    if (write_buffer_.size() < wanted) {
        write_buffer_.resize(wanted);
    }

    int16_t* dst = write_buffer_.data();
    for (int i = 0; i < samples; i++) {
        dst[2 * i] = data[i];
        dst[2 * i + 1] = data[i];  // 单声道复制到左右两个声道
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_handle_, dst, wanted * sizeof(int16_t),
                                      &bytes_written, pdMS_TO_TICKS(300));
    if (err != ESP_OK || bytes_written < wanted * sizeof(int16_t)) {
        if (++write_timeout_count_ % 50 == 1) {
            ESP_LOGW(TAG, "I2S TX short write: %u/%u bytes (%s)",
                     (unsigned)bytes_written, (unsigned)(wanted * sizeof(int16_t)), esp_err_to_name(err));
        }
    }

    // ===== 音频输出调试：约每 1.6 秒打印一次 =====
    static uint32_t out_log_cnt = 0;
    if ((++out_log_cnt % 50) == 0) {
        int64_t sum_sq = 0;
        int32_t peak = 0;
        for (int i = 0; i < samples; i++) {
            int32_t v = data[i];
            sum_sq += v * v;
            int32_t a = (v < 0) ? -v : v;
            if (a > peak) peak = a;
        }
        int rms = samples ? (int)sqrtf((float)(sum_sq / (int64_t)samples)) : 0;
        ESP_LOGI(TAG, "[AUDIO-OUT] %u/%u B %3d frm rms=%-5d peak=%-5d %s",
                 (unsigned)bytes_written, (unsigned)(wanted * sizeof(int16_t)),
                 samples, rms, (int)peak,
                 (err == ESP_OK) ? "OK" : esp_err_to_name(err));
    }

    return samples;
}
