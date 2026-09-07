#include "agp/provision.h"

#include "agp/config.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>

#include <cstring>

#define TAG "AgpProvision"

bool Provision::IsProvisioned() {
    Settings settings(AGP_SETTINGS_NS, false);
    thing_id_ = settings.GetString("thing_id");
    return !thing_id_.empty() && !settings.GetString("credential").empty();
}

std::string Provision::BuildPayload() {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "macAddress", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(body, "productId", CONFIG_PRODUCT_ID);
    cJSON_AddStringToObject(body, "fwVersion", esp_app_get_description()->version);
    if (!code_.empty()) {
        cJSON_AddStringToObject(body, "code", code_.c_str());
    }

    auto payload = cJSON_PrintUnformatted(body);
    std::string json(payload);
    cJSON_free(payload);
    cJSON_Delete(body);
    return json;
}

esp_err_t Provision::HandleResponse(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* thing_id = cJSON_GetObjectItem(root, "thingId");
    cJSON* credential = cJSON_GetObjectItem(root, "credential");
    if (cJSON_IsString(thing_id) && cJSON_IsString(credential)) {
        Settings settings(AGP_SETTINGS_NS, true);
        settings.SetString("thing_id", thing_id->valuestring);
        settings.SetString("credential", credential->valuestring);
        thing_id_ = thing_id->valuestring;
        code_.clear();
        ESP_LOGI(TAG, "Claimed as thing %s", thing_id_.c_str());
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsString(code)) {
        code_ = code->valuestring;
        ESP_LOGI(TAG, "Claim code issued: %s", code_.c_str());
        cJSON_Delete(root);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGE(TAG, "Response has neither a claim code nor a credential");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
}

/* AGP: POST {base}things/claim-code
 * No code in the body asks for one, echoing it back polls that claim.
 * 202 means issued but nobody has claimed it yet. */
esp_err_t Provision::Claim() {
    if (strlen(CONFIG_PRODUCT_ID) == 0) {
        ESP_LOGE(TAG, "CONFIG_PRODUCT_ID is empty, this build cannot identify itself to AGP");
        return ESP_ERR_INVALID_ARG;
    }

    std::string url = AgpUrl(AGP_CLAIM_CODE_PATH);
    if (url.empty()) {
        ESP_LOGE(TAG, "AGP base URL is not set");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    // Without this the API's body parser leaves the DTO empty and rejects it.
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(BuildPayload());

    if (!http->Open("POST", url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        http->Close();
        return ESP_ERR_TIMEOUT;
    }
    if (status_code == 404) {
        http->Close();
        // The claim expired underneath us; drop it so the next call asks for a new one.
        ESP_LOGW(TAG, "Claim code %s is no longer pending", code_.c_str());
        code_.clear();
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to claim, status code: %d, body: %s", status_code, http->ReadAll().c_str());
        http->Close();
        return ESP_FAIL;
    }

    auto data = http->ReadAll();
    http->Close();
    return HandleResponse(data);
}
