/*********** Mpu6050 *************/
// 调用示例：
// #include "boards/common/mpu6050.h"

// i2c_master_bus_handle_t i2c_bus_mpu6050_;
// Mpu6050* mpu6050_;


// 注意：需要先初始化I2C总线，例如：
// void InitializeI2cBusMpu6050() {
//     i2c_master_bus_config_t i2c_bus_config = {
//         .i2c_port = I2C_NUM_1,
//         .sda_io_num = GPIO_NUM_6,
//         .scl_io_num = GPIO_NUM_7,
//         .clk_source = I2C_CLK_SRC_DEFAULT,
//         .glitch_ignore_cnt = 7,
//         .intr_priority = 0,
//         .trans_queue_depth = 0,
//         .flags = {
//             .enable_internal_pullup = true,
//         },
//     };
//     ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_mpu6050_));
// }


// InitializeI2cBusMpu6050();
// mpu6050_ = new Mpu6050(i2c_bus_mpu6050_, 0x68); // 0x68 是 MPU6050 的默认 I2C 地址


#ifndef MPU6050_H
#define MPU6050_H

#include "i2c_device.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// MPU6050 寄存器地址定义
#define MPU6050_REG_WHO_AM_I         0x75    // 设备ID寄存器
#define MPU6050_REG_PWR_MGMT_1       0x6B    // 电源管理1寄存器
#define MPU6050_REG_ACCEL_CONFIG     0x1C    // 加速度计配置寄存器
#define MPU6050_REG_GYRO_CONFIG      0x1B    // 陀螺仪配置寄存器
#define MPU6050_REG_ACCEL_XOUT_H     0x3B    // 加速度计X轴高字节寄存器

// 设备默认I2C地址
#define MPU6050_DEFAULT_ADDR         0x68

// 校准系数
#define MPU6050_ACCEL_SCALE_FACTOR   16384.0f  // ±2g 范围下的加速度计缩放因子
#define MPU6050_GYRO_SCALE_FACTOR    131.0f    // ±250°/s 范围下的陀螺仪缩放因子

class Mpu6050 : public I2cDevice {
    public:
        // 数据结构
        struct Data {
            float ax, ay, az;  // 加速度 (g)
            float gx, gy, gz;  // 角速度 (°/s)
        };
    
        Mpu6050(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
        ~Mpu6050();
    
        // 读取加速度数据
        void Read_Mpu6050(Data& data);
    
    private:
        static constexpr const char* TAG_Mpu6050 = "Mpu6050";
        TaskHandle_t mpu6050_task_handle_ = nullptr;
    
        static void Mpu6050Task(void* arg);
        void StartMpu6050Task();
};

#endif // MPU6050_H