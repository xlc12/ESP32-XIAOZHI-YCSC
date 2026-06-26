#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "config.h"
#include "mcp_server.h"
#include "device_state.h"

/*
待机状态下
3.6v  平均ADC 2030
3.7   平均ADC 2100
3.8v  平均ADC 2200
3.9v  平均ADC 2280
4.0v  平均ADC 2350
4.1v  平均ADC 2440
4.2v  平均ADC 2530 
运行时，会少80
3.6v  平均ADC 1950
3.7v  平均ADC 2080
3.8v  平均ADC 2150
3.9v  平均ADC 2220
4.0v  平均ADC 2290
4.1v  平均ADC 2360
4.2v  平均ADC 2430



*/

class PowerManager {
private:
    // 电池电量区间-分压电阻为2个100k
    static constexpr struct {
        uint16_t adc;
        uint8_t level;
    } BATTERY_LEVELS[] = {{2030, 0}, {2450, 100}};
    static constexpr size_t BATTERY_LEVELS_COUNT = 2;
    static constexpr size_t ADC_VALUES_COUNT = 10;

    esp_timer_handle_t timer_handle_ = nullptr;
    gpio_num_t charging_pin_;
    gpio_num_t charging_complete_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    uint16_t adc_values_[ADC_VALUES_COUNT];
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    bool is_charging_ = false;
    bool is_charging_complete_ = false;

    
    adc_oneshot_unit_handle_t adc_handle_;

    void CheckBatteryStatus() {
        auto& app = Application::GetInstance();
        // 应用未完成初始化时不播放声音，避免 AudioService 未就绪导致 crash
        if (app.GetDeviceState() == kDeviceStateUnknown || app.GetDeviceState() == kDeviceStateStarting) {
            return;
        }
        static uint32_t last_low_battery_time_ = 0; // 电池电量过低的最后时间点
        static uint32_t last_charging_complete_time_ = 0; // 电池充电完成的最后时间点
        static uint32_t last_start_charging_flag = 0; //开始充电标志位
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

        is_charging_ = gpio_get_level(charging_pin_) == 0;
        is_charging_complete_ = gpio_get_level(charging_complete_pin_) == 0;
        ReadBatteryAdcData();

        // 如果电量低于低电量报警阈值且未在充电，且未充电完成，则进行报警
        if(battery_level_ <= LOW_BATTERY_ALARM_LEVEL && !is_charging_ && !is_charging_complete_) {
            ESP_LOGW("PowerManager", "电量过低，>>>>>>>>");
            // 在这里可以添加关机逻辑，例如调用系统API进行关机
            
            if (current_time - last_low_battery_time_ >= LOW_BATTERY_ALARM_INTERVAL_MS * 1000) {
                last_low_battery_time_ = current_time;
                app.PlaySound(Lang::Sounds::OGG_1_4_LOW_BATTERY);
            }
        }

        //开始充电提醒
        if(is_charging_ && (last_start_charging_flag == 0)){
            last_start_charging_flag = 1;
            app.PlaySound(Lang::Sounds::OGG_1_5_CHARGING);
            auto display = Board::GetInstance().GetDisplay();
            display->SetEmotion("Charging_1_1");
        }

        //充电器拔出
        if(!is_charging_){
            last_start_charging_flag = 0;
            // app.PlaySound(Lang::Sounds::OGG_1_10_CHONDIANWANCHENG);
        }

        //充电完成提示
        if(is_charging_complete_) {
            ESP_LOGI("PowerManager", "充电完成");
            if (current_time - last_charging_complete_time_ >= CHARGING_COMPLETE_ALARM_INTERVAL_MS * 1000) {
                last_charging_complete_time_ = current_time;
                // //循环播放提示音
                // for (int i = 0; i < 2; i++) {
                    
                //     //延时1秒
                //     vTaskDelay(pdMS_TO_TICKS(3000));
                // }
                app.PlaySound(Lang::Sounds::OGG_1_6_CHARG_END);
            }
        }
    }

    void ReadBatteryAdcData() {
        int adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, adc_channel_, &adc_value));

        adc_values_[adc_values_index_] = adc_value;
        adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
        if (adc_values_count_ < ADC_VALUES_COUNT) {
            adc_values_count_++;
        }

        uint32_t average_adc = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc += adc_values_[i];
        }
        average_adc /= adc_values_count_;
 
        CalculateBatteryLevel(average_adc);

        // ESP_LOGI("PowerManager", "ADC值: %d ，平均值: %ld ，电量: %u%% ，充电状态: %s ，充电完成状态: %s", 
        //          adc_value, 
        //          average_adc, 
        //          battery_level_, 
        //          is_charging_ ? "充电中" : "未充电", 
        //          is_charging_complete_ ? "充电完成" : "未充电完成");
    }

    void CalculateBatteryLevel(uint32_t average_adc) {
        if (average_adc <= BATTERY_LEVELS[0].adc) {
            battery_level_ = 0;
        } else if (average_adc >= BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].adc) {
            battery_level_ = 100;
        } else {
            float ratio = static_cast<float>(average_adc - BATTERY_LEVELS[0].adc) /
                          (BATTERY_LEVELS[1].adc - BATTERY_LEVELS[0].adc);
            battery_level_ = ratio * 100;
        }
    }

public:
    PowerManager(gpio_num_t charging_pin, gpio_num_t charging_complete_pin, adc_unit_t adc_unit = ADC_UNIT_2,
                 adc_channel_t adc_channel = ADC_CHANNEL_3)
        : charging_pin_(charging_pin), charging_complete_pin_(charging_complete_pin), adc_unit_(adc_unit), adc_channel_(adc_channel) {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_) | (1ULL << charging_complete_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);

        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    PowerManager* self = static_cast<PowerManager*>(arg);
                    self->CheckBatteryStatus();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1*1000*1000));  

        InitializeAdc();
        
    }


    //MCP工具初始化
    void InitializeMcpTools() {
        auto& mcp_server = McpServer::GetInstance();
        //mcp查看当前电量
        mcp_server.AddTool("luck.get_battery_level", "获取当前电量", 
            PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                std::string battery_level_ = "当前电量" + std::to_string(GetBatteryLevel());
                //充电状态
                std::string charge_status = IsCharging() ? "，正在充电" : "，未充电";
                return battery_level_ + charge_status;
            });

    }

    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = adc_unit_,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };

        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_config));
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging() { return is_charging_; }

    uint8_t GetBatteryLevel() { return battery_level_; }

    //获取是否充电完成
    bool IsChargingComplete() { return is_charging_complete_; }
};
#endif  // __POWER_MANAGER_H__