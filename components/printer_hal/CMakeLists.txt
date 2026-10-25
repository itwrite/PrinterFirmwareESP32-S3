# printer_hal 组件 —— 硬件抽象接口 + Mock 实现（纯 C++，设备/主机可编译）
# 真实喷头/走纸驱动在 Phase 7 依据硬件官方资料实现（当前仅有 Mock）。
idf_component_register(
    SRCS "src/printhead_hal_mock.cpp"
         "src/ricoh_mh5440_mock_hal.cpp"
         "src/stepper_hal_mock.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core
)
add_compile_options(-Werror)
