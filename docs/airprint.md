# AirPrint / IPP Foundation

The firmware now contains an optional IPP HTTP foundation using ESP-IDF's
`esp_http_server`. It is disabled unless `PrinterConfig.services.airprint=true`
and listens on the configured port (default 631).

Current behavior:

- `POST /ipp/print` with IPP operation `Get-Printer-Attributes` returns a
  bounded binary IPP response containing the printer name/model, URI, state and
  supported IPP version.
- `Print-Job` and other operations are rejected with a standard IPP error.
- `_ipp._tcp` mDNS advertisement is intentionally not enabled yet.
- PDF, PWG Raster and Apple Raster parsing are not implemented yet.

The device must not advertise itself as AirPrint-ready until `Print-Job` and at
least one bounded document format are connected to the shared raster backend.
