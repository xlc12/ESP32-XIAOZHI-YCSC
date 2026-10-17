#ifndef LED_PWM_CONTROLLER_H
#define LED_PWM_CONTROLLER_H

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <mutex>

// 单路 PWM 调光 LED 控制器（默认接法：高电平点亮）
//
// 该控制器同时接受两个控制来源：
//   1) 本地 MCP 工具（self.light.*）：控制开关与亮度，形成"基础状态"(base state)。
//   2) 服务器下发的情绪：优先于基础状态播放一段"呼吸"效果，效果结束后自动恢复基础状态。
//
// 冲突仲裁（情绪优先）：
//   - 情绪到来时立即开始播放，期间忽略/暂缓基础状态的显示；
//   - 情绪播放期间若收到新的情绪，则以新情绪重新开始计时；
//   - 情绪播放期间收到 MCP 命令只更新基础状态，不打断当前效果；
//   - 效果结束后自动回到最新的基础状态（这也是"重合"场景下的最终结果）。
//
// 说话期间保持：
//   - SetSpeaking(true) 后效果持续呼吸（不再受 duration_ms 限制），直到 SetSpeaking(false)；
//   - SetSpeaking(false) 立即结束效果并恢复基础状态。
//   - 未处于说话状态时（如仅收到 alert 且无人说话），仍按 duration_ms 定时结束。
class LedPwmController {
public:
    // 效果波形：不同情绪用不同波形，避免所有情绪都只是一个"平滑呼吸"
    enum class Waveform {
        kSine,       // 平滑呼吸：暗 -> 亮 -> 暗
        kFlash,      // 爆闪：每周期一次短促亮起
        kHeartbeat,  // 心跳：每周期两次连击后长间歇
        kTwinkle,    // 闪烁：每周期随机幅度的亮起
        kSawtooth,   // 渐亮后瞬间熄灭
    };

    // 情绪效果参数：亮度在 [min, max] 之间按 waveform 起伏，周期 period_ms，持续 duration_ms
    struct EmotionEffect {
        uint8_t min_duty;     // 0-100
        uint8_t max_duty;     // 0-100
        uint32_t period_ms;   // 一个完整波形周期的时长
        uint32_t duration_ms; // 效果总时长，结束后恢复基础状态
        Waveform waveform;    // 波形类型
    };

    LedPwmController(gpio_num_t gpio_num,
                     ledc_channel_t channel = LEDC_CHANNEL_1,
                     ledc_timer_t timer = LEDC_TIMER_1,
                     uint32_t freq_hz = 5000);

    // 注册 self.light.* 本地 MCP 工具
    void InitializeTools();

    // 服务器情绪变化回调：启动一次情绪呼吸效果，结束后恢复基础状态
    void OnEmotionChanged(const char* emotion);

    // 说话状态回调：为 true 时情绪效果持续呼吸不结束，为 false 时结束效果并恢复基础状态
    void SetSpeaking(bool speaking);

    // 暴露情绪映射，便于复用/测试
    static EmotionEffect EffectForEmotion(const char* emotion);

private:
    void SetBasePower(bool on);
    void SetBaseBrightness(uint8_t brightness);
    void SetOutput(uint8_t percent);

    static void TaskEntry(void* arg);
    void TaskLoop();

    gpio_num_t gpio_num_;
    ledc_channel_t channel_;
    ledc_timer_t timer_;

    std::mutex mutex_;
    // 基础状态（由 MCP 控制）；开机默认开灯
    bool power_ = true;
    uint8_t brightness_ = 60;
    // 情绪效果状态
    bool effect_active_ = false;
    EmotionEffect effect_ = {};
    int64_t effect_start_us_ = 0;
    bool hold_ = false;   // 说话期间保持效果不结束

    TaskHandle_t task_ = nullptr;
};

#endif // LED_PWM_CONTROLLER_H
