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
- [x] Host test suite: 40/40 passing.
- [x] Standard-library AirPrint query/evidence capture tool and real-device runbook.
- [x] Flash AirPrint service to ESP32-S3 and capture one real `Get-Printer-Attributes` response.
- [x] Pass CUPS `ipptool -tv` Get-Printer-Attributes interoperability check on the probe.
- [x] ESP-IDF firmware build passing.
- [x] MQTT control adapter foundation with bounded command reassembly.
- [x] MQTT `IJCH` chunk envelope validation and host coverage.
- [x] Third-party protocol documentation consolidated under `docs/protocols/`.
- [x] AirPrint Core IPP codec, request validation, HTTP parser, opt-in ESP-IDF transport and capability response foundation.
- [x] AirPrint Bonjour TXT/UUID builder and opt-in ESP-IDF mDNS service wrapper.
- [x] Opt-in AirPrint query/Print-Job service and image/urf path to the logical Ricoh Mock; default firmware remains disabled.
- [x] IPP operation dispatcher and standard error response builder foundation.
- [x] Single-active IPP Job lifecycle model and Job query/cancel dispatch foundation.
- [x] Streaming Print-Job ingestion and backend sink boundary without document decoding.
- [x] Decoder contract and AirPrint golden-sample manifest/bundle validation tooling without format guessing.

## Current Limitations

- [ ] RAW TCP is only a byte-stream ingress until a real thermal `submitRaw()` HAL is supplied.
- [ ] BLE is custom IJPD GATT, not Bluetooth Classic SPP.
- [ ] MQTT adapter is not enabled by the default firmware variant.
- [ ] Real printhead, stepper, cutter and sensor HALs remain hardware-specific.
- [x] Add deterministic RICOH MH5440 logical CMYK8 Mock HAL and non-owning HAL bundle injection.
- [x] Inject the Ricoh CMYK8 Mock profile/HAL into the default ESP32 app variant.
- [x] Match the default preview page geometry to the Ricoh logical head width and CMYK8 interleaved row layout.
- [x] Flash the default Ricoh Mock and complete ESP32-S3 real-device CMYK8/error-path regression.
- [x] Preserve real-device Ricoh Mock evidence, including boot/network identity and terminal-path results.
- [x] Capture macOS Photos/PrintKit AirPrint discovery/query and Print-Job pcap evidence.
- [x] Capture and hash two repeated real `image/urf` Print-Job document bodies before designing the decoder.
- [x] Independently analyze the captured URF public framing, page descriptor, stride, color and pixel CRC.
- [x] Implement and golden-test the verified public Apple Raster Mono8/SW PackBits decoder subset.
- [x] Verify the opt-in Print-Job stream -> Apple Raster decoder -> RasterSink vertical slice on both real golden documents.
- [x] Freeze and host-test the synchronous RasterSink row lifetime, callback ordering, multi-page accounting, and decoder-error propagation contract.
- [x] Add deterministic host Apple Raster decoder fuzz smoke coverage for valid, truncated, mutated, and incrementally chunked input.
- [x] Add an independent decoded-output byte budget so compressed input limits cannot hide expansion limits.
- [x] Define and test direct-decoder consumed-byte behavior versus Print-Job trailing-data rejection.
- [ ] Confirm reserved fields and backend pixel contract before implementing the ESP32 decoder.
- [x] Add bounded chunked HTTP request support for probe-side system-print capture without buffering the document.
- [x] Preserve complete captured Print-Job bodies and IPP requests as golden evidence bundles.
- [x] Run an opt-in ESP32 AirPrint Print-Job against a real Apple Raster golden and the Ricoh logical Mock HAL.

## MQTT Follow-up

- [x] Add an optional ESP-IDF `esp-mqtt` adapter foundation, disabled unless `PrinterConfig.services.mqtt` is true.
- [x] Validate URI/base topic/client identity before startup; invalid MQTT configuration is rejected by the adapter.
- [x] Subscribe to `<base>/<device_id>/command` and publish events to `<base>/<device_id>/event`.
- [x] Route complete MQTT command payloads into the shared ControlPlane channel.
- [x] Reassemble fragmented MQTT messages in a bounded queue and dispatch them
  from the app tick, not the MQTT callback thread.
- [x] Define topic namespace: `<base>/<device_id>/command`, `/event`, `/job/<job_id>/chunk`.
- [x] Define bounded binary chunk envelope: `IJCH` version 1, job ID, index, total, length and payload CRC32.
- [ ] Implement command messages with `msg_id`, `cmd`, and JSON `data`.
- [x] Publish command responses/events with QoS 1 and no retained job payloads.
- [x] Add bounded chunk envelope validation with job ID, index, total chunks, byte length and CRC32.
- [x] Route accepted MQTT chunks into the same IJPD frame parser after `start_print.transport=mqtt`.
- [x] Reject MQTT startup when URI/topic configuration is incomplete.
- [ ] Add host tests for topic parsing, duplicate chunks, missing chunks and CRC errors.

## Protocol and HAL Follow-up

- [ ] Add RAW TCP integration tests using a thermal mock profile.
- [ ] Make IJPD `start_print.mode="escpos"` either fully supported or remove it from the schema.
- [ ] Add ESC/POS command validation and a real thermal HAL adapter.
- [ ] Add profile-driven paper width, DPI, color mode and service metadata validation.
- [ ] Add integration tests for every enabled/disabled service combination.
- [ ] Measure task stack high-water marks and PSRAM usage for each printer variant.
- [ ] Flash and run a repeated RAW/IJPD regression on hardware before enabling new services by default.
- [x] Disable Wi-Fi RX IRAM optimization to recover approximately 9.6 KiB of
  DIRAM while retaining the general Wi-Fi IRAM fast path.
- [ ] Measure Wi-Fi TCP throughput and latency before and after the RX IRAM
  change; revert it if the printer data path misses its target.
