#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"


// add by xlc-mqtt -begin
#define ENB_MQTT_CALLBACK

#ifdef ENB_MQTT_CALLBACK
// 直接包含MQTT数据头文件
#include "app_mq_data.h"
#endif

#define ENB_MQTT_CALLBACK
// add by xlc -end


#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

// add by xlc-mqtt -begin
#ifdef ENB_MQTT_CALLBACK
    // MQTT命令处理函数声明
    bool check_mac_address(const char* mac,int cmd, int serial);
    void HandleMqttCommand(int cmd, const command_header_t* header, const void* command_data);
    void On_timer_alarm(const char* timer_id, const char* task, uint32_t timestamp);
    void PlaySuccessSound();
    void PlayFailureSound();
    void PlaySoundByType(int type);  // 或者使用枚举类型

    // 如果使用枚举，还需要定义枚举类型
    enum SoundType {
        SOUND_SUCCESS,
        SOUND_FAILURE,
        SOUND_WARNING,
        SOUND_NOTIFICATION
    };

#endif

#ifdef ENB_PALY_TTS
    // 添加这个简单的方法
    // void PlayTTS(const std::string& text);
#endif

// add by xlc -end

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);

    // add by xlc-mqtt -begin
    bool init_device_mqtt_cloud_connection(Ota& ota); // 初始化设备mqtt云连接

#ifdef ENB_MQTT_CALLBACK
    // MQTT命令处理的静态包装函数
    static void StaticHandleMqttCommand(int cmd, const command_header_t* header, const void* command_data);
    // 定时器回调的静态包装函数
    static void StaticOn_timer_alarm(const char* timer_id, const char* task, uint32_t timestamp);
#endif

    // add by xlc -end
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
