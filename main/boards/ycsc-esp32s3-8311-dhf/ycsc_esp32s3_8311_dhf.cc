#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"
#include "led/gpio_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <cstring>

#include "power_save_timer.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

//YCSC
// #include "boards/common/da218e.h"
// #include "gsensor_action.h"
#include <esp_lcd_gc9a01.h>
// #include "esp_lcd_gc9a01.h"



#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif


#define TAG "YcscEsp32S3Es8311Dhf"


/***** 运动控制串口 -begin *****/

// 运动控制命令值（帧格式：0x55 0x5A + 命令 + 0x5B）
typedef enum {
    MOTION_CMD_STOP = 0x00,     // 停止
    MOTION_CMD_FORWARD = 0x01,  // 前进
    MOTION_CMD_BACKWARD = 0x02, // 后退
    MOTION_CMD_LEFT = 0x03,     // 左转
    MOTION_CMD_RIGHT = 0x04,    // 右转
} motion_command_t;

/***** 运动控制串口 -end *****/


/***** 蓝牙遥控 -begin *****/

#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_remote.h"

static const char *BLE_TAG = "BLE_REMOTE";

/* ========================================================================
 *  DHF 音效循环播放：共11首（DHF1~DHF11），按顺序循环切换
 * ======================================================================== */
static const std::string_view DHF_SOUNDS[] = {
    Lang::Sounds::OGG_DHF1,
    Lang::Sounds::OGG_DHF2,
    Lang::Sounds::OGG_DHF3,
    Lang::Sounds::OGG_DHF4,
    Lang::Sounds::OGG_DHF5,
    Lang::Sounds::OGG_DHF6,
    Lang::Sounds::OGG_DHF7,
    Lang::Sounds::OGG_DHF8,
    Lang::Sounds::OGG_DHF9,
    Lang::Sounds::OGG_DHF10,
    Lang::Sounds::OGG_DHF11,
};
static const int DHF_SOUND_COUNT = sizeof(DHF_SOUNDS) / sizeof(DHF_SOUNDS[0]);
static int dhf_sound_index_ = DHF_SOUND_COUNT - 1;  // 初始指向最后一首，第一次按下即切到DHF1

/* ========================================================================
 *  LED 闪烁配置与控制（播放音频时同步闪亮）
 * ======================================================================== */
// ===== 可配置项：LED 闪烁间隔（单位：毫秒，修改此处即可调整闪烁速度）=====
#define LED_BLINK_INTERVAL_MS     300    // 亮/灭各保持的时间，例如300表示亮300ms灭300ms循环
// =========================================================================

static TaskHandle_t led_blink_task_handle_ = nullptr;
static volatile bool led_blink_running_ = false;
static volatile bool led_blink_prev_lamp_on_ = false;
static volatile uint32_t blink_generation_ = 0;  // 每次start递增1，被取代的旧任务检测到代次变化立即退出

/* ===== LED 控制函数前向声明（sound_switch 代码会提前调用它们）===== */
static void led_blink_task(void* arg);
static void stop_led_blink_if_running();
static void start_led_blink_if_needed();

/* ========================================================================
 *  切歌请求异步处理：避免阻塞 BLE(NimBLE) 任务导致按键丢失
 *  使用长度=1的队列，xQueueOverwrite 新请求覆盖旧请求（快速连按只响应最后一次）
 * ======================================================================== */
/* ========================================================================
 *  可靠的音频完全停止机制（在 worker 任务里执行，允许短时间阻塞）
 *  解决的残留场景：
 *    1. OpusCodecTask 已经从 decode_queue 取出 packet，正在解码（几十 ms），
 *       解完后会 push 到 playback_queue → 需要等它解完入队后再清一次
 *    2. AudioOutputTask 已经从 playback_queue 取出 task 正在写 I2S
 *       → ResetDecoder() 内部已做 EnableOutput(false)+10ms 截断 DMA
 *    3. PlaySound() 正在解析 OGG page → sound_generation_++ 会让它
 *       在每页边界检测到代次变化后自行退出
 * ======================================================================== */
static void stop_all_sound_playback() {
    auto& audio_svc = Application::GetInstance().GetAudioService();
    // 直接拿 Board 上的 Es8311AudioCodec 实例（和 AudioService 用的是同一个）
    // 用来绕过 ResetDecoder 的"output_enabled()==true 才关"的条件判断，
    // 每轮必关输出，把 AudioOutputTask 的自动 EnableOutput(true) 压制住
    auto* codec = Board::GetInstance().GetAudioCodec();

    // 第1次：sound_generation_++ 中止 PlaySound 解析 + 清空所有队列
    // ResetDecoder 内部自带条件式 EnableOutput(false)+10ms delay
    audio_svc.ResetDecoder();

    // 高频清理循环：Worker 优先级(6) > AudioOutputTask(4)，每 10ms 我们就能抢占回来
    // 强制关一次输出。即使 AudioOutputTask 检测到 output==false 自动 EnableOutput(true)，
    // 最多只给它留了约10ms的输出窗口 → 写入 DMA 的 PCM 不足，不可闻
    const int MAX_TRIES = 600;  // 20轮 * 10ms = 约200ms清理期（≈3~4个Opus帧）
    for (int i = 0; i < MAX_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // 关键1：每轮无条件先关输出（不管 ResetDecoder 内部是否执行）
        if (codec != nullptr) {
            codec->EnableOutput(false);
        }

        // 关键2：清队列（含 OpusCodecTask 刚解完 push 回来的残留；
        // 若 output 还是true，ResetDecoder 内部还会再做一次关输出+10ms等待）
        audio_svc.ResetDecoder();

        if (audio_svc.IsIdle()) {
            // IsIdle 说明队列已经空了，但为了把 OpusCodecTask 手上可能还在解的
            // 最后一帧也打掉，再巩固 3 轮（30ms）
            for (int j = 0; j < 3; j++) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (codec != nullptr) codec->EnableOutput(false);
                audio_svc.ResetDecoder();
            }
            ESP_LOGI(BLE_TAG, "stop_playback: 第%d轮后Idle，已再巩固3轮", i + 1);
            break;
        }
    }

    // 最终兜底：再强制关输出 + 清一次队列
    // 保持 output=false 状态进入 PlaySound，PlaySound 开头会自动 Enable(true) 恢复
    if (codec != nullptr) {
        codec->EnableOutput(false);
    }
    audio_svc.ResetDecoder();
    ESP_LOGI(BLE_TAG, "stop_playback: 完成，最终IsIdle=%d", audio_svc.IsIdle() ? 1 : 0);
}

