#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_timer.h"

static const char *TAG = "SUPERMINI";

httpd_handle_t server = NULL;
static int64_t disconnect_time = 0;
static bool ap_fallback_active = false;

// ============================================================================
// CONSOLE INPUT TASK (Handles Ctrl+C and Ctrl+D)
// ============================================================================
void console_task(void *pvParameters) {
    ESP_LOGI(TAG, "Console task started. Waiting for inputs...");
    while (1) {
        int c = getchar();
        if (c != EOF) {
            if (c == 0x03) { // Ctrl+C (ETX)
                ESP_LOGI(TAG, "Interrupt received (Ctrl+C)");
                
            } else if (c == 0x04) { // Ctrl+D (EOT)
                ESP_LOGI(TAG, "Soft Reboot received (Ctrl+D). Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(100)); 
                esp_restart();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ============================================================================
// WI-FI EVENT HANDLER (Auto Reconnect & AP Fallback)
// ============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (disconnect_time == 0) {
            disconnect_time = esp_timer_get_time();
        }
        int64_t elapsed_sec = (esp_timer_get_time() - disconnect_time) / 1000000;
        
        if (elapsed_sec >= 60) {
            if (!ap_fallback_active) {
                ESP_LOGW(TAG, "Wi-Fi disconnected for 60s. Enabling AP fallback...");
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                ap_fallback_active = true;
            }
            ESP_LOGW(TAG, "Disconnected. AP Active. Retrying STA in 3s...");
        } else {
            ESP_LOGW(TAG, "Disconnected from Wi-Fi. Retrying STA in 3s...");
        }
        
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
        
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        disconnect_time = 0;
        ap_fallback_active = false;
        
        ESP_LOGI(TAG, "Locking onto Router mode. Disabling AP to prevent interference.");
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

// ============================================================================
// WEB DASHBOARD HANDLERS
// ============================================================================
static void delayed_reboot_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    const char* html = R"raw_html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>SuperMini Dashboard</title>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <style>
        body { font-family: 'Segoe UI', Arial, sans-serif; background: #eef2f3; margin: 0; padding: 20px; color: #333; }
        .container { max-width: 600px; margin: 0 auto; }
        h1 { text-align: center; color: #2c3e50; }
        .card { background: white; border-radius: 12px; padding: 25px; box-shadow: 0 4px 15px rgba(0,0,0,0.05); }
        label { display: block; margin-top: 15px; margin-bottom: 5px; font-weight: bold; }
        button { background: #3498db; color: white; border: none; padding: 12px 20px; border-radius: 6px; cursor: pointer; width: 100%; margin-top: 15px; font-size: 16px; }
        button:hover { background: #2980b9; }
        select, input[type=password] { width: 100%; padding: 10px; box-sizing: border-box; border: 1px solid #ccc; border-radius: 6px; }
        #status { font-weight: bold; color: #16a085; text-align: center; margin-top: 10px; }
    </style>
</head>
<body>
<div class='container'>
    <h1>SuperMini Config</h1>
    <div class='card'>
        <button onclick='scan()'>Scan Wi-Fi Networks</button>
        <p id='status'></p>
        
        <label>Target SSID:</label>
        <select id='ssid'></select>
        
        <label>Wi-Fi Password:</label>
        <input type='password' id='pass' placeholder='Enter password'>
        
        <button style='background: #27ae60;' onclick='saveWiFi()'>Save and Reboot</button>
        <button style='background: #e74c3c; margin-top: 10px;' onclick='resetData()'>Factory Reset Device</button>
    </div>
</div>
<script>
    function fetchJSON(url, bodyData) {
        return fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(bodyData) });
    }
    function scan() {
        document.getElementById('status').innerText = 'Scanning Wi-Fi...';
        fetch('/scan').then(r => r.json()).then(d => {
            let s = document.getElementById('ssid');
            s.innerHTML = '';
            d.forEach(n => { s.innerHTML += '<option value="'+n+'">'+n+'</option>'; });
            document.getElementById('status').innerText = 'Found ' + d.length + ' network(s)';
        });
    }
    function saveWiFi() {
        let s = document.getElementById('ssid').value;
        let p = document.getElementById('pass').value;
        if(!s) return;
        fetchJSON('/save', {ssid: s, pass: p}).then(() => alert('Credentials Saved! Device Rebooting...'));
    }
    function resetData() {
        if(confirm("Are you sure you want to format NVS?")) {
            fetchJSON('/reset', {}).then(() => alert('Resetting...'));
        }
    }
</script>
</body>
</html>
)raw_html";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Reusable JSON POST extractor
static esp_err_t get_post_json(httpd_req_t *req, cJSON **json_out) {
    *json_out = NULL;
    int total_len = req->content_len;
    if (total_len >= 512 || total_len <= 0) return ESP_FAIL;
    
    char* buf = (char*)malloc(total_len + 1);
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';
    
    *json_out = cJSON_Parse(buf);
    free(buf);
    return (*json_out == NULL) ? ESP_FAIL : ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    esp_wifi_scan_stop();
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;
    
    cJSON *root = cJSON_CreateArray();
    if (esp_wifi_scan_start(&scan_config, true) == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            if (ap_count > 30) ap_count = 30; // Max out at 30 to save heap
            wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (ap_info != NULL) {
                if (esp_wifi_scan_get_ap_records(&ap_count, ap_info) == ESP_OK) {
                    for(int i = 0; i < ap_count; i++) {
                        if (strlen((char*)ap_info[i].ssid) > 0) {
                            cJSON_AddItemToArray(root, cJSON_CreateString((char*)ap_info[i].ssid));
                        }
                    }
                }
                free(ap_info);
            }
        }
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    cJSON *json = NULL;
    if (get_post_json(req, &json) == ESP_OK) {
        cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
        cJSON *pass_item = cJSON_GetObjectItem(json, "pass");
        
        if (ssid_item && pass_item) {
            nvs_handle_t my_handle;
            if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
                nvs_set_str(my_handle, "wifi_ssid", ssid_item->valuestring);
                nvs_set_str(my_handle, "wifi_pass", pass_item->valuestring);
                nvs_commit(my_handle);
                nvs_close(my_handle);
                ESP_LOGI(TAG, "Saved target network: %s", ssid_item->valuestring);
            }
            xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
        }
        cJSON_Delete(json);
        httpd_resp_sendstr(req, "OK");
    }
    return ESP_OK;
}

static esp_err_t reset_post_handler(httpd_req_t *req) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGW(TAG, "Factory reset ordered.");
    }
    xTaskCreate(delayed_reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

void start_webserver(void) {
    if (server == NULL) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = 8;
        config.stack_size = 8192;
        
        if (httpd_start(&server, &config) == ESP_OK) {
            httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_scan  = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_save  = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
            httpd_uri_t uri_reset = { .uri = "/reset", .method = HTTP_POST, .handler = reset_post_handler, .user_ctx = NULL };
            
            httpd_register_uri_handler(server, &uri_index);
            httpd_register_uri_handler(server, &uri_scan);
            httpd_register_uri_handler(server, &uri_save);
            httpd_register_uri_handler(server, &uri_reset);
            
            ESP_LOGI(TAG, "Web Server started on port %d", config.server_port);
        }
    }
}

// ============================================================================
// MAIN APPLICATION START
// ============================================================================
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C3 SuperMini Booting up!");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. Initialize Network Base
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    // 3. Configure Fallback AP Mode parameters
    wifi_config_t ap_config = {};
    strcpy((char*)ap_config.ap.ssid, "SuperMini_Config");
    ap_config.ap.ssid_len = strlen("SuperMini_Config");
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN; 

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // 4. Retrieve saved credentials
    nvs_handle_t my_handle;
    bool has_creds = false;
    char ssid[33] = {0}; 
    char pass[65] = {0};

    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t s_len = sizeof(ssid); 
        size_t p_len = sizeof(pass);
        if (nvs_get_str(my_handle, "wifi_ssid", ssid, &s_len) == ESP_OK &&
            nvs_get_str(my_handle, "wifi_pass", pass, &p_len) == ESP_OK) {
            has_creds = true;
        }
        nvs_close(my_handle);
    }

    if (has_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ap_fallback_active = false;
    } else {
        ap_fallback_active = true;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // 5. Connect to network if credentials exist
    if (has_creds) {
        ESP_LOGI(TAG, "Connecting to saved network: %s", ssid);
        wifi_config_t sta_config = {};
        strncpy((char*)sta_config.sta.ssid, ssid, 32);
        strncpy((char*)sta_config.sta.password, pass, 64);
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        
        disconnect_time = esp_timer_get_time(); 
        esp_wifi_connect();
    } else {
        ESP_LOGW(TAG, "No Wi-Fi saved. Fallback AP active (Connect to 'SuperMini_Config' / 192.168.4.1)");
    }

    // 6. Start Sub-systems
    start_webserver();
    xTaskCreate(console_task, "console_task", 4096, NULL, 5, NULL);

    while (1) {
        // Main Loop
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}