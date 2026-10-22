# AirPrint / IPP Foundation

The firmware now contains an optional IPP HTTP foundation using ESP-IDF's
`esp_http_server`. It is disabled unless `PrinterConfig.services.airprint=true`
and listens on the configured port (default 631).

Current behavior:

- `POST /ipp/print` with IPP operation `Get-Printer-Attributes` returns a
  bounded binary IPP response containing the printer name/model, URI, state and
  supported IPP version.
- `Print-Job` accepts one uncompressed PWG Raster v2 page when the selected
  Profile is Mono1 and the document width/DPI/bytes-per-line exactly match the
  Profile. The page is streamed row-by-row into the shared print/display path.
- Other document formats, color modes, compression modes, geometry and
  multi-page uploads are rejected.
- `Validate-Job` accepts only the explicitly supported placeholder format
  `application/octet-stream`; all real document formats are rejected until a
  document parser is installed.
- `_ipp._tcp` mDNS advertisement is intentionally not enabled yet.
- PDF, PWG Raster and Apple Raster parsing are not implemented yet.

The core now validates a PWG Raster v2 (`RaS2`) 1796-byte page header and row
layout, including dimensions, DPI, bit depth, bytes-per-line and compression.
This parser is not yet connected to HTTP `Print-Job`; document upload remains
disabled until the bounded page backend is connected to the shared raster job.

The device must not advertise itself as AirPrint-ready until `_ipp._tcp`
advertising and full client compatibility are verified. The current Print-Job
implementation is intentionally a restricted PWG Raster subset.
