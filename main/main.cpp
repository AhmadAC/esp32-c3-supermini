#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
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
#include "driver/ledc.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/err.h"

static const char *TAG = "SUPERMINI";

httpd_handle_t server = NULL;
static int64_t disconnect_time = 0;
static bool ap_fallback_active = false;

// ============================================================================
// MOTOR CONTROL GLOBALS & TASK
// ============================================================================
#define MOTOR_PIN_A 6
#define MOTOR_PIN_B 5

static float target_throttle = 0.0f;
static float current_throttle = 0.0f;
static bool breeze_mode = false;
static float loop_dir = 1.0f;

void init_motor_pwm(void) {
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_10_BIT; // 0 to 1023
    timer_cfg.timer_num = LEDC_TIMER_0;
    timer_cfg.freq_hz = 1000;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg_a = {};
    ch_cfg_a.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg_a.channel = LEDC_CHANNEL_0;
    ch_cfg_a.timer_sel = LEDC_TIMER_0;
    ch_cfg_a.intr_type = LEDC_INTR_DISABLE;
    ch_cfg_a.gpio_num = MOTOR_PIN_A;
    ch_cfg_a.duty = 0;
    ch_cfg_a.hpoint = 0;
    ledc_channel_config(&ch_cfg_a);

    ledc_channel_config_t ch_cfg_b = {};
    ch_cfg_b.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg_b.channel = LEDC_CHANNEL_1;
    ch_cfg_b.timer_sel = LEDC_TIMER_0;
    ch_cfg_b.intr_type = LEDC_INTR_DISABLE;
    ch_cfg_b.gpio_num = MOTOR_PIN_B;
    ch_cfg_b.duty = 0;
    ch_cfg_b.hpoint = 0;
    ledc_channel_config(&ch_cfg_b);
}

void motor_control_task(void *pvParameter) {
    const float step = 5.0f; 
    while (1) {
        if (breeze_mode) {
            float nxt = current_throttle + (loop_dir * step);
            if (nxt >= 100.0f || nxt <= 30.0f) {
                loop_dir *= -1.0f;
            }
            if (nxt > 100.0f) nxt = 100.0f;
            if (nxt < 30.0f) nxt = 30.0f;
            current_throttle = nxt;
        } else {
            float diff = target_throttle - current_throttle;
            if (fabs(diff) < step) {
                current_throttle = target_throttle;
            } else {
                current_throttle += (diff > 0) ? step : -step;
            }
        }

        uint32_t duty = (uint32_t)(fabs(current_throttle) / 100.0f * 1023.0f);
        if (duty > 1023) duty = 1023;

        if (current_throttle > 0.1f) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        } else if (current_throttle < -0.1f) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        } else {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        }

        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

