# platform_esp 组件 —— ESP-IDF 平台层（Wi-Fi / TCP×2 / BLE / PSRAM / 任务 / 日志）
# 只依赖 core 与 ESP-IDF；与 app 层通过 Host 回调注入交互（无反向依赖）。
idf_component_register(
    SRCS "src/ble_control_server.cpp"
         "src/control_tcp_server.cpp"
         "src/data_tcp_server.cpp"
         "src/display_service.cpp"
         "src/mdns_broadcast.cpp"
          "src/provisioning_portal.cpp"
          "src/psram_allocator.cpp"
          "src/raw_tcp_server.cpp"
          "src/task_layout.cpp"
         "src/wifi_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core bt esp_wifi esp_event esp_netif nvs_flash lwip esp_timer esp_hw_support esp_psram mdns esp_lcd esp_lcd_ili9341 driver
)
add_compile_options(-Werror)
