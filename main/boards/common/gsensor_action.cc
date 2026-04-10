#include "gsensor_action.h"

GsensorAction::GsensorAction(i2c_master_bus_handle_t bus, uint8_t addr) { 
    
    da218e_ = new Da218e(bus, addr);

    StartGsensorActionTask();
}

GsensorAction::~GsensorAction() {
    // 这里可以添加清理代码，如果有需要的话
    if (gsensor_action_task_handle_ != nullptr) {
        vTaskDelete(gsensor_action_task_handle_);
        gsensor_action_task_handle_ = nullptr;
    }


}

//获取GsensorState

void GsensorAction::SetGsensorState(GsensorState state) {
    auto& app = Application::GetInstance();
     if ((gsensor_state_ == state) && (gsensor_state_ == GSENSOR_STATE_STILL)) {
        return;
    }
    gsensor_state_ = state;
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    switch (state)
    {

        case GSENSOR_STATE_STILL:
            // ESP_LOGE(TAG_GSENSOR, "|||||AAAAAAAAAADSSAAAAAAAAAAA 状态变化：静止");
            break;

        case GSENSOR_STATE_PICKUP:
            // ESP_LOGE(TAG_GSENSOR, "|||||AAAAAAAAAADSSAAAAAAAAAAA 状态变化：静止 -> 拿起");
            break;

        case GSENSOR_STATE_SHAKE:
            if (current_time - last_shake_time_ < COOLING_TIME_MS) {
                ESP_LOGE(TAG_GSENSOR, "|||||AAAAAA 摇晃 过于频繁忽略");
                    return; // 如果距离上次触发时间小于间隔，直接返回
            }
            last_shake_time_ = current_time;
            ESP_LOGE(TAG_GSENSOR, "|||||AAAAAAAAAADSSAAAAA6666666AAAAAA 状态变化：摇晃摇晃摇晃摇晃 ");
            // 发送传感器数据touch-hand   shake-body
            app.SendSensorData("shake-body", "stop", "The device is being shaken.");
  
            vTaskDelay(pdMS_TO_TICKS(50));
            app.SendSensorData("shake-body", "start", "The device is being shaken.");
            break;

        case GSENSOR_STATE_THROW:
            if (current_time - last_throw_time_ < COOLING_TIME_MS) {
                ESP_LOGE(TAG_GSENSOR, "|||||AAAAAA 抛掷 过于频繁忽略");
                    return; // 如果距离上次触发时间小于间隔，直接返回
            }
            last_throw_time_ = current_time;
            ESP_LOGE(TAG_GSENSOR, "|||||AAAAAAAAAADSSAAAAAAAAAAA 状态变化：抛掷 -> 静止");
            app.SendSensorData("throw-it-up", "stop", "");
  
            vTaskDelay(pdMS_TO_TICKS(50));
            app.SendSensorData("throw-it-up", "start", "");
            break;
        
        default:
            break;
    }

};



float GsensorAction::ReadGsensorData() {
    return da218e_->ReadData();

}

bool GsensorAction::IsStill(float acc) {
   
            return true;

}
void GsensorAction::RecognizeAction(float acc) {
    
}

