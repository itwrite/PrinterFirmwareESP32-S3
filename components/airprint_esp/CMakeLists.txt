# airprint_esp —— ESP-IDF HTTP transport for AirPrint.
# Included by the default image; AIRPRINT_ENABLE=OFF excludes this component.
idf_component_register(
    SRCS "src/ipp_http_server.cpp"
         "src/airprint_mdns_service.cpp"
         "src/airprint_probe.cpp"
    INCLUDE_DIRS "include"
      REQUIRES airprint_core core printer_hal platform_esp lwip freertos esp_timer mdns
)
