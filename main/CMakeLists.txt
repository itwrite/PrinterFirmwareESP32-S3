idf_component_register(SRCS "app_main.cpp" INCLUDE_DIRS "." ".."
                       REQUIRES app core printer_hal)
add_compile_options(-Werror)
if(AIRPRINT_ENABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IJPD_AIRPRINT_ENABLED=1)
endif()
