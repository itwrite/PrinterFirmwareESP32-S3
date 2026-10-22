# ============================================================================
# host_tests —— 主机侧测试工程（仅系统编译器 + 自写断言，无第三方依赖）
#
# 构建：  cmake -S host_tests -B host_tests/build
#         cmake --build host_tests/build
# 运行：  ctest --test-dir host_tests/build --output-on-failure
# 说明：  -Wall -Wextra -Werror -fno-exceptions -fno-rtti
#         + AddressSanitizer/UndefinedBehaviorSanitizer（GCC/Clang）
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(ijpd_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Werror -fno-exceptions -fno-rtti)

# 清洗器选择：address（默认，含 UBSan）/ thread（验证无锁代码）/ none
set(IJPD_SANITIZER "address" CACHE STRING "sanitizer: address|thread|none")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(IJPD_SANITIZER STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    elseif(IJPD_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/core)
set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/app)

add_library(ijpd_core STATIC
    ${CORE_DIR}/src/bin_frame_parser.cpp
    ${CORE_DIR}/src/command_router.cpp
    ${CORE_DIR}/src/command_schema.cpp
    ${CORE_DIR}/src/crc32.cpp
    ${CORE_DIR}/src/errors.cpp
    ${CORE_DIR}/src/flow_control.cpp
    ${CORE_DIR}/src/frame_splitter.cpp
    ${CORE_DIR}/src/job_state_machine.cpp
    ${CORE_DIR}/src/json_lite.cpp
     ${CORE_DIR}/src/lz4_block.cpp
     ${CORE_DIR}/src/mqtt_chunk_protocol.cpp
     ${CORE_DIR}/src/pwg_raster.cpp
     ${CORE_DIR}/src/print_pipeline.cpp
    ${CORE_DIR}/src/slot_pool.cpp
    ${CORE_DIR}/src/stats.cpp
)
target_include_directories(ijpd_core PUBLIC ${CORE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Threads REQUIRED)
target_link_libraries(ijpd_core PUBLIC Threads::Threads)

add_library(ijpd_app STATIC
    ${APP_DIR}/src/control_plane.cpp
    ${APP_DIR}/src/device_info.cpp
)
target_include_directories(ijpd_app PUBLIC ${APP_DIR}/include)
target_link_libraries(ijpd_app PUBLIC ijpd_core)

# 跨语言校验工具（make_packets.py --decoder 使用；非测试）
add_executable(lz4_check lz4_check.cpp)
target_link_libraries(lz4_check PRIVATE ijpd_core)

enable_testing()

function(ijpd_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ijpd_core)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

function(ijpd_add_app_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ijpd_app)  # ijpd_app PUBLIC 传递 ijpd_core
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ijpd_add_test(test_crc32)
ijpd_add_test(test_frame_splitter)
ijpd_add_test(test_json_parse)
ijpd_add_test(test_json_writer)
ijpd_add_app_test(test_command_router)
ijpd_add_app_test(test_device_info)
ijpd_add_app_test(test_control_plane)
ijpd_add_test(test_lz4_block)
ijpd_add_test(test_bin_frame_parser)
ijpd_add_test(test_mqtt_chunk_protocol)
ijpd_add_test(test_pwg_raster)
ijpd_add_test(test_slot_pool)
ijpd_add_test(test_flow_control)
ijpd_add_test(test_job_state_machine)
ijpd_add_test(test_stats)
ijpd_add_test(test_print_pipeline)
ijpd_add_test(fuzz_json_splitter)
ijpd_add_app_test(test_middleware)
ijpd_add_test(fuzz_bin_lz4)

# Python 工具自检（含固件解压器交叉验证）
find_program(PYTHON3_EXEC python3)
if(PYTHON3_EXEC)
    add_test(NAME py_make_packets_selftest
             COMMAND ${PYTHON3_EXEC} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/make_packets.py
                     selftest --decoder $<TARGET_FILE:lz4_check>)
endif()
