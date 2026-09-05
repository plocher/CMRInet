#include "tracerconsole.h"

#include <string.h>

#include "generators.h"

TracerConsole* TracerConsole::instance_ = nullptr;

void TracerConsole::bind(CMRInet::CMRIHost& host,
                         CMRInet::testbed::TracerShell& engine,
                         TracerDisplay& display) {
  host_ = &host;
  engine_ = &engine;
  display_ = &display;
  instance_ = this;
  host_->onTrace(&TracerConsole::onTrace_, this);
}

void TracerConsole::onTrace_(void* context, bool transmit,
                             const CMRInet::CMRIPacket& packet) {
  static_cast<TracerConsole*>(context)->handleTrace_(transmit, packet);
}

void TracerConsole::handleTrace_(bool transmit,
                                 const CMRInet::CMRIPacket& packet) {
  if (capture_.active()) {
    capture_.record(transmit, packet, millis());
  } else {
    engine_->emitPacket(transmit, packet);
  }
}

void TracerConsole::lazyBeginThunk_() {
  if (instance_ != nullptr) instance_->lazyBegin();
}

void TracerConsole::lazyBegin() {
  if (!hostBegun_) {
    host_->begin();
    hostBegun_ = true;
  }
}

/// One newline-terminated verb from non-blocking CDC input. CRs are
/// dropped so a terminal sending CRLF works. Returns false when no
/// complete line is waiting.
bool TracerConsole::readVerb(char* out, size_t len) {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      lineBuf_[lineUsed_] = '\0';
      snprintf(out, len, "%s", lineBuf_);
      lineUsed_ = 0;
      return true;
    }
    if (lineUsed_ < sizeof(lineBuf_) - 1) {
      lineBuf_[lineUsed_++] = c;
    }
  }
  return false;
}

void TracerConsole::handleDisplayVerb(char* verbCopy) {
  // #64: Allow the harness to inject custom annotations on the OLED.
  if (!display_->ok()) return;
  char* saveptr = nullptr;
  strtok_r(verbCopy, " ", &saveptr);                       // "display"
  char* line_num_str = strtok_r(nullptr, " ", &saveptr);   // line number
  char* text = strtok_r(nullptr, "\n", &saveptr);          // rest of the line
  if (!line_num_str || !text) return;
  // Takes effect on the next render() tick -- no human-perceptible
  // reason to force an earlier redraw for text.
  display_->setAnnotation(atoi(line_num_str), text);
}

void TracerConsole::handleRebootVerb() {
  Serial.println("{\"event\":\"reboot\"}");
  Serial.flush();  // ensure the response goes out before we drop CDC
  ESP.restart();
}

void TracerConsole::handleRunVerb(char* verbCopy) {
  char* saveptr = nullptr;
  strtok_r(verbCopy, " ", &saveptr);  // "run"
  char* secs_s = strtok_r(nullptr, " ", &saveptr);
  if (!secs_s) return;
  lazyBegin();
  const uint32_t secs = strtoul(secs_s, nullptr, 10);
  // Keep miss/reject/xchg/unsolicited live during capture (#112);
  // packet traces still go only to the ring via handleTrace_.
  capture_.start(millis(), secs, host_->statistics().pollsSent);
  engine_->setBackoffTraceOnly(true);
}

void TracerConsole::handleResetVerb() {
  capture_.reset();
  engine_->setBackoffTraceOnly(false);
  tracer_generators::disableAll();
  Serial.println("{\"event\":\"reset\"}");
}

void TracerConsole::parkForQuit_() {
  tracer_generators::disableAll();
  for (unsigned ua = 0; ua <= 127u; ++ua) {
    CMRInet::RemoteNodeHandle* node = host_->node(static_cast<uint8_t>(ua));
    if (node != nullptr) {
      node->setEnabled(false);
    }
  }
}

void TracerConsole::tick(uint32_t nowMs) {
  engine_->setNow(nowMs);
  tracer_generators::tick(*host_, nowMs);

  if (capture_.tick(nowMs, host_->statistics().pollsSent)) {
    engine_->setBackoffTraceOnly(false);
  }

  char verb[128];
  if (!readVerb(verb, sizeof(verb))) return;

  char verbCopy[128];
  strncpy(verbCopy, verb, sizeof(verbCopy));

  bool handled = false;
  if (strncmp(verb, "enable", 6) == 0 ||
      strncmp(verb, "disable", 7) == 0 ||
      strncmp(verb, "configure", 9) == 0) {
    handled = tracer_generators::handleVerb(verbCopy, &TracerConsole::lazyBeginThunk_);
  } else if (strncmp(verb, "node ", 5) == 0) {
    // The node verbs live in the shared shell now. Design v1.2 D5 made
    // add/delete/geometry engine operations, and the shell is where
    // both tracer images (sketch and desktop) get them identically.
    // All this console still owns is the deferred begin(): a node verb
    // means the operator wants traffic, so the engine must be running
    // before the shell acts on it.
    lazyBegin();
    handled = false;  // fall through to the shell
  } else if (strncmp(verb, "display ", 8) == 0) {
    handleDisplayVerb(verbCopy);
    handled = true;
  } else if (strcmp(verb, "reboot") == 0) {
    handleRebootVerb();
    handled = true;
  } else if (strncmp(verb, "run ", 4) == 0) {
    handleRunVerb(verbCopy);
    handled = true;
  } else if (strcmp(verb, "dump") == 0) {
    capture_.dump();
    handled = true;
  } else if (strcmp(verb, "reset") == 0) {
    handleResetVerb();
    handled = true;
  }
  // No special-case for "status": the fallback below already sends
  // every unhandled verb (including bare "status") to the shell.

  if (!handled) {
    using VerbResult = CMRInet::testbed::TracerShell::VerbResult;
    if (engine_->handleVerb(verb) == VerbResult::kQuit && !finished_) {
      parkForQuit_();
      engine_->emitLine("final");
      finished_ = true;  // guards against re-parking on a stray second quit
    }
  }
}
