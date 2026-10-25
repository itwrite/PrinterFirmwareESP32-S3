# airprint_core —— platform-independent IPP/AirPrint protocol primitives.
# This component is intentionally not required by the default application yet.
idf_component_register(
    SRCS "src/ipp_codec.cpp"
         "src/ipp_attributes.cpp"
         "src/ipp_request.cpp"
         "src/http_request_parser.cpp"
         "src/airprint_capabilities.cpp"
         "src/airprint_mdns.cpp"
         "src/ipp_operations.cpp"
         "src/ipp_probe.cpp"
         "src/ipp_job.cpp"
         "src/ipp_print_job.cpp"
         "src/raster_decoder.cpp"
         "src/apple_raster_decoder.cpp"
         "src/apple_raster_print_backend.cpp"
         "src/airprint_sample.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ""
)
