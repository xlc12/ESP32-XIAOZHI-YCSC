#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include "settings.h"

// add by xlc-mqtt -begin
//app_mqtt5
#include "app_timer_manager.h"
#include "app_mqtt5.h"
#include "app_aes.h"
#include <esp_mac.h>
#define ENB_APPL_BROAD
// add by xlc -end

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        //add by xlc-mqtt -begin
        // 初始化设备mqtt云连接
        init_device_mqtt_cloud_connection(ota);
        // add by xlc -end

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_1_7_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed) {
                std::thread([display, progress, speed]() {
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer);
                }).detach();
            });

            if (!upgrade_success) {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start(); // Restart audio service
                board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            } else {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

// add by xlc-mqtt -begin
#ifdef ENB_MQTT_CALLBACK
// 初始化定时器管理器
    app_timer_manager_init();
    app_timer_manager_set_callback(Application::StaticOn_timer_alarm);
    ESP_LOGI(TAG, "Timer manager initialized with alarm callback");

    // 注册MQTT命令回调函数
    app_mq_data_set_command_callback(Application::StaticHandleMqttCommand);
    ESP_LOGI(TAG, "MQTT command callback registered");
#endif
// add by xlc -end

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    McpServer::GetInstance().AddCommonTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    // display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    // Print heap stats
    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    Board::GetInstance().WakeUp();
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("FaCai_1_2");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("FaCai_1_2");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("Listen_1_3");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetEmotion("Speak_1_4");

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        ESP_LOGI(TAG, "Send MCP message in main thread");
        protocol_->SendMcpMessage(payload);
    } else {
        ESP_LOGI(TAG, "Send MCP message in sub thread");
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}


