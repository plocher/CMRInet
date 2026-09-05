import sys
import time
import json
import serial
import threading
import subprocess
from pathlib import Path
from typing import Callable, Optional
import _gap_deltas

# Ports resolve through the #68 bench tooling (`extras/bench/bench`
# first, `bench_ports` fallback): roles key on USB serial in
# extras/bench/bench.json, so enumeration shuffles no longer matter.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import bench_ports

BENCH_CLI = Path(__file__).resolve().parents[2] / "bench"


def _resolve_role_with_bench_cli(role: str, role_id: Optional[str] = None) -> str:
    """Resolve a bench role by calling extras/bench/bench resolve."""
    command = [str(BENCH_CLI), "resolve", "--role", role]
    if role_id is not None:
        command.extend(["--id", role_id])
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )
    resolved = completed.stdout.strip()
    if not resolved:
        raise RuntimeError(
            f"bench resolve returned empty output for role={role} id={role_id}"
        )
    return resolved


def _resolve_role(role: str, role_id: Optional[str] = None) -> str:
    """Resolve a bench role, preferring bench CLI and falling back to module API."""
    try:
        return _resolve_role_with_bench_cli(role, role_id)
    except Exception:
        return bench_ports.resolve_or_exit(role, role_id)


def host_port() -> str:
    """Live device for the bench Host role."""
    return _resolve_role("Host")


def sniffer_tx_port() -> str:
    """Live device for the Sniffer/TX (poll-pair) role."""
    return _resolve_role("Sniffer", "TX")


def sniffer_rx_port() -> str:
    """Live device for the Sniffer/RX (reply-pair) role."""
    return _resolve_role("Sniffer", "RX")


DEFAULT_REAL_UA = 30
DEFAULT_PHANTOM_UA = 31
DEFAULT_NODE_ADDRESSES = (DEFAULT_REAL_UA, DEFAULT_PHANTOM_UA)
DEFAULT_LOOPBACK_BYTE = 2
DEFAULT_LOOPBACK_BIT = 1
# Walker defaults: no speed presets -- walk speed carries no diagnostic
# meaning now that #47 is fixed; any period in the 250-1000ms band gives
# a visually identifiable pattern on the bench LEDs.
DEFAULT_WALKER_PERIOD_MS = 500
DEFAULT_WALKER_BYTE = 3
DEFAULT_WALKER_INVERT = 0


def _write_command(ser, cmd: bytes, delay_s: float = 0.1) -> None:
    ser.write(cmd)
    time.sleep(delay_s)


def send_command(ser, command: str, delay_s: float = 0.1) -> None:
    """Send one plain-text verb line to the tracer shell."""
    _write_command(ser, f"{command}\n".encode("utf-8"), delay_s=delay_s)


def send_generator_command(
    ser,
    action: str,
    generator: str,
    ua: Optional[int] = None,
    extra_args: str = "",
    delay_s: float = 0.1,
) -> None:
    """Send a generator control verb with optional UA targeting."""
    cmd = f"{action} {generator}"
    if ua is not None:
        cmd += f" ua {ua}"
    if extra_args:
        cmd += f" {extra_args}"
    _write_command(ser, f"{cmd}\n".encode("utf-8"), delay_s=delay_s)


def configure_loopback_write_read(
    ser,
    ua: int,
    src_byte: int = DEFAULT_LOOPBACK_BYTE,
    src_bit: int = DEFAULT_LOOPBACK_BIT,
    dst_byte: int = DEFAULT_LOOPBACK_BYTE,
    dst_bit: int = DEFAULT_LOOPBACK_BIT,
    delay_s: float = 0.1,
) -> None:
    """Configure loopback as write(read()) with explicit src/dst byte+bit."""
    extra = (
        "mode write_read "
        f"src_byte {src_byte} src_bit {src_bit} "
        f"dst_byte {dst_byte} dst_bit {dst_bit}"
    )
    send_generator_command(
        ser,
        "configure",
        "toggleoutfrominput",
        ua=ua,
        extra_args=extra,
        delay_s=delay_s,
    )

def _readline_text(ser, raw_line_sink: Optional[Callable[[bytes], None]] = None) -> str:
    try:
        raw_line = ser.readline()
    except StopIteration:
        return ""
    if raw_line and raw_line_sink is not None:
        raw_line_sink(raw_line)
    return raw_line.decode("utf-8", errors="replace").strip()


