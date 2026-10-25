# airprint_esp —— opt-in ESP-IDF HTTP transport for AirPrint.
# The default project excludes this component until AIRPRINT_ENABLE=ON.
idf_component_register(
    SRCS "src/ipp_http_server.cpp"
         "src/airprint_mdns_service.cpp"
         "src/airprint_probe.cpp"
    INCLUDE_DIRS "include"
      REQUIRES airprint_core core printer_hal platform_esp lwip freertos esp_timer mdns
)