// add by xlc-mqtt -begin
bool Application::init_device_mqtt_cloud_connection(Ota& ota) {

    
    // #ifdef ENB_OTA_LUMA_FUNC
    #if 1

    #if 0//old test
                        bool result = ota.DeviceRegister("84f7037f19c1", "1982827294154412034", 1);
                        if (result) {
                            app_mqtt5_set_cfg("wifi_84f7037f19c1_1982827294154412034", "84f7037f19c1", "TA2sfFcSaQT9xCFzYKjOtxGg4Z0Wc9SenduW1ZJVyYDFyuCGM6+EU8e+yQPElGCm", "1982827294154412034");
                            test_app_mqtt5();
                        }
    #else//真实
                        // 使用默认URL注册
                        #define PK_20251105_STR     "5u5PgufMY/tuZ5wl1uflbA=="// 2025-11-05//lin new pk1105_bA__
                        #define base64_key PK_20251105_STR
                        #define C_DEVICE_TYEP 1
                        char *test2_plaintext = NULL;//"wifi_84f7037f19c1_1982827294154412034";
                        const char *expected_base64_2 = "TA2sfFcSaQT9xCFzYKjOtxGg4Z0Wc9SenduW1ZJVyYDFyuCGM6+EU8e+yQPElGCm";

                        char client_id[52];//13+33+2+4=52
                        char username[13];//device_id
                        char password[65];
                        char uuid[33];//uuid
                        
                        uint8_t mac[6];
                        esp_read_mac(mac, ESP_MAC_WIFI_STA);
                        snprintf(username, sizeof(username), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);


                        //snprintf(username, sizeof(username), "%s", "84f7037f19c1");//SystemInfo::GetMacAddress().c_str());
                        //char ota_url[128];
                        memset(uuid, 0, sizeof(uuid));
                        // blufi_storage_read_app_uuid(uuid);
                        Settings settings("blufi", false);
                        std::string uuid_str = settings.GetString("blufi_app_uuid");//读APP UUID到内存中
                        strlcpy(uuid, uuid_str.c_str(), sizeof(uuid));
                        if (strlen(uuid) == 0){
                            ESP_LOGE(TAG, "uuid is empty, use default uuid\n");
                            snprintf(uuid, sizeof(uuid), "%s", "1982827294154412034");//TODOSystemInfo::GetUniqueId().c_str());
                        }
                        snprintf(client_id, sizeof(client_id), "wifi_%s_%s", username, uuid);
                        bool result = ota.DeviceRegister(username, uuid, C_DEVICE_TYEP);
                        if (result) {
                            ESP_LOGW("MAIN", "Device registration successful");
                        } else {
                            ESP_LOGE("MAIN", "Device registration failed");
                        }


                        test2_plaintext = client_id;

                        if (result) {
                            // 测试用例2：您提供的新测试用例
                                ESP_LOGD(TAG,"\n=== 测试用例2 ===\n");
                                ESP_LOGD(TAG,"明文: %s\n", test2_plaintext);
                                ESP_LOGD(TAG,"期望Base64: %s\n", expected_base64_2);
                                
                                // 加密
                                char *actual_base64_2 = java_aes_encrypt_to_base64(test2_plaintext, base64_key);
                                if (actual_base64_2) {
                                    ESP_LOGD(TAG,"实际Base64: %s\n", actual_base64_2);
                                    
                                    if (strcmp(actual_base64_2, expected_base64_2) == 0) {
                                        ESP_LOGD(TAG,"✓ 加密结果匹配！\n");
                                    } else {
                                        ESP_LOGD(TAG,"✗ 加密结果不匹配！\n");
                                    }
                                    
                                    // 解密验证
                                    #if 1
                                    char *decrypted_2 = java_aes_decrypt_from_base64(actual_base64_2, base64_key);
                                    if (decrypted_2) {
                                        ESP_LOGD(TAG,"解密验证: %s\n", decrypted_2);
                                        
                                        if (strcmp(decrypted_2, test2_plaintext) == 0) {
                                            //ESP_LOGD(TAG,"✓ 解密验证成功！\n");
                                            ESP_LOGI(TAG,"✓ 解密验证成功！\n");
                                        } else {
                                            //ESP_LOGD(TAG,"✗ 解密验证失败！\n");
                                            ESP_LOGW(TAG,"✗ 解密验证失败！\n");
                                        }
                                        strlcpy(password, actual_base64_2, sizeof(password));
                                        free(decrypted_2);
                                    }
                                    #endif
                                    
                                    free(actual_base64_2);
                                }
                        }

                        if (result) {
                            //;app_mqtt5_set_cfg("wifi_84f7037f19c1_1982827294154412034", "84f7037f19c1", "TA2sfFcSaQT9xCFzYKjOtxGg4Z0Wc9SenduW1ZJVyYDFyuCGM6+EU8e+yQPElGCm", "1982827294154412034");
                            app_mqtt5_set_cfg(client_id, username, password, uuid);
                            char ssid[32];
                            // 读取wifi ssid
                            // blufi_storage_read_wifi_ssid(ssid);
                            //get_device_status()->wifi_ssid = ssid;
                            strlcpy(get_device_status()->wifi_ssid, ssid, sizeof(get_device_status()->wifi_ssid));
                            test_app_mqtt5();
                            
                        }
    #endif
    #endif//ENB OTA_LUMA_FUNC
    
    return result;
}
// add by xlc -end



/***************************  *****************************/



// add by xlc-mqtt -begin
#ifdef ENB_MQTT_CALLBACK
// 简单版本 - 直接播放成功/失败音
void Application::PlaySuccessSound()
{
    PlaySound(Lang::Sounds::OGG_SUCCESS);
}

void Application::PlayFailureSound()
{
    PlaySound(Lang::Sounds::OGG_EXCLAMATION);
}

