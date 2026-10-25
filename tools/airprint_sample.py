#!/usr/bin/env python3
"""Create, import, and validate AirPrint golden-sample bundles.

This tool intentionally does not decode Apple Raster or URF. It records the
captured IPP body and the bytes that a future decoder must explain. The import
command only copies raw evidence; it never infers raster fields.
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import sys


FORMATS = {"image/urf", "image/pwg-raster", "image/apple-raster"}
MANIFEST_VERSION = 1


def load_json(path: pathlib.Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError("manifest must be a JSON object")
    return value


def validate_manifest(manifest: dict) -> None:
    required = ("sample_name", "document_format", "sha256", "ipp_body_bytes",
                "document_offset", "document_bytes")
    missing = [key for key in required if key not in manifest]
    if missing:
        raise ValueError("missing manifest fields: " + ",".join(missing))
    if not isinstance(manifest["document_format"], str):
        raise ValueError("document_format must be a string")
    if manifest["document_format"] not in FORMATS:
        raise ValueError("unsupported document_format")
    if not isinstance(manifest["sha256"], str) or len(manifest["sha256"]) != 64:
        raise ValueError("sha256 must contain 64 hex characters")
    try:
        int(manifest["sha256"], 16)
        body = int(manifest["ipp_body_bytes"])
        offset = int(manifest["document_offset"])
        length = int(manifest["document_bytes"])
    except (TypeError, ValueError) as error:
        raise ValueError("manifest numeric fields are invalid") from error
    if body <= 0 or offset < 0 or length <= 0 or offset > body or length > body - offset:
        raise ValueError("document range is outside ipp_body_bytes")
    if "manifest_version" in manifest:
        if manifest["manifest_version"] != MANIFEST_VERSION:
            raise ValueError("unsupported manifest_version")
    if "artifacts" in manifest:
        validate_artifacts(manifest["artifacts"])
    if manifest.get("expected_values_known", False):
        for field in ("expected_width_px", "expected_height_px", "expected_dpi_x",
                      "expected_dpi_y"):
            try:
                value = int(manifest.get(field, 0))
            except (TypeError, ValueError) as error:
                raise ValueError("known expected values require " + field) from error
            if value <= 0:
                raise ValueError("known expected values require " + field)


def validate_artifacts(artifacts: object) -> None:
    if not isinstance(artifacts, dict) or "ipp_body" not in artifacts:
        raise ValueError("artifacts must contain ipp_body")
    for name, artifact in artifacts.items():
        if not isinstance(name, str) or not isinstance(artifact, dict):
            raise ValueError("invalid artifact entry")
        relative = artifact.get("path")
        if (not isinstance(relative, str) or not relative or
                pathlib.PurePath(relative).is_absolute() or
                ".." in pathlib.PurePath(relative).parts):
            raise ValueError("artifact path must stay inside the bundle")
        digest = artifact.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64:
            raise ValueError("artifact sha256 must contain 64 hex characters")
        try:
            int(digest, 16)
            size = int(artifact.get("bytes"))
        except (TypeError, ValueError) as error:
            raise ValueError("invalid artifact hash or length") from error
        if size < 0:
            raise ValueError("artifact length must not be negative")


def artifact_record(relative: str, data: bytes) -> dict:
    return {
        "bytes": len(data),
        "path": relative,
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def manifest_for_body(args: argparse.Namespace, body: bytes) -> dict:
    if args.document_offset < 0 or args.document_bytes <= 0:
        raise ValueError("invalid document range")
    if args.document_offset > len(body) or args.document_bytes > len(body) - args.document_offset:
        raise ValueError("document range is outside body")
    result = {
        "sample_name": args.sample_name,
        "document_format": args.document_format,
        "sha256": hashlib.sha256(body).hexdigest(),
        "ipp_body_bytes": len(body),
        "document_offset": args.document_offset,
        "document_bytes": args.document_bytes,
        "expected_values_known": False,
    }
    validate_manifest(result)
    return result


def create_manifest(args: argparse.Namespace) -> dict:
    body = pathlib.Path(args.body).read_bytes()
    return manifest_for_body(args, body)


def command_create(args: argparse.Namespace) -> int:
    manifest = create_manifest(args)
    pathlib.Path(args.output).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


def copy_artifact(source: pathlib.Path, bundle: pathlib.Path, relative: str) -> dict:
    data = source.read_bytes()
    destination = bundle / relative
    if source.resolve() != destination.resolve():
        shutil.copyfile(source, destination)
    return artifact_record(relative, data)


def command_import(args: argparse.Namespace) -> int:
    bundle = pathlib.Path(args.output_dir)
    bundle.mkdir(parents=True, exist_ok=True)
    body_source = pathlib.Path(args.body)
    body = body_source.read_bytes()
    manifest = manifest_for_body(args, body)
    manifest["manifest_version"] = MANIFEST_VERSION
    manifest["artifacts"] = {
        "ipp_body": copy_artifact(body_source, bundle, "ipp_body.bin"),
    }

    optional_artifacts = (
        ("document", args.document, "document.bin"),
        ("analysis", args.analysis, "analysis.json"),
        ("pcap", args.pcap, "capture.pcap"),
        ("http_capture", args.http_capture, "http_capture.bin"),
        ("ipp_attributes", args.ipp_attributes, "ipp_attributes.bin"),
        ("capture_metadata", args.capture_metadata, "capture_metadata.json"),
        ("expected_pixels", args.expected_pixels, "expected_pixels.bin"),
    )
    for name, source_name, relative in optional_artifacts:
        if source_name:
            manifest["artifacts"][name] = copy_artifact(
                pathlib.Path(source_name), bundle, relative
            )

    validate_manifest(manifest)
    manifest_path = bundle / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


def bundle_artifact_path(bundle: pathlib.Path, relative: str) -> pathlib.Path:
    root = bundle.resolve()
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ValueError("artifact path escapes bundle") from error
    return candidate


def check_bundle(bundle: pathlib.Path) -> dict:
    manifest = load_json(bundle / "manifest.json")
    validate_manifest(manifest)
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise ValueError("bundle manifest must contain artifacts")
    for name, artifact in artifacts.items():
        path = bundle_artifact_path(bundle, artifact["path"])
        if not path.is_file():
            raise ValueError(f"missing artifact: {name}")
        data = path.read_bytes()
        if len(data) != int(artifact["bytes"]):
            raise ValueError(f"artifact length does not match: {name}")
        if hashlib.sha256(data).hexdigest() != artifact["sha256"].lower():
            raise ValueError(f"artifact SHA-256 does not match: {name}")

    body_artifact = artifacts["ipp_body"]
    if int(body_artifact["bytes"]) != int(manifest["ipp_body_bytes"]):
        raise ValueError("ipp_body artifact length does not match manifest")
    if body_artifact["sha256"].lower() != manifest["sha256"].lower():
        raise ValueError("ipp_body artifact hash does not match manifest")
    return manifest


def command_check(args: argparse.Namespace) -> int:
    if args.bundle:
        if args.body:
            raise ValueError("--body cannot be used with --bundle")
        manifest = check_bundle(pathlib.Path(args.bundle))
    else:
        manifest = load_json(pathlib.Path(args.manifest))
        validate_manifest(manifest)
    if args.body:
        body = pathlib.Path(args.body).read_bytes()
        if len(body) != int(manifest["ipp_body_bytes"]):
            raise ValueError("body length does not match manifest")
        digest = hashlib.sha256(body).hexdigest()
        if digest != manifest["sha256"].lower():
            raise ValueError("body SHA-256 does not match manifest")
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="create a manifest from a captured body")
    create.add_argument("--body", required=True)
    create.add_argument("--output", required=True)
    create.add_argument("--sample-name", required=True)
    create.add_argument("--document-format", required=True, choices=sorted(FORMATS))
    create.add_argument("--document-offset", type=int, required=True)
    create.add_argument("--document-bytes", type=int, required=True)
    create.set_defaults(func=command_create)
    import_parser = sub.add_parser(
        "import", help="import raw capture artifacts into a canonical sample bundle"
    )
    import_parser.add_argument("--body", required=True)
    import_parser.add_argument("--output-dir", required=True)
    import_parser.add_argument("--sample-name", required=True)
    import_parser.add_argument("--document-format", required=True, choices=sorted(FORMATS))
    import_parser.add_argument("--document-offset", type=int, required=True)
    import_parser.add_argument("--document-bytes", type=int, required=True)
    import_parser.add_argument("--pcap")
    import_parser.add_argument("--document")
    import_parser.add_argument("--analysis")
    import_parser.add_argument("--http-capture")
    import_parser.add_argument("--ipp-attributes")
    import_parser.add_argument("--capture-metadata")
    import_parser.add_argument("--expected-pixels")
    import_parser.set_defaults(func=command_import)
    check = sub.add_parser("check", help="validate a manifest and optional body")
    manifest_source = check.add_mutually_exclusive_group(required=True)
    manifest_source.add_argument("--manifest")
    manifest_source.add_argument("--bundle")
    check.add_argument("--body")
    check.set_defaults(func=command_check)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