void GsensorAction::GsensorActionTask( ) {
    //一秒打印一次数据
    int log_count = 0;
    //允许打印
    bool allow_log = false;
    float acc_samples_[10] = {0.0f}; // 初始化数组元素为0
    // GsensorAction* g_a = static_cast<GsensorAction*>(arg);
    while (true) {

        gsensor_acc = ReadGsensorData();
        gsensor_last_acc = gsensor_acc;
        
        if (allow_log) {
            allow_log = false;
        }
        if (log_count >= 10) {
            log_count = 0;
            allow_log = true;
        }

        //将数据10次数据存到数组
        
        acc_samples_[log_count] = gsensor_acc;
        log_count++;
        // gsensor_action->RecognizeAction(gsensor_action->gsensor_acc);
        if (allow_log) {

            allow_log = false;
            //打印数组所有数据
            // ESP_LOGW(TAG_GSENSOR, "Gsensor 数组数据: %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f",
            // acc_samples_[0], acc_samples_[1], acc_samples_[2], acc_samples_[3], acc_samples_[4],
            // acc_samples_[5], acc_samples_[6], acc_samples_[7], acc_samples_[8], acc_samples_[9]);
            // ESP_LOGW(TAG_GSENSOR, "当前加速度: %.2f g", gsensor_acc);
            // ESP_LOGW(TAG_GSENSOR, "上次加速度: %.2f g", gsensor_last_acc);
            // 修复：使用循环重置数组所有元素
            for (int i = 0; i < 10; i++) {
                acc_samples_[i] = 99.0f;
            }
        }
        
        

        //连续判断5秒，如果在这5秒内加速度值都在0.95g到1.05g之间，则认为是静止状态，但允许中途出现N次超过范围的情况
        if (gsensor_acc > 0.94f && gsensor_acc < 1.05f) {
            gsensor_still_count++;
            if (gsensor_still_count >= GSENSOR_STILL_COUNT) { // 5秒内满足条件
                gsensor_still_count = 0; // 重置计数器
                gsensor_still_error_count = 0; // 重置误差计数器
                Gsensor_Last_IS_Still = Gsensor_Is_Still;
                Gsensor_Is_Still = true;
                
                ESP_LOGE(TAG_GSENSOR, "Gsensor 静止状态");
                SetGsensorState(GSENSOR_STATE_STILL);
            }
        } else {
        
            //允许中途出现几次误差，不影响静止状态判断
            if (gsensor_last_acc < 1.12f && gsensor_last_acc > 0.88f) {
                gsensor_still_error_count++;
                if (gsensor_still_error_count >  GSENSOR_STILL_ERROR_COUNT) {
                    gsensor_still_count = 0;
                    gsensor_still_error_count = 0;
                    Gsensor_Last_IS_Still = Gsensor_Is_Still;
                    Gsensor_Is_Still = false;
                // ESP_LOGE(TAG_GSENSOR, "Gsensor444444444 静止误差");
                } else {
                    gsensor_still_count++;
                    // gsensor_still_error_count++;
                }
            } else {
                gsensor_still_count = 0;    
                gsensor_still_error_count = 0;
                Gsensor_Last_IS_Still = Gsensor_Is_Still;
                Gsensor_Is_Still = false;
                // ESP_LOGE(TAG_GSENSOR, "Gsensor4444444444444455555 拿起状态");
            }
        }

        /********** 拿起状态判断 **********/
        if (gsensor_state_ == GSENSOR_STATE_STILL) {
            if (!Gsensor_Is_Still) {
                SetGsensorState(GSENSOR_STATE_PICKUP);
                
            }
        } else {
            if (Gsensor_Is_Still) {
                // SetGsensorState(GSENSOR_STATE_STILL);
                // ESP_LOGE(TAG_GSENSOR, "GsensorDDDDDDDDDDDDDDDDDDDDDDDDD 状态变化：拿起 -> 静止");
            }
        }




        /********** 摇晃状态判断 **********/
        if ((gsensor_acc > GSENSOR_SHAKE_THRESHOLD) && (gsensor_shake_duration_count < GSENSOR_SHAKE_DURATION_COUNT)) {
            //开始摇晃计数
            gsensor_shake_count++;
            gsensor_shake_duration_count++;
            if (gsensor_shake_count >= GSENSOR_SHAKE_COUNT) { // 连续次数超过阈值
                //检测到摇晃状态
                gsensor_shake_count = 0;
                gsensor_shake_duration_count = 0;
                Gsensor_Is_Shake = true;
                ESP_LOGE(TAG_GSENSOR, "Gsensor 摇晃状态");
                SetGsensorState(GSENSOR_STATE_SHAKE);
            }
        } else {
            if ((gsensor_shake_duration_count < GSENSOR_SHAKE_DURATION_COUNT) && (gsensor_shake_count != 0)) {
                gsensor_shake_duration_count++;
                Gsensor_Is_Shake = false;
            }else {
                //超时-结束摇晃计数
                gsensor_shake_count = 0;
                gsensor_shake_duration_count = 0;
            }
            
        }


        /********** 抛掷状态判断 **********/
        //jiasudu小于0.5g，且持续2秒以上，则认为是抛掷状态
        if (gsensor_acc < GSENSOR_THROW_THRESHOLD) {
            gsensor_throw_count++;
            if (gsensor_throw_count >= GSENSOR_THROW_COUNT) { 
                //检测到抛掷状态
                gsensor_throw_count = 0;
                Gsensor_Is_Throw = true;
                ESP_LOGE(TAG_GSENSOR, "Gsensor 抛掷状态");
                SetGsensorState(GSENSOR_STATE_THROW);
            }
        } else {
            //结束抛掷计数
            gsensor_throw_count = 0;
            Gsensor_Is_Throw = false;
        }



        vTaskDelay(pdMS_TO_TICKS(GSENSOR_READ_FREQ_MS)); // 每200ms读取一次数据
    }
}

void GsensorAction::StartGsensorActionTask() {
    
    if (gsensor_action_task_handle_ == nullptr) {
        xTaskCreate([](void* arg) {
            GsensorAction* g_a = static_cast<GsensorAction*>(arg);
            g_a->GsensorActionTask();
        }
        , "GsensorActionTask", 1024 * 3, this, configMAX_PRIORITIES - 1,
                    &gsensor_action_task_handle_);
    }
}