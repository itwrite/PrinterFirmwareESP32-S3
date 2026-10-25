#!/usr/bin/env python3
"""Self-test for the raw AirPrint query evidence tool."""

import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading


ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "airprint_query.py"


class QueryServer:
    def __init__(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.request = b""
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.thread.join(timeout=5)
        self.server.close()

    def run(self):
        connection, _ = self.server.accept()
        with connection:
            chunks = []
            while b"\r\n\r\n" not in b"".join(chunks):
                chunks.append(connection.recv(4096))
            request = b"".join(chunks)
            header, body = request.split(b"\r\n\r\n", 1)
            content_length = next(
                int(line.split(b":", 1)[1].strip())
                for line in header.split(b"\r\n")
                if line.lower().startswith(b"content-length:")
            )
            while len(body) < content_length:
                body += connection.recv(4096)
            self.request = header + b"\r\n\r\n" + body[:content_length]
            response_body = bytes((2, 0, 0, 0, 0, 0, 0, 1, 3))
            response = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/ipp\r\n"
                + f"Content-Length: {len(response_body)}\r\n".encode("ascii")
                + b"Connection: close\r\n\r\n"
                + response_body
            )
            connection.sendall(response)


def main():
    with tempfile.TemporaryDirectory() as directory, QueryServer() as server:
        output = pathlib.Path(directory) / "query"
        result = subprocess.run(
            [sys.executable, str(TOOL), "--host", "127.0.0.1",
             "--port", str(server.port), "--output-dir", str(output),
             "--printer-uri", "ipp://probe.local/ipp/print",
             "--requested-attribute", "printer-name",
             "--requested-attribute", "operations-supported"],
            check=False, capture_output=True, text=True,
        )
        assert result.returncode == 0, result.stderr
        assert server.request.startswith(b"POST /ipp/print HTTP/1.1\r\n")
        _, request_body = server.request.split(b"\r\n\r\n", 1)
        assert request_body[:8] == bytes((2, 0, 0, 11, 0, 0, 0, 1))
        assert b"printer-uri" in request_body
        assert (output / "request.http").read_bytes() == server.request
        assert (output / "response.ipp").read_bytes()[8:] == b"\x03"
        metadata = json.loads((output / "metadata.json").read_text(encoding="utf-8"))
        assert metadata["http_status"] == 200
        assert metadata["ipp_response_status_code"] == 0
        assert metadata["requested_attributes"] == ["printer-name", "operations-supported"]
    print("test_airprint_query: passed")


if __name__ == "__main__":
    main()