// 带参数版本
void Application::PlaySoundByType(int type)
{
    switch (type) {
        case SOUND_SUCCESS:
            PlaySound(Lang::Sounds::OGG_SUCCESS);
            break;
        case SOUND_FAILURE:
            PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            break;
        case SOUND_WARNING:
            PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
            break;
        case SOUND_NOTIFICATION:
            PlaySound(Lang::Sounds::OGG_POPUP);
            break;
        default:
            break;
    }
}
// 静态包装函数实现
void Application::StaticOn_timer_alarm(const char* timer_id, const char* task, uint32_t timestamp)
{
    Application::GetInstance().On_timer_alarm(timer_id, task, timestamp);
}

void Application::On_timer_alarm(const char* timer_id, const char* task, uint32_t timestamp)
{
    ESP_LOGI(TAG, "On_timer_alarm: %s, %s, %lu", timer_id, task, timestamp);
    ESP_LOGW(TAG, "🎯 [闹钟到了！！！！]TIMER ALARM: %s - %s (timestamp: %lu)", timer_id, task, timestamp);
    //;PlaySound(Lang::Sounds::P3_POPUP);
    
    if (task == NULL) {
        // PlayTTS("唐老鸭的闹钟到了！！");
    } else {
        // PlayTTS(task);
    }
    
    
    
    char* response = create_108_timer_reach_response_json(CMD_TIMER_REACH, timestamp);
    if (response) {
        app_mqtt5_dev2server(response);
        free(response);
        //;PlaySoundByType(SOUND_SUCCESS);
    }
}

// 静态包装函数实现
void Application::StaticHandleMqttCommand(int cmd, const command_header_t* header, const void* command_data)
{
    Application::GetInstance().HandleMqttCommand(cmd, header, command_data);
}

 bool Application::check_mac_address(const char* mac,int cmd, int serial) {
        //;return true;//调试用

        const char* current_device_id = app_mqtt5_get_device_id();

        if (mac == NULL || strcmp(mac, current_device_id) != 0) {
            ESP_LOGW(TAG, "MAC address mismatch: expected %s, got %s", 
                    current_device_id, mac ? mac : "NULL");
            char* failed_response = create_failed_response(cmd, serial, "MAC address mismatch");
            if (failed_response) {
                app_mqtt5_dev2app(failed_response);
                free(failed_response);
                PlaySoundByType(SOUND_FAILURE);
            }
            return false;
        }
        return true;
    }
