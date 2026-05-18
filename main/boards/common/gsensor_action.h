#ifndef GSENSOR_ACTION_H
#define GSENSOR_ACTION_H

#include "i2c_device.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"

#include <cmath>

#include "da218e.h"

#define TAG_GSENSOR "GsensorAction" 

//冷却时间
#define COOLING_TIME_MS 15000

//读取数据频率，单位：毫秒
#define GSENSOR_READ_FREQ_MS 100

//静止状态判断阈值，单位：g
#define GSENSOR_STILL_THRESHOLD 0.1f

//静止状态判断时长，单位：秒
#define GSENSOR_STILL_DURATION 5

//判断静止状态所需连续次数，公式：次数=判断时长/读取频率，单位：次
#define GSENSOR_STILL_COUNT (GSENSOR_STILL_DURATION * 1000 / GSENSOR_READ_FREQ_MS)

//判断静止状态允许误差次数
#define GSENSOR_STILL_ERROR_COUNT 3


//判断摇晃状态所需连续次数
#define GSENSOR_SHAKE_COUNT 5
//判断摇晃状态阈值，单位：g
#define GSENSOR_SHAKE_THRESHOLD 2.4f
//判断摇晃检测时长，单位：秒
#define GSENSOR_SHAKE_DURATION 3
//判断摇晃检测时长，公式：次数=时长/读取频率，单位：次
#define GSENSOR_SHAKE_DURATION_COUNT (GSENSOR_SHAKE_DURATION * 1000 / GSENSOR_READ_FREQ_MS)


//yuzhi 2024-10-12
//判断抛掷状态所需连续次数
#define GSENSOR_THROW_COUNT 2

//判断抛掷状态阈值，单位：g
#define GSENSOR_THROW_THRESHOLD 0.35f







class GsensorAction {
    public:

        Da218e* da218e_;
        //传感器当前值
        float gsensor_acc;
        //传感器上次值
        float gsensor_last_acc;

        //是否静止状态，静态变量
        bool Gsensor_Is_Still=true;
        //上一次是否静止状态
        bool Gsensor_Last_IS_Still=false;

        //判断静止状态计数
        int gsensor_still_count = 0;
        //判断静止状态误差计数器
        int gsensor_still_error_count = 0;

        
        //判断是否摇晃
        bool Gsensor_Is_Shake;
        //判断摇晃计数器
        int gsensor_shake_count = 0;
        //判断摇晃持续时间计数器
        int gsensor_shake_duration_count = 0;

        //判断是否抛掷
        bool Gsensor_Is_Throw;
        //判断抛掷状态计数
        int gsensor_throw_count = 0;

        //上一次触发摇晃事件时间
        uint32_t last_shake_time_ = 0;
        //上一次触发抛掷事件时间
        uint32_t last_throw_time_ = 0;


        //状态枚举类型
        enum  GsensorState {
            //静止状态
            GSENSOR_STATE_STILL,

            //拿起状态
            GSENSOR_STATE_PICKUP,

            //摇晃状态
            GSENSOR_STATE_SHAKE,

            //抛掷状态
            GSENSOR_STATE_THROW,

            //未知状态
            GSENSOR_STATE_UNKNOWN,
            
        };

        GsensorAction(i2c_master_bus_handle_t bus, uint8_t addr);
        ~GsensorAction();

        //读取传感器数据
        float ReadGsensorData();

        void GsensorActionTask();
        void StartGsensorActionTask();

        //动作识别
        void RecognizeAction(float acc);

        //判断是否静止
        bool IsStill(float acc);

        //获取GsensorState
        GsensorState GetGsensorState() const { return gsensor_state_; };
        void SetGsensorState(GsensorState state) ;

        

    private:
        TaskHandle_t gsensor_action_task_handle_ = nullptr;
        volatile GsensorState gsensor_state_ = GSENSOR_STATE_UNKNOWN;
        
       
};

#endif 