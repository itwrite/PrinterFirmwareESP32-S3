#!/usr/bin/env python3
"""Extract raw IPP Print-Job evidence from a classic Ethernet pcap.

The extractor reassembles IPv4/TCP payloads, decodes HTTP Content-Length or
chunked framing, and finds the IPP end-of-attributes boundary. It deliberately
does not interpret Apple Raster/URF fields or pixel bytes.
"""

import argparse
import hashlib
import ipaddress
import json
import pathlib
import struct
import sys


PCAP_GLOBAL_HEADER = 24
PCAP_PACKET_HEADER = 16
ETHERNET_HEADER = 14
IPV4_PROTOCOL_TCP = 6
IPP_HEADER_BYTES = 8
IPP_END_OF_ATTRIBUTES = 0x03
IPP_GROUP_TAGS = set(range(0x01, 0x08))


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_pcap(path: pathlib.Path):
    data = path.read_bytes()
    if len(data) < PCAP_GLOBAL_HEADER:
        raise ValueError("pcap is shorter than its global header")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0xA1B2C3D4:
        endian = "<"
    elif magic == 0xD4C3B2A1:
        endian = ">"
    else:
        raise ValueError("unsupported pcap byte order or file format")
    link_type = struct.unpack_from(endian + "I", data, 20)[0]
    if link_type != 1:
        raise ValueError("only Ethernet link type is supported")

    offset = PCAP_GLOBAL_HEADER
    while offset + PCAP_PACKET_HEADER <= len(data):
        ts_sec, ts_usec, captured, original = struct.unpack_from(
            endian + "IIII", data, offset
        )
        offset += PCAP_PACKET_HEADER
        if offset + captured > len(data):
            raise ValueError("pcap packet is truncated")
        packet = data[offset:offset + captured]
        offset += captured
        yield ts_sec, ts_usec, packet


def tcp_payload(packet: bytes):
    if len(packet) < ETHERNET_HEADER + 20:
        return None
    if struct.unpack_from(">H", packet, 12)[0] != 0x0800:
        return None
    ip = packet[ETHERNET_HEADER:]
    ip_header_bytes = (ip[0] & 0x0F) * 4
    if len(ip) < ip_header_bytes + 20 or ip[9] != IPV4_PROTOCOL_TCP:
        return None
    source = str(ipaddress.IPv4Address(ip[12:16]))
    target = str(ipaddress.IPv4Address(ip[16:20]))
    tcp = ip[ip_header_bytes:]
    source_port, target_port, sequence, _ack, offset_flags = struct.unpack_from(
        ">HHIIH", tcp, 0
    )
    tcp_header_bytes = ((offset_flags >> 12) & 0x0F) * 4
    if len(tcp) < tcp_header_bytes:
        return None
    return (source, source_port, target, target_port), sequence, tcp[tcp_header_bytes:]


def reassemble(segments: list[tuple[int, bytes]]) -> bytes:
    if not segments:
        return b""
    output = bytearray()
    next_sequence = None
    for sequence, payload in sorted(segments, key=lambda item: item[0]):
        if next_sequence is None:
            next_sequence = sequence
        if sequence > next_sequence:
            raise ValueError("tcp stream has a sequence gap")
        skip = max(0, next_sequence - sequence)
        if skip < len(payload):
            output.extend(payload[skip:])
            next_sequence = sequence + len(payload)
    return bytes(output)


def split_http(data: bytes) -> tuple[dict[str, str], bytes]:
    marker = b"\r\n\r\n"
    separator = data.find(marker)
    if separator < 0:
        raise ValueError("HTTP headers are incomplete")
    lines = data[:separator].split(b"\r\n")
    if not lines or not lines[0].startswith(b"POST "):
        raise ValueError("stream is not an HTTP POST request")
    headers = {"": lines[0].decode("iso-8859-1")}
    for line in lines[1:]:
        if b":" not in line:
            raise ValueError("malformed HTTP header")
        key, value = line.split(b":", 1)
        headers[key.decode("ascii").lower()] = value.strip().decode("iso-8859-1")
    return headers, data[separator + len(marker):]


def decode_chunked(data: bytes) -> tuple[bytes, int]:
    output = bytearray()
    offset = 0
    while True:
        line_end = data.find(b"\r\n", offset)
        if line_end < 0:
            raise ValueError("chunk size line is incomplete")
        line = data[offset:line_end].split(b";", 1)[0].strip()
        if not line:
            raise ValueError("empty chunk size")
        try:
            size = int(line, 16)
        except ValueError as error:
            raise ValueError("invalid chunk size") from error
        offset = line_end + 2
        if size == 0:
            while True:
                trailer_end = data.find(b"\r\n", offset)
                if trailer_end < 0:
                    raise ValueError("chunk trailer is incomplete")
                if trailer_end == offset:
                    return bytes(output), trailer_end + 2
                offset = trailer_end + 2
        if offset + size + 2 > len(data):
            raise ValueError("chunk payload is incomplete")
        output.extend(data[offset:offset + size])
        offset += size
        if data[offset:offset + 2] != b"\r\n":
            raise ValueError("chunk payload has no CRLF terminator")
        offset += 2


