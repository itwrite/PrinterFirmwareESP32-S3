#!/usr/bin/env python3
"""Self-tests for airprint_sample.py without external dependencies."""

import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "airprint_sample.py"


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *args],
                          check=False, capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory() as directory:
        base = pathlib.Path(directory)
        body = base / "capture.ipp"
        manifest = base / "capture.json"
        body.write_bytes(b"IPP-HEADER-DOCUMENT")
        created = run("create", "--body", str(body), "--output", str(manifest),
                      "--sample-name", "mono-a4-001", "--document-format", "image/urf",
                      "--document-offset", "11", "--document-bytes", "8")
        assert created.returncode == 0, created.stderr
        checked = run("check", "--manifest", str(manifest), "--body", str(body))
        assert checked.returncode == 0, checked.stderr
        value = json.loads(checked.stdout)
        assert value["document_format"] == "image/urf"
        body.write_bytes(b"corrupted")
        rejected = run("check", "--manifest", str(manifest), "--body", str(body))
        assert rejected.returncode == 2

        body.write_bytes(b"IPP-HEADER-DOCUMENT")
        http_capture = base / "capture.http"
        ipp_attributes = base / "attributes.txt"
        metadata = base / "metadata.json"
        expected_pixels = base / "pixels.bin"
        pcap = base / "capture.pcap"
        document = base / "document.bin"
        analysis = base / "analysis.json"
        http_capture.write_bytes(b"HTTP/1.1 200 OK\r\n")
        ipp_attributes.write_text("document-format=image/urf\n", encoding="utf-8")
        metadata.write_text('{"source":"manual-capture"}\n', encoding="utf-8")
        expected_pixels.write_bytes(b"pixel-reference")
        pcap.write_bytes(b"pcap-evidence")
        document.write_bytes(b"document-evidence")
        analysis.write_text('{"format":"analysis"}\n', encoding="utf-8")
        bundle = base / "bundle"
        imported = run(
            "import", "--body", str(body), "--output-dir", str(bundle),
            "--sample-name", "mono-a4-001", "--document-format", "image/urf",
            "--document-offset", "11", "--document-bytes", "8",
            "--pcap", str(pcap),
            "--document", str(document),
            "--analysis", str(analysis),
            "--http-capture", str(http_capture),
            "--ipp-attributes", str(ipp_attributes),
            "--capture-metadata", str(metadata),
            "--expected-pixels", str(expected_pixels),
        )
        assert imported.returncode == 0, imported.stderr
        checked_bundle = run("check", "--bundle", str(bundle))
        assert checked_bundle.returncode == 0, checked_bundle.stderr
        bundle_value = json.loads(checked_bundle.stdout)
        assert bundle_value["manifest_version"] == 1
        assert "expected_pixels" in bundle_value["artifacts"]
        assert "pcap" in bundle_value["artifacts"]
        assert "document" in bundle_value["artifacts"]
        assert "analysis" in bundle_value["artifacts"]

        expected_pixels.write_bytes(b"changed-source-does-not-matter")
        assert (bundle / "expected_pixels.bin").read_bytes() == b"pixel-reference"
        (bundle / "expected_pixels.bin").write_bytes(b"tampered")
        rejected_bundle = run("check", "--bundle", str(bundle))
        assert rejected_bundle.returncode == 2

        bundle_manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
        bundle_manifest["document_offset"] = -1
        (bundle / "manifest.json").write_text(
            json.dumps(bundle_manifest), encoding="utf-8"
        )
        rejected_range = run("check", "--bundle", str(bundle))
        assert rejected_range.returncode == 2
    print("test_airprint_sample: passed")


if __name__ == "__main__":
    main()
