#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

//YCSC
// #include "boards/common/da218e.h"
// #include "gsensor_action.h"
#include <esp_lcd_gc9a01.h>
#include "otto_emoji_display.h" 
#include "power_manager.h"
#include "power_save_timer.h"

#include "touch_element/touch_button.h"


#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif


#define TAG "YcscEsp32S3Es8311WgxlCat"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);
class YcscEsp32S3Es8311WgxlCat : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;
    PowerManager* power_manager_;
    esp_timer_handle_t motor_timer_;
    bool motor_running_;

    PowerSaveTimer *power_save_timer_;

    static void MotorTimerCallback(void* arg) {
        YcscEsp32S3Es8311WgxlCat* board = static_cast<YcscEsp32S3Es8311WgxlCat*>(arg);
        board->MotorStop();
    }

    /****** 添加ESP32S3触摸,GPIO9 --begin ******/
    // 触摸按键句柄
    static touch_button_handle_t touch_btn;

    static void touch_event_cb(touch_button_handle_t btn, touch_button_message_t *msg, void *arg)
    {
        switch(msg->event)
        {
            case TOUCH_BUTTON_EVT_ON_PRESS:
                printf("触摸按下\n");
                break;
            case TOUCH_BUTTON_EVT_ON_RELEASE:
                printf("触摸松开\n");
                break;
            case TOUCH_BUTTON_EVT_ON_LONGPRESS:
                printf("长按触发\n");
                break;
            default:
                break;
        }
    }


    /**
     * @brief 初始化触摸按键 (GPIO9 / TOUCH_PAD_NUM9)
     * 
     * 调用顺序:
     *   1. touch_element_install()     - 安装全局触摸元素驱动
     *   2. touch_button_install()      - 安装按键子模块，设置阈值分压和默认长按时间
     *   3. touch_button_create()       - 创建单个触摸按键实例，指定通道和灵敏度
     *   4. touch_button_set_longpress()- 设置该按键的长按触发时长(ms)
     *   5. touch_button_subscribe_event()- 订阅事件类型(按下/松开/长按)
     *   6. touch_button_set_dispatch_method()- 设置为回调模式(必须!默认是事件队列模式)
     *   7. touch_button_set_callback() - 绑定回调函数
     *   8. touch_element_start()       - 启动触摸处理
     * 
     * 灵敏度说明:
     *   - threshold_divider: 阈值分压系数(0~1), 越小越灵敏, 隔塑料可降到0.5
     *   - channel_sens: 通道灵敏度(0~1), 越小越灵敏, 隔塑料可降到0.1~0.2
     */
    void InitializeTouch()
    {
        // 1. 全局触摸配置并安装
        touch_elem_global_config_t global_cfg = TOUCH_ELEM_GLOBAL_DEFAULT_CONFIG();
        touch_element_install(&global_cfg);

        // 2. 触摸按键模块初始化 (threshold_divider越小越灵敏)
        touch_button_global_config_t btn_global_cfg = {
            .threshold_divider = 0.5,   // 阈值分压系数(默认0.8, 降低以适配塑料外壳)
            .default_lp_time = 1000,    // 默认长按时间1000ms
        };
        touch_button_install(&btn_global_cfg);

        // 3. 创建触摸按键实例 (channel_sens越小越灵敏)
        touch_button_config_t btn_cfg = {
            .channel_num = TOUCH_PAD_NUM9,  // GPIO9 对应触摸通道9
            .channel_sens = 0.2,            // 灵敏度0.2 (默认0.5, 降低以适配塑料外壳)
        };
        touch_button_create(&btn_cfg, &touch_btn);

        // 4. 设置长按触发时间为1000ms
        touch_button_set_longpress(touch_btn, 1000);

        // 5. 订阅事件: 按下、松开、长按
        touch_button_subscribe_event(touch_btn,
            TOUCH_ELEM_EVENT_ON_PRESS | TOUCH_ELEM_EVENT_ON_RELEASE | TOUCH_ELEM_EVENT_ON_LONGPRESS,
            NULL);

        // 6. 设置为回调模式 (重要! 默认是事件队列模式, 不设置回调不会触发)
        touch_button_set_dispatch_method(touch_btn, TOUCH_ELEM_DISP_CALLBACK);

        // 7. 绑定触摸事件回调函数
        touch_button_set_callback(touch_btn, touch_event_cb);

        // 8. 启动触摸处理
        touch_element_start();
    }
    
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
                ESP_LOGE(TAG, "5555555555555555555555555555555555");
                return;
            }
            app.ToggleChatState();
            //日志打印
            ESP_LOGI(TAG, "66666666666666666666666666666666");
            // power_save_timer_->WakeUp();
        });
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
        display_ = new OttoEmojiDisplay(panel_io, panel,
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



        // xlc add -begin

       //左屏水平镜像
       gpio_set_direction(DISPLAY_CS_PIN, GPIO_MODE_OUTPUT);
       gpio_set_level(DISPLAY_CS_PIN, 1);
       //延时100ms
       vTaskDelay(pdMS_TO_TICKS(50));
       //esp_lcd_panel_swap_xy(lcd_panel, true);
       esp_lcd_panel_mirror(panel, false,true);  // 水平镜像
       vTaskDelay(pdMS_TO_TICKS(50));
       gpio_set_level(DISPLAY_CS_PIN, 0);

       InitializeTouch();
                            
       //xlc add -end     

    }

   
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        // 定义设备的属性
        mcp_server.AddTool("self.motor",
            "指到谁谁喝酒，当听到转圈、指到谁谁喝酒时执行该操作; ",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                //随机转动时间，范围在1-5秒之间
                int time = rand() % 15 + 3;
                //正转反转随机1或2：MOTOR_FORWARD、MOTOR_BACKWARD
                int direction = rand() % 2 + 1;
                if (direction == 1) {
                    direction = MOTOR_FORWARD;
                } else {
                    direction = MOTOR_BACKWARD;
                }
                //日志输出转动时间和转动方向
                ESP_LOGI(TAG, "电机转动MotorControl: direction=%d, time=%d", direction, time);
                MotorControl(direction, time*1000);
                return true;
            });
    }


    //ADC电池管理
    void InitializePowerManager() {
        power_manager_ =
        new PowerManager(POWER_CHARGE_DETECT_PIN, POWER_CHARGE_COMPLETE_PIN, POWER_ADC_UNIT, POWER_ADC_CHANNEL);
    }

    //省电管理
    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 10, 300);
        power_save_timer_->OnEnterSleepMode([this]() {

            auto display = GetDisplay();
            display->SetEmotion("Sleep_1_5");
            ESP_LOGI(TAG, "省电管理： 开启");


        });
        power_save_timer_->OnExitSleepMode([this]() {
            auto display = GetDisplay();
            display->SetEmotion("FaCai_1_2");
            ESP_LOGI(TAG, "省电管理： 关闭");
        });
        power_save_timer_->OnShutdownRequest([this]() {
        
                // 日志输出省电模式开启
                ESP_LOGI(TAG, "省电管理： 关机");
            
        });

        power_save_timer_->SetEnabled(true);
    }

    //电机控制
    //电机初始化
    void InitializeMotor() {
        motor_running_ = false;
        gpio_set_direction(MOTOR_IN_1_PIN, GPIO_MODE_OUTPUT);
        gpio_set_direction(MOTOR_IN_2_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(MOTOR_IN_1_PIN, 0); 
        gpio_set_level(MOTOR_IN_2_PIN, 0); 

        esp_timer_create_args_t timer_args = {
            .callback = &MotorTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "motor_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &motor_timer_));
    }

    //电机控制：参数1：正转、反转和停止；参数2：转动时间，单位毫秒，为负数时表示持续转动
    void MotorControl(int direction, int time) {
        if (direction == 0) {
            MotorStop();
            return;
        }
        
        if (motor_running_) {
            esp_timer_stop(motor_timer_);
        }
        
        if (direction == 1) {
            MotorForward();
        } else if (direction == 2) {
            MotorBackward();
        }
        
        motor_running_ = true;
        
        if (time >= 0) {
            esp_timer_start_once(motor_timer_, time * 1000);
        }
    }

    //电机正转  
    void MotorForward() {
        gpio_set_level(MOTOR_IN_1_PIN, 0); 
        gpio_set_level(MOTOR_IN_2_PIN, 1); 
    }

    //电机反转
    void MotorBackward() {
        gpio_set_level(MOTOR_IN_1_PIN, 1); 
        gpio_set_level(MOTOR_IN_2_PIN, 0); 
    }

    //电机停止
    void MotorStop() {
        gpio_set_level(MOTOR_IN_1_PIN, 0); 
        gpio_set_level(MOTOR_IN_2_PIN, 0); 
        motor_running_ = false;
        esp_timer_stop(motor_timer_);
    }

public:
    YcscEsp32S3Es8311WgxlCat() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
    
        InitializeTools();

        InitializePowerManager();
        InitializeMotor();

        //开机后自动进入低功耗模式
        GetBacklight()->RestoreBrightness();

        InitializePowerSaveTimer();
        
        //开机正转1秒，反转1秒，停止
        MotorControl(MOTOR_FORWARD, 2000);
        vTaskDelay(pdMS_TO_TICKS(2000));
        MotorControl(MOTOR_BACKWARD, 2000);
        vTaskDelay(pdMS_TO_TICKS(2000));
        MotorControl(MOTOR_STOP, 0);

        


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

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void WakeUp() override {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    }

};

touch_button_handle_t YcscEsp32S3Es8311WgxlCat::touch_btn = nullptr;

DECLARE_BOARD(YcscEsp32S3Es8311WgxlCat);