def wait_for_sustained_quiet(ser, max_wait_s: float = 30.0, quiet_window_s: float = 2.0,
                             sample_limit: int = 0, drain_first: bool = True,
                             line_sink: Optional[Callable[[str], None]] = None,
                             raw_line_sink: Optional[Callable[[bytes], None]] = None) -> bool:
    print("Waiting for sustained bus quiet...")
    if drain_first:
        flush_lines(ser)
    deadline = time.time() + max_wait_s
    quiet_start = None
    activity_samples = 0
    while time.time() < deadline:
        line = _readline_text(ser, raw_line_sink=raw_line_sink)
        if line:
            if line_sink is not None:
                line_sink(line)
            if activity_samples < sample_limit:
                print(f"  activity: {line}")
                activity_samples += 1
            quiet_start = None
            continue
        if quiet_start is None:
            quiet_start = time.time()
            continue
        if time.time() - quiet_start >= quiet_window_s:
            print("Bus is quiet.")
            return True
    print("ERROR: Bus did not become quiet before timeout.")
    return False

def quiesce_traffic_preserving_ring(ser, node_addresses=DEFAULT_NODE_ADDRESSES,
                                    max_wait_s: float = 30.0, quiet_window_s: float = 2.0,
                                    line_sink: Optional[Callable[[str], None]] = None,
                                    raw_line_sink: Optional[Callable[[bytes], None]] = None) -> bool:
    for ua in node_addresses:
        send_generator_command(ser, "disable", "walker", ua=ua)
        send_generator_command(ser, "disable", "toggleoutfrominput", ua=ua)
    for ua in node_addresses:
        _write_command(ser, f"node disable {ua}\n".encode("utf-8"))
    return wait_for_sustained_quiet(
        ser,
        max_wait_s=max_wait_s,
        quiet_window_s=quiet_window_s,
        drain_first=False,
        line_sink=line_sink,
        raw_line_sink=raw_line_sink,
    )


def _capture_serial_stream(port_name: str, out_file: Path, stop_event: threading.Event,
                           errors: list[str], label: str) -> None:
    ser = None
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
        with out_file.open("wb") as handle:
            while not stop_event.is_set():
                waiting = ser.in_waiting
                if waiting > 0:
                    data = ser.read(waiting)
                    if data:
                        handle.write(data)
                        handle.flush()
                else:
                    time.sleep(0.005)
    except Exception as exc:
        errors.append(f"{label} capture failed on {port_name}: {exc}")
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass


def shutdown_and_verify_quiet(ser, node_addresses=DEFAULT_NODE_ADDRESSES,
                              max_wait_s: float = 30.0,
                              quiet_window_s: float = 2.0,
                              line_sink: Optional[Callable[[str], None]] = None,
                              raw_line_sink: Optional[Callable[[bytes], None]] = None) -> bool:
    ser.write(b"display 2 " + "resetting".encode("utf-8") + b"\n")
    _write_command(ser, b"reset\n", delay_s=0.5)
    flush_lines(ser)
    print("Host quiesced.")

    for ua in node_addresses:
        send_generator_command(ser, "disable", "walker", ua=ua)
        send_generator_command(ser, "disable", "toggleoutfrominput", ua=ua)
    for ua in node_addresses:
        _write_command(ser, f"node disable {ua}\n".encode("utf-8"))
    return wait_for_sustained_quiet(
        ser,
        max_wait_s=max_wait_s,
        quiet_window_s=quiet_window_s,
        line_sink=line_sink,
        raw_line_sink=raw_line_sink,
    )

