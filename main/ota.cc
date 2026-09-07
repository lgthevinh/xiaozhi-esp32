#include "ota.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>

#include <cstring>

#define TAG "Ota"

#define OTA_CHECK_PATH "firmware/check"


Ota::Ota() {
}

Ota::~Ota() {
}

std::string Ota::GetBaseUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::string Ota::BuildUrl(const std::string& path) {
    std::string url = GetBaseUrl();
    if (url.empty()) {
        return url;
    }
    if (url.back() != '/') {
        url += '/';
    }
    return url + path;
}

std::string Ota::GetCheckVersionUrl() {
    return BuildUrl(OTA_CHECK_PATH);
}

bool Ota::HasMqttConfig() {
    Settings settings("mqtt", false);
    return !settings.GetString("endpoint").empty();
}

bool Ota::HasWebsocketConfig() {
    Settings settings("websocket", false);
    return !settings.GetString("url").empty();
}

/* AGP: GET {base}firmware/check?productId&version[&thingId]
 * 200 carries the released build, 204 means this device already runs it. */
esp_err_t Ota::CheckVersion() {
    auto app_desc = esp_app_get_description();
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    has_new_version_ = false;
    firmware_version_.clear();
    firmware_url_.clear();

    if (strlen(CONFIG_PRODUCT_ID) == 0) {
        ESP_LOGE(TAG, "CONFIG_PRODUCT_ID is empty, this build cannot identify itself to AGP");
        return ESP_ERR_INVALID_ARG;
    }

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    url += "?productId=" + std::string(CONFIG_PRODUCT_ID) + "&version=" + current_version_;

    // Written by the claim flow, absent until this device is adopted. Sending it
    // is what lets AGP record the running version against the thing.
    Settings settings("agp", false);
    auto thing_id = settings.GetString("thing_id");
    if (!thing_id.empty()) {
        url += "&thingId=" + thing_id;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http->Open("GET", url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code == 204) {
        http->Close();
        ESP_LOGI(TAG, "Current is the released version");
        return ESP_OK;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    auto data = http->ReadAll();
    http->Close();

    // Response: { "version": "1.4.0", "url": "https://", "sizeBytes": 1234, "sha256": "hex" }
    // sizeBytes and sha256 are ignored: esp_ota_end() verifies the SHA-256 the
    // IDF appends to every app image, which already refuses a corrupt download.
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *firmware_url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(version) || !cJSON_IsString(firmware_url)) {
        ESP_LOGE(TAG, "Response has no version or url");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    firmware_version_ = version->valuestring;
    firmware_url_ = firmware_url->valuestring;

    // AGP answers 204 when the device already runs the release, so a 200 is the
    // whole decision here — a pinned rollback included.
    has_new_version_ = true;
    ESP_LOGI(TAG, "Released version: %s", firmware_version_.c_str());

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            heap_caps_free(buffer);
            return false;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB) or it's the last chunk
        bool is_last_chunk = (ret == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}
