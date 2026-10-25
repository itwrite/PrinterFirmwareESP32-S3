# AirPrint iOS Compatibility Run

Date: 2026-10-25

## Findings

1. The duplicate printer entries came from the AirPrint responder publishing both
   the canonical `_ipp._tcp` service and the `_universal._sub._ipp._tcp` subtype.
   They referenced the same host, port, resource path, and UUID. The service now
   publishes only one canonical `_ipp._tcp` record and emits a subtype goodbye once
   to clear records left by older firmware.
2. The print failure was caused by an advertised color capability without a matching
   decoder. The earlier firmware advertised `Color=T` and `color-supported=true`,
   but accepted only Apple Raster `SW/Mono8` pages. A color photo can use Apple
   Raster `sRGB/RGB/AdobeRGB/CMYK` 8-bit pages, which were rejected as
   `UnsupportedFormat` and aborted the Job.

## Latest Device Verification

- RGB8 Apple Raster synthetic job: HTTP `200`, IPP `successful-ok`, completed.
- CMYK8 Apple Raster synthetic job: HTTP `200`, IPP `successful-ok`, completed.
- Real golden photo `urf-photo-preview-707738/ipp_body.bin`: HTTP `200`, IPP
  `successful-ok`, Job `3` completed, `pages-completed=1`.
- `Validate-Job` for `image/urf`: HTTP `200`, IPP `successful-ok`.
- Exact `Get-Job-Attributes` for Job `3`: HTTP `200`, IPP `successful-ok`.
- `Get-Jobs` response includes the correct `job-printer-uri`.
- `_universal._sub._ipp._tcp.local` returned no record after the cleanup-enabled
  firmware restarted; `_ipp._tcp` returned one service.
- The captured macOS Print-Job metadata contains both `image/urf` and
  `application/octet-stream` document-format values. The parser now accepts this
  standards-compatible duplicate format declaration for query/validation paths as
  well as the streaming Print-Job path.

## iPhone Submission Confirmation

After the duplicate `requesting-user-name` fix was flashed, the device JobStore
reported the iPhone submission as:

- Job ID: `2`
- Job name: `照片`
- Originating user: `mobile`
- Document format: `image/urf`
- State: `completed` (`9`)
- Pages completed: `1`
- Job k-octets: `2415`
- `Get-Job-Attributes`: HTTP `200`, IPP `successful-ok`

The iPhone request therefore reached the ESP32, passed IPP metadata parsing,
decoded the document, and completed the logical Mock path. Physical output is not
expected from this firmware because the active backend is `MockRicohMh5440Hal`.

The color conversion is intentionally logical: RGB/CMYK raster is converted to
grayscale and then to the Mock contract `[C=0,M=0,Y=0,K=255-gray]`. It does not
validate real Ricoh electrical firing or physical color output.
