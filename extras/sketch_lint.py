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
For each sketch it harvests the real cross-compiler command -- compiler,
board defines, include paths -- via `arduino-cli compile
--only-compilation-database`, then recompiles the sketch's translation unit
compile-only with warnings rebound to *our* code:

  * drop `-w` (the suppression) and the core's `-Werror=return-type`;
  * add `-Wall -Wextra -Werror -Wswitch`;
  * demote every third-party include dir to `-isystem` (esp32 core, tools
    libs, Wire/SPI, Adafruit_GFX/BusIO/SSD1306) so their header noise is
    suppressed;
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

Usage
-----
  extras/sketch_lint.py                # lint all sketches
  extras/sketch_lint.py SimpleHost      # lint one sketch

Environment
-----------
  ARDUINO_CLI   default `arduino-cli` (resolved on PATH)
  LIBS_DIR      default `~/Dropbox/Arduino/libraries`
  FQBN          default `esp32:esp32:XIAO_ESP32C6`

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
DEFAULT_SKETCHES = ("SimpleHost", "XiaoHostTracer", "XiaoSniffer")

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

    That covers `src` and `examples/<sketch>` -- the code this gate is
    meant to bind to. Everything else (esp32 core, tools, Wire/SPI, the
    Adafruit libs) is third-party and becomes `-isystem`.
    """
    try:
        path.resolve().relative_to(REPO)
        return True
    except ValueError:
        return False


def rewrite_args(args: list[str]) -> list[str]:
    """Rewrite a harvested compile command into the lint command.

    The compiler (args[0]) is kept. Each `-I<path>` is kept on `-I` if it
    is ours, else demoted to `-isystem <path>`. Warning flags are dropped
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
            if is_ours(inc):
                out.append(tok)
            else:
                out.append("-isystem")
                out.append(str(inc))
            i += 1
            continue
        if tok == "-I":
            # `-I path` form (two tokens).
            inc = Path(rest[i + 1])
            if is_ours(inc):
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


def lint_sketch(sketch: str, cli: str, libs_dir: Path, fqbn: str) -> tuple[bool, str]:
    """Lint one sketch. Returns (passed, detail)."""
    ino = REPO / "examples" / sketch / f"{sketch}.ino"
    if not ino.is_file():
        return False, f"sketch not found: {ino}"

    with tempfile.TemporaryDirectory(prefix=f"sketchlint_{sketch}_") as tmp:
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
            return False, f"arduino-cli not found: {cli}"
        if proc.returncode != 0:
            return (
                False,
                f"compile-DB harvest failed:\n{proc.stderr.strip() or proc.stdout.strip()}",
            )

        cdb = build_dir / "compile_commands.json"
        if not cdb.is_file():
            return False, f"no compile_commands.json in {build_dir}"

        try:
            db = json.loads(cdb.read_text())
        except json.JSONDecodeError as exc:
            return False, f"could not parse compile_commands.json: {exc}"

        # Select the sketch's own translation unit: the preprocessed .ino.cpp.
        needle = f"{sketch}.ino.cpp"
        entries = [e for e in db if needle in os.path.basename(e.get("file", ""))]
        if len(entries) != 1:
            names = [e.get("file", "?") for e in db]
            return (
                False,
                f"expected one '{needle}' TU, found {len(entries)}; "
                f"db has {len(db)} entries: {names[:5]}...",
            )
        entry = entries[0]
        args = entry.get("arguments")
        if not args:
            # 'command' (string) form -- not produced by arduino-cli 1.5.1,
            # but handle it defensively by shlex-splitting.
            import shlex

            cmd_str = entry.get("command", "")
            args = shlex.split(cmd_str)
        if not args:
            return False, "TU entry has no arguments"

        lint_args = rewrite_args(args)
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


def main(argv: list[str]) -> int:
    cli = find_arduino_cli()
    libs_dir = Path(env_str("LIBS_DIR", str(Path.home() / "Dropbox/Arduino/libraries")))
    fqbn = env_str("FQBN", "esp32:esp32:XIAO_ESP32C6")

    sketches = argv if argv else list(DEFAULT_SKETCHES)

    print(f"arduino-cli: {cli}")
    print(f"libs dir:    {libs_dir}")
    print(f"fqbn:        {fqbn}")
    print()

    any_fail = False
    for sketch in sketches:
        passed, detail = lint_sketch(sketch, cli, libs_dir, fqbn)
        if passed:
            print(f"PASS  {sketch}")
        else:
            any_fail = True
            print(f"FAIL  {sketch}")
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
