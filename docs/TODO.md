# Printer Firmware TODO

This file is the persistent handoff between implementation stages. Update the
checkboxes and the next-stage section whenever a stage is completed.

## Completed

- [x] IJPD control protocol over TCP 9100 and BLE GATT.
- [x] IJPD binary data protocol over TCP 9101 with Hello, ordering, CRC32 and LZ4.
- [x] Buffered receive/print/display path with PSRAM page cache.
- [x] Incremental ILI9341 preview rendering and RGB565 wire-byte correction.
- [x] Generic `PrinterConfig` and opt-in service capability flags.
- [x] Runtime service capability reporting in `get_device_info`.
- [x] Separate ESC/POS RAW TCP ingress foundation, default port 9102.
- [x] RAW job exclusivity, cancellation, idle/total timeout handling and terminal events.
- [x] Host test suite: 18/18 passing.
- [x] ESP-IDF firmware build passing.
- [x] MQTT control adapter foundation with bounded command reassembly.
- [x] MQTT `IJCH` chunk envelope validation and host coverage.
- [x] Optional IPP HTTP foundation with `Get-Printer-Attributes` and explicit `Print-Job` rejection.
- [x] IPP `Validate-Job` format gate; unsupported document formats are rejected.

## Current Limitations

- [ ] RAW TCP is only a byte-stream ingress until a real thermal `submitRaw()` HAL is supplied.
- [ ] BLE is custom IJPD GATT, not Bluetooth Classic SPP.
- [ ] MQTT adapter is not enabled by the default firmware variant.
- [ ] AirPrint IPP/HTTP service and document parsing are not implemented.
- [ ] PDF/PWG Raster/Apple Raster conversion is not implemented on-device.
- [ ] Real printhead, stepper, cutter and sensor HALs remain hardware-specific.

## Next Stage: MQTT Control Adapter

- [x] Add an optional ESP-IDF `esp-mqtt` adapter foundation, disabled unless `PrinterConfig.services.mqtt` is true.
- [x] Validate URI/base topic/client identity before startup; invalid MQTT configuration is rejected by the adapter.
- [x] Subscribe to `<base>/<device_id>/command` and publish events to `<base>/<device_id>/event`.
- [x] Route complete MQTT command payloads into the shared ControlPlane channel.
- [x] Reassemble fragmented MQTT messages in a bounded queue and dispatch them
  from the app tick, not the MQTT callback thread.
- [ ] Define topic namespace: `<base>/<device_id>/command`, `/event`, `/state`, `/job/<job_id>/chunk`.
- [x] Define bounded binary chunk envelope: `IJCH` version 1, job ID, index, total, length and payload CRC32.
- [ ] Implement command messages with `msg_id`, `cmd`, and JSON `data`.
- [ ] Publish command responses/events with QoS 1 and no retained job payloads.
- [x] Add bounded chunk envelope validation with job ID, index, total chunks, byte length and CRC32.
- [x] Route accepted MQTT chunks into the same IJPD frame parser after `start_print.transport=mqtt`.
- [x] Reject MQTT startup when URI/topic configuration is incomplete.
- [ ] Add host tests for topic parsing, duplicate chunks, missing chunks and CRC errors.

## Following Stage: AirPrint Foundation

- [ ] Add optional `_ipp._tcp` mDNS advertisement, disabled by default.
- [x] Add `esp_http_server` listener on the configured IPP port (default 631).
- [x] Implement a minimal `Get-Printer-Attributes` response with Profile model data.
- [x] Implement bounded single-page PWG Raster `Print-Job` for Mono1 profiles.
- [x] Implement `Validate-Job` rejection for unsupported document formats.
- [x] Select and document the first supported document format: PWG Raster v2 header/row layout.
- [x] Add a bounded platform-independent PWG Raster v2 header parser and host tests.
- [x] Add a bounded document upload path and translate accepted pages into the raster job backend.
- [ ] Do not advertise AirPrint until `Print-Job` and at least one document format are functional.

## Protocol and HAL Follow-up

- [ ] Add RAW TCP integration tests using a thermal mock profile.
- [ ] Add ESC/POS command validation and a real thermal HAL adapter.
- [ ] Add profile-driven paper width, DPI, color mode and service metadata validation.
- [ ] Add integration tests for every enabled/disabled service combination.
- [ ] Measure task stack high-water marks and PSRAM usage for each printer variant.
- [ ] Flash and run a repeated RAW/IJPD regression on hardware before enabling new services by default.
