#!/usr/bin/env python3
"""Self-test for conservative Apple Raster framing analysis."""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "airprint_urf_analyze.py"


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *args],
                          check=False, capture_output=True, text=True)


def packbits_rows(rows: list[bytes], bytes_per_pixel: int) -> bytes:
    output = bytearray()
    for row in rows:
        output.append(0)  # one row
        index = 0
        while index < len(row):
            pixel = row[index:index + bytes_per_pixel]
            repeat = 1
            while (index + repeat * bytes_per_pixel < len(row) and
                   row[index + repeat * bytes_per_pixel:
                       index + (repeat + 1) * bytes_per_pixel] == pixel and
                   repeat < 128):
                repeat += 1
            if repeat >= 2:
                output.append(repeat - 1)
                output.extend(pixel)
                index += repeat * bytes_per_pixel
                continue
            literal_start = index
            index += bytes_per_pixel
            while index < len(row) and (index - literal_start) // bytes_per_pixel < 128:
                next_pixel = row[index:index + bytes_per_pixel]
                if (index + bytes_per_pixel < len(row) and
                        row[index + bytes_per_pixel:index + 2 * bytes_per_pixel] == next_pixel):
                    break
                index += bytes_per_pixel
            count = (index - literal_start) // bytes_per_pixel
            output.append(257 - count)
            output.extend(row[literal_start:index])
    return bytes(output)


def make_document(path: pathlib.Path, rows: list[bytes]) -> None:
    width = len(rows[0])
    header = bytearray(32)
    header[0] = 8
    header[1] = 0
    header[2] = 1
    header[12:16] = struct.pack(">I", width)
    header[16:20] = struct.pack(">I", len(rows))
    header[20:24] = struct.pack(">I", 100)
    path.write_bytes(b"UNIRAST\x00" + struct.pack(">I", 1) + header +
                     packbits_rows(rows, 1))


def main():
    with tempfile.TemporaryDirectory() as directory:
        base = pathlib.Path(directory)
        first = base / "first.urf"
        second = base / "second.urf"
        make_document(first, [bytes((0, 0, 0, 0)), bytes((255, 255, 255, 255))])
        make_document(second, [bytes((0, 1, 2, 3)), bytes((4, 5, 6, 7))])

        analyzed = run("analyze", "--document", str(first))
        assert analyzed.returncode == 0, analyzed.stderr
        value = json.loads(analyzed.stdout)
        page = value["pages"][0]
        assert value["sync"] == "UNIRAST\u0000"
        assert page["width_px"] == 4
        assert page["height_px"] == 2
        assert page["dpi_x"] == 100
        assert page["rows_decoded"] == 2
        assert value["trailing_bytes"] == 0

        compared = run("compare", "--document", str(first),
                       "--document", str(second))
        assert compared.returncode == 0, compared.stderr
        comparison = json.loads(compared.stdout)
        assert comparison["same_verified_fields"]["width_px"]
        assert not comparison["same_verified_fields"]["row_crc32"]
    print("test_airprint_urf_analyze: passed")


if __name__ == "__main__":
    main()