static QueueHandle_t sound_switch_queue_ = nullptr;  // 存放 uint8_t（任意值即可，只为触发）
static TaskHandle_t sound_switch_task_handle_ = nullptr;

/* ========================================================================
 *  独立的"播放一次"任务（一次性任务，跑完自删）
 *  - 原因：PlaySound() 是同步解析 OGG（逐页扫描+逐包解析push进decode_queue），
 *    对几十秒的音频会花 2~5 秒甚至更久。若放在 Worker 里同步执行，
 *    Worker 无法回到 xQueueReceive，这段时间所有按键只能 Overwrite 但不被处理，
 *    表现为"音频播放时按键没有立即响应"（用户日志铁证）。
 *  - 解决：Worker 只做 Stop+LED 控制（≈200ms），把耗时的 PlaySound 解析扔到这个
 *    独立的一次性任务里。Worker 创建完任务后立即回到 xQueueReceive 等待下一次按键。
 *  - 中止安全：若这个任务在解析 OGG 时用户又按了键，Worker 会再次调用
 *    ResetDecoder() → sound_generation_++，PlaySound() 内部在每页解析前都会
 *    检查 sound_generation_ 代次，变化则立即 return，不会往队列里推旧歌数据。
 * ======================================================================== */
static void dhf_play_once_task(void* arg) {
    // 注意：用 (intptr_t) 把索引值传进来，**不能读全局 dhf_sound_index_**
    // 因为任务创建后到实际运行前，可能用户又按了新键覆盖了全局值
    const int idx = (int)(intptr_t)arg;
    ESP_LOGI(BLE_TAG, "dhf_play_once: 开始解析 DHF%d（OGG同步解析+push队列，耗时较长）", idx + 1);
    Application::GetInstance().PlaySound(DHF_SOUNDS[idx]);
    ESP_LOGI(BLE_TAG, "dhf_play_once: DHF%d 解析完成（后续播放由OpusCodec/AudioOutput任务处理）", idx + 1);
    vTaskDelete(nullptr);  // 一次性任务：解析完成后自删
}

/* ========================================================================
 *  开机"延迟启动 DHF1 播放"任务
 *  - 重要：为什么要延迟？Board 构造函数期间（构造函数还没return时），
 *    Application 成员 audio_service_ 的 codec_ 指针还没被赋值为
 *    Es8311AudioCodec 实例（赋值发生在 Application::Init 的后半段，
 *    在 Board 构造**完全返回后**才进行）。若在构造函数里立即起任务
 *    调 AudioService::PlaySound → codec_->output_enabled() → codec_==nullptr
 *    → EXCVADDR=0x0000000F → LoadProhibited GuruMeditation 崩溃（用户日志铁证）。
 *  - 解决：先 vTaskDelay(500ms) 让出 CPU，等 Board 构造返回、Application Init
 *    走完、AudioService.codec_ 非空、AudioCodec 初始化完毕后，再开始播放。
 *  - 这是一次性任务：播完 DHF1 后自己 vTaskDelete(nullptr)
 * ======================================================================== */
static void boot_delay_play_dhf1_task(void* arg) {
    (void)arg;
    // 1. 先等 500ms：让 Board 构造函数return + Application::Init 走完
    //    + AudioService::codec_ = Es8311 真实实例，避免空指针崩溃
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. 启动 LED 同步闪烁（跟切歌时完全一样的视觉反馈）
    ESP_LOGI(BLE_TAG, "boot_delay_play: 500ms到，启动LED闪烁 + 解析DHF1");
    start_led_blink_if_needed();

    // 3. 同步解析 + push DHF1 队列（OGG解析耗时 200~1500ms，在本任务里执行）
    constexpr int BOOT_SOUND_IDX = 0;  // 0 → DHF_SOUNDS[0] → DHF1.ogg
    Application::GetInstance().PlaySound(DHF_SOUNDS[BOOT_SOUND_IDX]);
    ESP_LOGI(BLE_TAG, "boot_delay_play: DHF1 解析完成（OpusCodec/AudioOutput 继续播后续帧）");

    vTaskDelete(nullptr);  // 一次性任务：做完就自删
}

/* ========================================================================
 *  同步等待当前正在播放的音频**完全播完**（调用方线程阻塞，直到喇叭播完最后一声）
 *  - 用途：OnShutdownRequest 中播放 DHF11 关机提示音——必须等**真实播出完毕**
 *          再关 PA / 背光 / 进 deep sleep，否则 I2S/DMA 时钟一关，最后几
 *          百毫秒的音频会被拦腰截断，用户听到"咔"或者提示音只播了一半。
 *  - 等待判据：AudioService::IsIdle() 连续 true ≥ 200ms
 *      · IsIdle=true 仅表示 decode_queue / output_queue 都空了
 *      · 再额外等 200ms 是等 I2S DMA FIFO 里最后几帧 PCM 真正被 DAC 输出到喇叭
 *  - 超时保护：最多 15s（一首 DHFx 音频通常 2~10s），极端异常时不能卡死休眠流程
 * ======================================================================== */
