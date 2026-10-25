# core 组件（平台无关，可主机编译）
# 注意：本组件严禁包含 ESP-IDF 头文件；主机侧测试见 /host_tests。
idf_component_register(
    SRCS "src/bin_frame_parser.cpp"
         "src/command_router.cpp"
         "src/command_schema.cpp"
         "src/crc32.cpp"
         "src/errors.cpp"
         "src/flow_control.cpp"
         "src/frame_splitter.cpp"
         "src/job_state_machine.cpp"
         "src/json_lite.cpp"
         "src/lz4_block.cpp"
         "src/mqtt_chunk_protocol.cpp"
         "src/print_pipeline.cpp"
         "src/slot_pool.cpp"
         "src/stats.cpp"
    INCLUDE_DIRS "include" "../../profiles"
    REQUIRES ""
)
