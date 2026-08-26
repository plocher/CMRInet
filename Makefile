# Top-level Makefile for the CMRInet library.
#
# The library's existing -Werror gates live in tests/ (unit tests) and
# extras/desktop/ (the desktop tracer). This adds the sketch warning gate
# (issue #93) and a single `check` target that runs all three, so the whole
# compile-time gate surface is one command.
#
#   make sketch-lint   # the example-sketch warning gate (issue #93)
#   make test          # the desktop unit-test gate (-Wall -Wextra -Werror)
#   make desktop       # the desktop tracer gate (-Wall -Wextra -Werror)
#   make check         # all three, in order
#
# See docs/sketch-warning-gate.md for why the sketch gate is separate from
# the build (the ESP32 core suppresses warnings with a trailing -w).

PYTHON ?= python3
SKETCH_LINT := extras/sketch_lint.py

.PHONY: sketch-lint test desktop check clean

sketch-lint:
	@$(PYTHON) $(SKETCH_LINT)

test:
	@$(MAKE) -C tests test

desktop:
	@$(MAKE) -C extras/desktop

check: test desktop sketch-lint

clean:
	@$(MAKE) -C tests clean
	@$(MAKE) -C extras/desktop clean
