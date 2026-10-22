# MQTT Adapter Foundation

MQTT is an optional control transport. It is disabled unless the selected
`PrinterConfig` sets `services.mqtt=true` and supplies both `mqtt_uri` and
`mqtt_base_topic`.

## Topics

For `base_topic = printers` and device `IJPD-ABC123`:

```text
printers/IJPD-ABC123/command
printers/IJPD-ABC123/event
```

The command payload is the same IJPD JSON request body used by TCP/BLE. The
adapter accepts a payload with or without the trailing `#`, adds it when
needed, and routes it through the shared `ControlPlane`. Responses and print
events are published to the event topic with QoS 1.

## Current Boundary

- MQTT control/event transport is implemented with ESP-IDF `esp-mqtt`.
- Incoming fragmented MQTT messages are reassembled into a bounded 2048-byte
  command buffer before entering `ControlPlane`.
- The adapter is polled from the app control tick; MQTT callbacks never call
  the non-thread-safe command router directly.
- Large raster/ESC/POS payload transfer over MQTT is not implemented yet.
- Retained job payloads, chunk indexes, CRC, duplicate detection and resume
  are next-stage work. Until then, MQTT should be used only for control and
  status messages.
