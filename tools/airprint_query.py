#!/usr/bin/env python3
"""Send one raw IPP Get-Printer-Attributes request and save evidence.

This tool intentionally does not decode printer attributes or raster data. It
preserves the complete HTTP request/response and the IPP body bytes so that a
later analysis can be independently reproduced.
"""

import argparse
import datetime
import hashlib
import json
import pathlib
import socket
import struct
import sys


IPP_VERSION = {
    "1.1": (1, 1),
    "2.0": (2, 0),
}
GET_PRINTER_ATTRIBUTES = 0x000B


def be16(value: int) -> bytes:
    return struct.pack(">H", value)


def be32(value: int) -> bytes:
    return struct.pack(">I", value)


def attribute(tag: int, name: str, value: bytes) -> bytes:
    name_bytes = name.encode("ascii")
    if not name_bytes or len(name_bytes) > 0xFFFF or len(value) > 0xFFFF:
        raise ValueError("IPP attribute name or value is too large")
    return bytes((tag,)) + be16(len(name_bytes)) + name_bytes + be16(len(value)) + value


def continuation(tag: int, value: bytes) -> bytes:
    if len(value) > 0xFFFF:
        raise ValueError("IPP attribute value is too large")
    return bytes((tag,)) + b"\x00\x00" + be16(len(value)) + value


def build_ipp_query(version: str, request_id: int, printer_uri: str,
                    requested_attributes: list[str]) -> bytes:
    major, minor = IPP_VERSION[version]
    if not 0 <= request_id <= 0xFFFFFFFF:
        raise ValueError("request-id must fit in uint32")
    body = bytearray(bytes((major, minor)))
    body += be16(GET_PRINTER_ATTRIBUTES)
    body += be32(request_id)
    body.append(0x01)  # operation-attributes-tag
    body += attribute(0x47, "attributes-charset", b"utf-8")
    body += attribute(0x48, "attributes-natural-language", b"en")
    body += attribute(0x45, "printer-uri", printer_uri.encode("utf-8"))
    if requested_attributes:
        body += attribute(0x44, "requested-attributes",
                          requested_attributes[0].encode("ascii"))
        for value in requested_attributes[1:]:
            body += continuation(0x44, value.encode("ascii"))
    body.append(0x03)  # end-of-attributes-tag
    return bytes(body)


def default_host_header(host: str, port: int) -> str:
    display = host
    if ":" in host and not host.startswith("["):
        display = f"[{host}]"
    if port in (80, 443):
        return display
    return f"{display}:{port}"


def build_http_request(host_header: str, path: str, body: bytes) -> bytes:
    if not path.startswith("/"):
        raise ValueError("path must start with '/'")
    headers = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: {host_header}\r\n"
        "Content-Type: application/ipp\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode("ascii")
    return headers + body


def receive_response(sock: socket.socket) -> bytes:
    chunks = []
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def parse_http_response(data: bytes) -> tuple[int, str, dict[str, str], bytes]:
    marker = b"\r\n\r\n"
    if marker not in data:
        raise ValueError("HTTP response has no header terminator")
    header_bytes, body = data.split(marker, 1)
    lines = header_bytes.split(b"\r\n")
    if not lines or not lines[0].startswith(b"HTTP/1."):
        raise ValueError("invalid HTTP status line")
    status_parts = lines[0].split(b" ", 2)
    if len(status_parts) < 2:
        raise ValueError("invalid HTTP status line")
    try:
        status = int(status_parts[1])
    except ValueError as error:
        raise ValueError("invalid HTTP status code") from error
    reason = status_parts[2].decode("iso-8859-1") if len(status_parts) == 3 else ""
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if b":" not in line:
            raise ValueError("invalid HTTP response header")
        name, value = line.split(b":", 1)
        headers[name.decode("ascii").lower()] = value.strip().decode("iso-8859-1")
    if "transfer-encoding" in headers:
        raise ValueError("chunked HTTP response is not supported for raw evidence")
    if "content-length" in headers:
        try:
            expected = int(headers["content-length"])
        except ValueError as error:
            raise ValueError("invalid response Content-Length") from error
        if expected != len(body):
            raise ValueError("response Content-Length does not match received bytes")
    return status, reason, headers, body


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def artifact(path: str, data: bytes) -> dict[str, object]:
    return {"bytes": len(data), "path": path, "sha256": sha256(data)}


def write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def run_query(args: argparse.Namespace) -> dict[str, object]:
    output = pathlib.Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    printer_uri = args.printer_uri or f"ipp://{args.host}/ipp/print"
    requested = args.requested_attribute or ["all"]
    ipp_request = build_ipp_query(args.ipp_version, args.request_id, printer_uri, requested)
    host_header = args.host_header or default_host_header(args.host, args.port)
    http_request = build_http_request(host_header, args.path, ipp_request)

    with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        sock.sendall(http_request)
        http_response = receive_response(sock)

    status, reason, headers, ipp_response = parse_http_response(http_response)
    if len(ipp_response) < 8:
        raise ValueError("IPP response body is shorter than its 8-byte header")

    (response_major, response_minor, ipp_status, response_id) = struct.unpack(
        ">BBHI", ipp_response[:8]
    )
    files = {
        "ipp_request": ("request.ipp", ipp_request),
        "http_request": ("request.http", http_request),
        "ipp_response": ("response.ipp", ipp_response),
        "http_response": ("response.http", http_response),
    }
    artifacts = {}
    for name, (relative, data) in files.items():
        (output / relative).write_bytes(data)
        artifacts[name] = artifact(relative, data)

    metadata = {
        "captured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": args.host,
        "port": args.port,
        "path": args.path,
        "printer_uri": printer_uri,
        "ipp_version_requested": args.ipp_version,
        "request_id": args.request_id,
        "requested_attributes": requested,
        "http_status": status,
        "http_reason": reason,
        "http_headers": headers,
        "ipp_response_version": f"{response_major}.{response_minor}",
        "ipp_response_status_code": ipp_status,
        "ipp_response_request_id": response_id,
        "artifacts": artifacts,
    }
    write_json(output / "metadata.json", metadata)
    return metadata


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True,
                        help="device IP address or mDNS hostname")
    parser.add_argument("--port", type=int, default=631)
    parser.add_argument("--path", default="/ipp/print")
    parser.add_argument("--printer-uri")
    parser.add_argument("--host-header")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--request-id", type=int, default=1)
    parser.add_argument("--ipp-version", choices=sorted(IPP_VERSION), default="2.0")
    parser.add_argument("--requested-attribute", action="append")
    return parser


def main() -> int:
    try:
        metadata = run_query(build_parser().parse_args())
        print(json.dumps(metadata, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
