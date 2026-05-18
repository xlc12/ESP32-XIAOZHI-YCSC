#include "da218e.h"

Da218e::Da218e(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
    // 读取CHIPID寄存器，验证设备连接
    ESP_LOGI(TAG_Da218e, "DA218E 设备初始化66666666666666666666666:");
    uint8_t chip_id = ReadReg(DA218E_REG_CHIPID);
    
    if (chip_id != 0x13) {
        ESP_LOGI(TAG_Da218e, "DA218E 设备 ID 错误: 0x%02X", chip_id);
        return;
    }
    
    ESP_LOGI(TAG_Da218e, "DA218E 设备 ID: 0x%02X", chip_id);
    
    // 设置默认量程为±2g
    if (SetRange(Da218eRange::RANGE_4G)) {
        ESP_LOGI(TAG_Da218e, "DA218E 设备初始化成功，默认量程设置为±2g");
    } else {
        ESP_LOGW(TAG_Da218e, "DA218E 设备初始化成功，但量程设置失败");
    }

    WriteReg(DA218E_REG_POWERMODE_BW, 0x14);
    WriteReg(DA218E_REG_ODR_AXIS_DISABLE, 0x07);

    
    // StartDa218eTask();
}

Da218e::~Da218e() {
    // 清理任务
    if (da218e_task_handle_ != nullptr) {
        vTaskDelete(da218e_task_handle_);
        da218e_task_handle_ = nullptr;
    }
}

bool Da218e::IsNewDataAvailable() {
    uint8_t new_data_flag = ReadReg(DA218E_REG_NEWDATA_FLAG);
    return (new_data_flag == 1);
}

float Da218e::ReadData() {
    // 检查是否有新数据
    // if (!IsNewDataAvailable()) {
    //     ESP_LOGI(TAG_Da218e, "DA218E 没有新数据可用");
    //     return false;
    // }
  

    
    // 分别读取每个轴的LSB和MSB寄存器

    uint8_t ax_lsb = ReadReg(DA218E_REG_ACC_X_LSB);  // 0x02
    uint8_t ax_msb = ReadReg(DA218E_REG_ACC_X_MSB);  // 0x03
    uint8_t ay_lsb = ReadReg(DA218E_REG_ACC_Y_LSB);  // 0x04
    uint8_t ay_msb = ReadReg(DA218E_REG_ACC_Y_MSB);  // 0x05
    uint8_t az_lsb = ReadReg(DA218E_REG_ACC_Z_LSB);  // 0x06
    uint8_t az_msb = ReadReg(DA218E_REG_ACC_Z_MSB);  // 0x07
    

    int16_t ax_raw = (ax_msb  << 4) | (ax_lsb & 0x0F);
    // 扩展为16位补码（12位→16位，符号位扩展）
    if (ax_raw & 0x800) {  // 12位最高位（Bit11）为1，是负数
        ax_raw |= 0xF000;  // 高4位补1
    }

    int16_t ay_raw = (ay_msb  << 4) | (ay_lsb & 0x0F);
    // 扩展为16位补码（12位→16位，符号位扩展）
    if (ay_raw & 0x800) {  // 12位最高位（Bit11）为1，是负数
        ay_raw |= 0xF000;  // 高4位补1
    }


    int16_t az_raw = (az_msb  << 4) | (az_lsb & 0x0F);
    // 扩展为16位补码（12位→16位，符号位扩展）
    if (az_raw & 0x800) {  // 12位最高位（Bit11）为1，是负数
        az_raw |= 0xF000;  // 高4位补1
    }

    

    
    // ESP_LOGI(TAG_Da218e, "DA218E 加速度原始数据: ax=%d, ay=%d, az=%d", ax_raw, ay_raw, az_raw);
    
    // 转换为实际加速度值（g），根据所选量程使用相应的缩放因子
    // 加速度值（g）=（合并后的12位数据）× 灵敏度系数
    float scale_factor = 512.0f;
    // data.ax = ax_raw / scale_factor;
    // data.ay = ay_raw / scale_factor;
    // data.az = az_raw / scale_factor;

    float ax_raw_f = ax_raw / scale_factor;
    float ay_raw_f = ay_raw / scale_factor;
    float az_raw_f = az_raw / scale_factor;


    float acc = sqrt(ax_raw_f * ax_raw_f + ay_raw_f * ay_raw_f + az_raw_f * az_raw_f);
    // ESP_LOGI(TAG_Da218e, "DA218E 合成加速度aaaaaaa: %.2f g", acc);
    
    return acc; ;
    
}

