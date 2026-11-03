
cmake_minimum_required(VERSION 3.16)

# AirPrint is enabled in the standard firmware image. Set
# -DAIRPRINT_ENABLE=OFF to exclude the IPP core and ESP-IDF transport.
option(AIRPRINT_ENABLE "Include the AirPrint IPP components" ON)
if(NOT AIRPRINT_ENABLE)
    set(EXCLUDE_COMPONENTS "airprint_core;airprint_esp" CACHE STRING
        "ESP-IDF components excluded from the default build" FORCE)
elseif(EXCLUDE_COMPONENTS STREQUAL "airprint_core;airprint_esp" OR
       EXCLUDE_COMPONENTS STREQUAL "airprint_core")
    unset(EXCLUDE_COMPONENTS CACHE)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(AIRPRINT_ENABLE "${AIRPRINT_ENABLE}")
project(ijpd_printer)
