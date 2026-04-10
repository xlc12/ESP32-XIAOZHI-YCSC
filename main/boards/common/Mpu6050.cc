#include "mpu6050.h"

Mpu6050::Mpu6050(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
    // 读取WHO_AM_I寄存器，验证设备连接
    uint8_t who_am_i = ReadReg(MPU6050_REG_WHO_AM_I);
    
    if (who_am_i != MPU6050_DEFAULT_ADDR) {
        ESP_LOGI(TAG_Mpu6050, "6666666666666 MPU6050 设备 ID 错误: 0x%02X", who_am_i);
        return;
    }
    
    ESP_LOGI(TAG_Mpu6050, "6666666666666 MPU6050 设备 ID: 0x%02X", who_am_i);
    
    // 唤醒 MPU6050
    WriteReg(MPU6050_REG_PWR_MGMT_1, 0x00);
    ESP_LOGI(TAG_Mpu6050, "6666666666666 唤醒 MPU6050");
    
    // 配置加速度计范围为 ±2g
    WriteReg(MPU6050_REG_ACCEL_CONFIG, 0x00);
    ESP_LOGI(TAG_Mpu6050, "6666666666666 配置加速度计范围为 ±2g");
    
    // 配置陀螺仪范围为 ±250°/s
    WriteReg(MPU6050_REG_GYRO_CONFIG, 0x00);
    ESP_LOGI(TAG_Mpu6050, "6666666666666 配置陀螺仪范围为 ±250°/s");
    
    StartMpu6050Task();
}

Mpu6050::~Mpu6050() {
    // 清理任务
    if (mpu6050_task_handle_ != nullptr) {
        vTaskDelete(mpu6050_task_handle_);
        mpu6050_task_handle_ = nullptr;
    }
}

void Mpu6050::Read_Mpu6050(Data& data) {
    uint8_t buffer[14];
    ReadRegs(MPU6050_REG_ACCEL_XOUT_H, buffer, 14);
    
    //加速度
    int16_t ax_raw = (buffer[0] << 8) | buffer[1];
    int16_t ay_raw = (buffer[2] << 8) | buffer[3];
    int16_t az_raw = (buffer[4] << 8) | buffer[5];
    ESP_LOGI(TAG_Mpu6050, "6666666666666 加速度原始数据: ax=%d, ay=%d, az=%d", ax_raw, ay_raw, az_raw);
    data.ax = ax_raw / MPU6050_ACCEL_SCALE_FACTOR;
    data.ay = ay_raw / MPU6050_ACCEL_SCALE_FACTOR;
    data.az = az_raw / MPU6050_ACCEL_SCALE_FACTOR;
    
    //跳过温度数据（6-7字节）
    
    //陀螺仪
    int16_t gx_raw = (buffer[8] << 8) | buffer[9];
    int16_t gy_raw = (buffer[10] << 8) | buffer[11];
    int16_t gz_raw = (buffer[12] << 8) | buffer[13];
    ESP_LOGI(TAG_Mpu6050, "6666666666666 陀螺仪原始数据: gx=%d, gy=%d, gz=%d", gx_raw, gy_raw, gz_raw);
    data.gx = gx_raw / MPU6050_GYRO_SCALE_FACTOR;
    data.gy = gy_raw / MPU6050_GYRO_SCALE_FACTOR;
    data.gz = gz_raw / MPU6050_GYRO_SCALE_FACTOR;
}

void Mpu6050::Mpu6050Task(void* arg) {
    Mpu6050* mpu6050 = static_cast<Mpu6050*>(arg);
    while (true) {
        Mpu6050::Data data;
        mpu6050->Read_Mpu6050(data);
        // ESP_LOGI(TAG_Mpu6050, "MPU6050 数据: ax=%.2f g, ay=%.2f g, az=%.2f g, gx=%.2f °/s, gy=%.2f °/s, gz=%.2f °/s",
                //  data.ax, data.ay, data.az, data.gx, data.gy, data.gz);
        vTaskDelay(pdMS_TO_TICKS(000));
    }
}

void Mpu6050::StartMpu6050Task() {
    if (mpu6050_task_handle_ == nullptr) {
        xTaskCreate(Mpu6050Task, "Mpu6050Task", 1024 * 3, this, configMAX_PRIORITIES - 1,
                    &mpu6050_task_handle_);
    }
}