// ============================================================================
// CONSOLE INPUT TASK
// ============================================================================
void console_task(void *pvParameters) {
    ESP_LOGI(TAG, "Console task started. Waiting for inputs...");
    while (1) {
        int c = getchar();
        if (c != EOF) {
            if (c == 0x03) { 
                ESP_LOGI(TAG, "Interrupt received (Ctrl+C)");
            } else if (c == 0x04) { 
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
// CAPTIVE PORTAL DNS TASK
// ============================================================================
void dns_server_task(void *pvParameters) {
    struct sockaddr_in serv_addr;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(53);
    
    if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    char rx_buffer[128];
    char tx_buffer[128];
    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);
    
    ESP_LOGI(TAG, "Captive Portal DNS Server listening on port 53");
    
    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &addr_len);
        if (len > 12) {
            if (len > (int)sizeof(tx_buffer)) len = sizeof(tx_buffer);
            memcpy(tx_buffer, rx_buffer, len);
            
            tx_buffer[2] |= 0x80; 
            tx_buffer[6] = tx_buffer[4]; 
            tx_buffer[7] = tx_buffer[5];
            
            int p = len;
            if (p + 16 <= (int)sizeof(tx_buffer)) {
                tx_buffer[p++] = 0xC0; tx_buffer[p++] = 0x0C; 
                tx_buffer[p++] = 0x00; tx_buffer[p++] = 0x01; 
                tx_buffer[p++] = 0x00; tx_buffer[p++] = 0x01; 
                tx_buffer[p++] = 0x00; tx_buffer[p++] = 0x00; tx_buffer[p++] = 0x00; tx_buffer[p++] = 0x3C; 
                tx_buffer[p++] = 0x00; tx_buffer[p++] = 0x04; 
                tx_buffer[p++] = 192; tx_buffer[p++] = 168; tx_buffer[p++] = 4; tx_buffer[p++] = 1; 
                sendto(sock, tx_buffer, p, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// WI-FI EVENT HANDLER
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

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    if (ap_fallback_active) {
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
    } else {
        httpd_resp_set_hdr(req, "Location", "/app");
    }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Intercepts all OS connectivity checks seamlessly (silences "URI not found" warnings)
static esp_err_t captive_portal_wildcard_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    if (ap_fallback_active) {
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
    } else {
        httpd_resp_set_hdr(req, "Location", "/app");
    }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t setup_get_handler(httpd_req_t *req) {
    const char* setup_html = R"raw_html(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>ESP Setup</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
    :root { --primary: #0ea5e9; --bg: #0f172a; --card: #1e293b; --text: #f1f5f9; }
    body { font-family: -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 15px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; margin:0;}
    .container { width: 100%; max-width: 420px; }
    .card { background: var(--card); padding: 25px; border-radius: 20px; border: 1px solid #334155; text-align: center; margin-top: 15px; }
    h2 { color: var(--primary); margin-top: 0; }
    input, select { width: 100%; padding: 12px; margin: 8px 0 20px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: white; box-sizing: border-box; }
    button { width: 100%; padding: 15px; border: none; border-radius: 12px; font-weight: bold; cursor: pointer; color: white; margin-top:10px; transition: transform 0.1s; }
    button:active { transform: scale(0.96); opacity: 0.9; }
    .btn-green { background: #10b981; }
    .btn-red { background: #ef4444; }
    .btn-blue { background: #3b82f6; }
    .status-bar { padding: 12px; border-radius: 10px; font-weight: bold; text-align: center; font-size: 0.85rem; border: 1px solid; text-transform: uppercase; background: #451a03; color: #fbbf24; border-color: #f59e0b; }
</style></head>
<body>
    <div class="container">
        <div class="status-bar">WIFI SETUP MODE</div>
        <div class="card">
            <h2>WiFi Setup</h2>
            <div id="status-msg" style="font-size:0.8rem;color:#64748b;margin-bottom:5px">Ready to Scan</div>
            <button class="btn-green" onclick="scan()">Scan Networks</button>
            <select id="ssid" style="margin-top:10px;"><option value="">-- Select --</option></select>
            <input type="password" id="pass" placeholder="Password">
            <button class="btn-green" onclick="save()">Save and Reboot</button>
            <button class="btn-blue" style="margin-top:15px;" onclick="location.href='/app'">Skip to Fan Control</button>
            <button class="btn-red" style="margin-top:15px;" onclick="resetData()">Factory Reset Device</button>
        </div>
    </div>
    <script>
    function scan(){
        document.getElementById('status-msg').innerText="Scanning...";
        fetch('/scan').then(r=>r.json()).then(d=>{
            const s=document.getElementById('ssid'); s.innerHTML='<option value="">-- Select --</option>';
            d.forEach(n=>{let o=document.createElement('option');o.value=n;o.innerText=n;s.appendChild(o)});
            document.getElementById('status-msg').innerText="Networks Found: " + d.length;
        }).catch(()=>{ document.getElementById('status-msg').innerText="Scan Error"; });
    }
    function save(){
        const s=document.getElementById('ssid').value, p=document.getElementById('pass').value;
        if(!s) return alert('Select SSID');
        fetch('/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ssid: s, pass: p}) })
        .then(r=>r.text()).then(t=>{ alert('Saved! Rebooting...'); });
    }
    function resetData(){
        if(confirm("Are you sure?")) fetch('/reset', { method: 'POST' }).then(() => alert('Resetting...'));
    }
    </script>
</body></html>
)raw_html";
    httpd_resp_send(req, setup_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t app_get_handler(httpd_req_t *req) {
    const char* app_html = R"raw_html(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Fan Control</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
    :root { --primary: #0ea5e9; --bg: #0f172a; --card: #1e293b; --text: #f1f5f9; }
    body { font-family: -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 15px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; margin:0;}
    .container { width: 100%; max-width: 420px; }
    .card { background: var(--card); padding: 25px; border-radius: 20px; border: 1px solid #334155; text-align: center; margin-top: 15px; }
    button { width: 100%; padding: 15px; border: none; border-radius: 12px; font-weight: bold; cursor: pointer; color: white; transition: transform 0.1s; }
    button:active { transform: scale(0.96); opacity: 0.9; }
    .btn-blue { background: #3b82f6; margin-top:10px; }
    .btn-red { background: #ef4444; }
    .btn-gray { background: #475569; }
    .status-bar { padding: 12px; border-radius: 10px; font-weight: bold; text-align: center; font-size: 0.85rem; border: 1px solid; text-transform: uppercase; background: #172554; color: #93c5fd; border-color: #3b82f6; }
    .grid-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-top: 20px; }
    input[type=range] { -webkit-appearance: none; background: transparent; width: 100%; margin: 20px 0; }
    input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; height: 24px; width: 24px; border-radius: 50%; background: var(--primary); margin-top: -10px; }
    input[type=range]::-webkit-slider-runnable-track { width: 100%; height: 4px; background: #475569; border-radius: 2px; }
</style>
<script>
    function updateThrottle(val) {
        let d = val == 0 ? "Stopped" : (val > 0 ? "Forward " + val + "%" : "Reverse " + Math.abs(val) + "%");
        document.getElementById('disp').innerText = d;
        fetch('/api/throttle?val=' + val);
    }
    function sendCmd(u, d_text, d_val) {
        fetch(u).then(() => {
            if(d_text) document.getElementById('disp').innerText = d_text;
            if(d_val !== undefined) document.getElementById('slider').value = d_val;
        });
    }
</script>
</head>
<body>
    <div class="container">
        <div class="status-bar">FAN CONTROL ONLINE</div>
        <div class="card">
            <h2 id="disp" style="font-size:1.8rem;margin:15px 0;color:white;text-shadow:0 0 10px #0ea5e9">Stopped</h2>
            <input type="range" id="slider" min="-100" max="100" value="0" step="5" oninput="updateThrottle(this.value)">
            <div class="grid-3">
                <button class="btn-gray" onclick="sendCmd('/api/throttle?val=-100','Reverse Max', -100)">Rev</button>
                <button class="btn-red" onclick="sendCmd('/api/stop','Stopped', 0)">STOP</button>
                <button class="btn-gray" onclick="sendCmd('/api/throttle?val=100','Forward Max', 100)">Fwd</button>
            </div>
            <button class="btn-blue" onclick="sendCmd('/api/loop','Breeze Mode')">Breeze Mode</button>
            <button class="btn-gray" style="margin-top:20px; background:#0f172a; border:1px solid #475569;" onclick="location.href='/setup'">Go to Wi-Fi Setup</button>
        </div>
    </div>
</body></html>
)raw_html";
    httpd_resp_send(req, app_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

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
            if (ap_count > 30) ap_count = 30;
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

static esp_err_t throttle_get_handler(httpd_req_t *req) {
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "val", param, sizeof(param)) == ESP_OK) {
            target_throttle = atof(param);
            if (target_throttle > 100.0f) target_throttle = 100.0f;
            if (target_throttle < -100.0f) target_throttle = -100.0f;
            breeze_mode = false;
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t stop_get_handler(httpd_req_t *req) {
    target_throttle = 0.0f;
    breeze_mode = false;
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t loop_get_handler(httpd_req_t *req) {
    breeze_mode = true;
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void start_webserver(void) {
    if (server == NULL) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = 16;
        config.stack_size = 8192;
        config.uri_match_fn = httpd_uri_match_wildcard; // ENABLES WILDCARD MATCHING
        
        if (httpd_start(&server, &config) == ESP_OK) {
            httpd_uri_t uri_root     = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_setup    = { .uri = "/setup", .method = HTTP_GET, .handler = setup_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_app      = { .uri = "/app", .method = HTTP_GET, .handler = app_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_scan     = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_save     = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL };
            httpd_uri_t uri_reset    = { .uri = "/reset", .method = HTTP_POST, .handler = reset_post_handler, .user_ctx = NULL };
            httpd_uri_t uri_throttle = { .uri = "/api/throttle", .method = HTTP_GET, .handler = throttle_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_stop     = { .uri = "/api/stop", .method = HTTP_GET, .handler = stop_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_loop     = { .uri = "/api/loop", .method = HTTP_GET, .handler = loop_get_handler, .user_ctx = NULL };
            httpd_uri_t uri_favicon  = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = NULL };
            
            // The Wildcard Catchall (must be registered last!)
            httpd_uri_t uri_catchall = { .uri = "/*", .method = HTTP_GET, .handler = captive_portal_wildcard_handler, .user_ctx = NULL };
            
            httpd_register_uri_handler(server, &uri_root);
            httpd_register_uri_handler(server, &uri_setup);
            httpd_register_uri_handler(server, &uri_app);
            httpd_register_uri_handler(server, &uri_scan);
            httpd_register_uri_handler(server, &uri_save);
            httpd_register_uri_handler(server, &uri_reset);
            httpd_register_uri_handler(server, &uri_throttle);
            httpd_register_uri_handler(server, &uri_stop);
            httpd_register_uri_handler(server, &uri_loop);
            httpd_register_uri_handler(server, &uri_favicon);
            
            // Register Catch-all to intercept OS checks silently
            httpd_register_uri_handler(server, &uri_catchall);
            
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

    // 2. Initialize Hardware & Tasks
    init_motor_pwm();
    xTaskCreate(motor_control_task, "motor_task", 4096, NULL, 4, NULL);
    xTaskCreate(console_task, "console_task", 4096, NULL, 5, NULL);

    // 3. Initialize Network Base
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

    // 4. Configure Fallback AP Mode parameters
    wifi_config_t ap_config = {};
    strcpy((char*)ap_config.ap.ssid, "SuperMini_Config");
    ap_config.ap.ssid_len = strlen("SuperMini_Config");
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN; 

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // 5. Retrieve saved credentials
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

    // Fix for ESP32-C3 SuperMini Wi-Fi dropout issues
    ESP_LOGI(TAG, "Configuring TX Power to 8.5 dBm to mitigate onboard antenna mismatch...");
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34)); // 34 * 0.25 dBm = 8.5 dBm

    // 6. Connect to network if credentials exist
    if (has_creds) {
        ESP_LOGI(TAG, "Connecting to saved network: %s", ssid);
        wifi_config_t sta_config = {};
        strncpy((char*)sta_config.sta.ssid, ssid, 32);
        strncpy((char*)sta_config.sta.password, pass, 64);
        
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        
        disconnect_time = esp_timer_get_time(); 
        esp_wifi_connect();
    } else {
        ESP_LOGW(TAG, "No Wi-Fi saved. Fallback AP active (Connect to 'SuperMini_Config' / 192.168.4.1)");
    }

    // 7. Start Sub-systems now that the TCP/IP network stack is up
    start_webserver();
    xTaskCreate(dns_server_task, "dns_task", 4096, NULL, 5, NULL);

    while (1) {
        // Main Loop
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}