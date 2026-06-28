#include "otto_emoji_display.h"

#include <esp_log.h>
#include <font_awesome.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "display/lcd_display.h"

#define TAG "OttoEmojiDisplay"

// 表情映射表 - 将原版21种表情映射到现有6个GIF
const OttoEmojiDisplay::EmotionMap OttoEmojiDisplay::emotion_maps_[] = {

    {"Charging_1_1", &Charging_1_1},
    {"FaCai_1_2", &FaCai_1_2},
    {"Listen_1_3", &Listen_1_3},
    {"Speak_1_4", &Speak_1_4},
    {"Sleep_1_5", &Sleep_1_5},
    {"wait_1_6", &wait_1_6},




    // 中性/平静类表情 -> staticstate
    {"neutral", &Listen_1_3},
    {"relaxed", &Listen_1_3},
    {"sleepy", &Listen_1_3},

    // 积极/开心类表情 -> happy
    {"happy", &Listen_1_3},
    {"laughing", &Listen_1_3},
    {"funny", &Listen_1_3},
    {"loving", &Listen_1_3},
    {"confident", &Listen_1_3},
    {"winking", &Listen_1_3},
    {"cool", &Listen_1_3},
    {"delicious", &Listen_1_3},
    {"kissy", &Listen_1_3},
    {"silly", &Listen_1_3},

    // 悲伤类表情 -> sad
    {"sad", &Listen_1_3},
    {"crying", &Listen_1_3},

    // 愤怒类表情 -> anger
    {"angry", &Listen_1_3},

    // 惊讶类表情 -> scare
    {"surprised", &Listen_1_3},
    {"shocked", &Listen_1_3},

    // 思考/困惑类表情 -> buxue
    {"thinking", &Listen_1_3},
    {"confused", &Listen_1_3},
    {"embarrassed", &Listen_1_3},

    {nullptr, nullptr}  // 结束标记
};

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy, DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    fonts),
      emotion_gif_(nullptr) {
    // 立即隐藏父类创建的状态栏，避免在SetupGifContainer执行前闪现
    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    //隐藏表情标签
    lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    //隐藏表情GIF
    lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    SetupGifContainer();
};

void OttoEmojiDisplay::SetupGifContainer() {
    DisplayLockGuard lock(this);

    // 立即删除状态栏（父类构造函数中已创建），UpdateStatusBar已重写为空操作
    if (status_bar_) {
        lv_obj_del(status_bar_);
        status_bar_ = nullptr;
        // status_bar_ 的删除会递归删除其所有子对象，必须将相关指针置空防止悬空指针
        status_label_ = nullptr;
        notification_label_ = nullptr;
        emotion_label_ = nullptr;
        mute_label_ = nullptr;
        network_label_ = nullptr;
        battery_label_ = nullptr;
    }

    // emotion_label_ 可能已随 status_bar_ 一起被删除（微信模式），也可能还是 content_ 的子对象
    // 但此处总要尝试删除（非微信模式），由于上面已置空，不会重复删除
    if (emotion_label_) {
        lv_obj_del(emotion_label_);
        emotion_label_ = nullptr;
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
        chat_message_label_ = nullptr;
    }
    if (content_) {
        lv_obj_del(content_);
        content_ = nullptr;
    }

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    emotion_label_ = lv_label_create(content_);
    lv_label_set_text(emotion_label_, "");
    lv_obj_set_width(emotion_label_, 0);
    lv_obj_set_style_border_width(emotion_label_, 0, 0);
    lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_gif_ = lv_gif_create(content_);
    int gif_size = LV_HOR_RES;
    lv_obj_set_size(emotion_gif_, gif_size, gif_size);
    lv_obj_set_style_border_width(emotion_gif_, 0, 0);
    lv_obj_set_style_bg_opa(emotion_gif_, LV_OPA_TRANSP, 0);
    lv_obj_center(emotion_gif_);
    lv_gif_set_src(emotion_gif_, &FaCai_1_2);

    chat_message_label_ = nullptr;

    LcdDisplay::SetTheme("dark");
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion || !emotion_gif_) {
        return;
    }

    DisplayLockGuard lock(this);

    for (const auto& map : emotion_maps_) {
        if (map.name && strcmp(map.name, emotion) == 0) {
            lv_gif_set_src(emotion_gif_, map.gif);
            ESP_LOGI(TAG, "设置表情: %s", emotion);
            return;
        }
    }

    lv_gif_set_src(emotion_gif_, &FaCai_1_2);
    ESP_LOGI(TAG, "未知表情'%s'，使用默认", emotion);
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    // 仅显示表情，不显示任何文字
    return;
}

void OttoEmojiDisplay::SetIcon(const char* icon) {
    if (!icon) {
    return;
    }

    DisplayLockGuard lock(this);

    if (chat_message_label_ != nullptr) {
        std::string icon_message = std::string(icon) + " ";

        if (strcmp(icon, FONT_AWESOME_DOWNLOAD) == 0) {
            icon_message += "正在升级...";
        } else {
            icon_message += "系统状态";
        }

        lv_label_set_text(chat_message_label_, icon_message.c_str());
        lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

        ESP_LOGI(TAG, "设置图标: %s", icon);
    }
}