void Da218e::Da218eTask(void* arg) {
    Da218e* da218e = static_cast<Da218e*>(arg);
    int count = 0;
    float acc_samples_[10] = {0.0f};
    float acc = 0.0f;
    while (true) {
        // ESP_LOGI(TAG_Da218e, "DA218E ");
        // ESP_LOGI(TAG_Da218e, "DA218E ");
        // ESP_LOGI(TAG_Da218e, "DA218E ");
        
        Da218e::Data data;
        if (1) {
            da218e->ReadData();
            // ESP_LOGI(TAG_Da218e, "DA218E 数据: ax=%.2f g, ay=%.2f g, az=%.2f g",
            //          data.ax, data.ay, data.az);

            //输出合成加速度
            //  acc = sqrt(data.ax * data.ax + data.ay * data.ay + data.az * data.az);
            // ESP_LOGI(TAG_Da218e, "DA218E 合成加速度ggg: %.2f g", acc);

        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // 每秒检查一次数据
        //通过数组记录每一次的数据
        acc_samples_[count] = acc;
        count++;
        
        
        if (count >= 10) {
            count = 0;
            ESP_LOGE(TAG_Da218e, "DA218E 1111111111111111111111111111111111111111");
            //一次性打印数组所以数据
            ESP_LOGE(TAG_Da218e, "DA218E 数组数据: %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f",
                     acc_samples_[0], acc_samples_[1], acc_samples_[2], acc_samples_[3], acc_samples_[4],
                     acc_samples_[5], acc_samples_[6], acc_samples_[7], acc_samples_[8], acc_samples_[9]);
            
            
        }
    }
}

void Da218e::StartDa218eTask() {
    if (da218e_task_handle_ == nullptr) {
        xTaskCreate(Da218eTask, "Da218eTask", 1024 * 3, this, configMAX_PRIORITIES - 1,
                    &da218e_task_handle_);
    }
}

bool Da218e::SetRange(Da218eRange range) {
    // 检查量程是否有效
    if (range < Da218eRange::RANGE_2G || range > Da218eRange::RANGE_8G) {
        ESP_LOGI(TAG_Da218e, "DA218E 无效的量程设置");
        return false;
    }
    
    // 写入量程配置寄存器（WriteReg返回void，不检查返回值）
    WriteReg(DA218E_REG_RANGE, static_cast<uint8_t>(range));
    
    // 更新当前量程
    current_range_ = range;
    
    // 记录量程设置信息
    const char* range_str = nullptr;
    switch (range) {
        case Da218eRange::RANGE_2G:
            range_str = "±2g";
            break;
        case Da218eRange::RANGE_4G:
            range_str = "±4g";
            break;
        case Da218eRange::RANGE_8G:
            range_str = "±8g";
            break;
        default:
            range_str = "未知";
            break;
    }
    
    ESP_LOGI(TAG_Da218e, "DA218E 量程设置成功: %s", range_str);
    return true;
}

Da218eRange Da218e::GetRange() const {
    return current_range_;
}

float Da218e::GetCurrentScaleFactor() const {
    switch (current_range_) {
        case Da218eRange::RANGE_2G:
            return DA218E_SCALE_FACTOR_2G;
        case Da218eRange::RANGE_4G:
            return DA218E_SCALE_FACTOR_4G;
        case Da218eRange::RANGE_8G:
            return DA218E_SCALE_FACTOR_8G;
        default:
            return DA218E_SCALE_FACTOR_2G; // 默认返回±2g的缩放因子
    }
}