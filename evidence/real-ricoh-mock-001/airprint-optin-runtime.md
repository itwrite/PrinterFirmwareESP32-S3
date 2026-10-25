# Opt-In AirPrint Runtime Run

Date: 2026-10-25

The explicit `AIRPRINT_ENABLE=ON` image was flashed to the same ESP32-S3 while
preserving the `Xiaomi_D551` NVS credentials.

## Runtime

- Device MAC: `10:20:ba:71:d5:d0`
- Device IP: `192.168.31.100`
- TCP/IP service: `631`
- mDNS: `_ipp._tcp.local`
- Instance: `RICOH MH5440 Mock`
- Print-Job mode: enabled
- Query response: HTTP `200`, IPP `successful-ok`
- CUPS `ipptool -tv ... get-printer-attributes.test`: passed

## Real Print-Job

- Input: `evidence/golden-samples/urf-photo-preview-707738/ipp_body.bin`
- Input bytes: `707738`
- HTTP content type: `application/ipp`
- HTTP response: `200 OK`
- IPP response: `successful-ok`
- Job ID: `1`
- Job state: `completed` (`9`)
- `pages-completed`: `1`
- `job-k-octets`: `692`
- Current IJPD status after completion: `idle`

The ESP32 opt-in service accepted the real Apple Raster/URF body, decoded the
verified Mono8/SW subset, converted rows to the logical 320-pixel CMYK8 Mock
contract, and completed the job through `MockRicohMh5440Hal`.

This proves the opt-in ESP32 logical Mock path. It does not prove physical
Ricoh printhead firing, ink behavior, or controller waveform compatibility.
