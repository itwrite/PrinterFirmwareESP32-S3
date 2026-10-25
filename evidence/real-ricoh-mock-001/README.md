# Real RICOH MH5440 Mock Run

Date: 2026-10-25

## Device

- Board: ESP32-S3, chip revision v0.2
- MAC: `10:20:ba:71:d5:d0`
- STA SSID: `Xiaomi_D551`
- STA IP: `192.168.31.100`
- Flash: 16 MB
- PSRAM: 8 MB
- Firmware: default `build/ijpd_printer.bin`
- Profile: `RICOH-MH5440-MOCK`
- Logical format: CMYK8, 320 px/channel, 1280 bytes/row, reference 600 dpi

## Flash and Provisioning

- Application flashed through `/dev/cu.usbmodem101`.
- Existing NVS partition was backed up before provisioning.
- Only the NVS partition at `0x9000` (`0x6000` bytes) was rewritten with the target
  Wi-Fi credentials; the application and other partitions were not erased.
- The plaintext temporary provisioning files were created under the approved temp
  directory and are not part of this evidence bundle.

## Results

| Test | Result |
|---|---|
| Boot / PSRAM init / display init | Pass |
| STA association and DHCP | Pass, `192.168.31.100` |
| TCP control `9100` / data `9101` listeners | Pass |
| `get_device_info` | Pass; reports Ricoh logical CMYK8 Mock |
| 6400-byte CMYK8 raster, 5 rows, 1280 bytes/row | Pass; `bytes_printed=6400`, `duration_ms=56` |
| Explicit `pixel_bits=8`, `image_planes=4`, CMYK channel order | Pass; `bytes_printed=6400`, `duration_ms=54` |
| Preview page geometry | Pass; serial log `job 320x5 8b 4p layout=interleaved width_um=54100 cache=preview-only` |
| Cancel after packet 2 | Pass; final status `cancelled` |
| Corrupt final packet CRC | Pass; `PACKET_CRC_MISMATCH` |
| Data connection closed after packet 2 | Pass; `DATA_CONNECTION_LOST` |
| No data connection | Pass; Job reached `error` after timeout |
| No data connection with final-event client | Pass; `JOB_TIMEOUT` |
| Successful job after timeout cleanup | Pass; `bytes_printed=6400` |
| `feed_paper` 1000 um | Pass; `moved_um=1000` |
| `cut_paper` | Expected `UNSUPPORTED` |

## Notes

- No physical printhead or ink system was attached. These are firmware, network,
  protocol, pipeline, and logical Mock HAL tests only.
- This evidence was captured before the BLE advertising payload fix and therefore
  records the historical `adv fields rc=4` warning. The current firmware moves
  the full device name to scan response; the post-fix probe boot log confirms
  `advertising as IJPD-Printer` with no `adv fields rc=4`. BLE client connection
  and GATT control loopback remain a separate validation item.
- The first large cancel attempt caused the device to close the data socket before
  the old client drained its send buffer; `tools/send_job.py` was updated to treat
  `BrokenPipeError`/connection reset as an expected terminal-path condition.
