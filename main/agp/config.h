#ifndef _AGP_CONFIG_H_
#define _AGP_CONFIG_H_

#include <string>

#include "settings.h"

#define AGP_SETTINGS_NS "agp"

#define AGP_CLAIM_CODE_PATH "things/claim-code"
#define AGP_FIRMWARE_CHECK_PATH "firmware/check"
#define AGP_AUDIO_WS_PATH "ws/thing/audio"

// The wifi provisioning portal can override the compiled-in base URL.
inline std::string AgpBaseUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

inline std::string AgpUrl(const char* path) {
    std::string url = AgpBaseUrl();
    if (url.empty()) {
        return url;
    }
    if (url.back() != '/') {
        url += '/';
    }
    return url + path;
}

inline std::string AgpWsUrl() {
    std::string url = AgpUrl(AGP_AUDIO_WS_PATH);
    if (url.rfind("https://", 0) == 0) {
        url.replace(0, 5, "wss");
    } else if (url.rfind("http://", 0) == 0) {
        url.replace(0, 4, "ws");
    }
    return url;
}

#endif  // _AGP_CONFIG_H_
