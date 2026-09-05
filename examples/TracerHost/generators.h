// generators.h — TracerHost's stimulus-generator control surface
// (walker / toggleoutfrominput / stall) over HostServices.h's
// Orchestrator.
//
// Pool storage, C&C verb parsing/dispatch, and per-instance status
// reporting all live here (see GeneratorParser.h for the key/value
// grammar this dispatches into) so TracerHost.ino stays the thin
// setup()/loop() router. Single-consumer today (this sketch only) --
// kept as its own file for readability, not reuse: TracerHost.ino was
// pushing 900 lines before this split.

#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "CMRIHost.h"

namespace tracer_generators {

/// Idempotent; registers every walker/toggle/stall slot with the
/// orchestrator. Safe to call from setup() and defensively from
/// handleVerb() (mirrors the original setupServices() guard).
void begin();

/// Slots registered with the orchestrator, for
/// TracerShell::setStatusExtender's itemCount.
int serviceCount();

/// Advance every enabled generator one tick.
void tick(CMRInet::CMRIHost& host, uint32_t nowMs);

/// Dispatch one `enable|disable|configure <generator> ...` verb.
/// `lazyBeginFn` runs before a generator starts driving output, so the
/// sketch's deferred host.begin() still happens on first real traffic
/// -- this module has no direct access to the sketch's host/transport
/// objects. Returns false when `cmd`'s first token isn't
/// enable/disable/configure, so the caller can fall through to the
/// shared shell.
bool handleVerb(char* cmd, void (*lazyBeginFn)());

/// Disable every walker, every toggle, and the stall generator (the
/// `reset` verb's generator-side cleanup).
void disableAll();

/// TracerShell::StatusItemWriter: one enabled service instance's
/// status per index; 0 length means nothing to report at that index.
size_t writeStatusItem(void* context, int index, char* buffer, size_t cap);

}  // namespace tracer_generators
