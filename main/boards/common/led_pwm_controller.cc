#include "led_pwm_controller.h"

#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#define TAG "LedPwmController"

namespace {
// 10bit 分辨率下的满量程
constexpr uint32_t kDutyMax = 1023;
// 呼吸曲线平滑度：每 10ms 更新一次
constexpr uint32_t kTickMs = 10;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kPi = 3.14159265359f;

// 一次短促亮起：相位落在 [start, start + width] 内时形成 0 -> 1 -> 0 的尖峰
float Beat(float phase, float start, float width) {
    float x = (phase - start) / width;
    if (x <= 0.0f || x >= 1.0f) {
        return 0.0f;
    }
    float s = sinf(kPi * x);
    return s * s * s;
}

// 把周期内相位 [0,1) 按波形映射为亮度系数 [0,1]
float WaveformLevel(LedPwmController::Waveform waveform, float phase, uint32_t cycle) {
    switch (waveform) {
        case LedPwmController::Waveform::kSine:
            return 0.5f * (1.0f - cosf(kTwoPi * phase));
        case LedPwmController::Waveform::kFlash:
            return Beat(phase, 0.12f, 0.30f);
        case LedPwmController::Waveform::kHeartbeat: {
            float first = Beat(phase, 0.00f, 0.16f);
            float second = Beat(phase, 0.26f, 0.16f);
            return first > second ? first : second;
        }
        case LedPwmController::Waveform::kTwinkle: {
            // 每个周期的峰值随机，避免机械感
            uint32_t h = cycle * 2654435761u;
            float amp = 0.45f + 0.55f * (float)((h >> 16) & 0xFF) / 255.0f;
            return amp * Beat(phase, 0.05f, 0.28f);
        }
        case LedPwmController::Waveform::kSawtooth:
            return phase * phase;
        default:
            return 0.0f;
    }
}
}  // namespace

LedPwmController::LedPwmController(gpio_num_t gpio_num,
                                   ledc_channel_t channel,
                                   ledc_timer_t timer,
                                   uint32_t freq_hz)
    : gpio_num_(gpio_num), channel_(channel), timer_(timer) {
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_10_BIT;
    timer_cfg.timer_num = timer_;
    timer_cfg.freq_hz = freq_hz;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {};
    channel_cfg.gpio_num = gpio_num_;
    channel_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_cfg.channel = channel_;
    channel_cfg.intr_type = LEDC_INTR_DISABLE;
    channel_cfg.timer_sel = timer_;
    channel_cfg.duty = 0;
    channel_cfg.hpoint = 0;
    channel_cfg.flags.output_invert = 0;  // 高电平点亮
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    xTaskCreate(TaskEntry, "led_pwm", 2560, this, 2, &task_);
    ESP_LOGI(TAG, "Initialized LED PWM on GPIO%d (channel %d, timer %d, %lu Hz)",
             gpio_num_, channel_, timer_, (unsigned long)freq_hz);
}

void LedPwmController::InitializeTools() {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool("self.light.get_state",
        "Get the LED light state, including power and brightness (0-100).",
        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            std::lock_guard<std::mutex> lock(mutex_);
            char buffer[48];
            snprintf(buffer, sizeof(buffer), "{\"power\": %s, \"brightness\": %u}",
                     power_ ? "true" : "false", (unsigned)brightness_);
            return std::string(buffer);
        });

    mcp_server.AddTool("self.light.turn_on", "Turn on the LED light. (开灯)",
        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SetBasePower(true);
            return true;
        });

    mcp_server.AddTool("self.light.turn_off", "Turn off the LED light. (关灯)",
        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SetBasePower(false);
            return true;
        });

    mcp_server.AddTool("self.light.set_brightness",
        "Set the LED light brightness, 0-100. (设置灯光亮度，0 表示关闭)",
        PropertyList({
            Property("brightness", kPropertyTypeInteger, 0, 100)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int brightness = properties["brightness"].value<int>();
            SetBaseBrightness((uint8_t)brightness);
            return true;
        });
}

void LedPwmController::SetBasePower(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    power_ = on;
    ESP_LOGI(TAG, "Base power %s", on ? "on" : "off");
}

