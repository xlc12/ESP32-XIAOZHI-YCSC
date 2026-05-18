#ifndef DA218E_H
#define DA218E_H

#include "i2c_device.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>

// DA218E 寄存器地址定义
#define DA218E_REG_CHIPID          0x01    // 芯片ID寄存器
#define DA218E_REG_ACC_X_LSB       0x02    // X轴加速度低字节寄存器
#define DA218E_REG_NEWDATA_FLAG    0x0A    // 新数据标志寄存器
#define DA218E_REG_ACC_X_MSB       0x03    // X轴加速度高字节寄存器
#define DA218E_REG_ACC_Y_LSB       0x04    // Y轴加速度低字节寄存器
#define DA218E_REG_ACC_Y_MSB       0x05    // Y轴加速度高字节寄存器
#define DA218E_REG_ACC_Z_LSB       0x06    // Z轴加速度低字节寄存器
#define DA218E_REG_ACC_Z_MSB       0x07    // Z轴加速度高字节寄存器
#define DA218E_REG_RANGE           0x0F    // 量程配置寄存器
#define DA218E_REG_POWERMODE_BW    0x11   
#define DA218E_REG_ODR_AXIS_DISABLE       0x10   


// 设备默认I2C地址
#define DA218E_DEFAULT_ADDR        0x27

// 量程枚举类型
enum class Da218eRange {
    RANGE_2G = 0x00,  // ±2g
    RANGE_4G = 0x01,  // ±4g
    RANGE_8G = 0x02   // ±8g
};

// 校准系数（不同量程下的加速度计缩放因子）
#define DA218E_SCALE_FACTOR_2G     1024.0f  // ±2g 范围下的加速度计缩放因子
#define DA218E_SCALE_FACTOR_4G     512.0f   // ±4g 范围下的加速度计缩放因子
#define DA218E_SCALE_FACTOR_8G     256.0f   // ±8g 范围下的加速度计缩放因子

class Da218e : public I2cDevice {
    public:
        // 数据结构
        struct Data {
            float ax, ay, az;  // 加速度 (g)
        };
    
        Da218e(i2c_master_bus_handle_t i2c_bus, uint8_t addr = DA218E_DEFAULT_ADDR);
        ~Da218e();
    
        // 检查是否有新数据
        bool IsNewDataAvailable();
        
        // 读取加速度数据
        float ReadData();
        
        // 设置加速度计量程
        bool SetRange(Da218eRange range);
        
        // 获取当前加速度计量程
        Da218eRange GetRange() const;
    
    private:
        static constexpr const char* TAG_Da218e = "Da218e";
        TaskHandle_t da218e_task_handle_ = nullptr;
        Da218eRange current_range_ = Da218eRange::RANGE_2G;  // 默认量程为±2g
        
        // 根据当前量程获取缩放因子
        float GetCurrentScaleFactor() const;
    
        static void Da218eTask(void* arg);
        void StartDa218eTask();
};

#endif // DA218E_H