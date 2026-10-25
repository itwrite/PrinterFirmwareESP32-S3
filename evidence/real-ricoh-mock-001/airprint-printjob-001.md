# AirPrint Print-Job Run

Date: 2026-10-25

- Firmware: opt-in `AIRPRINT_ENABLE=ON` image
- Device: `192.168.31.100`, MAC `10:20:ba:71:d5:d0`
- Service: TCP `631`, `_ipp._tcp.local`
- Input: real golden IPP body from `urf-photo-preview-707738/ipp_body.bin`
- Input size: `707738` bytes
- HTTP content type: `application/ipp`
- HTTP result: `200 OK`
- IPP result: `successful-ok`
- Job ID: `1`
- Job state: `9` (`completed`)
- Pages completed: `1`
- Job k-octets: `692`
- mDNS instance: `RICOH MH5440 Mock`
- mDNS TXT: `pdl=image/urf`, `Color=T`, `URF=V1.4,CP1,RS600,W8,DM1,IS1,MT1,FN3`

The service accepted the real Apple Raster document, decoded Mono8/SW rows, converted
each row to the explicit logical Mock contract (320 pixels x 4 CMYK8 bytes), and sent
the result to `MockRicohMh5440Hal`. This validates the opt-in host/ESP32 Mock path only;
it does not validate physical inkjet firing or Ricoh controller timing.