def http_body(headers: dict[str, str], payload: bytes) -> tuple[bytes, int, bool]:
    transfer = headers.get("transfer-encoding", "").lower()
    if transfer == "chunked":
        body, consumed = decode_chunked(payload)
        return body, consumed, True
    if "content-length" not in headers:
        raise ValueError("HTTP request has neither Content-Length nor chunked encoding")
    length = int(headers["content-length"])
    if length > len(payload):
        raise ValueError("HTTP body is incomplete")
    return payload[:length], length, False


def ipp_attribute_end(body: bytes) -> int:
    if len(body) < IPP_HEADER_BYTES:
        raise ValueError("IPP body is shorter than its header")
    offset = IPP_HEADER_BYTES
    while offset < len(body):
        tag = body[offset]
        offset += 1
        if tag == IPP_END_OF_ATTRIBUTES:
            return offset
        if tag in IPP_GROUP_TAGS:
            continue
        if offset + 2 > len(body):
            raise ValueError("IPP attribute name length is truncated")
        name_length = struct.unpack_from(">H", body, offset)[0]
        offset += 2 + name_length
        if offset + 2 > len(body):
            raise ValueError("IPP attribute value length is truncated")
        value_length = struct.unpack_from(">H", body, offset)[0]
        offset += 2 + value_length
        if offset > len(body):
            raise ValueError("IPP attribute value is truncated")
    raise ValueError("IPP end-of-attributes tag is missing")


def ipp_operation(body: bytes) -> int:
    if len(body) < IPP_HEADER_BYTES:
        raise ValueError("IPP body is shorter than its header")
    return struct.unpack_from(">H", body, 2)[0]


def extract_flows(pcap: pathlib.Path) -> list[dict]:
    flows: dict[tuple[str, int, str, int], list[tuple[int, bytes]]] = {}
    reverse: dict[tuple[str, int, str, int], list[tuple[int, bytes]]] = {}
    for _ts_sec, _ts_usec, packet in read_pcap(pcap):
        parsed = tcp_payload(packet)
        if parsed is None:
            continue
        key, sequence, payload = parsed
        if not payload:
            continue
        if key[2] == "172.20.10.3" and key[3] == 631:
            flows.setdefault(key, []).append((sequence, payload))
        elif key[0] == "172.20.10.3" and key[1] == 631:
            reverse.setdefault(key, []).append((sequence, payload))

    results = []
    for key, segments in sorted(flows.items()):
        try:
            request = reassemble(segments)
            headers, transfer_payload = split_http(request)
            body, transfer_bytes, chunked = http_body(headers, transfer_payload)
            end = ipp_attribute_end(body)
            operation = ipp_operation(body)
        except ValueError:
            continue
        if operation != 0x0002 or end >= len(body):
            continue
        response_key = (key[2], key[3], key[0], key[1])
        response = reassemble(reverse.get(response_key, []))
        results.append({
            "key": key,
            "request": request,
            "body": body,
            "response": response,
            "headers": headers,
            "transfer_bytes": transfer_bytes,
            "chunked": chunked,
            "document_offset": end,
        })
    return results


def write_artifact(path: pathlib.Path, data: bytes) -> dict:
    path.write_bytes(data)
    return {"bytes": len(data), "path": path.name, "sha256": sha256(data)}


def extract_command(args: argparse.Namespace) -> int:
    output = pathlib.Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    flows = extract_flows(pathlib.Path(args.pcap))
    if not flows:
        raise ValueError("no complete Print-Job flows found")
    index = []
    for number, flow in enumerate(flows, 1):
        prefix = f"flow-{number:02d}"
        request_path = output / f"{prefix}.request.http"
        body_path = output / f"{prefix}.body.ipp"
        document_path = output / f"{prefix}.document.bin"
        response_path = output / f"{prefix}.response.http"
        body = flow["body"]
        document = body[flow["document_offset"]:]
        metadata = {
            "source_pcap": str(pathlib.Path(args.pcap)),
            "client": {"address": flow["key"][0], "port": flow["key"][1]},
            "server": {"address": flow["key"][2], "port": flow["key"][3]},
            "http_request_line": flow["headers"].get("", ""),
            "http_transfer_encoding": flow["headers"].get("transfer-encoding", ""),
            "chunked": flow["chunked"],
            "ipp_operation": ipp_operation(body),
            "ipp_body_bytes": len(body),
            "document_offset": flow["document_offset"],
            "document_bytes": len(document),
            "artifacts": {
                "request_http": write_artifact(request_path, flow["request"]),
                "ipp_body": write_artifact(body_path, body),
                "document": write_artifact(document_path, document),
                "response_http": write_artifact(response_path, flow["response"]),
            },
        }
        metadata_path = output / f"{prefix}.metadata.json"
        metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
        index.append({"metadata": metadata_path.name, **metadata})
    (output / "index.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n",
                                        encoding="utf-8")
    print(json.dumps(index, indent=2, sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    extract = sub.add_parser("extract", help="extract complete Print-Job flows")
    extract.add_argument("--pcap", required=True)
    extract.add_argument("--output-dir", required=True)
    extract.set_defaults(func=extract_command)
    args = parser.parse_args()
    try:
        return args.func(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
