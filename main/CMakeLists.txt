idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_http_server esp_netif json)