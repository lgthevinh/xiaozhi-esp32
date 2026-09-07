#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

class Ota {
public:
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig();
    bool HasWebsocketConfig();
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    std::string GetCheckVersionUrl();

private:
    bool has_new_version_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::string GetBaseUrl();
    std::string BuildUrl(const std::string& path);
};

#endif // _OTA_H
