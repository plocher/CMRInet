# Sketch warning gate (issue #93)

The library's `src/` is gated: `tests/Makefile` and `extras/desktop/Makefile`
compile it under `-Wall -Wextra -Werror`. The example sketches are not, and
the ESP32 core actively suppresses warnings for the sketch translation
unit, so an unhandled enumerator — or any ordinary warning — in a sketch
ships silently.

This gate makes that fail a documented, single command.

## Run it

```
make sketch-lint
```

That runs `extras/sketch_lint.py`, which lints all three example sketches
(`SimpleHost`, `TracerHost`, `XiaoSniffer`). Lint one:

```
python3 extras/sketch_lint.py SimpleHost
```

`make check` runs the sketch gate together with the two existing
`-Werror` gates (`make -C tests test`, `make -C extras/desktop`).

## What it checks

Each sketch is compiled to object only (`-c -o /dev/null`) under
`-Wall -Wextra -Werror -Wswitch`, with warnings bound to *our* code only:

- `-Wswitch` makes an unhandled enumerator in a sketch a hard error. This
  is the one that catches a `switch` over `RemoteNodeState` that has grown
  a case the sketch did not notice.
- `-Wall -Wextra -Werror` widen the net to the next unrelated warning — the
  gate's real destination, not just the enum that prompted it.

## Why a separate lint (why not just `arduino-cli`)

The ESP32 core appends `-w` (suppress all warnings) *after* our flags, and
last wins. Measured on `esp32:esp32@3.3.10`, the flags reaching
`SimpleHost.ino.cpp` are:

```
-Wall -Wextra -w -Werror=return-type -w
```

No `arduino-cli` invocation we have found switches them back on:

- `arduino-cli compile --warnings all` is ignored by the core recipe.
- `--build-property compiler.cpp.extra_flags=-Wall -Wextra` is accepted,
  but the core appends `-w` after it.

So a clean `arduino-cli compile` does not mean a sketch is warning-clean.
The gate recompiles the sketch under our own warning policy instead.

## How it works

1. `arduino-cli compile --only-compilation-database` harvests the real
   cross-compiler command for each sketch — compiler, board defines,
   include paths — into a `compile_commands.json`. This is the same
   toolchain and flags arduino-cli would use, captured before the build
   runs.
2. The gate takes the sketch's translation unit (the preprocessed
   `<Name>.ino.cpp`) and rewrites its command:
   - drop `-w` (the suppression) and the core's `-Werror=return-type`;
   - add `-Wall -Wextra -Werror -Wswitch`;
   - **demote every third-party include dir to `-isystem`** — the esp32
     core, tools, Wire/SPI, and the Adafruit GFX/BusIO/SSD1306 libraries —
     so their header noise is suppressed (third-party code is not ours to
     gate);
   - keep the two CMRInet include dirs on `-I` (`examples/<sketch>` and
     `src`) so our code stays gated;
   - keep all board `-D` defines verbatim, notably
     `-DARDUINO_USB_CDC_ON_BOOT=1` (the define that moves `Serial` onto the
     USB CDC console — harvesting instead of overriding `build.extra_flags`
     means we never clobber it);
   - `-o /dev/null` — compile only, no link, no artifact kept.
3. Run the rewritten command. An unhandled enumerator or any warning in the
   sketch is a hard error.

The "ours vs third-party" split is a repo-root test: an include dir is ours
if it lives inside this repository tree (`src`, `examples/<sketch>`); every
other include dir is third-party and becomes `-isystem`. No toolchain paths
are hardcoded.

## No hardware required

The gate compiles but never flashes. `--only-compilation-database` skips
the actual build, and the rewritten command is `-c -o /dev/null` — no
object, no link, no upload. No testbench or board needs to be present.

## Environment knobs

| Variable     | Default                              | Purpose                          |
|--------------|--------------------------------------|----------------------------------|
| `ARDUINO_CLI`| `arduino-cli` (resolved on `PATH`)   | The `arduino-cli` to drive the harvest |
| `LIBS_DIR`   | `~/Dropbox/Arduino/libraries`        | Where the Adafruit libraries live |
| `FQBN`       | `esp32:esp32:XIAO_ESP32C6`           | The board/FQBN to compile for    |

`PYTHON` (default `python3`) selects the interpreter for the `make` target.

## Wiring

`extras/bench/flash_and_probe.sh` runs the gate as a pre-flight before its
`arduino-cli compile` step, and aborts on failure — so the bench cannot
flash a sketch that would have failed the gate.

## Scope and the CI question

The gate is a local documented command today. Whether it also runs in CI is
decided under the wayfinder map #72, not here; this gate leaves a clean
seam (a single script with a non-zero exit code) for #72 to wire in.
