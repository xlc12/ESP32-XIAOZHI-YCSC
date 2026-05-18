#ifndef _MOTOR_CONTROLLER_H_
#define _MOTOR_CONTROLLER_H_

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <cstring>

#define TAG "MotorController"

class MotorController {
private:
    gpio_num_t horiz_a_pin_;
    gpio_num_t horiz_b_pin_;
    gpio_num_t vert_a_pin_;
    gpio_num_t vert_b_pin_;
    TaskHandle_t random_motor_task_handle_;
    bool random_motor_running_;

    void SetMotorDirection(gpio_num_t pin_a, gpio_num_t pin_b, int direction) {
        if (direction == 1) {
            gpio_set_level(pin_a, 1);
            gpio_set_level(pin_b, 0);
        } else if (direction == -1) {
            gpio_set_level(pin_a, 0);
            gpio_set_level(pin_b, 1);
        } else {
            gpio_set_level(pin_a, 0);
            gpio_set_level(pin_b, 0);
        }
    }

    void StopAllMotors() {
        SetMotorDirection(horiz_a_pin_, horiz_b_pin_, 0);
        SetMotorDirection(vert_a_pin_, vert_b_pin_, 0);
    }

    static void RandomMotorTask(void* arg) {
        MotorController* controller = static_cast<MotorController*>(arg);
        
        while (controller->random_motor_running_) {
            int horiz_dir = (esp_random() % 3) - 1;
            int vert_dir = (esp_random() % 3) - 1;
            
            controller->SetMotorDirection(controller->horiz_a_pin_, controller->horiz_b_pin_, horiz_dir);
            controller->SetMotorDirection(controller->vert_a_pin_, controller->vert_b_pin_, vert_dir);
            
            int delay_ms = (esp_random() % 500) + 200;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        
        controller->StopAllMotors();
        vTaskDelete(NULL);
    }

public:
    MotorController() : horiz_a_pin_(GPIO_NUM_NC), horiz_b_pin_(GPIO_NUM_NC),
                       vert_a_pin_(GPIO_NUM_NC), vert_b_pin_(GPIO_NUM_NC),
                       random_motor_task_handle_(nullptr), random_motor_running_(false) {}

    void Init(gpio_num_t horiz_a, gpio_num_t horiz_b, gpio_num_t vert_a, gpio_num_t vert_b) {
        horiz_a_pin_ = horiz_a;
        horiz_b_pin_ = horiz_b;
        vert_a_pin_ = vert_a;
        vert_b_pin_ = vert_b;

        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << horiz_a_pin_) | (1ULL << horiz_b_pin_) |
                               (1ULL << vert_a_pin_) | (1ULL << vert_b_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        StopAllMotors();
        ESP_LOGI(TAG, "MotorController initialized");
    }

    void RunWagTailSequence(int cycles, int forward_ms, int reverse_ms) {
        ESP_LOGI(TAG, "Starting wag tail sequence: %d cycles", cycles);
        
        for (int i = 0; i < cycles; i++) {
            ESP_LOGI(TAG, "Cycle %d: Forward", i + 1);
            SetMotorDirection(horiz_a_pin_, horiz_b_pin_, 1);
            SetMotorDirection(vert_a_pin_, vert_b_pin_, 1);
            vTaskDelay(pdMS_TO_TICKS(forward_ms));
            
            ESP_LOGI(TAG, "Cycle %d: Reverse", i + 1);
            SetMotorDirection(horiz_a_pin_, horiz_b_pin_, -1);
            SetMotorDirection(vert_a_pin_, vert_b_pin_, -1);
            vTaskDelay(pdMS_TO_TICKS(reverse_ms));
        }
        
        StopAllMotors();
        ESP_LOGI(TAG, "Wag tail sequence completed");
    }

    void StartRandomMovement() {
        if (random_motor_running_) {
            return;
        }
        
        random_motor_running_ = true;
        xTaskCreate(RandomMotorTask, "random_motor", 4096, this, 5, &random_motor_task_handle_);
        ESP_LOGI(TAG, "Random motor movement started");
    }

    void StopRandomMovement() {
        if (!random_motor_running_) {
            return;
        }
        
        random_motor_running_ = false;
        if (random_motor_task_handle_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            random_motor_task_handle_ = nullptr;
        }
        ESP_LOGI(TAG, "Random motor movement stopped");
    }
};

#endif // _MOTOR_CONTROLLER_H_