# app 组件 —— 应用组装层
# printer_app 依赖 ESP-IDF（esp_mac 等），仅设备侧构建；
# control_plane / device_info 为纯逻辑，同时参与主机侧测试（host_tests 按文件引用）。
idf_component_register(
    SRCS "src/control_plane.cpp"
         "src/device_info.cpp"
         "src/print_service.cpp"
         "src/printer_app.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core platform_esp printer_hal
)
add_compile_options(-Werror)