def reboot_and_reconnect(port, timeout=10.0):
    print(f"Rebooting host on {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        ser.write(b"reboot\n")
        ser.flush()
        time.sleep(0.5)
        ser.close()
    except Exception:
        pass
        
    # Wait for the port to drop and reappear
    time.sleep(2.0)
    
    start = time.time()
    while time.time() - start < timeout:
        try:
            # Busy loop trying to open the port
            ser = serial.Serial(port, 115200, timeout=0.5)
            print("Port re-enumerated.")
            time.sleep(2.0) # Allow Arduino to settle after CDC open
            return ser
        except Exception:
            time.sleep(0.5)
            
    raise serial.SerialException("Failed to reconnect after reboot")



def read_host_status_snapshot(ser, timeout_s: float = 8.0) -> Optional[dict]:
    """Read the host-scope status bundle from the tracer shell.

    Bare `status` emits:
      event=status     host counters + degraded ledger + identity
      event=roster     live node table
      event=generator  zero or more lines, one per enabled generator
                        service instance (walker/toggle/stall)

    The generator lines are a diagnostic tail of unknown length (one per
    currently-enabled service), so this reader does not wait on them:
    it returns as soon as status+roster are both seen. Any "generator"
    lines still on the wire are harmless -- later reads that only match
    on "status"/"roster"/a specific UA simply skip past them. Older
    firmware emitted one monolithic line with status+roster combined;
    that shape still works here too.
    """
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    ser.write(b"status\n")
    deadline = time.time() + timeout_s
    status_doc: Optional[dict] = None
    roster_doc: Optional[dict] = None

    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line or not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        event = doc.get("event")
        # Node-scope status lines also say role=host; they carry a top-level
        # "ua". Host-scope status has no top-level ua (only roster entries do).
        if event == "status" and "ua" not in doc:
            status_doc = doc
            # Legacy single-line shape already carries the roster.
            if isinstance(doc.get("roster"), list):
                return doc
        elif event == "roster":
            roster_doc = doc
        if status_doc is not None and roster_doc is not None:
            merged = dict(status_doc)
            if "roster" in roster_doc:
                merged["roster"] = roster_doc["roster"]
            return merged
    # Prefer a partial status line (ledger/identity) over nothing: the
    # degraded-lane analyzer can still score grants/denials without a
    # roster, and dual-node falls back to per-node status for states.
    if status_doc is not None and roster_doc is not None:
        merged = dict(status_doc)
        if "roster" in roster_doc:
            merged["roster"] = roster_doc["roster"]
        return merged
    return status_doc


def read_node_status(ser, ua: int, timeout_s: float = 3.0) -> Optional[dict]:
    """Read one per-node status JSON payload from the tracer shell."""
    ser.write(f"status {ua}\n".encode("utf-8"))
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if doc.get("event") != "status":
            continue
        if doc.get("ua") != ua:
            continue
        return doc
    return None


def sync_and_validate_boot(ser, timeout=15.0):
    # Send a status to ask for identity directly (works on fresh boot without nodes).
    # Prefer the merged bundle reader so split status lines still yield image/version.
    snapshot = read_host_status_snapshot(ser, timeout_s=min(timeout, 8.0))
    if snapshot is not None:
        image = snapshot.get("image")
        version = snapshot.get("version")
        if image == "tracer_host" and version:
            ver_parts = tuple(int(x) for x in str(version).split("."))
            if ver_parts >= (0, 4, 0):
                print(f"Verified boot: {image} v{version}")
                return True
            print(
                f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.",
                file=sys.stderr,
            )
            sys.exit(1)
        if image is not None and image != "tracer_host":
            print(
                f"ERROR: Expected image 'tracer_host', got '{image}'. Check flash.",
                file=sys.stderr,
            )
            sys.exit(1)

    # Fallback: scan the stream for identity on status/epoch (or truncated lines).
    start = time.time()
    while time.time() - start < timeout:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        print(f"BOOT: {line}")
        if not line.startswith("{"):
            continue
        try:
            doc = json.loads(line)
            event = doc.get("event")
            if event not in ("status", "epoch"):
                continue
            image = doc.get("image")
            version = doc.get("version")
        except json.JSONDecodeError:
            # Truncated lines are possible under CDC backpressure; only parse
            # identity if this line is clearly status/epoch scoped.
            if '"event":"status"' not in line and '"event":"epoch"' not in line:
                continue
            import re
            img_match = re.search(r'\"image\":\"([^\"]+)\"', line)
            ver_match = re.search(r'\"version\":\"([^\"]+)\"', line)
            image = img_match.group(1) if img_match else None
            version = ver_match.group(1) if ver_match else None

        if image != "tracer_host":
            print(f"ERROR: Expected image 'tracer_host', got '{image}'. Check flash.", file=sys.stderr)
            sys.exit(1)
        if not version:
            print(f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.", file=sys.stderr)
            sys.exit(1)
        ver_parts = tuple(int(x) for x in version.split("."))
        if ver_parts < (0, 4, 0):
            print(f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.", file=sys.stderr)
            sys.exit(1)
        print(f"Verified boot: {image} v{version}")
        return True
    return False

def flush_lines(ser):
    # drain any remaining data
    ser.reset_input_buffer()

def run_combo(ser, s, p, mode, traffic, secs, out_dir, tag,
              capture_sniffers: bool = False,
              sniffer_tx_port: Optional[str] = None,
              sniffer_rx_port: Optional[str] = None,
              phantom_ua: int = DEFAULT_PHANTOM_UA,
              walker_period_ms: int = DEFAULT_WALKER_PERIOD_MS,
              walker_byte: int = DEFAULT_WALKER_BYTE,
              walker_invert: int = DEFAULT_WALKER_INVERT):
    print(f"\n--- Running combo: stall={s}ms period={p}ms mode={mode} ---")
    log_file = out_dir / f"{tag}.log"
    host_raw_file = out_dir / f"packets.{tag}.Host.raw"
    tx_raw_file = out_dir / f"packets.{tag}.TX.raw"
    rx_raw_file = out_dir / f"packets.{tag}.RX.raw"

    sniffer_stop_event = None
    sniffer_threads: list[threading.Thread] = []
    sniffer_errors: list[str] = []
    if capture_sniffers:
        tx_port = sniffer_tx_port or _resolve_role("Sniffer", "TX")
        rx_port = sniffer_rx_port or _resolve_role("Sniffer", "RX")
        tx_raw_file.touch()
        rx_raw_file.touch()
        sniffer_stop_event = threading.Event()
        sniffer_threads = [
            threading.Thread(
                target=_capture_serial_stream,
                args=(tx_port, tx_raw_file, sniffer_stop_event,
                      sniffer_errors, "TX"),
                daemon=True,
            ),
            threading.Thread(
                target=_capture_serial_stream,
                args=(rx_port, rx_raw_file, sniffer_stop_event,
                      sniffer_errors, "RX"),
                daemon=True,
            ),
        ]
        for thread in sniffer_threads:
            thread.start()

    def _stop_sniffers() -> None:
        if sniffer_stop_event is None:
            return
        sniffer_stop_event.set()
        for thread in sniffer_threads:
            thread.join(timeout=2.0)

    def _apply_sniffer_status(result: _gap_deltas.AnalyzerResult
                              ) -> _gap_deltas.AnalyzerResult:
        if capture_sniffers and sniffer_errors:
            for err in sniffer_errors:
                print(f"ERROR: {err}", file=sys.stderr)
            result.verdict = "ERROR_SNIFFER_CAPTURE"
        return result

    with host_raw_file.open("wb") as host_raw:
        def _host_raw_sink(raw_line: bytes) -> None:
            host_raw.write(raw_line)
            host_raw.flush()
        def _host_marker(phase: str, **extra_fields) -> None:
            payload = {"event": "harness_marker", "phase": phase}
            payload.update(extra_fields)
            host_raw.write((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))
            host_raw.flush()

        _host_marker(
            "combo_start",
            tag=tag,
            stall_ms=s,
            period_ms=p,
            mode=mode,
            traffic=traffic,
            walker_period_ms=walker_period_ms,
            walker_byte=walker_byte,
            walker_invert=walker_invert,
            secs=secs,
        )

        # Send commands
        ser.write(b"reset\n")
        time.sleep(0.1)
        flush_lines(ser)
        for ua in DEFAULT_NODE_ADDRESSES:
            _write_command(ser, f"node enable {ua}\n".encode("utf-8"))
        flush_lines(ser)

        # Traffic
        dstring = ""
        dstringsep = "t:"
        if "walker" in traffic:
            cmd = (
                f"enable walker UA 30 period {walker_period_ms} "
                f"byte {walker_byte} invert {walker_invert}\n"
            )
            ser.write(cmd.encode("utf-8"))
            dstring += f"{dstringsep}w"
            dstringsep = ", "
            time.sleep(0.1)
        if "loopback" in traffic:
            ser.write(b"enable toggleoutfrominput UA 30\n")
            dstring += f"{dstringsep}l"
            dstringsep = ", "
            time.sleep(0.1)
        if dstring:
            ser.write(f"display 1 {dstring}\n".encode("utf-8"))
            time.sleep(0.1)
        if s > 0:
            cmd = f"enable stall {s} period {p} mode {mode}\n"
            ser.write(cmd.encode("utf-8"))
            dstring = f"Run: stall:{s}/{p}/{mode}"
            ser.write(b"display 2 " + dstring.encode("utf-8") + b"\n")
            time.sleep(0.1)

        flush_lines(ser)

        # Run
        time.sleep(0.1)
        cmd = f"run {secs}\n"
        ser.write(cmd.encode("utf-8"))
        _host_marker("run_command_sent")

        print(f"Waiting for END CAPTURE marker (secs={secs})...")
        deadline = time.time() + secs + 5.0
        end_capture_seen = False

        while time.time() < deadline:
            line = _readline_text(ser, raw_line_sink=_host_raw_sink)
            if not line:
                continue
            if line.startswith("END CAPTURE"):
                end_capture_seen = True
                _host_marker("end_capture_marker_seen", marker=line)
                print(f"Seen run-window marker: {line}")
                break

        if not end_capture_seen:
            print(f"ERROR_TIMEOUT: END CAPTURE not seen for {tag}")
            _host_marker("end_capture_marker_timeout")
            ser.write(b"display 2 " + "run ERROR_TIMEOUT".encode("utf-8") + b"\n")
            time.sleep(0.1)
            shutdown_and_verify_quiet(
                ser,
                raw_line_sink=_host_raw_sink,
            )
            timeout_res = _gap_deltas.AnalyzerResult(
                verdict="ERROR_TIMEOUT", phantom_ua=-1, first_t_ms=-1, last_t_ms=-1
            )
            _host_marker("combo_complete", verdict=timeout_res.verdict)
            _stop_sniffers()
            return _apply_sniffer_status(timeout_res)

        ser.write(b"display 2 " + "run complete".encode("utf-8") + b"\n")
        time.sleep(0.1)

        _host_marker("quiesce_start")
        quiet_ok = quiesce_traffic_preserving_ring(
            ser,
            raw_line_sink=_host_raw_sink,
        )
        _host_marker("quiesce_complete", quiet_ok=quiet_ok)

        # Dump after the bus is quiet so all scenario traffic has been processed.
        _host_marker("dump_start")
        ser.write(b"display 2 " + "dumping ring".encode("utf-8") + b"\n")
        ser.write(b"dump\n")
        print("Dumping ring...")
        deadline = time.time() + 10.0

        dump_lines = []
        in_dump = False
        dump_end_seen = False

        while time.time() < deadline:
            line = _readline_text(ser, raw_line_sink=_host_raw_sink)
            if not line:
                continue

            if line.startswith("BEGIN DUMP"):
                in_dump = True
                _host_marker("dump_begin_seen", marker=line)
            elif line == "END DUMP":
                dump_end_seen = True
                _host_marker("dump_complete")
                break
            elif in_dump:
                dump_lines.append(line)

        if not dump_end_seen:
            _host_marker("dump_timeout")

        print(f"Captured {len(dump_lines)} lines for {tag}")
        ser.write(b"display 1 " + f"dumped {len(dump_lines)} lines".encode("utf-8") + b"\n")
        ser.write(b"display 2 " + "quiesce complete".encode("utf-8") + b"\n")

        import datetime
        with log_file.open("w") as f:
            f.write(f"# CMRI Tracer Capture\n")
            f.write(f"# tag: {tag}\n")
            f.write(f"# stall_ms: {s}\n")
            f.write(f"# period_ms: {p}\n")
            f.write(f"# mode: {mode}\n")
            f.write(f"# traffic: {traffic}\n")
            f.write(
                f"# walker: period_ms={walker_period_ms} "
                f"byte={walker_byte} invert={walker_invert}\n"
            )
            f.write(f"# secs: {secs}\n")
            f.write(f"# timestamp: {datetime.datetime.now().isoformat()}\n")
            for line in dump_lines:
                f.write(line + "\n")

        res = _gap_deltas.analyze_lines(dump_lines, phantom_ua=phantom_ua)
        if not quiet_ok:
            res.verdict = "ERROR_NOT_QUIET"
        _host_marker("combo_complete", verdict=res.verdict, dump_lines=len(dump_lines))

    _stop_sniffers()
    return _apply_sniffer_status(res)

