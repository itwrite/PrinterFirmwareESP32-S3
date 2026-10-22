# MQTT Job Chunks

The MQTT control adapter handles JSON commands and status events. Large print
payloads use a separate binary topic:

```text
<base>/<device_id>/job/<job_id>/chunk
```

Each payload is a bounded `IJCH` envelope:

```text
magic[4]       "IJCH"
version u8     1
flags u8       0
job_id_len u8  1..40
reserved u8    0
index u32      little endian, zero based
total u32      little endian
length u32     little endian
crc32 u32      little endian, payload only
job_id bytes
payload bytes
```

The firmware validates this envelope in the platform-independent core, including
job ID characters, bounds, index and CRC. A job created with
`start_print.transport="mqtt"` routes accepted payloads into the existing IJPD
frame parser; the payload must therefore contain the normal 12-byte IJPD Hello
followed by the existing binary frames, split at arbitrary MQTT chunk boundaries.