static void wait_for_playback_done_sync(const char* log_tag) {
    auto& audio_svc = Application::GetInstance().GetAudioService();
    ESP_LOGI(log_tag, "等待音频播放完成（队列+DMA尾部）...");
    int idle_streak_ms = 0;           // IsIdle 连续为 true 的累计时长
    int total_wait_ms  = 0;           // 总等待时间（超时保护）
    const int TIMEOUT_MS   = 15000;   // 硬超时 15s
    const int IDLE_HOLD_MS = 200;     // 连续 Idle ≥ 200ms 才算"真播完"
    while (total_wait_ms < TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        total_wait_ms += 20;
        if (audio_svc.IsIdle()) {
            idle_streak_ms += 20;
            if (idle_streak_ms >= IDLE_HOLD_MS) {
                ESP_LOGI(log_tag, "音频播放完成（连续Idle=%dms，总等=%dms）",
                         idle_streak_ms, total_wait_ms);
                return;
            }
        } else {
            idle_streak_ms = 0;  // 还在输出帧，说明没播完，重置"连续Idle"
        }
    }
    ESP_LOGW(log_tag, "等待音频播放超时（%dms仍未完成），强制继续后续流程", TIMEOUT_MS);
}

// 切歌 worker 任务：阻塞在队列上，收到请求执行"停止+LED启动+创建解析任务"后立即回队列
// —— 关键：Worker 绝不自己跑 PlaySound（耗时几秒），最多200ms就回xQueueReceive接新键
static void sound_switch_worker_task(void* arg) {
    (void)arg;
    ESP_LOGI(BLE_TAG, "切歌worker任务已启动");
    uint8_t dummy;
    while (true) {
        // 阻塞等待切歌请求（INDEFINITE=永久等）
        if (xQueueReceive(sound_switch_queue_, &dummy, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(BLE_TAG, "worker: 处理切歌请求，当前索引=%d", dhf_sound_index_ + 1);
        // 1. 完全停止上一首（多轮清队列，截断解码/输出残留）≈200ms
        stop_all_sound_playback();
        // 2. 停止旧的LED闪烁任务（如果有），保存新的原LED状态并重新启动闪烁（非阻塞≈0ms）
        start_led_blink_if_needed();
        // 3. 创建一次性任务执行耗时的 PlaySound(OGG解析)，Worker 自己立即回队列
        //    —— 把"此刻的索引值"通过 arg 传进去，避免后续按键覆盖全局导致播放错歌
        const int idx_snapshot = dhf_sound_index_;
        BaseType_t ret = xTaskCreate(dhf_play_once_task, "dhf_play", 4096,
                                     (void*)(intptr_t)idx_snapshot, 5, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(BLE_TAG, "创建dhf_play_once任务失败: %d，降级Worker同步解析", ret);
            // 极端降级：Worker 自己同步跑 PlaySound（虽然会短暂阻塞，但至少保证功能）
            Application::GetInstance().PlaySound(DHF_SOUNDS[idx_snapshot]);
        }
        // 立即回 xQueueReceive，等待下一次按键请求
        ESP_LOGI(BLE_TAG, "worker: 切歌调度完成，已回到接收队列等待新按键");
    }
}

// 在初始化阶段调用：创建队列 + worker 任务
static void sound_switch_init_async() {
    sound_switch_queue_ = xQueueCreate(1, sizeof(uint8_t));
    if (sound_switch_queue_ == nullptr) {
        ESP_LOGE(BLE_TAG, "创建切歌队列失败");
        return;
    }
    BaseType_t ret = xTaskCreate(sound_switch_worker_task, "sound_sw", 4096, nullptr, 6, &sound_switch_task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(BLE_TAG, "创建切歌worker任务失败: %d", ret);
        vQueueDelete(sound_switch_queue_);
        sound_switch_queue_ = nullptr;
    }
}

// BLE 回调中调用：最轻量，立即返回，不阻塞 BLE 任务
static void sound_switch_request_async() {
    if (sound_switch_queue_ == nullptr) {
        ESP_LOGW(BLE_TAG, "切歌队列未初始化，同步降级处理");
        // 极端降级（不应该发生）：同步处理（此时会短暂阻塞 BLE 但保证功能正确）
        stop_all_sound_playback();
        start_led_blink_if_needed();
        Application::GetInstance().PlaySound(DHF_SOUNDS[dhf_sound_index_]);
        return;
    }
    uint8_t dummy = 1;
    // xQueueOverwrite: 长度=1的队列，新值覆盖旧值，不阻塞；快速连按保证响应最后一次
    xQueueOverwrite(sound_switch_queue_, &dummy);
}

// LED闪烁任务：只要音频服务不是Idle状态，就周期性翻转LAMP_GPIO
// ---- 修复：使用"先闪后检查"+代次取代机制，避免切歌时IsIdle短暂为true导致立即自杀 ----
static void led_blink_task(void* arg) {
    (void)arg;
    // 保存本任务创建时的闪烁代次快照
    const uint32_t my_gen = blink_generation_;
    bool blink_state = true;  // 先亮
    ESP_LOGI(BLE_TAG, "LED闪烁任务启动(gen=%u)，间隔=%dms",
             (unsigned)my_gen, LED_BLINK_INTERVAL_MS);

    while (true) {
        /* ========== 1. 先立即执行闪烁动作（先做事！即使 IsIdle 短暂 true 也先闪一次）========== */
        gpio_set_level(LAMP_GPIO, blink_state ? 1 : 0);
        blink_state = !blink_state;

        /* ========== 2. Delay 让出 CPU（此时闪烁已发生/PlaySound可同步推队列）========== */
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL_MS));

        /* ========== 3. Delay 之后再检查 3 个停止条件（满足任一立即退出）========== */
        // 条件A: 闪烁代次变了 → 被新切歌任务取代，立即退（不恢复LED，新任务会接手）
        if (blink_generation_ != my_gen) {
            ESP_LOGI(BLE_TAG, "LED(gen=%u)被新任务(gen=%u)取代，退出",
                     (unsigned)my_gen, (unsigned)blink_generation_);
            break;
        }
        // 条件B: 外部强制停止标志
        if (!led_blink_running_) {
            ESP_LOGI(BLE_TAG, "LED(gen=%u)被外部stop停止", (unsigned)my_gen);
            break;
        }
        // 条件C: 音频播放完毕（此时至少经过一个闪烁间隔，PlaySound早已推完首包队列，
        //         若仍然IsIdle才是真正播放完，而非切歌中间的瞬态）
        auto& audio_svc = Application::GetInstance().GetAudioService();
        if (audio_svc.IsIdle()) {
            ESP_LOGI(BLE_TAG, "音频播放完毕，停止LED闪烁(gen=%u)", (unsigned)my_gen);
            break;
        }
    }

    // 恢复原状态前必须先确认"本代次仍然有效"——若被新任务取代，
    // 新任务已保存新的 prev_lamp_on 并自行闪烁，此处不能乱恢复（会和新任务抢GPIO）
    if (blink_generation_ == my_gen) {
        gpio_set_level(LAMP_GPIO, led_blink_prev_lamp_on_ ? 1 : 0);
        ESP_LOGI(BLE_TAG, "LED(gen=%u)恢复原状态（%s）",
                 (unsigned)my_gen, led_blink_prev_lamp_on_ ? "ON" : "OFF");
        led_blink_running_ = false;
    }
    // 清handle标志（stop函数据此判断任务退出）
    led_blink_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

// 停止LED闪烁（非阻塞！——避免阻塞worker任务，LED任务将在下一次循环检测时自行退出）
static void stop_led_blink_if_running() {
    // 仅置标志即可，LED任务下次循环检查代次/running_时会立即退出
    // 最多等待 LED_BLINK_INTERVAL_MS（闪烁1次的时间）——对worker无影响
    led_blink_running_ = false;
}

// 启动LED闪烁（若已有任务在运行，通过代次+1让旧任务自行退出，新任务立即接手闪烁）
static void start_led_blink_if_needed() {
    // 停止旧任务（非阻塞置标志）
    stop_led_blink_if_running();
    // ---- 关键：代次+1，旧任务即使还在vTaskDelay中，返回后会立即检测到退出 ----
    //            这样无需等待旧任务退出，也不会出现两个任务同时闪的乱序
    // （注意：blink_generation_ 是 volatile uint32_t，后缀++已deprecated，改用赋值形式）
    blink_generation_ = blink_generation_ + 1;
    // 保存此刻（切歌完成、新歌即将播放时）的LED状态作为新歌播放完的恢复基准
    led_blink_prev_lamp_on_ = (gpio_get_level(LAMP_GPIO) == 1);
    led_blink_running_ = true;
    BaseType_t ret = xTaskCreate(led_blink_task, "led_blink", 2048, nullptr, 5, &led_blink_task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(BLE_TAG, "创建LED闪烁任务失败: %d", ret);
        led_blink_running_ = false;
        led_blink_task_handle_ = nullptr;
    }
}

/* ========================================================================
 *  键值定义（协议: [0x55][0x52][键值][0x5B]，键值为第3字节）
 *  根据你实际按键收到的值修改
 * ======================================================================== */
#define KEY_STOP     0xAA   // 示例：刚才收到的数据 55 52 11 5B，键值=0x11
#define KEY_UP        0x01
#define KEY_DOWN      0x02
#define KEY_LEFT      0x03
#define KEY_RIGHT     0x04
#define KEY_LAMP_ON    0x05  //灯光开关
#define KEY_SOUND_ON   0x06  //声音开关

/***** 蓝牙遥控 -end *****/

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb2, (uint8_t[]){0x2f}, 1, 0},
    {0xb3, (uint8_t[]){0x03}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x01}, 1, 0},
    {0xac, (uint8_t[]){0xcb}, 1, 0},
    {0xab, (uint8_t[]){0x0e}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x19}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xe8, (uint8_t[]){0x24}, 1, 0},
    {0xe9, (uint8_t[]){0x48}, 1, 0},
    {0xea, (uint8_t[]){0x22}, 1, 0},
    {0xc6, (uint8_t[]){0x30}, 1, 0},
    {0xc7, (uint8_t[]){0x18}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1f, 0x28, 0x04, 0x3e, 0x2a, 0x2e, 0x20, 0x00, 0x0c, 0x06,
                0x00, 0x1c, 0x1f, 0x0f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x00, 0x2d, 0x2f, 0x3c, 0x6f, 0x1c, 0x0b, 0x00, 0x00, 0x00,
                0x07, 0x0d, 0x11, 0x0f},
    14, 0},
};