// 实际的命令处理函数实现
void Application::HandleMqttCommand(int cmd, const command_header_t* header, const void* command_data)
{
    ESP_LOGI(TAG, "Processing MQTT command: %d", cmd);
    
    const char* current_device_id = app_mqtt5_get_device_id();
    if (current_device_id == NULL) {
        ESP_LOGE(TAG, "Invalid device ID");
        char* error_response = create_error_response(header->cmd, header->serial, "Invalid device ID");
        if (error_response) {
            app_mqtt5_dev2app(error_response);
            free(error_response);
            PlaySoundByType(SOUND_FAILURE);
        }
        return;
    }

    int ret;
   
    switch (cmd) {
        case CMD_QUERY_DEVICE_INFO:  // 101 - 查询设备信息
        {
            const device_query_cmd_t* query_cmd = (const device_query_cmd_t*)command_data;
            if (query_cmd == NULL) {
                ESP_LOGE(TAG, "Query command data is NULL");
                char* error_response = create_error_response(header->cmd, header->serial, "Invalid command data");
                if (error_response) {
                    app_mqtt5_dev2app(error_response);
                    free(error_response);
                    PlaySoundByType(SOUND_FAILURE);
                }
                break;
            }
            
            if (check_mac_address(query_cmd->mac, header->cmd, header->serial)) {
                ESP_LOGI(TAG, "Query device info for MAC: %s", query_cmd->mac);
                //;report_device_info();
                report_device_status();
                PlaySoundByType(SOUND_SUCCESS);
            }
            break;
        }
            
        case CMD_DEVICE_ONLINE:  // 103 - 删除设备
        {
            const device_query_cmd_t* online_cmd = (const device_query_cmd_t*)command_data;
            if (online_cmd == NULL) {
                ESP_LOGE(TAG, "Online command data is NULL");
                char* error_response = create_error_response(header->cmd, header->serial, "Invalid command data");
                if (error_response) {
                    app_mqtt5_dev2app(error_response);
                    free(error_response);
                    PlaySoundByType(SOUND_FAILURE);
                  
                }
                break;
            }
            
            if (check_mac_address(online_cmd->mac, header->cmd, header->serial)) {
                ESP_LOGI(TAG, "Device online notification for MAC: %s", online_cmd->mac);
                // 发送上线应答
                char* response = create_success_response(header->cmd, header->serial);
                if (response) {
                    app_mqtt5_dev2app(response);
                    free(response);
                    PlaySoundByType(SOUND_SUCCESS);

                    ESP_LOGW(TAG, "Resetting WiFi configuration");
                // Set a flag and reboot the device to enter the network configuration mode

#ifdef ENB_MQTT_CALLBACK
                    {
                        //Settings settings("wifi", true);
                        //settings.SetInt("force_ap", 1);
                        //==================================blufi==================================
                        // blufi_storage_write_has_config(false); 
                        //==========================================================================
                    }
                    //GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    // Reboot the device
                    esp_restart();
                }
 #endif               
                // 同时上报设备状态
                vTaskDelay(pdMS_TO_TICKS(100));
                //report_device_status();
            }
            break;
        }
            
        case CMD_WIFI_CONFIG:  // 104 - WiFi配置
        {
            const wifi_config_cmd_t* wifi_cmd = (const wifi_config_cmd_t*)command_data;
            if (wifi_cmd == NULL) {
                ESP_LOGE(TAG, "WiFi config command data is NULL");
                char* error_response = create_error_response(header->cmd, header->serial, "Invalid command data");
                if (error_response) {
                    app_mqtt5_dev2app(error_response);
                    free(error_response);
                    PlaySoundByType(SOUND_FAILURE);
                }
                break;
            }
            
            if (check_mac_address(wifi_cmd->mac, header->cmd, header->serial)) {
                ESP_LOGI(TAG, "WiFi config: SSID=%s, Password=%s",
                        wifi_cmd->wifi_ssid, wifi_cmd->wifi_pwd);
                int ret = 0;
#ifdef ENB_MQTT_CALLBACK
                // 这里实现WiFi配置逻辑
                // TODO: 添加实际的WiFi配置代码
                // 例如：保存WiFi配置到NVS，触发WiFi重连等
                // ret = blufi_wifi_switch_connection(wifi_cmd->wifi_ssid, wifi_cmd->wifi_pwd);
                // PlaySoundByType(SOUND_SUCCESS);
#endif
                if (0 != ret) {
                    ESP_LOGE(TAG, "blufi_wifi_switch_connection failed");
                    char* error_response = create_error_response(header->cmd, header->serial, "WiFi config failed");
                    if (error_response) {
                        app_mqtt5_dev2app(error_response);
                        free(error_response);
                        PlaySoundByType(SOUND_FAILURE);
                    }
                    break;
                }
               
                // 更新设备状态中的WiFi SSID
                strncpy(get_device_status()->wifi_ssid, wifi_cmd->wifi_ssid, 
                       sizeof(get_device_status()->wifi_ssid) - 1);
                
                char* response = create_success_response(header->cmd, header->serial);
                if (response) {
                    app_mqtt5_dev2app(response);
                    free(response);
                    
                }
                
                // 延迟上报新的设备状态
                vTaskDelay(pdMS_TO_TICKS(200));
                report_device_status();
            }
            break;
        }
            
        case CMD_OTA_UPGRADE:  // 105 - OTA升级
        {
            const ota_upgrade_cmd_t* ota_cmd = (const ota_upgrade_cmd_t*)command_data;
            if (ota_cmd == NULL) {
                ESP_LOGE(TAG, "OTA command data is NULL");
                char* error_response = create_error_response(header->cmd, header->serial, "Invalid command data");
                if (error_response) {
                    app_mqtt5_dev2app(error_response);
                    free(error_response);
                    PlaySoundByType(SOUND_FAILURE);
                }
                break;
            }
            
            ESP_LOGI(TAG, "OTA upgrade: version=%s, url=%s, silence=%d", 
                    ota_cmd->version, ota_cmd->upgrade_url, ota_cmd->is_silence);
            
            // 这里实现OTA升级逻辑
            // TODO: 添加实际的OTA升级代码
            // 例如：下载固件、校验、重启升级等
            
            // 立即响应OTA命令接收
            char* response = create_success_response(header->cmd, header->serial);
            if (response) {
                app_mqtt5_dev2app(response);
                free(response);
                PlaySoundByType(SOUND_SUCCESS);
            }
            
            // 开始OTA升级流程（在后台任务中执行）
            // start_ota_upgrade(ota_cmd->upgrade_url, ota_cmd->version);
            
            break;
        }
            
       case CMD_TIMER_SET:  // 107 - 定时器设置
        {
            const timer_set_cmd_t* timer_cmd = (const timer_set_cmd_t*)command_data;
            if (timer_cmd == NULL) {
                ESP_LOGE(TAG, "Timer set command data is NULL");
                char* error_response = create_error_response(header->cmd, header->serial, "Invalid command data");
                if (error_response) {
                    app_mqtt5_dev2app(error_response);
                    free(error_response);
                    PlaySoundByType(SOUND_FAILURE);
                }
                break;
            }
            
            ESP_LOGI(TAG, "Timer set: received %d timers", timer_cmd->timer_count);
            
            // 打印所有定时器信息
            for (size_t i = 0; i < timer_cmd->timer_count; i++) {
                const timer_item_t* timer = &timer_cmd->timers[i];
                ESP_LOGI(TAG, "Timer[%d]: id=%s, timestamp=%lu, task=%s", 
                        i, timer->id, timer->timestamp, timer->task);
            }
            
            // 这里实现定时器设置逻辑
            // TODO: 添加实际的定时器设置代码
            // 例如：创建硬件定时器、保存定时任务到NVS等
            
        // 使用定时器管理器处理定时器
            int result = app_timer_manager_add_timers(timer_cmd->timers, timer_cmd->timer_count);
            if (result == 0) {
                ESP_LOGI(TAG, "Timers added to manager successfully");
                
                // 打印当前定时器状态
                app_timer_manager_print_status();
                
                char* response = create_success_response(header->cmd, header->serial);
                if (response) {
                    app_mqtt5_dev2app(response);
                    free(response);
                    PlaySoundByType(SOUND_SUCCESS);
                }
            } else {
                ESP_LOGE(TAG, "Failed to add timers to manager");
                char* response = create_failed_response(header->cmd, header->serial, "Failed to set timers");
                if (response) {
                    app_mqtt5_dev2app(response);
                    free(response);
                    PlaySoundByType(SOUND_FAILURE);
                }
            }
            break;
        }
            
        //case CMD_TIMER_REACH:  // 108 - 定时器到达
        //    break;
            
        case CMD_DEVICE_CONTROL:  // 109 - 设备控制命令
        {
            const device_control_cmd_t* control_cmd = (const device_control_cmd_t*)command_data;
            if (control_cmd == NULL) {
                ESP_LOGE(TAG, "Control command data is NULL");
                char* error_response = create_error_response(header->cmd, header->serial, "Invalid command data");
                if (error_response) {
                    app_mqtt5_dev2app(error_response);
                    free(error_response);
                    PlaySoundByType(SOUND_FAILURE);
                }
                break;
            }
            
            if (!check_mac_address(control_cmd->mac, header->cmd, header->serial)) {
                break;
            }
            
            ESP_LOGI(TAG, "Control command: type=%d, value=%d", control_cmd->control_type, control_cmd->control_value);
            
            bool success = true;
            const char* result_msg = "success";
            
            // 执行控制命令（直接更新状态）
            switch (control_cmd->control_type) {
                case CONTROL_TYPE_VOLUME:
                    get_device_status()->volume = control_cmd->control_value;
                    ESP_LOGI(TAG, "Volume set to: %d", control_cmd->control_value);
#ifdef ENB_APPL_BROAD
                    {
                        auto &board = Board::GetInstance();
                        board.SetPowerSaveMode(false);
                        auto codec = board.GetAudioCodec();
                        codec->SetOutputVolume(control_cmd->control_value);
                        ESP_LOGW(TAG, "Set volume done");
                    }
#endif                    
                    break;
                       
                case CONTROL_TYPE_BRIGHTNESS:
                    get_device_status()->brightness = control_cmd->control_value;
                    ESP_LOGI(TAG, "Brightness set to: %d", control_cmd->control_value);
#ifdef ENB_APPL_BROAD
                    {
                        auto& board = Board::GetInstance();
                        auto backlight = board.GetBacklight();
                        backlight->SetBrightness(control_cmd->control_value, true);
                        ESP_LOGW(TAG, "Set brightness done");
                    }
#endif                    
                    break;
                        
                default:
                    ESP_LOGE(TAG, "Unknown control type: %d", control_cmd->control_type);
                    success = false;
                    result_msg = "Unknown control type";
                    break;
            }
            
            // 立即发送命令应答
            char* response;
            if (success) {
                response = create_success_response(header->cmd, header->serial);
                PlaySoundByType(SOUND_SUCCESS);
            } else {
                response = create_failed_response(header->cmd, header->serial, result_msg);
                PlaySoundByType(SOUND_FAILURE);
            }
            
            if (response) {
                app_mqtt5_dev2app(response);
                free(response);
                ESP_LOGI(TAG, "Command response sent immediately");
            }
            
            // 命令执行成功后，延迟上报状态变化
            if (success) {
                vTaskDelay(pdMS_TO_TICKS(300));  // 延迟300ms避免冲突
                report_device_status();
                ESP_LOGI(TAG, "Status reported after command");
            }
            break;
        }

        case CMD_GET_DEVICE_LIST:  // 110 - 获取设备列表
        {
            ESP_LOGI(TAG, "CMD_110: Get device list command received");
            
            // 110命令不需要检查MAC地址，直接响应成功
            char* response = create_success_response(header->cmd, header->serial);
            if (response) {
                app_mqtt5_dev2app(response);
                free(response);
                PlaySoundByType(SOUND_SUCCESS);
            }
            
            // 根据文档说明，设备需要通过CMD-102上报当前状态
            vTaskDelay(pdMS_TO_TICKS(200));
            report_device_status();
            
            ESP_LOGI(TAG, "CMD_110: Response sent and status reported");
            break;
        }

        default:
        {
            ESP_LOGW(TAG, "Unsupported command: %d", cmd);
            
            char* unsupported_response = create_failed_response(header->cmd, header->serial, "Unsupported command");
            if (unsupported_response) {
                app_mqtt5_dev2app(unsupported_response);
                free(unsupported_response);
                PlaySoundByType(SOUND_FAILURE);
            }
            break;
        }
    }
}

#endif//

 
#ifdef ENB_PALY_TTS
void Application::PlayTTS(const std::string& text) {
//     if (protocol_ == nullptr) {
//         ESP_LOGE(TAG, "Protocol is null, cannot play TTS");
//         return;
//     }
    
//     // 确保音频通道打开
//     if (!protocol_->IsAudioChannelOpened()) {
//         if (!protocol_->OpenAudioChannel()) {
//             ESP_LOGE(TAG, "Failed to open audio channel for TTS");
//             return;
//         }
//     }
    
//     protocol_->SendTextForTTS(text);
//     ESP_LOGI(TAG, "TTS request sent: %s", text.c_str());
// }
#endif
// add by xgy -end