void LedPwmController::SetBaseBrightness(uint8_t brightness) {
    if (brightness > 100) {
        brightness = 100;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    brightness_ = brightness;
    // 设置非零亮度视为开灯，0 视为关灯
    power_ = (brightness > 0);
    ESP_LOGI(TAG, "Base brightness %u (power %s)", (unsigned)brightness_, power_ ? "on" : "off");
}

LedPwmController::EmotionEffect LedPwmController::EffectForEmotion(const char* emotion) {
    // 情绪 -> 呼吸参数：越"激烈"的情绪越亮、频率越快；越"低落/困倦"的情绪越暗、越慢
    static const struct {
        const char* name;
        EmotionEffect effect;
    } kEffects[] = {
        //                             min max  period duration  waveform
        {"neutral",     {10,  60, 1800, 4000, Waveform::kSine}},
        {"happy",       {20, 100,  520, 4000, Waveform::kTwinkle}},
        {"laughing",    {25, 100,  420, 4000, Waveform::kHeartbeat}},
        {"funny",       {20, 100,  380, 4000, Waveform::kFlash}},
        {"silly",       {25, 100,  450, 4000, Waveform::kTwinkle}},
        {"sad",         { 3,  30, 3500, 5000, Waveform::kSine}},
        {"crying",      { 5,  32, 2600, 5000, Waveform::kSine}},
        {"angry",       {10, 100,  220, 3000, Waveform::kFlash}},
        {"loving",      {20, 100,  900, 4000, Waveform::kHeartbeat}},
        {"kissy",       {20,  90,  750, 4000, Waveform::kHeartbeat}},
        {"embarrassed", {12,  65, 1400, 4000, Waveform::kSine}},
        {"surprised",   { 0, 100,  300, 2500, Waveform::kFlash}},
        {"shocked",     { 5, 100,  200, 2500, Waveform::kFlash}},
        {"thinking",    { 8,  55, 1500, 4000, Waveform::kSawtooth}},
        {"confused",    {10,  70, 1200, 4000, Waveform::kSawtooth}},
        {"winking",     {12,  90,  600, 3000, Waveform::kFlash}},
        {"cool",        {18,  95, 1000, 4000, Waveform::kSawtooth}},
        {"confident",   {20, 100,  700, 4000, Waveform::kFlash}},
        {"relaxed",     {10,  45, 2800, 5000, Waveform::kSine}},
        {"delicious",   {15,  85, 1000, 4000, Waveform::kHeartbeat}},
        {"sleepy",      { 2,  12, 4200, 5000, Waveform::kSine}},
    };
    static const EmotionEffect kDefault = {10, 60, 1800, 4000, Waveform::kSine};  // 未知情绪按 neutral 处理

    if (emotion != nullptr) {
        for (const auto& item : kEffects) {
            if (strcmp(item.name, emotion) == 0) {
                return item.effect;
            }
        }
    }
    return kDefault;
}

void LedPwmController::OnEmotionChanged(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') {
        return;
    }

    EmotionEffect effect = EffectForEmotion(emotion);
    std::lock_guard<std::mutex> lock(mutex_);
    // 情绪优先：无论当前是基础状态还是上一个情绪效果，都以新情绪重新开始
    effect_ = effect;
    effect_active_ = true;
    effect_start_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "Emotion '%s' -> brightness %u-%u%%, period %ums, duration %ums",
             emotion, (unsigned)effect_.min_duty, (unsigned)effect_.max_duty,
             (unsigned)effect_.period_ms, (unsigned)effect_.duration_ms);
}

void LedPwmController::SetSpeaking(bool speaking) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hold_ == speaking) {
        return;
    }
    hold_ = speaking;
    if (!speaking) {
        // 说话结束：不再保持效果，立即结束并恢复基础状态
        effect_active_ = false;
        audio_level_ = 0;
        audio_env_ = 0.0f;
        ESP_LOGI(TAG, "Speaking stopped, restore base state (power %s, brightness %u)",
                 power_ ? "on" : "off", (unsigned)brightness_);
    } else {
        ESP_LOGI(TAG, "Speaking started, hold emotion effect");
    }
}

void LedPwmController::SetAudioLevel(uint8_t level) {
    if (level > 100) {
        level = 100;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    audio_level_ = level;
}

void LedPwmController::SetOutput(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (kDutyMax * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_);
}

void LedPwmController::TaskEntry(void* arg) {
    static_cast<LedPwmController*>(arg)->TaskLoop();
}

void LedPwmController::TaskLoop() {
    while (true) {
        uint8_t percent;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (effect_active_) {
                int64_t elapsed_us = esp_timer_get_time() - effect_start_us_;
                // 说话期间（hold_）不按 duration_ms 结束，持续呼吸到说话结束
                if (!hold_ && elapsed_us >= (int64_t)effect_.duration_ms * 1000) {
                    // 情绪效果结束，恢复基础状态
                    effect_active_ = false;
                    percent = power_ ? brightness_ : 0;
                } else {
                    // 按情绪波形计算亮度系数 [0,1]
                    float t_ms = elapsed_us / 1000.0f;
                    float period_ms = (float)effect_.period_ms;
                    float phase = fmodf(t_ms, period_ms) / period_ms;
                    uint32_t cycle = (uint32_t)(t_ms / period_ms);
                    float level = WaveformLevel(effect_.waveform, phase, cycle);
                    if (hold_) {
                        // 说话中：叠加实时音量包络，声音大更亮、音节停顿处变暗
                        // 快起慢落（每 10ms 衰减 6%），避免亮度抖动
                        float target = audio_level_ * 0.01f;
                        audio_env_ = target > audio_env_ ? target : audio_env_ * 0.94f;
                        level *= 0.25f + 0.75f * audio_env_;
                    }
                    float duty = effect_.min_duty + level * (effect_.max_duty - effect_.min_duty);
                    percent = (uint8_t)(duty + 0.5f);
                }
            } else {
                percent = power_ ? brightness_ : 0;
            }
        }
        SetOutput(percent);
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}
