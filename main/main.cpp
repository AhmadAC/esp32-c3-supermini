#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "SUPERMINI";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C3 SuperMini Booting up in C++!");

    while (1) {
        ESP_LOGI(TAG, "Running main loop...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}