#ifndef _AGP_CONFIG_H_
#define _AGP_CONFIG_H_

#include <string>

#include "settings.h"

#define AGP_SETTINGS_NS "agp"

#define AGP_CLAIM_CODE_PATH "things/claim-code"
#define AGP_FIRMWARE_CHECK_PATH "firmware/check"

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

#endif  // _AGP_CONFIG_H_
