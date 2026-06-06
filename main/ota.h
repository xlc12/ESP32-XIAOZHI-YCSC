#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

// add by xlc-mqtt -begin
#define ENB_OTA_LUMA_FUNC
// add by xlc -end

class Ota {
public:
    Ota();
    ~Ota();

    bool CheckVersion();
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();

// add by xlc-mqtt -begin
#if 1//ENB_OTA_LUMA_FUNC
    // 设备注册接口
    bool DeviceRegister(const std::string& mac, const std::string& uid, int deviceType);
    bool DeviceRegister(const std::string& mac, const std::string& uid, int deviceType, const std::string& url);

    // 获取注册结果的函数
    std::string GetRegisterUid() const { return register_uid_; }
    int GetRegisterStatus() const { return register_status_; }
    std::string GetRegisterDeviceCode() const { return register_device_code_; }
    std::string GetRegisterMessage() const { return register_message_; }
    std::string GetRegisterDk() const { return register_dk_; }
    std::string GetRegisterErrorMsg() const { return register_error_msg_; }
#endif
// add by xlc -end

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

// add by xlc-mqtt -begin
#if 1//ENB_OTA_LUMA_FUNC
    // 设备注册结果存储 - 确保这些声明存在
    std::string register_uid_;
    int register_status_ = 0;
    std::string register_device_code_;
    std::string register_message_;
    std::string register_dk_;
    std::string register_error_msg_;
#endif
// add by xlc -end

    bool Upgrade(const std::string& firmware_url);
    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
