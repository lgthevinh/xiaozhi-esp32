#ifndef _AGP_PROVISION_H_
#define _AGP_PROVISION_H_

#include <string>

#include <esp_err.h>

class Provision {
public:
    bool IsProvisioned();

    // ESP_OK once the credential is stored. ESP_ERR_TIMEOUT means nobody has
    // claimed the device yet: show GetCode() and call again.
    esp_err_t Claim();

    const std::string& GetCode() const { return code_; }
    const std::string& GetThingId() const { return thing_id_; }

private:
    std::string code_;
    std::string thing_id_;

    std::string BuildPayload();
    esp_err_t HandleResponse(const std::string& body);
};

#endif  // _AGP_PROVISION_H_
