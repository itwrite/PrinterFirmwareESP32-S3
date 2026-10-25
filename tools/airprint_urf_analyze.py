#!/usr/bin/env python3
"""Conservative host-side analysis of Apple Raster/URF golden samples.

The parser implements only the public CUPS Apple Raster framing and modified
PackBits row framing. It reports observed descriptors and checksums for review;
it is not the ESP32 decoder and does not infer undocumented fields.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib


APPLE_MAGIC = b"UNIRAST\x00"
HEADER_BYTES = 32
SUPPORTED_COLOR_SPACES = {
    0: ("SW", 1),
    1: ("sRGB", 3),
    2: ("CIELab", 3),
    3: ("AdobeRGB", 3),
    4: ("W", 1),
    5: ("RGB", 3),
    6: ("CMYK", 4),
}
WHITE_COLOR_SPACES = {0, 1, 2, 3, 4, 5}


class Cursor:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def take(self, length: int) -> bytes:
        if length < 0 or self.offset + length > len(self.data):
            raise ValueError(f"truncated at offset {self.offset}, need {length} bytes")
        value = self.data[self.offset:self.offset + length]
        self.offset += length
        return value

    def u8(self) -> int:
        return self.take(1)[0]

    def u32be(self) -> int:
        return struct.unpack(">I", self.take(4))[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_page_header(header: bytes, page_index: int) -> dict:
    if len(header) != HEADER_BYTES:
        raise ValueError("Apple Raster page header must be 32 bytes")
    bits_per_pixel = header[0]
    color_space_code = header[1]
    duplex_code = header[2]
    print_quality = header[3]
    media_type = header[4]
    media_position = header[5]
    width = struct.unpack_from(">I", header, 12)[0]
    height = struct.unpack_from(">I", header, 16)[0]
    dpi = struct.unpack_from(">I", header, 20)[0]
    color_info = SUPPORTED_COLOR_SPACES.get(color_space_code)
    if color_info is None:
        raise ValueError(f"unsupported public Apple Raster color-space code {color_space_code}")
    color_space, colors = color_info
    if width == 0 or height == 0 or dpi == 0:
        raise ValueError("Apple Raster page dimensions and dpi must be non-zero")
    if bits_per_pixel not in (8, 16, 24, 32, 48, 64):
        raise ValueError(f"unsupported public bits-per-pixel value {bits_per_pixel}")
    if bits_per_pixel not in (colors * 8, colors * 16):
        raise ValueError("bits-per-pixel is inconsistent with the public color-space mapping")
    bytes_per_pixel = bits_per_pixel // 8
    bytes_per_row = width * bytes_per_pixel
    if bytes_per_row > 16 * 1024 * 1024:
        raise ValueError("row exceeds the analysis limit")
    return {
        "page_index": page_index,
        "bits_per_pixel": bits_per_pixel,
        "bits_per_color": bits_per_pixel // colors,
        "color_space_code": color_space_code,
        "color_space": color_space,
        "colors": colors,
        "duplex_code": duplex_code,
        "print_quality_code": print_quality,
        "media_type_code": media_type,
        "media_position_code": media_position,
        "width_px": width,
        "height_px": height,
        "dpi_x": dpi,
        "dpi_y": dpi,
        "bytes_per_pixel": bytes_per_pixel,
        "bytes_per_row": bytes_per_row,
        "header_hex": header.hex(),
    }


def decode_page_rows(cursor: Cursor, descriptor: dict) -> dict:
    row_bytes = descriptor["bytes_per_row"]
    height = descriptor["height_px"]
    bytes_per_pixel = descriptor["bytes_per_pixel"]
    clear_byte = 0xFF if descriptor["color_space_code"] in WHITE_COLOR_SPACES else 0x00
    row_crc = 0
    decoded_bytes = 0
    compressed_start = cursor.offset
    row_index = 0
    command_count = 0
    clear_rows = 0
    max_repeat = 0
    max_commands_per_row = 0

    while row_index < height:
        repeat = cursor.u8() + 1
        if repeat > height - row_index:
            raise ValueError(f"row repeat {repeat} exceeds remaining height at row {row_index}")
        max_repeat = max(max_repeat, repeat)
        row = bytearray()
        row_commands = 0
        clear_to_end = False
        while len(row) < row_bytes:
            command = cursor.u8()
            command_count += 1
            row_commands += 1
            if command == 0x80:
                row.extend(bytes((clear_byte,)) * (row_bytes - len(row)))
                clear_to_end = True
                continue
            if command & 0x80:
                count = 257 - command
                value_bytes = count * bytes_per_pixel
                if len(row) + value_bytes > row_bytes:
                    raise ValueError(f"literal row command exceeds row {row_index}")
                row.extend(cursor.take(value_bytes))
            else:
                count = command + 1
                if len(row) + count * bytes_per_pixel > row_bytes:
                    raise ValueError(f"repeat row command exceeds row {row_index}")
                pixel = cursor.take(bytes_per_pixel)
                row.extend(pixel * count)
        max_commands_per_row = max(max_commands_per_row, row_commands)
        if clear_to_end:
            clear_rows += repeat
        for _ in range(repeat):
            row_crc = zlib.crc32(row, row_crc)
            decoded_bytes += len(row)
        row_index += repeat

    return {
        "compressed_bytes": cursor.offset - compressed_start,
        "decoded_bytes": decoded_bytes,
        "rows_decoded": row_index,
        "row_crc32": f"{row_crc & 0xFFFFFFFF:08x}",
        "row_commands": command_count,
        "max_commands_per_row": max_commands_per_row,
        "clear_to_end_rows": clear_rows,
        "max_row_repeat": max_repeat,
    }


def analyze_document(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    cursor = Cursor(data)
    magic = cursor.take(8)
    if magic != APPLE_MAGIC:
        raise ValueError(f"expected public Apple Raster sync UNIRAST\\0, got {magic!r}")
    page_count = cursor.u32be()
    if page_count == 0 or page_count > 1024:
        raise ValueError("page count is outside the analysis limit")

    pages = []
    for page_index in range(1, page_count + 1):
        header_offset = cursor.offset
        descriptor = parse_page_header(cursor.take(HEADER_BYTES), page_index)
        descriptor["header_offset"] = header_offset
        descriptor.update(decode_page_rows(cursor, descriptor))
        descriptor["data_offset"] = header_offset + HEADER_BYTES
        descriptor["page_end_offset"] = cursor.offset
        pages.append(descriptor)

    result = {
        "format": "apple-raster-public-cups-framing",
        "source": str(path),
        "bytes": len(data),
        "sha256": sha256(data),
        "sync": magic.decode("ascii", errors="replace"),
        "page_count_declared": page_count,
        "page_count_analyzed": len(pages),
        "trailing_bytes": len(data) - cursor.offset,
        "pages": pages,
        "analysis_limits": {
            "max_bytes_per_row": 16 * 1024 * 1024,
            "max_pages": 1024,
        },
        "unknowns": [
            "undocumented/vendor meaning of reserved page-header bytes",
            "application-level interpretation of the captured image content",
            "whether all client variants use the same row encoding and page count",
        ],
        "references": [
            "https://github.com/OpenPrinting/libcups/blob/master/cups/raster.h",
            "https://raw.githubusercontent.com/OpenPrinting/libcups/master/cups/raster-stream.c",
            "https://github.com/istopwg/ippsample/blob/master/tools/ippdoclint.c",
        ],
    }
    return result


def command_analyze(args: argparse.Namespace) -> int:
    result = analyze_document(pathlib.Path(args.document))
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(output, encoding="utf-8")
    print(output, end="")
    return 0


def command_compare(args: argparse.Namespace) -> int:
    analyses = [analyze_document(pathlib.Path(path)) for path in args.document]
    fields = (
        "page_count_declared", "width_px", "height_px", "dpi_x", "dpi_y",
        "bits_per_pixel", "color_space_code", "bytes_per_row", "rows_decoded",
        "decoded_bytes", "row_crc32",
    )
    comparison = {
        "documents": analyses,
        "same_verified_fields": {
            field: len({
                analysis[field] if field == "page_count_declared" else
                analysis["pages"][0][field]
                for analysis in analyses
            }) == 1
            for field in fields
        },
        "note": "Comparison is limited to public framing and row statistics; no raster field is inferred beyond the cited CUPS format.",
    }
    output = json.dumps(comparison, indent=2, sort_keys=True) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(output, encoding="utf-8")
    print(output, end="")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    analyze = sub.add_parser("analyze", help="analyze one Apple Raster document")
    analyze.add_argument("--document", required=True)
    analyze.add_argument("--output")
    analyze.set_defaults(func=command_analyze)
    compare = sub.add_parser("compare", help="compare public framing across documents")
    compare.add_argument("--document", action="append", required=True)
    compare.add_argument("--output")
    compare.set_defaults(func=command_compare)
    args = parser.parse_args()
    try:
        return args.func(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
