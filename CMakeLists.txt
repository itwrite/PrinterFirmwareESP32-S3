
cmake_minimum_required(VERSION 3.16)

# AirPrint is an opt-in build surface. The default IJPD image must not even
# compile the new IPP core until its ESP-IDF transport is intentionally enabled.
option(AIRPRINT_ENABLE "Include the opt-in AirPrint components" OFF)
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
