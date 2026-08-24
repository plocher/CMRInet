#!/usr/bin/env python3
"""bench_ports — serial-keyed USB port resolution for the CMRInet bench.

Roles (Host, Sniffer TX/RX, Dongle, Nodes) are named in bench.json and keyed
by USB serial number, which the Xiao ESP32-C6 boards expose as a MAC-shaped
iSerialNumber. The /dev/cu.usbmodem* names are location-derived and treated
as enumeration artifacts, never as configuration. See issue #68.

One bench.json holds multiple explicitly named bench sets (top-level
"Benches" map plus a "Default" name), so several benches can share one file
and several benches can be physically attached to one host at once.

Testing hook: --ports-file (or BENCH_PORTS_FAKE) injects a fake enumeration
(JSON list of {device, serial_number, location, vid, pid, description}) so
the CLI runs end-to-end without hardware.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Optional

EXIT_OK = 0
EXIT_CONFIG = 2
EXIT_MISSING = 3
EXIT_MISMATCH = 4

BENCH_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = BENCH_DIR / "bench.json"
CONFIG_ENV = "CMRINET_BENCH_CONFIG"
BENCH_ENV = "CMRINET_BENCH"
PORTS_ENV = "BENCH_PORTS_FAKE"

ESP32_VID_PID = (0x303A, 0x1001)  # Xiao ESP32-C6 USB JTAG/serial
FTDI_VID_PID = (0x0403, 0x6001)   # RS485 dongle

HOST_IMAGE = "xiao_host_tracer"
SNIFFER_IMAGE = "xiao_sniffer"


class BenchError(Exception):
    """A user-facing failure carrying the CLI exit code."""

    def __init__(self, message: str, exit_code: int = EXIT_CONFIG):
        super().__init__(message)
        self.exit_code = exit_code


@dataclass
class BenchDevice:
    """One resolved bench role: config entry merged with live USB data."""

    type: str
    id: str
    serial: str
    location: Optional[str]      # advisory, from bench.json
    device: str                  # live /dev path
    vid: Optional[int]
    pid: Optional[int]
    description: str
    live_location: Optional[str]

    def to_dict(self) -> dict:
        return asdict(self)


# --------------------------------------------------------------------------
# Enumeration
# --------------------------------------------------------------------------

def _coerce_vid_pid(value) -> Optional[int]:
    """Accept ints or hex strings (0x303A) in ports-file fixtures."""
    if value is None:
        return None
    if isinstance(value, str):
        return int(value, 0)
    return int(value)


def list_usb_ports(ports_file: Optional[str] = None) -> list:
    """Enumerate candidate ports, or load a fake enumeration for tests."""
    source = ports_file or os.environ.get(PORTS_ENV)
    if source:
        with open(source, "r", encoding="utf-8") as handle:
            rows = json.load(handle)
        return [
            SimpleNamespace(
                device=row.get("device"),
                serial_number=row.get("serial_number"),
                location=row.get("location"),
                vid=_coerce_vid_pid(row.get("vid")),
                pid=_coerce_vid_pid(row.get("pid")),
                description=row.get("description", ""),
            )
            for row in rows
        ]
    from serial.tools import list_ports

    return list(list_ports.comports())


def _is_candidate(port) -> bool:
    """True for boards we might own: ESP32-C6 Xiaos and FTDI dongles."""
    if (port.vid, port.pid) in (ESP32_VID_PID, FTDI_VID_PID):
        return True
    return bool(port.device and "cu.usb" in port.device)


def _describe_attached(ports: list) -> str:
    """One line per attached candidate, for loud-complaint error messages."""
    lines = []
    for port in ports:
        if _is_candidate(port):
            lines.append(
                f"    {port.device}  serial={port.serial_number}"
                f"  location={port.location}  {port.description}"
            )
    return "\n".join(lines) if lines else "    (no candidate boards attached)"


# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------

def load_config(config_path: Optional[str] = None) -> tuple[Path, dict]:
    """Load the bench config file and validate its top-level shape."""
    raw = config_path or os.environ.get(CONFIG_ENV) or str(DEFAULT_CONFIG)
    path = Path(raw)
    if not path.exists():
        raise BenchError(f"bench config not found: {path}")
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise BenchError(f"bench config {path} is not valid JSON: {exc}")
    if isinstance(doc, list):
        raise BenchError(
            f"bench config {path} is in the legacy flat seed shape; "
            "run: bench import <seedfile> --bench <name>"
        )
    if not isinstance(doc, dict) or not isinstance(doc.get("Benches"), dict):
        raise BenchError(
            f"bench config {path} needs a top-level \"Benches\" map; "
            "run: bench import <seedfile> --bench <name>"
        )
    return path, doc


def select_bench(doc: dict, bench: Optional[str] = None) -> tuple[str, list]:
    """Pick one named bench set: --bench > $CMRINET_BENCH > Default."""
    benches = doc["Benches"]
    name = bench or os.environ.get(BENCH_ENV) or doc.get("Default")
    if not name:
        if len(benches) == 1:
            name = next(iter(benches))
        else:
            raise BenchError(
                "config holds benches "
                f"{sorted(benches)} but none was selected "
                f"(use --bench, ${BENCH_ENV}, or set a Default)"
            )
    if name not in benches:
        raise BenchError(
            f"bench '{name}' not in config; available: {sorted(benches)}"
        )
    roles = benches[name]
    if not isinstance(roles, list):
        raise BenchError(f"bench '{name}' is not a list of role entries")
    return name, roles


def find_role(roles: list, type_: str, id_: Optional[str]) -> dict:
    """Find the {Type, ID} entry for a role; ambiguous Type needs --id."""
    matches = [r for r in roles if r.get("Type") == type_]
    if id_ is not None:
        matches = [r for r in matches if str(r.get("ID")) == str(id_)]
    if not matches:
        wanted = f"{type_}/{id_}" if id_ is not None else type_
        raise BenchError(f"role not in bench: {wanted}")
    if len(matches) > 1:
        ids = [str(r.get("ID")) for r in matches]
        raise BenchError(
            f"role {type_} is ambiguous; add --id (one of {ids})"
        )
    entry = matches[0]
    if not entry.get("Serial"):
        raise BenchError(
            f"role {type_}/{entry.get('ID')} has no Serial; "
            "enrich it with: bench import <seedfile> --bench <name>"
        )
    return entry


# --------------------------------------------------------------------------
# Resolution
# --------------------------------------------------------------------------

def usb_resolve(
    type_: str,
    id_: Optional[str] = None,
    bench: Optional[str] = None,
    config: Optional[str] = None,
    ports_file: Optional[str] = None,
) -> BenchDevice:
    """Resolve a bench role to its live device, keyed by USB serial."""
    _, doc = load_config(config)
    bench_name, roles = select_bench(doc, bench)
    entry = find_role(roles, type_, id_)
    serial = str(entry["Serial"])
    ports = list_usb_ports(ports_file)
    for port in ports:
        if (port.serial_number or "").lower() == serial.lower():
            return BenchDevice(
                type=entry["Type"],
                id=str(entry.get("ID")),
                serial=port.serial_number,
                location=entry.get("Location"),
                device=port.device,
                vid=port.vid,
                pid=port.pid,
                description=port.description or "",
                live_location=port.location,
            )
    wanted = f"{entry['Type']}/{entry.get('ID')}"
    raise BenchError(
        f"bench '{bench_name}' role {wanted} expects serial {serial}, "
        "but no attached board reports it.\n"
        "Attached candidates:\n" + _describe_attached(ports) + "\n"
        "If cables moved, update the Serial values (bench import) "
        "or fix the wiring.",
        EXIT_MISSING,
    )


# --------------------------------------------------------------------------
# Behavioral verification (only for images we own)
# --------------------------------------------------------------------------

_PROBE_RULES = {
    "Host": (True, HOST_IMAGE, 8.0),
    "Sniffer": (False, SNIFFER_IMAGE, 6.5),
}


def probe_device(
    device: str,
    type_: str,
    timeout_s: Optional[float] = None,
    opener=None,
) -> tuple[bool, str]:
    """Open a port and check the image identity behind it.

    Hosts answer the `status` verb (identity landed in #66); sniffers are
    passive but emit image-bearing stats lines every 5 s. Returns
    (ok, detail). Roles without a probe rule are never sent here.
    """
    send_status, expected_image, default_timeout = _PROBE_RULES[type_]
    timeout = timeout_s if timeout_s is not None else default_timeout
    import serial

    try:
        ser = opener(device, 115200, timeout=0.1) if opener else serial.Serial(
            device, 115200, timeout=0.1
        )
    except Exception as exc:
        return False, f"open failed: {exc}"
    try:
        time.sleep(0.5)  # let the CDC open settle before talking
        ser.reset_input_buffer()  # drop stale output flood; we want fresh lines
        if send_status:
            ser.write(b"status\n")
        deadline = time.time() + timeout
        resent = False
        lines_seen = 0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("{"):
                continue
            lines_seen += 1
            try:
                doc = json.loads(line)
                image = doc.get("image")
            except json.JSONDecodeError:
                # Truncated lines happen under CDC backpressure (same
                # workaround as _tracer_client.sync_and_validate_boot).
                match = re.search(r'"image":"([^"]+)"', line)
                image = match.group(1) if match else None
            if image:
                return image == expected_image, f"image={image}"
            # A busy tracer can miss the first status write; ask once more
            # halfway through the window before declaring a mismatch.
            if send_status and not resent and time.time() > deadline - timeout / 2:
                ser.write(b"status\n")
                resent = True
        return False, f"no image line within {timeout}s ({lines_seen} json lines seen)"
    finally:
        ser.close()


def verify_role(
    entry: dict, device_record: BenchDevice, opener=None
) -> tuple[str, str]:
    """Classify one resolved role: ok / mismatch / unverifiable."""
    type_ = entry["Type"]
    if type_ == "Dongle":
        if (device_record.vid, device_record.pid) == FTDI_VID_PID:
            return "ok", "ftdi vid:pid"
        return "mismatch", (
            f"expected vid:pid {FTDI_VID_PID[0]:04x}:{FTDI_VID_PID[1]:04x}"
        )
    if type_ in _PROBE_RULES:
        ok, detail = probe_device(device_record.device, type_, opener=opener)
        return ("ok" if ok else "mismatch"), detail
    return "unverifiable", "presence only (image not owned)"


# --------------------------------------------------------------------------
# Discover / enrich (seed-driven; CLI verbs: import, seed)
# --------------------------------------------------------------------------

def enrich_seed(
    seed_entries: list,
    verify: bool = True,
    ports_file: Optional[str] = None,
    opener=None,
) -> tuple[list, list, list]:
    """Harvest serials for a seed's roles from live enumeration.

    The seed's per-role USB (or Location) is the human-supplied, one-time
    disambiguator of bench membership. Returns (enriched, missing, mismatches).
    """
    ports = list_usb_ports(ports_file)
    by_device = {p.device: p for p in ports}
    by_location = {p.location: p for p in ports if p.location}
    enriched: list = []
    missing: list = []
    mismatches: list = []
    for entry in seed_entries:
        port = by_device.get(entry.get("USB")) or by_location.get(
            entry.get("Location")
        )
        if port is None:
            missing.append(entry)
            continue
        record = {
            "Type": entry.get("Type"),
            "ID": entry.get("ID"),
            "Serial": port.serial_number,
            "Location": port.location,
        }
        enriched.append(record)
        if verify and entry.get("Type") in _PROBE_RULES:
            ok, detail = probe_device(port.device, entry["Type"], opener=opener)
            if not ok:
                mismatches.append(
                    f"{entry.get('Type')}/{entry.get('ID')} at {port.device}: "
                    f"{detail}"
                )
    return enriched, missing, mismatches


# --------------------------------------------------------------------------
# Output helpers: -o console | - | <file>  (jBOM vocabulary)
# --------------------------------------------------------------------------

def _console_table(rows: list[dict], columns: list[str]) -> str:
    """Render rows as a justified plain-text table."""
    widths = {
        col: max([len(col)] + [len(str(row.get(col, ""))) for row in rows])
        for col in columns
    }
    header = "  ".join(col.ljust(widths[col]) for col in columns)
    lines = [header, "  ".join("-" * widths[col] for col in columns)]
    for row in rows:
        lines.append(
            "  ".join(str(row.get(col, "")).ljust(widths[col]) for col in columns)
        )
    return "\n".join(lines)


def emit(payload, rows: list[dict], columns: list[str], output: Optional[str]) -> None:
    """Send a verb's result to the selected destination.

    console -> justified table on stdout; '-' -> JSON on stdout;
    path -> JSON (.json) or CSV (.csv) file.
    """
    if output in (None, "console"):
        print(_console_table(rows, columns))
        return
    if output == "-":
        json.dump(payload, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return
    path = Path(output)
    if path.suffix == ".csv":
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns)
            writer.writeheader()
            for row in rows:
                writer.writerow({col: row.get(col, "") for col in columns})
    else:
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    eprint(f"wrote {path}")


def eprint(message: str) -> None:
    """Diagnostics always go to stderr; stdout stays machine-clean."""
    print(message, file=sys.stderr)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _add_common(parser: argparse.ArgumentParser, include_bench: bool = True) -> None:
    # import adds its own required --bench (the set being written), so it
    # asks for the common flags without the selection one.
    if include_bench:
        parser.add_argument("--bench", help="named bench set in the config file")
    parser.add_argument("--config", help=f"config file (default: {DEFAULT_CONFIG})")
    parser.add_argument(
        "--ports-file",
        help=f"inject a fake USB enumeration from JSON (or ${PORTS_ENV})",
    )
    parser.add_argument(
        "-o",
        "--output",
        metavar="DEST",
        help="console | - (JSON to stdout) | <file.json|file.csv>",
    )
    detail = parser.add_mutually_exclusive_group()
    detail.add_argument("-q", "--quiet", action="store_true")
    detail.add_argument("-v", "--verbose", action="store_true")


def _resolve_role_from_args(args) -> tuple[str, Optional[str]]:
    role_id = getattr(args, "role_id", None)
    return args.role, role_id


def _cmd_list(args) -> int:
    _, doc = load_config(args.config)
    if args.all:
        default = doc.get("Default")
        rows = [
            {
                "Bench": name,
                "Roles": len(roles),
                "Default": "*" if name == default else "",
            }
            for name, roles in doc["Benches"].items()
        ]
        payload = {"default": default, "benches": sorted(doc["Benches"])}
        if args.quiet:
            for name in sorted(doc["Benches"]):
                print(name)
            return EXIT_OK
        emit(payload, rows, ["Bench", "Roles", "Default"], args.output)
        return EXIT_OK

    bench_name, roles = select_bench(doc, args.bench or args.name)
    ports = list_usb_ports(args.ports_file)
    by_serial = {
        (p.serial_number or "").lower(): p for p in ports if p.serial_number
    }
    rows = []
    records = []
    for entry in roles:
        port = by_serial.get(str(entry.get("Serial", "")).lower())
        status = "OK" if port else "MISSING"
        row = {
            "Type": entry.get("Type", ""),
            "ID": entry.get("ID", ""),
            "Serial": entry.get("Serial", ""),
            "Location": entry.get("Location", "") or "",
            "Device": port.device if port else "-",
            "Status": status,
        }
        rows.append(row)
        records.append({**row, "LiveLocation": port.location if port else None})
    if args.quiet:
        for row in rows:
            print(row["Device"])
        return EXIT_OK
    columns = ["Type", "ID", "Serial", "Location", "Device", "Status"]
    emit({"bench": bench_name, "roles": records}, rows, columns, args.output)
    return EXIT_OK


def _cmd_resolve(args) -> int:
    type_, role_id = _resolve_role_from_args(args)
    record = usb_resolve(
        type_, role_id, args.bench, args.config, args.ports_file
    )
    if args.verbose or args.output:
        payload = record.to_dict()
        rows = [{k: v for k, v in payload.items()}]
        emit(payload, rows, list(payload.keys()), args.output or "-")
    else:
        print(record.device)
    return EXIT_OK


def _cmd_check(args) -> int:
    _, doc = load_config(args.config)
    bench_name, roles = select_bench(doc, args.bench)
    rows = []
    worst = EXIT_OK
    for entry in roles:
        label = f"{entry.get('Type')}/{entry.get('ID')}"
        try:
            record = usb_resolve(
                entry.get("Type"),
                str(entry.get("ID")),
                bench_name,
                args.config,
                args.ports_file,
            )
        except BenchError:
            rows.append({"Role": label, "Device": "-", "Check": "MISSING"})
            worst = max(worst, EXIT_MISSING)
            continue
        if args.no_verify:
            verdict, detail = "present", "not probed"
        else:
            verdict, detail = verify_role(entry, record)
            if verdict == "mismatch":
                worst = max(worst, EXIT_MISMATCH)
        rows.append(
            {
                "Role": label,
                "Device": record.device,
                "Check": verdict.upper(),
                "Detail": detail,
            }
        )
        if args.verbose:
            eprint(f"check {label} at {record.device}: {verdict} ({detail})")
    if not args.quiet:
        emit(
            {"bench": bench_name, "checks": rows},
            rows,
            ["Role", "Device", "Check", "Detail"],
            args.output,
        )
    return worst


def _cmd_import(args) -> int:
    seed_path = Path(args.seedfile)
    if not seed_path.exists():
        raise BenchError(f"seed file not found: {seed_path}")
    try:
        seed = json.loads(seed_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise BenchError(f"seed file {seed_path} is not valid JSON: {exc}")
    if isinstance(seed, dict) and "Benches" in seed:
        raise BenchError(
            f"{seed_path} is already a structured bench config, not a seed"
        )
    if not isinstance(seed, list):
        raise BenchError(f"seed file {seed_path} must be a flat list of roles")

    enriched, missing, mismatches = enrich_seed(
        seed, verify=not args.no_verify, ports_file=args.ports_file
    )
    for entry in missing:
        eprint(
            f"MISSING: seed role {entry.get('Type')}/{entry.get('ID')} "
            f"at {entry.get('USB') or entry.get('Location')} is not attached"
        )
    for detail in mismatches:
        eprint(f"MISMATCH: {detail}")
    # Missing roles stay in the set with Serial null, so the bench's role
    # list is preserved and resolving one later fails with a clear
    # "run bench import" message instead of pretending the role is gone.
    for entry in missing:
        enriched.append(
            {
                "Type": entry.get("Type"),
                "ID": entry.get("ID"),
                "Serial": None,
                "Location": entry.get("Location"),
            }
        )

    if args.dry_run:
        emit(enriched, enriched, ["Type", "ID", "Serial", "Location"], args.output or "-")
    else:
        config_path = Path(args.config or DEFAULT_CONFIG)
        if config_path.exists():
            doc = json.loads(config_path.read_text(encoding="utf-8"))
            if isinstance(doc, list):
                doc = {"Default": None, "Benches": {}}
        else:
            doc = {"Default": None, "Benches": {}}
        if not isinstance(doc.get("Benches"), dict):
            raise BenchError(f"{config_path} has no top-level Benches map")
        if args.bench in doc["Benches"] and not args.force:
            raise BenchError(
                f"bench '{args.bench}' already exists in {config_path}; "
                "use --force to overwrite"
            )
        doc["Benches"][args.bench] = enriched
        if not doc.get("Default"):
            doc["Default"] = args.bench
        config_path.write_text(
            json.dumps(doc, indent=2) + "\n", encoding="utf-8"
        )
        eprint(
            f"imported {len(enriched)} roles as bench '{args.bench}' "
            f"in {config_path} (Default={doc['Default']})"
        )

    return max(
        EXIT_MISSING if missing else EXIT_OK,
        EXIT_MISMATCH if mismatches else EXIT_OK,
    )


def _cmd_export(args) -> int:
    _, doc = load_config(args.config)
    bench_name, roles = select_bench(doc, args.bench)
    if args.format == "seed":
        ports = list_usb_ports(args.ports_file)
        by_serial = {
            (p.serial_number or "").lower(): p for p in ports if p.serial_number
        }
        payload = []
        for entry in roles:
            port = by_serial.get(str(entry.get("Serial", "")).lower())
            if port is None:
                eprint(
                    f"note: {entry.get('Type')}/{entry.get('ID')} "
                    "is not currently attached; USB left null"
                )
            payload.append(
                {
                    "Type": entry.get("Type"),
                    "ID": entry.get("ID"),
                    "USB": port.device if port else None,
                    "Location": entry.get("Location"),
                }
            )
    else:
        payload = roles
    rows = [
        {
            "Type": r.get("Type", ""),
            "ID": r.get("ID", ""),
            "Serial": r.get("Serial", "") or "",
            "Location": r.get("Location", "") or "",
            "USB": r.get("USB", "") or "",
        }
        for r in payload
    ]
    emit(payload, rows, ["Type", "ID", "Serial", "Location", "USB"], args.output or "-")
    eprint(f"exported bench '{bench_name}'")
    return EXIT_OK


def _cmd_seed(args) -> int:
    ports = [p for p in list_usb_ports(args.ports_file) if _is_candidate(p)]
    template = [
        {
            "Type": "",
            "ID": "",
            "USB": port.device,
            "Serial": port.serial_number,
            "Location": port.location,
        }
        for port in ports
    ]
    rows = [
        {
            "USB": t["USB"] or "",
            "Serial": t["Serial"] or "",
            "Location": t["Location"] or "",
        }
        for t in template
    ]
    emit(template, rows, ["USB", "Serial", "Location"], args.output or "-")
    eprint(
        "fill in Type/ID for each row, then: "
        "bench import <file> --bench <name>"
    )
    return EXIT_OK


def _cmd_default(args) -> int:
    config_path, doc = load_config(args.config)
    if args.name is None:
        default = doc.get("Default")
        if args.output or args.verbose:
            emit(
                {"default": default},
                [{"Default": default or ""}],
                ["Default"],
                args.output or "-",
            )
        else:
            print(default or "")
        return EXIT_OK
    if args.name not in doc["Benches"]:
        raise BenchError(
            f"bench '{args.name}' not in config; "
            f"available: {sorted(doc['Benches'])}"
        )
    doc["Default"] = args.name
    config_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    eprint(f"Default bench set to '{args.name}' in {config_path}")
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    """Build the bench CLI argument parser."""
    parser = argparse.ArgumentParser(
        prog="bench",
        description="Serial-keyed USB port resolution for the CMRInet bench.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="list benches or roles")
    p_list.add_argument("name", nargs="?", help="bench set name")
    p_list.add_argument("-a", "--all", action="store_true", help="list all bench sets")
    _add_common(p_list)
    p_list.set_defaults(func=_cmd_list)

    p_check = sub.add_parser("check", help="resolve and verify every role")
    p_check.add_argument("--no-verify", action="store_true",
                         help="resolve only; skip behavioral probes")
    _add_common(p_check)
    p_check.set_defaults(func=_cmd_check)

    p_resolve = sub.add_parser("resolve", help="print the live device for a role")
    p_resolve.add_argument("--role", required=True,
                           help="role Type, e.g. Host, Sniffer, Node, Dongle")
    p_resolve.add_argument("--id", "--address", dest="role_id",
                           help="role ID, e.g. RX, TX, or a node address")
    _add_common(p_resolve)
    p_resolve.set_defaults(func=_cmd_resolve)

    p_import = sub.add_parser("import", help="enrich a seed into a named bench set")
    p_import.add_argument("seedfile", help="flat seed JSON (Type/ID/USB entries)")
    p_import.add_argument("--bench", required=True, help="name for the new set")
    p_import.add_argument("--force", action="store_true",
                          help="overwrite an existing set")
    p_import.add_argument("--dry-run", action="store_true",
                          help="print the enriched set; do not write the config")
    p_import.add_argument("--no-verify", action="store_true",
                          help="skip behavioral verification probes")
    _add_common(p_import, include_bench=False)
    p_import.set_defaults(func=_cmd_import)

    p_export = sub.add_parser("export", help="print one bench set")
    p_export.add_argument("--format", choices=["final", "seed"], default="final")
    _add_common(p_export)
    p_export.set_defaults(func=_cmd_export)

    p_seed = sub.add_parser("seed", help="print a seed template of attached boards")
    _add_common(p_seed)
    p_seed.set_defaults(func=_cmd_seed)

    p_default = sub.add_parser("default", help="get or set the Default bench")
    p_default.add_argument("name", nargs="?", help="bench set to make default")
    _add_common(p_default)
    p_default.set_defaults(func=_cmd_default)

    return parser


def main(argv: Optional[list] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except BenchError as exc:
        eprint(f"error: {exc}")
        return exc.exit_code


if __name__ == "__main__":
    sys.exit(main())