class YcscEsp32S3Es8311Dhf : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button lamp_button_;
    bool lamp_on_ = false;
    Display* display_;
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;
    PowerSaveTimer* power_save_timer_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ResetWifiConfiguration();
                return;
            }
            app.ToggleChatState();
        });
    }

    void SetLamp(bool on) {
        lamp_on_ = on;
        gpio_set_level(LAMP_GPIO, on ? 1 : 0);
    }

    void ToggleLamp() {
        SetLamp(!lamp_on_);
    }

    void InitializeLamp() {
        // 灯（IO16）配置为输入输出双向（输出驱动LED，输入可读取当前电平）
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << LAMP_GPIO);
        io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        SetLamp(false);

        // 按键（IO48）：按一次亮，再按一次灭
        lamp_button_.OnClick([this]() {
            ToggleLamp();
        });
    }

    static void SendMotionFrame(motion_command_t cmd) {
        uint8_t frame[4] = {0x55, 0x5A, (uint8_t)cmd, 0x5B};
        uart_write_bytes(MOTION_UART_PORT, frame, sizeof(frame));
    }

    void InitializeMotionUart() {
        uart_config_t uart_cfg = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        // 仅发送，不安装接收缓冲区
        ESP_ERROR_CHECK(uart_driver_install(MOTION_UART_PORT, 256, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(MOTION_UART_PORT, &uart_cfg));
        ESP_ERROR_CHECK(uart_set_pin(MOTION_UART_PORT, MOTION_UART_TX_PIN,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }


    
    // 运动控制接口：发送一帧运动指令
    void SendMotionCommand(motion_command_t cmd) {
        SendMotionFrame(cmd);
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;


#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
#elif defined(LCD_TYPE_GC9A01_160X160_SERIAL)
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

        esp_lcd_panel_reset(panel);


        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            {
                .text_font = &font_puhui_16_4,
                .icon_font = &font_awesome_16_4,
        #if CONFIG_USE_WECHAT_MESSAGE_STYLE
                .emoji_font = font_emoji_32_init(),
        #else
                .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
        #endif
            });

    }


    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        // 运动控制 MCP 工具：供语音/AI 调用，action 参数控制前后左右和停止
        mcp_server.AddTool(
            "self.motion.control",
            "控制设备运动。action 可选值：forward(前进)、backward(后退)、left(左转)、right(右转)、stop(停止)。"
            "每次调用发送一帧对应指令。"
            "当用户要求跳舞或表演时，自己组合10个动作。",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("stop"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();

                motion_command_t cmd = MOTION_CMD_STOP;
                bool valid = true;

                if (action == "forward" || action == "前进") {
                    cmd = MOTION_CMD_FORWARD;
                } else if (action == "backward" || action == "后退") {
                    cmd = MOTION_CMD_BACKWARD;
                } else if (action == "left" || action == "左转") {
                    cmd = MOTION_CMD_LEFT;
                } else if (action == "right" || action == "右转") {
                    cmd = MOTION_CMD_RIGHT;
                } else if (action == "stop" || action == "停止" || action == "停下") {
                    cmd = MOTION_CMD_STOP;
                } else {
                    valid = false;
                }

                if (!valid) {
                    return std::string("无效的运动指令: ") + action;
                }

                SendMotionCommand(cmd);
                return std::string("运动指令已执行: ") + action;
            });

        // 灯光控制 MCP 工具：供语音/AI 控制灯（IO16）
        mcp_server.AddTool(
            "self.lamp.turn_on",
            "打开灯",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                SetLamp(true);
                return true;
            });

        mcp_server.AddTool(
            "self.lamp.turn_off",
            "关闭灯",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                SetLamp(false);
                return true;
            });

        mcp_server.AddTool(
            "self.lamp.toggle",
            "切换灯的亮灭状态",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ToggleLamp();
                return lamp_on_;
            });

        mcp_server.AddTool(
            "self.lamp.get_state",
            "获取灯的当前状态",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                return lamp_on_ ? "{\"power\": true}" : "{\"power\": false}";
            });
    }


    /***** 蓝牙遥控 -begin *****/

    /* ========================================================================
 *  按键回调：在这里判断键值，执行你的控制逻辑
 * ======================================================================== */
    static void on_key_event(const ble_remote_key_event_t *event)
    {
        ESP_LOGI(BLE_TAG, "key=0x%02X (raw len=%u)", (unsigned)event->key_code, event->raw_len);

        switch (event->key_code) {
        case KEY_STOP:
            ESP_LOGE(BLE_TAG, ">>> STOP 按下，执行停止");
            // 这里写你的控制代码，比如控制 GPIO、发消息等
            SendMotionFrame(MOTION_CMD_STOP);
            break;

        case KEY_UP:
            ESP_LOGE(BLE_TAG, ">>> UP 按下");
            SendMotionFrame(MOTION_CMD_FORWARD);
            break;

        case KEY_LEFT:
            ESP_LOGE(BLE_TAG, ">>> LEFT 按下");
            SendMotionFrame(MOTION_CMD_LEFT);
            break;

        case KEY_RIGHT:
            ESP_LOGE(BLE_TAG, ">>> RIGHT 按下");
            SendMotionFrame(MOTION_CMD_RIGHT);
            break;

        case KEY_LAMP_ON:
            ESP_LOGE(BLE_TAG, ">>> LAMP_ON 按下");
            static_cast<YcscEsp32S3Es8311Dhf&>(Board::GetInstance()).ToggleLamp();
            break;

        case KEY_SOUND_ON:
            // ===== 关键：此处运行在 NimBLE 主机任务，绝对不能阻塞 =====
            // 只做索引递增（原子）+ 发送异步请求，立即返回
            dhf_sound_index_ = (dhf_sound_index_ + 1) % DHF_SOUND_COUNT;
            ESP_LOGE(BLE_TAG, ">>> SOUND_ON 按下，准备播放 DHF%d（异步处理）", dhf_sound_index_ + 1);
            sound_switch_request_async();
            break;

        case KEY_DOWN:
            ESP_LOGE(BLE_TAG, ">>> DOWN 按下");
            SendMotionFrame(MOTION_CMD_BACKWARD);
            break;

        default:
            ESP_LOGW(BLE_TAG, ">>> 未知按键 key=0x%02X, raw:", (unsigned)event->key_code);
            for (uint16_t i = 0; i < event->raw_len; i++) {
                printf("%02X ", event->raw[i]);
            }
            printf("\n");
            break;
        }
    }


        /* ========================================================================
    *  连接事件回调（推荐）：连上 / 就绪 / 断开，适合做提示
    * ======================================================================== */
    static void on_conn_event(ble_remote_conn_event_t event)
    {
        switch (event) {
        case BLE_REMOTE_CONN_CONNECTED:
            ESP_LOGI(BLE_TAG, ">>> 遥控器已连接");
            // 这里做连接成功提示，比如亮灯、蜂鸣器等
            break;

        case BLE_REMOTE_CONN_READY:
            ESP_LOGI(BLE_TAG, ">>> 遥控器就绪，可以接收按键");
            // 这里做就绪提示
            break;

        case BLE_REMOTE_CONN_DISCONNECTED:
            ESP_LOGW(BLE_TAG, ">>> 遥控器已断开，正在自动重连...");
            // 这里做断开提示，比如闪灯、提示音等
            break;

        default:
            break;
        }
    }

    /* ========================================================================
    *  状态回调（可选）：细粒度状态变化
    * ======================================================================== */
    static void on_state_change(ble_remote_state_t state)
    {
        switch (state) {
        case BLE_REMOTE_STATE_IDLE:
            ESP_LOGW(BLE_TAG, "=== 已断开 ===");
            break;
        case BLE_REMOTE_STATE_SCANNING:
            ESP_LOGI(BLE_TAG, "=== 扫描中 ===");
            break;
        case BLE_REMOTE_STATE_CONNECTING:
            ESP_LOGI(BLE_TAG, "=== 连接中 ===");
            break;
        case BLE_REMOTE_STATE_CONNECTED:
            ESP_LOGI(BLE_TAG, "=== 已连接 ===");
            break;
        case BLE_REMOTE_STATE_NOTIFY_READY:
            ESP_LOGI(BLE_TAG, "=== 就绪，可以接收按键 ===");
            break;
        default:
            break;
        }
    }
    

    

    /***** 蓝牙遥控 -end *****/



        //省电管理
    void InitializePowerSaveTimer() {
        // 检测是否从深度睡眠中被按键（RTC GPIO 中断）唤醒
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            ESP_LOGI(TAG, "按键唤醒，正常启动");
        }

        power_save_timer_ = new PowerSaveTimer(-1, 20, 600);//600秒,10分钟
        power_save_timer_->OnEnterSleepMode([this]() {

            auto display = GetDisplay();
            // display->SetEmotion("Sleep_1_5");
            ESP_LOGE(TAG, "省电管理： 开启");


        });
        power_save_timer_->OnExitSleepMode([this]() {
            auto display = GetDisplay();
            // display->SetEmotion("wait_1_6");
            ESP_LOGI(TAG, "省电管理： 关闭");
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "省电管理：准备进入深度睡眠，先播放关机提示音 DHF11");

            // 1. 发送停止指令（运动控制先停）
            SendMotionFrame(MOTION_CMD_STOP);

            // 2. 先清掉当前可能在播放的其他音频，保证等会儿DHF11干净起播
            stop_all_sound_playback();

            // 3. 确保**功放是打开的**（不能关早了！），否则播DHF11时喇叭不响
            gpio_set_level(AUDIO_CODEC_PA_PIN, 1);

            // 4. LED同步闪亮（关机提示音也要有视觉反馈）—— 新启动一轮闪烁
            start_led_blink_if_needed();

            // 5. 同步解析 + push DHF11 到播放队列（索引=10，即最后一首）
            //    ——这里故意不放独立任务，因为接下来要同步等它播完
            constexpr int SHUTDOWN_SOUND_IDX = DHF_SOUND_COUNT - 1;  // = 10 → DHF11
            ESP_LOGI(TAG, "OnShutdownRequest: 开始解析 DHF%d（关机提示音，同步解析）", SHUTDOWN_SOUND_IDX + 1);
            Application::GetInstance().PlaySound(DHF_SOUNDS[SHUTDOWN_SOUND_IDX]);
            ESP_LOGI(TAG, "OnShutdownRequest: DHF%d 解析完成，开始等待播放完成...", SHUTDOWN_SOUND_IDX + 1);

            // 6. 阻塞等待 DHF11 **真实完整播出完毕**（含 I2S DMA 尾部 200ms）
            //    —— 必须等完再关 PA/背光/休眠，否则最后 200~500ms 声音被截断
            wait_for_playback_done_sync(TAG);

            // 7. 停止LED闪烁任务（代次自增，让 blink_task 自己退出），并恢复LED原状态
            // （注意：blink_generation_ 是 volatile uint32_t，后缀++已deprecated，改用赋值形式）
            blink_generation_ = blink_generation_ + 1;
            led_blink_running_ = false;
            if (led_blink_task_handle_ != nullptr) {
                led_blink_task_handle_ = nullptr;
            }
            gpio_set_level(LAMP_GPIO, led_blink_prev_lamp_on_ ? 1 : 0);
            led_blink_prev_lamp_on_ = false;

            // 8. 关闭背光 & 关闭功放（**等声音播完才关**！）
            gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 1 : 0);
            gpio_set_level(AUDIO_CODEC_PA_PIN, 0);

            ESP_LOGI(TAG, "省电管理：关机提示音播放完毕，正式进入休眠");

            /*****************************************************************
             *  休眠模式选择（关键：GPIO47 不是 RTC GPIO，deep sleep 无法按键唤醒，
             *                硬件又不能改→所以 light sleep 是唯一可行软件方案）
             *  - GPIO0~21（RTC域）：deep sleep（功耗~μA级）+ 尝试 ext0/ext1/gpio_deep_sleep
             *  - GPIO>21（数字域，如GPIO47）：**light sleep**（功耗几十~几百μA，仍远
             *    低于Active 100mA+） + gpio_wakeup_enable 普通GPIO中断唤醒 +
             *    1小时timer兜底；唤醒后主动 esp_restart() → 等同于重开机（播DHF1等）
             *****************************************************************/
            const bool is_rtc_gpio = (BOOT_BUTTON_GPIO >= GPIO_NUM_0 &&
                                      BOOT_BUTTON_GPIO <= GPIO_NUM_21);

            if (is_rtc_gpio) {
                /******************************************************************
                 *  RTC GPIO（0~21）：走 deep sleep 原流程
                 *  三种唤醒方式逐个尝试（ext0→ext1→gpio_deep_sleep），全部失败也不
                 *  abort，最多deep sleep后无法按键唤醒只能RESET/断电。
                 ******************************************************************/
                bool wakeup_ok = false;
                esp_err_t ext0_err = esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);
                if (ext0_err == ESP_OK) {
                    wakeup_ok = true;
                    ESP_LOGI(TAG, "DEEP_SLEEP：ext0 配置成功 (gpio=%d, 低电平)", (int)BOOT_BUTTON_GPIO);
                } else {
                    ESP_LOGW(TAG, "DEEP_SLEEP：ext0 失败 err=0x%x(%s)，尝试 ext1",
                             ext0_err, esp_err_to_name(ext0_err));
                    // ext1 单引脚场景：ALL_LOW 已在ESP32-S3 deprecated，改用 ANY_LOW（单pin下两者等价）
                    esp_err_t ext1_err = esp_sleep_enable_ext1_wakeup(
                        1ULL << (uint32_t)BOOT_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
                    if (ext1_err == ESP_OK) {
                        wakeup_ok = true;
                        ESP_LOGI(TAG, "DEEP_SLEEP：ext1 配置成功 (gpio=%d, ANY_LOW)", (int)BOOT_BUTTON_GPIO);
                    } else {
                        ESP_LOGW(TAG, "DEEP_SLEEP：ext1 失败 err=0x%x(%s)，尝试 gpio_wakeup",
                                 ext1_err, esp_err_to_name(ext1_err));
                        // gpio_wakeup_enable：官方GPIO deep sleep 唤醒的新 API（对RTC GPIO在deep sleep也生效）
                        // 替代在当前ESP-IDF v5.5.3中已移除的 gpio_deep_sleep_wakeup_enable()
                        esp_err_t gds_err = gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
                        if (gds_err == ESP_OK) {
                            esp_err_t g_enable_err = esp_sleep_enable_gpio_wakeup();
                            if (g_enable_err == ESP_OK) {
                                wakeup_ok = true;
                                ESP_LOGI(TAG, "DEEP_SLEEP：gpio_wakeup 成功 (gpio=%d, LOW_LEVEL)",
                                         (int)BOOT_BUTTON_GPIO);
                            } else {
                                ESP_LOGW(TAG, "DEEP_SLEEP：esp_sleep_enable_gpio_wakeup 失败 err=0x%x(%s)",
                                         g_enable_err, esp_err_to_name(g_enable_err));
                            }
                        } else {
                            ESP_LOGW(TAG, "DEEP_SLEEP：gpio_wakeup_enable 失败 err=0x%x(%s)",
                                     gds_err, esp_err_to_name(gds_err));
                        }
                    }
                }
                if (!wakeup_ok) {
                    ESP_LOGE(TAG, "DEEP_SLEEP：三种唤醒源全部失败（gpio=%d不支持deep sleep按键唤醒）。"
                             "deep sleep后只能RESET/断电重上电唤醒。", (int)BOOT_BUTTON_GPIO);
                }
                // RTC 上下拉（失败不 abort）
                esp_err_t pu_err = rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
                if (pu_err != ESP_OK) {
                    ESP_LOGW(TAG, "rtc_gpio_pullup_en(gpio=%d)失败: 0x%x(%s)",
                             (int)BOOT_BUTTON_GPIO, pu_err, esp_err_to_name(pu_err));
                }
                esp_err_t pd_err = rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO);
                if (pd_err != ESP_OK) {
                    ESP_LOGW(TAG, "rtc_gpio_pulldown_dis(gpio=%d)失败: 0x%x(%s)",
                             (int)BOOT_BUTTON_GPIO, pd_err, esp_err_to_name(pd_err));
                }

                ESP_LOGI(TAG, "--- 进入 deep sleep（永不返回，除非唤醒源生效）---");
                esp_deep_sleep_start();
            } else {
                /******************************************************************
                 *  非 RTC GPIO（如 GPIO47）：必须使用 light sleep + 普通GPIO唤醒
                 *  light sleep 期间数字域保持电源，GPIO中断控制器仍然运行，所以
                 *  gpio_wakeup_enable(GPIO47) 能正常生效。
                 *  - 流程：配置GPIO输入+上拉→配置gpio低电平唤醒→配置1h timer兜底
                 *          →esp_light_sleep_start()→唤醒→esp_restart()软重启
                 *  - 软重启会走完整Board构造+延迟播DHF1开机音+BLE重连，和上电
                 *    表现完全一致（用户体感就是"按键开机"）。
                 ******************************************************************/
                ESP_LOGI(TAG, "LIGHT_SLEEP：BOOT_GPIO=%d 非RTC GPIO，采用light sleep+普通GPIO唤醒",
                         (int)BOOT_BUTTON_GPIO);

                // 1) 确保 BOOT 按键引脚配置为 INPUT + PULL_UP（硬件设计是按键按下→接地）
                //    —— light sleep 下 gpio_set_pull_mode 仍然有效
                gpio_config_t boot_gpio_cfg = {};
                boot_gpio_cfg.pin_bit_mask = 1ULL << (uint32_t)BOOT_BUTTON_GPIO;
                boot_gpio_cfg.mode         = GPIO_MODE_INPUT;
                boot_gpio_cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
                boot_gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
                boot_gpio_cfg.intr_type    = GPIO_INTR_DISABLE;  // 唤醒由 gpio_wakeup_enable 控制
                esp_err_t cfg_err = gpio_config(&boot_gpio_cfg);
                if (cfg_err != ESP_OK) {
                    ESP_LOGW(TAG, "LIGHT_SLEEP：gpio_config(gpio=%d)失败 err=0x%x(%s)，继续尝试",
                             (int)BOOT_BUTTON_GPIO, cfg_err, esp_err_to_name(cfg_err));
                }

                // 2) 配置 GPIO 低电平唤醒（light sleep下支持任意GPIO，不限于RTC域）
                esp_err_t gw_err = gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
                bool gpio_wakeup_ok = false;
                if (gw_err == ESP_OK) {
                    esp_err_t ge_err = esp_sleep_enable_gpio_wakeup();
                    if (ge_err == ESP_OK) {
                        gpio_wakeup_ok = true;
                        ESP_LOGI(TAG, "LIGHT_SLEEP：gpio_wakeup 配置成功 (gpio=%d, LOW_LEVEL)",
                                 (int)BOOT_BUTTON_GPIO);
                    } else {
                        ESP_LOGW(TAG, "LIGHT_SLEEP：esp_sleep_enable_gpio_wakeup 失败 err=0x%x(%s)",
                                 ge_err, esp_err_to_name(ge_err));
                    }
                } else {
                    ESP_LOGW(TAG, "LIGHT_SLEEP：gpio_wakeup_enable(gpio=%d)失败 err=0x%x(%s)",
                             (int)BOOT_BUTTON_GPIO, gw_err, esp_err_to_name(gw_err));
                }

                if (!gpio_wakeup_ok) {
                    ESP_LOGE(TAG, "LIGHT_SLEEP：GPIO唤醒配置失败！"
                             "light sleep后无法通过BOOT键唤醒，只能RESET/断电重启。");
                }

                // 3) 进入 light sleep（该函数阻塞，被唤醒后返回）
                ESP_LOGI(TAG, "--- 进入 light sleep（BOOT按键按下低电平后唤醒并软重启）---");
                esp_err_t ls_err = esp_light_sleep_start();
                if (ls_err != ESP_OK) {
                    ESP_LOGE(TAG, "LIGHT_SLEEP：esp_light_sleep_start 失败 err=0x%x(%s)，降级直接重启",
                             ls_err, esp_err_to_name(ls_err));
                } else {
                    // light sleep 正常唤醒返回：记录唤醒原因便于调试
                    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
                    const char* cause_str = "UNKNOWN";
                    switch (cause) {
                    case ESP_SLEEP_WAKEUP_GPIO:    cause_str = "GPIO(BOOT按键)"; break;
                    case ESP_SLEEP_WAKEUP_TIMER:   cause_str = "TIMER";           break;
                    case ESP_SLEEP_WAKEUP_EXT0:    cause_str = "EXT0";            break;
                    case ESP_SLEEP_WAKEUP_EXT1:    cause_str = "EXT1";            break;
                    default: break;
                    }
                    ESP_LOGI(TAG, "LIGHT_SLEEP：唤醒成功，原因=%s（原因code=%d）。执行软重启回到'开机'流程",
                             cause_str, (int)cause);
                }

                // 4) 软重启 → 重新跑 app_main → Board 构造 → 延迟播 DHF1 + BLE重连……
                //    用户体感：按BOOT → 设备开机（等同于从断电状态上电启动）
                esp_restart();
            }
        });

        power_save_timer_->SetEnabled(true);
    }

