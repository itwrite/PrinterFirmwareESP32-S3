# AirPrint Job Query Run

Date: 2026-10-25

- Firmware: latest opt-in `AIRPRINT_ENABLE=ON` image
- Device: `192.168.31.100`, MAC `10:20:ba:71:d5:d0`
- Service: TCP `631`, `_ipp._tcp.local`
- Submission: real golden `urf-photo-preview-707738/ipp_body.bin`
- Print-Job result: HTTP `200 OK`, IPP `successful-ok`
- Job list result: `Get-Jobs` returned Job `1`/`2`, state `completed` (`9`)
- Exact `job-uri` selector: HTTP `200 OK`, IPP `successful-ok`, Job attributes returned
- `job-id=1` selector: HTTP `200 OK`, IPP `successful-ok`, Job attributes returned
- Mismatched `job-uri` selector: HTTP `200 OK`, IPP `client-error-not-found`

The parser now accepts job-scoped operations with `job-uri` or `job-id` without
requiring a `printer-uri`. The dispatcher validates both selectors against the
shared completed JobStore. This covers the post-`Print-Job` query path used by
AirPrint clients; the standard CUPS `get-job-attributes.test` template remains
unsuitable because it sends the printer URI as `job-uri`.
