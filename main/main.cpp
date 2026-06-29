#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "SUPERMINI";

void console_task(void *pvParameters) {
    ESP_LOGI(TAG, "Console task started. Waiting for inputs...");
    
    while (1) {
        int c = getchar();
        
        if (c != EOF) {
            if (c == 0x03) { // 3 is the ASCII hex code for Ctrl+C (ETX)
                ESP_LOGI(TAG, "Interrupt received (Ctrl+C)");
                // Add any specific logic you want to run on interrupt here
                
            } else if (c == 0x04) { // 4 is the ASCII hex code for Ctrl+D (EOT)
                ESP_LOGI(TAG, "Soft Reboot received (Ctrl+D). Rebooting...");
                
                // Allow a brief moment for the log to flush to the USB interface
                vTaskDelay(pdMS_TO_TICKS(100)); 
                
                // Trigger the software reset
                esp_restart();
            }
        } else {
            // If getchar() is in non-blocking mode and returns EOF, yield 
            // for 10ms to prevent the FreeRTOS task watchdog from triggering.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C3 SuperMini Booting up in C++!");

    // Start a dedicated task to constantly read standard input.
    // This frees up the USB CDC hardware buffer and prevents host "Write timeout" errors.
    xTaskCreate(console_task, "console_task", 4096, NULL, 5, NULL);

    while (1) {
        ESP_LOGI(TAG, "Running main loop...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}