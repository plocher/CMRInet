#!/usr/bin/env python3
"""Functional tests for the bench CLI (bench_ports.py).

Drives the real command surface in subprocesses against a fixture bench
config plus a fake USB enumeration injected with --ports-file. No hardware
required. Behavioral verification (probe_device) is exercised separately
against the live bench, so check/import here always run --no-verify.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

BENCH_DIR = Path(__file__).resolve().parent
CLI = BENCH_DIR / "bench_ports.py"

# Fake serials (not MAC-shaped) so the fixtures stay readable in review;
# resolution is a case-insensitive string match, so any token works.
HOST_SERIAL = "FAKE-SERIAL-HOST"
SNIFFER_RX_SERIAL = "FAKE-SERIAL-SNIFFRX"
SNIFFER_TX_SERIAL = "FAKE-SERIAL-SNIFFTX"
NODE30_SERIAL = "FAKE-SERIAL-NODE30"
DONGLE_SERIAL = "BG04ID4L"
BETA_HOST_SERIAL = "FAKE-SERIAL-BETAHOST"
STRANGER_SERIAL = "FAKE-SERIAL-STRANGER"

# Ports as the OS reports them today.
PORTS_A = [
    {"device": "/dev/cu.Bluetooth-Incoming-Port"},
    {"device": "/dev/cu.usbmodem4111", "serial_number": HOST_SERIAL,
     "location": "4-1.1", "vid": "0x303A", "pid": "0x1001",
     "description": "USB JTAG/serial debug unit"},
    {"device": "/dev/cu.usbmodem4222", "serial_number": SNIFFER_RX_SERIAL,
     "location": "4-1.2", "vid": "0x303A", "pid": "0x1001",
     "description": "USB JTAG/serial debug unit"},
    {"device": "/dev/cu.usbmodem4333", "serial_number": SNIFFER_TX_SERIAL,
     "location": "4-1.3.1", "vid": "0x303A", "pid": "0x1001",
     "description": "USB JTAG/serial debug unit"},
    {"device": "/dev/cu.usbmodem4444", "serial_number": NODE30_SERIAL,
     "location": "4-1.3.2", "vid": "0x303A", "pid": "0x1001",
     "description": "USB JTAG/serial debug unit"},
    {"device": "/dev/cu.usbserial-BG04ID4L", "serial_number": DONGLE_SERIAL,
     "location": "5-1", "vid": "0x0403", "pid": "0x6001",
     "description": "FT232R USB UART"},
    # Somebody else's bench, attached to the same host:
    {"device": "/dev/cu.usbmodem4999", "serial_number": STRANGER_SERIAL,
     "location": "7-1.1", "vid": "0x303A", "pid": "0x1001",
     "description": "USB JTAG/serial debug unit"},
]

# The same boards after a reshuffle: serials unchanged, names/locations moved.
PORTS_SHUFFLED = [
    {**row, "device": row["device"].replace("usbmodem4", "usbmodem9"),
     "location": ("9" + row["location"][1:]) if row.get("location") else None}
    for row in PORTS_A
    if row.get("serial_number")
]
for row in PORTS_SHUFFLED:
    if row["serial_number"] == DONGLE_SERIAL:
        row["device"] = "/dev/cu.usbserial-BG04ID4L"  # FTDI names carry serial

# PORTS with the host board unplugged.
PORTS_NO_HOST = [
    row for row in PORTS_A if row.get("serial_number") != HOST_SERIAL
]

CONFIG = {
    "Default": "alpha",
    "Benches": {
        "alpha": [
            # Lowercase on purpose: serial matching is case-insensitive.
            {"Type": "Host", "ID": "Host", "Serial": HOST_SERIAL.lower(),
             "Location": "4-1.1"},
            {"Type": "Sniffer", "ID": "RX", "Serial": SNIFFER_RX_SERIAL,
             "Location": "4-1.2"},
            {"Type": "Sniffer", "ID": "TX", "Serial": SNIFFER_TX_SERIAL,
             "Location": "4-1.3.1"},
            {"Type": "Node", "ID": "30", "Serial": NODE30_SERIAL,
             "Location": "4-1.3.2"},
            {"Type": "Dongle", "ID": "RS485", "Serial": DONGLE_SERIAL,
             "Location": "5-1"},
        ],
        "beta": [
            {"Type": "Host", "ID": "Host", "Serial": BETA_HOST_SERIAL},
        ],
    },
}

SEED = [
    {"Type": "Host", "ID": "Host", "USB": "/dev/cu.usbmodem4111"},
    {"Type": "Sniffer", "ID": "RX", "USB": "/dev/cu.usbmodem4222"},
    {"Type": "Sniffer", "ID": "TX", "USB": "/dev/cu.usbmodem4333"},
    {"Type": "Node", "ID": "30", "USB": "/dev/cu.usbmodem4444"},
    {"Type": "Node", "ID": "31", "USB": "/dev/cu.usbmodem4000"},  # absent
]


@pytest.fixture
def fixtures(tmp_path):
    """Write the fixture config / enumerations / seed into a tmp dir."""
    paths = {
        "config": tmp_path / "bench.json",
        "ports": tmp_path / "ports_a.json",
        "ports_shuffled": tmp_path / "ports_shuffled.json",
        "ports_no_host": tmp_path / "ports_no_host.json",
        "seed": tmp_path / "seed.json",
    }
    paths["config"].write_text(json.dumps(CONFIG, indent=2))
    paths["ports"].write_text(json.dumps(PORTS_A))
    paths["ports_shuffled"].write_text(json.dumps(PORTS_SHUFFLED))
    paths["ports_no_host"].write_text(json.dumps(PORTS_NO_HOST))
    paths["seed"].write_text(json.dumps(SEED))
    return paths


def run_cli(*args, env_extra=None):
    """Run the bench CLI in a subprocess; return the CompletedProcess."""
    env = os.environ.copy()
    for var in ("CMRINET_BENCH", "CMRINET_BENCH_CONFIG", "BENCH_PORTS_FAKE"):
        env.pop(var, None)
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        [sys.executable, str(CLI), *[str(a) for a in args]],
        capture_output=True,
        text=True,
        env=env,
    )


def std_args(fixtures, ports="ports"):
    return ["--config", fixtures["config"], "--ports-file", fixtures[ports]]


# ----------------------------------------------------------------------
# resolve
# ----------------------------------------------------------------------

def test_resolve_quiet_prints_bare_path(fixtures):
    result = run_cli("resolve", "--role", "Host", *std_args(fixtures))
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem4111\n"
    assert result.stderr == ""


def test_resolve_survives_name_shuffle(fixtures):
    args = std_args(fixtures, ports="ports_shuffled")
    result = run_cli("resolve", "--role", "Host", *args)
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem9111\n"


def test_resolve_sniffer_requires_id(fixtures):
    result = run_cli("resolve", "--role", "Sniffer", *std_args(fixtures))
    assert result.returncode == 2
    assert "ambiguous" in result.stderr


def test_resolve_sniffer_and_node_by_id(fixtures):
    result = run_cli(
        "resolve", "--role", "Sniffer", "--id", "TX", *std_args(fixtures)
    )
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem4333\n"
    result = run_cli(
        "resolve", "--role", "Node", "--address", "30", *std_args(fixtures)
    )
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem4444\n"


def test_resolve_missing_device_complains_loudly(fixtures):
    args = std_args(fixtures, ports="ports_no_host")
    result = run_cli("resolve", "--role", "Host", *args)
    assert result.returncode == 3
    assert "no attached board reports it" in result.stderr
    assert "Attached candidates:" in result.stderr
    assert SNIFFER_RX_SERIAL in result.stderr  # helps the human fix the file
    assert result.stdout == ""


def test_resolve_unknown_role_is_config_error(fixtures):
    result = run_cli("resolve", "--role", "Widget", *std_args(fixtures))
    assert result.returncode == 2
    assert "role not in bench" in result.stderr


def test_resolve_role_and_id_are_case_insensitive(fixtures):
    # The user's bug report: `node 31` failed where `Node 31` worked.
    result = run_cli(
        "resolve", "--role", "node", "--id", "30", *std_args(fixtures)
    )
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem4444\n"
    result = run_cli(
        "resolve", "--role", "sniffer", "--id", "rx", *std_args(fixtures)
    )
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem4222\n"


def test_resolve_verbose_dumps_json_record(fixtures):
    result = run_cli("resolve", "--role", "Host", "-v", *std_args(fixtures))
    assert result.returncode == 0
    record = json.loads(result.stdout)
    assert record["serial"] == HOST_SERIAL
    assert record["device"] == "/dev/cu.usbmodem4111"
    assert record["location"] == "4-1.1"
    assert record["live_location"] == "4-1.1"
    assert record["vid"] == 0x303A


def test_ports_env_var_injects_enumeration(fixtures):
    result = run_cli(
        "resolve", "--role", "Host", "--config", fixtures["config"],
        env_extra={"BENCH_PORTS_FAKE": str(fixtures["ports"])},
    )
    assert result.returncode == 0
    assert result.stdout == "/dev/cu.usbmodem4111\n"


# ----------------------------------------------------------------------
# list
# ----------------------------------------------------------------------

def test_list_all_benches(fixtures):
    result = run_cli("list", "-a", "--config", fixtures["config"])
    assert result.returncode == 0
    assert "alpha" in result.stdout and "beta" in result.stdout
    payload = json.loads(run_cli(
        "list", "-a", "-o", "-", "--config", fixtures["config"]).stdout)
    assert payload == {"default": "alpha", "benches": ["alpha", "beta"]}


def test_list_all_quiet_bare_names(fixtures):
    result = run_cli("list", "-a", "-q", "--config", fixtures["config"])
    assert result.stdout == "alpha\nbeta\n"


def test_list_bench_reconciles_live_ports(fixtures):
    args = std_args(fixtures, ports="ports_shuffled")
    result = run_cli("list", "--bench", "alpha", "-o", "-", *args)
    assert result.returncode == 0
    roles = {r["Type"] + "/" + r["ID"]: r
             for r in json.loads(result.stdout)["roles"]}
    assert roles["Host/Host"]["Device"] == "/dev/cu.usbmodem9111"
    assert roles["Host/Host"]["Status"] == "OK"
    assert roles["Sniffer/RX"]["Device"] == "/dev/cu.usbmodem9222"


def test_list_marks_missing_roles(fixtures):
    args = std_args(fixtures, ports="ports_no_host")
    result = run_cli("list", "--bench", "alpha", *args)
    assert result.returncode == 0
    assert "MISSING" in result.stdout


def test_list_positional_bench_name(fixtures):
    result = run_cli("list", "beta", *std_args(fixtures))
    assert result.returncode == 0
    assert "MISSING" in result.stdout  # beta's host is not attached


# ----------------------------------------------------------------------
# check
# ----------------------------------------------------------------------

def test_check_all_present_no_verify(fixtures):
    result = run_cli("check", "--no-verify", *std_args(fixtures))
    assert result.returncode == 0
    assert "Host/Host" in result.stdout


def test_check_missing_role_exit_3(fixtures):
    args = std_args(fixtures, ports="ports_no_host")
    result = run_cli("check", "--no-verify", *args)
    assert result.returncode == 3
    assert "MISSING" in result.stdout


# ----------------------------------------------------------------------
# import
# ----------------------------------------------------------------------

def test_import_dry_run_enriches_without_writing(fixtures):
    result = run_cli(
        "import", fixtures["seed"], "--bench", "gamma", "--dry-run",
        "--no-verify", "-o", "-",
        "--ports-file", fixtures["ports"], "--config", fixtures["config"],
    )
    # Node 31 is absent from the enumeration: carried through, exit 3.
    assert result.returncode == 3
    assert "MISSING" in result.stderr
    enriched = json.loads(result.stdout)
    by_id = {(r["Type"], r["ID"]): r for r in enriched}
    assert by_id[("Host", "Host")]["Serial"] == HOST_SERIAL
    assert by_id[("Sniffer", "TX")]["Serial"] == SNIFFER_TX_SERIAL
    assert by_id[("Node", "31")]["Serial"] is None
    # Dry-run must not touch the config file.
    assert json.loads(fixtures["config"].read_text()) == CONFIG


def test_import_writes_new_config_and_default(tmp_path, fixtures):
    target = tmp_path / "newbench.json"
    result = run_cli(
        "import", fixtures["seed"], "--bench", "gamma", "--no-verify",
        "--ports-file", fixtures["ports"], "--config", target,
    )
    assert result.returncode == 3  # Node 31 absent
    doc = json.loads(target.read_text())
    assert doc["Default"] == "gamma"
    assert len(doc["Benches"]["gamma"]) == 5
    host = next(r for r in doc["Benches"]["gamma"] if r["Type"] == "Host")
    assert host["Serial"] == HOST_SERIAL
    assert host["Location"] == "4-1.1"


def test_import_refuses_clobber_without_force(fixtures):
    base = ["import", fixtures["seed"], "--bench", "alpha", "--no-verify",
            "--ports-file", fixtures["ports"], "--config", fixtures["config"]]
    result = run_cli(*base)
    assert result.returncode == 2
    assert "--force" in result.stderr
    result = run_cli(*base, "--force")
    assert result.returncode == 3  # writes now; exit 3 only for absent Node 31
    doc = json.loads(fixtures["config"].read_text())
    assert any(r["Type"] == "Node" and r["ID"] == "31"
               for r in doc["Benches"]["alpha"])


def test_import_preserves_other_sets(fixtures):
    result = run_cli(
        "import", fixtures["seed"], "--bench", "gamma", "--no-verify",
        "--ports-file", fixtures["ports"], "--config", fixtures["config"],
    )
    assert result.returncode == 3
    doc = json.loads(fixtures["config"].read_text())
    assert doc["Benches"]["beta"] == CONFIG["Benches"]["beta"]
    assert doc["Benches"]["alpha"] == CONFIG["Benches"]["alpha"]
    assert doc["Default"] == "alpha"  # pre-existing Default is not stolen


def test_import_rejects_structured_config_as_seed(fixtures):
    result = run_cli(
        "import", fixtures["config"], "--bench", "gamma", "--no-verify",
        "--ports-file", fixtures["ports"], "--config", fixtures["config"],
    )
    assert result.returncode == 2
    assert "not a seed" in result.stderr


# ----------------------------------------------------------------------
# export / seed / default
# ----------------------------------------------------------------------

def test_export_final_round_trips_stored_set(fixtures):
    result = run_cli(
        "export", "--bench", "alpha", "--config", fixtures["config"]
    )
    assert result.returncode == 0
    assert json.loads(result.stdout) == CONFIG["Benches"]["alpha"]


def test_export_seed_format_uses_live_devices(fixtures):
    result = run_cli(
        "export", "--bench", "alpha", "--format", "seed",
        *std_args(fixtures, ports="ports_shuffled"),
    )
    assert result.returncode == 0
    by_id = {(r["Type"], r["ID"]): r for r in json.loads(result.stdout)}
    assert by_id[("Host", "Host")]["USB"] == "/dev/cu.usbmodem9111"
    assert "Serial" not in by_id[("Host", "Host")]  # seed shape has no Serial


def test_seed_template_lists_candidates_only(fixtures):
    result = run_cli("seed", "--ports-file", fixtures["ports"])
    assert result.returncode == 0
    template = json.loads(result.stdout)
    devices = [row["USB"] for row in template]
    assert "/dev/cu.usbmodem4111" in devices
    assert "/dev/cu.usbserial-BG04ID4L" in devices
    assert "/dev/cu.Bluetooth-Incoming-Port" not in devices
    assert all(row["Type"] == "" for row in template)  # human fills roles


def test_default_get_and_set(fixtures):
    result = run_cli("default", "--config", fixtures["config"])
    assert result.returncode == 0
    assert result.stdout == "alpha\n"
    result = run_cli("default", "beta", "--config", fixtures["config"])
    assert result.returncode == 0
    assert json.loads(fixtures["config"].read_text())["Default"] == "beta"
    result = run_cli("default", "nope", "--config", fixtures["config"])
    assert result.returncode == 2


# ----------------------------------------------------------------------
# config handling / errors
# ----------------------------------------------------------------------

def test_malformed_config_is_config_error(tmp_path, fixtures):
    bad = tmp_path / "bad.json"
    bad.write_text("{not json")
    result = run_cli("list", "-a", "--config", bad)
    assert result.returncode == 2
    assert "not valid JSON" in result.stderr


def test_legacy_flat_config_points_at_import(fixtures):
    result = run_cli("list", "-a", "--config", fixtures["seed"])
    assert result.returncode == 2
    assert "bench import" in result.stderr


def test_multi_bench_without_default_needs_selection(fixtures):
    config = json.loads(fixtures["config"].read_text())
    del config["Default"]
    fixtures["config"].write_text(json.dumps(config))
    result = run_cli("resolve", "--role", "Host", *std_args(fixtures))
    assert result.returncode == 2
    assert "none was selected" in result.stderr
    # ...and the env var resolves the ambiguity.
    result = run_cli(
        "resolve", "--role", "Host", *std_args(fixtures),
        env_extra={"CMRINET_BENCH": "alpha"},
    )
    assert result.returncode == 0


def test_output_to_file_json_and_csv(fixtures, tmp_path):
    json_out = tmp_path / "roles.json"
    csv_out = tmp_path / "roles.csv"
    result = run_cli(
        "list", "--bench", "alpha", "-o", json_out, *std_args(fixtures)
    )
    assert result.returncode == 0
    assert json.loads(json_out.read_text())["bench"] == "alpha"
    result = run_cli(
        "list", "--bench", "alpha", "-o", csv_out, *std_args(fixtures)
    )
    assert result.returncode == 0
    header = csv_out.read_text().splitlines()[0]
    assert header == "Type,ID,Serial,Location,Device,Status"
