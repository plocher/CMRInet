#!/usr/bin/env python3
"""Sketch warning gate for the CMRInet library (issue #93).

Why this exists
---------------
`src/` is gated: `tests/Makefile` and `extras/desktop/Makefile` compile it
under `-Wall -Wextra -Werror`. `examples/` is not, and the ESP32 core
actively suppresses warnings for the sketch translation unit -- the flags
reaching `SimpleHost.ino.cpp` are `... -w -Werror=return-type -w`, and the
last `-w` wins. No `arduino-cli` invocation switches warnings back on
(`--warnings all` is ignored by the core recipe; `compiler.cpp.extra_flags`
gets `-w`'d after it). So an unhandled enumerator, or any ordinary warning,
in an example sketch ships silently. `RemoteNodeState` grew
`kMisconfigured`/`kDegraded` under #84 and two sketch copies of the switch
rotted to "??" unnoticed for exactly this reason.

What it does
------------
For each sketch, under each of its FQBNs, it harvests the real
cross-compiler commands -- compiler, board defines, include paths -- via
`arduino-cli compile --only-compilation-database`, then recompiles every
sketch-folder translation unit (the concatenated `.ino.cpp` plus each
support `.cpp` such as `iox.cpp` / `display.cpp`) compile-only with
warnings rebound to *our* code:

  * drop `-w` (the suppression) and the core's `-Werror=return-type`;
  * add `-Wall -Wextra -Werror -Wswitch`;
  * demote every third-party include dir to `-isystem` (esp32 core, tools
    libs, Wire/SPI, Adafruit_GFX/BusIO/SSD1306) so their header noise is
    suppressed -- except under `arduino:avr` FQBNs, where the demotion
    itself breaks the core (see fqbn_demotes_includes);
  * keep the two CMRInet include dirs on `-I` (`examples/<sketch>` and
    `src`) so our code stays gated;
  * keep all board `-D` defines verbatim -- notably
    `-DARDUINO_USB_CDC_ON_BOOT=1`, which HANDOFF.md warns is clobbered if
    you override `build.extra_flags`. Harvesting instead of overriding
    means we never touch it;
  * `-o /dev/null` (compile only, no link, no artifact kept).

`-Wswitch` makes an unhandled enumerator in a sketch a hard error; `-Wall
-Wextra` catch the broader class of "next unrelated warnings" that is this
gate's real destination. The sketch TU is the preprocessed `.ino.cpp`, so
the sketch's own code is the main translation unit -- not a system header --
and its switches are checked.

Per-sketch FQBNs live in SKETCH_FQBNS: ProMiniSMININode is dual-target
(one sketch, two boards) and lints under both `esp32:esp32:XIAO_ESP32C6`
and `arduino:avr:pro:cpu=16MHzatmega328`, so both arch branches are gated.
Sketches not in the map lint under the default ESP32 FQBN.

Usage
-----
  extras/sketch_lint.py                # lint all sketches
  extras/sketch_lint.py SimpleHost      # lint one sketch

Environment
-----------
  ARDUINO_CLI   default `arduino-cli` (resolved on PATH)
  LIBS_DIR      default `~/Dropbox/Arduino/libraries`
  FQBN          when set, overrides every FQBN including the per-sketch
                map. Unset: each sketch uses its SKETCH_FQBNS entry or
                the default `esp32:esp32:XIAO_ESP32C6`.

Exit code is non-zero if any sketch fails the lint. Pure stdlib; no deps.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The sketches the gate covers. Add new examples here.
# Front-door examples live under examples/. Single-use bench jigs live under
# extras/bench/. The gate accepts either a bare name (resolved below) or a
# repo-relative path such as extras/bench/XiaoBenchCal.
DEFAULT_SKETCHES = (
    "SimpleHost",
    "SimpleNode",
    "TracerHost",
    "XiaoSniffer",
    "TracerNode",
    "XiaoNode",
    "ProMiniSMININode",
    "extras/bench/XiaoBenchCal",
    "extras/bench/XiaoBenchEcho",
    "extras/bench/XiaoBenchEchoCancel",
)

# Per-sketch FQBN map. A sketch listed here lints once under each of its
# FQBNs; every other sketch lints under the default. ProMiniSMININode is
# dual-target: one sketch, two boards (cpNode-Xiao ESP32-C6 and
# cpNode-ProMini ATmega328P), so both arch branches are gated.
DEFAULT_FQBN = "esp32:esp32:XIAO_ESP32C6"
SKETCH_FQBNS = {
    "ProMiniSMININode": (
        "esp32:esp32:XIAO_ESP32C6",
        "arduino:avr:pro:cpu=16MHzatmega328",
    ),
}

# Third-party libraries the OLED sketches pull in. Passing them
# unconditionally is harmless: arduino-cli only makes them available, and an
# unused library does not error. Kept in sync with flash_and_probe.sh.
THIRD_PARTY_LIBS = (
    "Adafruit_GFX_Library",
    "Adafruit_SSD1306",
    "Adafruit_BusIO",
)

# Warning flags the gate imposes. `-Wswitch` is the one that catches an
# unhandled enumerator; `-Wall -Wextra` widen the net to the next unrelated
# warning.
LINT_WARNINGS = ["-Wall", "-Wextra", "-Werror", "-Wswitch"]

# Flags to strip from the harvested command. `-w` is the core's suppression
# (last-wins, so it would cancel our warnings); `-Werror=return-type` is the
# core's own promoted warning, kept out so our `-Werror` is the single
# warning policy on this TU.
#
# Dependency-generation flags (`-MMD`, `-MD`, `-MP`) are stripped too: the
# harvest carries them so arduino-cli can track header changes, but a lint
# compile owes no dep file, and with `-o /dev/null` the derived name
# `/dev/null.d` is not writable (`fatal error: opening dependency file
# /dev/null.d`). Dropping them is the clean fix.
DROP_FLAGS = {"-w", "-Werror=return-type", "-MMD", "-MD", "-MP"}


def env_str(name: str, default: str) -> str:
    val = os.environ.get(name)
    return val if val else default


def find_arduino_cli() -> str:
    """The arduino-cli to drive the compile-DB harvest with."""
    cli = env_str("ARDUINO_CLI", "arduino-cli")
    if cli == "arduino-cli":
        resolved = shutil.which("arduino-cli")
        if resolved:
            return resolved
    return cli


def library_args(repo: Path, libs_dir: Path) -> list[str]:
    args = [f"--library={repo}"]
    for lib in THIRD_PARTY_LIBS:
        args.append(f"--library={libs_dir / lib}")
    return args


def is_ours(path: Path) -> bool:
    """An include dir is 'ours' if it lives inside the repo tree.

    That covers `src`, `examples/<sketch>`, and `extras/bench/<sketch>` --
    the code this gate is meant to bind to. Everything else (esp32 core, tools, Wire/SPI, the
    Adafruit libs) is third-party and becomes `-isystem`.
    """
    try:
        path.resolve().relative_to(REPO)
        return True
    except ValueError:
        return False


def is_under(path: Path, parent: Path) -> bool:
    """True when `path` resolves to a location inside `parent`.

    Used on the harvested TU paths, which live under the temporary build
    dir (arduino-cli compiles the sketch from prepared copies, and on
    macOS the tempdir `/var` prefix resolves to `/private/var`).
    """
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def rewrite_args(args: list[str], demote: bool = True) -> list[str]:
    """Rewrite a harvested compile command into the lint command.

    The compiler (args[0]) is kept. Each `-I<path>` is kept on `-I` if it
    is ours, else -- when `demote` -- demoted to `-isystem <path>`. With
    `demote` false every `-I` passes through verbatim (the AVR
    exception; see fqbn_demotes_includes). Warning flags are dropped
    and replaced with LINT_WARNINGS. The `-o <file>` target becomes
    `/dev/null`. Everything else (defines, `-std=`, `-c`, `-Os`, `-f*`,
    `-m*`, ...) passes through verbatim.
    """
    if not args:
        return args
    compiler = args[0]
    rest = args[1:]

    out: list[str] = [compiler]
    # Track whether we have already inserted our warning flags, so they land
    # once, in a stable position (right after the compiler). Their actual
    # position does not affect gcc semantics.
    out.extend(LINT_WARNINGS)

    i = 0
    while i < len(rest):
        tok = rest[i]
        if tok in DROP_FLAGS:
            i += 1
            continue
        if tok == "-o":
            # Skip the output filename too; we set our own.
            i += 2
            continue
        if tok.startswith("-o") and len(tok) > 2:
            # `-ofoo` form; drop just this token.
            i += 1
            continue
        if tok.startswith("-I") and len(tok) > 2:
            # `-Ipath` form.
            inc = Path(tok[2:])
            if is_ours(inc) or not demote:
                out.append(tok)
            else:
                out.append("-isystem")
                out.append(str(inc))
            i += 1
            continue
        if tok == "-I":
            # `-I path` form (two tokens).
            inc = Path(rest[i + 1])
            if is_ours(inc) or not demote:
                out.append(tok)
                out.append(rest[i + 1])
            else:
                out.append("-isystem")
                out.append(rest[i + 1])
            i += 2
            continue
        out.append(tok)
        i += 1

    out.append("-o")
    out.append("/dev/null")
    return out


def resolve_sketch_ino(sketch: str) -> Path | None:
    """Resolve a sketch name or repo-relative path to its .ino file.

    Accepts:
      - bare example name: SimpleHost -> examples/SimpleHost/SimpleHost.ino
      - bare bench name: XiaoBenchCal -> extras/bench/XiaoBenchCal/...
      - explicit path: extras/bench/XiaoBenchCal or examples/TracerHost
    """
    sketch = sketch.strip().rstrip("/")
    candidates: list[Path] = []
    p = Path(sketch)
    if p.suffix == ".ino":
        candidates.append(REPO / p)
    else:
        name = p.name
        candidates.append(REPO / p / f"{name}.ino")
        candidates.append(REPO / "examples" / name / f"{name}.ino")
        candidates.append(REPO / "extras" / "bench" / name / f"{name}.ino")
    for c in candidates:
        if c.is_file():
            return c
    return None


def fqbns_for(sketch_name: str, env_fqbn: str | None) -> tuple[str, ...]:
    """The FQBNs to lint one sketch under.

    `FQBN` in the environment overrides everything (all sketches, one
    FQBN). Otherwise the per-sketch map decides, with the default ESP32
    FQBN as the fallback.
    """
    if env_fqbn:
        return (env_fqbn,)
    return SKETCH_FQBNS.get(sketch_name, (DEFAULT_FQBN,))


def fqbn_demotes_includes(fqbn: str) -> bool:
    """True when the lint may demote third-party include dirs to -isystem.

    The AVR core must stay on -I. avr-g++ 7.3.0-atmel3.6.1-arduino7
    misparses the AVR core's C++ headers (Arduino.h, WString.h) when
    they arrive via -isystem: every declaration takes C linkage, and
    the core fails its own compile with "conflicting declaration of C
    function" for atexit, random, makeWord, and the StringSumHelper
    operator+ overloads (ArduinoCore-avr issue #475; any build system
    passing the core on -isystem reproduces it). The demotion also
    buys nothing on AVR: the core and its bundled library headers
    compile warning-free under the gate's flags, so keeping them on
    -I costs no third-party noise.
    """
    return not fqbn.startswith("arduino:avr")


def lint_tu(entry: dict, demote: bool = True) -> tuple[bool, str]:
    """Recompile one harvested TU under the gate's warning policy.

    `demote` controls the third-party -isystem demotion (false for
    AVR FQBNs; see fqbn_demotes_includes).
    """
    args = entry.get("arguments")
    if not args:
        # 'command' (string) form -- not produced by arduino-cli 1.5.1,
        # but handle it defensively by shlex-splitting.
        import shlex

        cmd_str = entry.get("command", "")
        args = shlex.split(cmd_str)
    if not args:
        return False, "TU entry has no arguments"

    lint_args = rewrite_args(args, demote)
    workdir = entry.get("directory", str(REPO))
    try:
        lint_proc = subprocess.run(
            lint_args, capture_output=True, text=True, check=False, cwd=workdir
        )
    except FileNotFoundError:
        return False, f"cross-compiler not found: {lint_args[0]}"

    if lint_proc.returncode == 0:
        return True, "ok"
    # Surface the compiler's own diagnostics; strip trailing whitespace.
    diag = (lint_proc.stderr + lint_proc.stdout).strip()
    return False, diag


def lint_sketch(
    sketch: str, cli: str, libs_dir: Path, env_fqbn: str | None
) -> list[tuple[str, str, bool, str]]:
    """Lint one sketch under each of its FQBNs.

    Returns one (fqbn, tu_name, passed, detail) per sketch-folder
    translation unit per FQBN.
    """
    ino = resolve_sketch_ino(sketch)
    if ino is None:
        return [("-", sketch, False, f"sketch not found: {sketch}")]
    sketch_name = ino.stem

    results: list[tuple[str, str, bool, str]] = []
    for fqbn in fqbns_for(sketch_name, env_fqbn):
        with tempfile.TemporaryDirectory(prefix=f"sketchlint_{sketch_name}_") as tmp:
            build_dir = Path(tmp)
            cmd = [
                cli,
                "compile",
                "--only-compilation-database",
                "--fqbn",
                fqbn,
                *library_args(REPO, libs_dir),
                "--build-path",
                str(build_dir),
                str(ino),
            ]
            try:
                proc = subprocess.run(
                    cmd, capture_output=True, text=True, check=False
                )
            except FileNotFoundError:
                results.append((fqbn, sketch_name, False, f"arduino-cli not found: {cli}"))
                continue
            if proc.returncode != 0:
                results.append((
                    fqbn,
                    sketch_name,
                    False,
                    f"compile-DB harvest failed:\n{proc.stderr.strip() or proc.stdout.strip()}",
                ))
                continue

            cdb = build_dir / "compile_commands.json"
            if not cdb.is_file():
                results.append((fqbn, sketch_name, False, f"no compile_commands.json in {build_dir}"))
                continue
            try:
                db = json.loads(cdb.read_text())
            except json.JSONDecodeError as exc:
                results.append((fqbn, sketch_name, False, f"could not parse compile_commands.json: {exc}"))
                continue

            # Every TU the builder prepared from the sketch folder: the
            # concatenated .ino.cpp plus each support .cpp (iox.cpp,
            # display.cpp, ...). Support files are example code too, so
            # the gate binds them the same way (they were ungated before
            # the ProMiniSMININode dual-target work).
            sketch_src = build_dir / "sketch"
            entries = [
                e for e in db if is_under(Path(e.get("file", "")), sketch_src)
            ]
            if not entries:
                results.append((
                    fqbn,
                    sketch_name,
                    False,
                    f"no sketch TUs under {sketch_src}; db has {len(db)} entries",
                ))
                continue
            demote = fqbn_demotes_includes(fqbn)
            for entry in entries:
                tu = os.path.basename(entry.get("file", "?"))
                passed, detail = lint_tu(entry, demote)
                results.append((fqbn, tu, passed, detail))
    return results


def main(argv: list[str]) -> int:
    cli = find_arduino_cli()
    libs_dir = Path(env_str("LIBS_DIR", str(Path.home() / "Dropbox/Arduino/libraries")))
    env_fqbn = os.environ.get("FQBN") or None

    sketches = argv if argv else list(DEFAULT_SKETCHES)

    print(f"arduino-cli: {cli}")
    print(f"libs dir:    {libs_dir}")
    if env_fqbn:
        print(f"fqbn:        {env_fqbn} (env override)")
    else:
        print(f"fqbn:        {DEFAULT_FQBN} (default; per-sketch overrides in SKETCH_FQBNS)")
    print()

    any_fail = False
    for sketch in sketches:
        for fqbn, tu, passed, detail in lint_sketch(sketch, cli, libs_dir, env_fqbn):
            if passed:
                print(f"PASS  {sketch} [{fqbn}] {tu}")
            else:
                any_fail = True
                print(f"FAIL  {sketch} [{fqbn}] {tu}")
                for line in detail.splitlines():
                    print(f"      | {line}")
    print()
    if any_fail:
        print("sketch-lint: FAIL (one or more sketches produced warnings)")
        return 1
    print("sketch-lint: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