public:
    YcscEsp32S3Es8311Dhf() : boot_button_(BOOT_BUTTON_GPIO), lamp_button_(LAMP_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeLamp();

        InitializeTools();
        InitializeMotionUart();
        // SendMotionFrame(MOTION_CMD_FORWARD);
        GetBacklight()->RestoreBrightness();

        /***** 蓝牙遥控 -begin *****/
        ESP_LOGE(BLE_TAG, "=== BLE Remote App Start ===");
        /* 注册回调 */
        ble_remote_register_key_callback(on_key_event);
        ble_remote_register_conn_callback(on_conn_event);     // 连接事件（连上/就绪/断开）
        ble_remote_register_state_callback(on_state_change);  // 细粒度状态（可选）

        /* 初始化并自动连接 */
        esp_err_t ret = ble_remote_init();
        if (ret != ESP_OK) {
            ESP_LOGE(BLE_TAG, "ble_remote_init failed: %s", esp_err_to_name(ret));
            return;
        }
        /* 创建切歌异步处理队列 + Worker 任务（必须在回调注册后使用）*/
        sound_switch_init_async();

        /**********************************************************************
         *  开机自动播放 DHF1 + LED同步闪烁（延迟启动，500ms后执行）
         *  - 为什么不能立即执行？Board 构造还在执行阶段，Application::audio_service_
         *    的 codec_ 指针还没被赋值为 Es8311 实例（赋值在 Board 构造完全返回后、
         *    Application::Init 后半段才执行）。立即调 PlaySound 会访问空指针
         *    codec_ → LoadProhibited 崩溃（EXCVADDR=0x0000000F）。
         *  - 策略：这里只创建 boot_delay_play_dhf1_task，该任务会先 vTaskDelay(500ms)
         *    等待所有初始化完成，然后再 start_led_blink_if_needed() + PlaySound(DHF1)
         *  - 非阻塞：xTaskCreate 几十μs完成，不阻塞 InitializePowerSaveTimer() 继续
         **********************************************************************/
        ESP_LOGI(BLE_TAG, "开机：注册延迟播放任务（500ms后启动LED闪烁+播放DHF1）");
        BaseType_t boot_delay_ret = xTaskCreate(boot_delay_play_dhf1_task, "boot_dhfrun",
                                                4096, nullptr, 5, nullptr);
        if (boot_delay_ret != pdPASS) {
            ESP_LOGE(BLE_TAG, "开机：创建boot_delay_play任务失败: %d（跳过开机播放）", boot_delay_ret);
        }
        /***** 蓝牙遥控 -end *****/

        InitializePowerSaveTimer();
    }


    virtual AudioCodec* GetAudioCodec() override {
         static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 0);
        return &led;
    }

};

DECLARE_BOARD(YcscEsp32S3Es8311Dhf);
