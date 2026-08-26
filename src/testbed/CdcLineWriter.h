// CdcLineWriter.h — the chunked CDC line writer, pulled out of the
// Xiao Host R&D sketch (#99) so the chunking, the room check, and the
// reserved-terminator-budget split run under the desktop -Werror gate
// and a fake port.
//
// The logic is identical to the sketch-local copy it replaces; only the
// location changes. The sketch bound `Serial` directly; this header
// binds a CdcConsole seam, so a test fakes the stream, the clock, and
// the yield without hardware.
//
// The #86 defect lived here: the terminator was written with no room
// check, and setTxTimeoutMs(0) makes a write discard-and-return when
// the buffer is full — so the newline dropped at the one moment it
// matters most (the buffer is full precisely when the body exhausted
// its budget). The fix is the room check and the reserved terminator
// slice, both preserved verbatim below and covered by
// tests/test_cdc_line.cpp.
//
// Testbed-only: like TracerShell.h, this lives in CMRInet::testbed and
// costs nothing in a sketch that never includes it. No CMRI qualifier:
// this speaks the operator's C&C stream, not the wire (CONTEXT.md D1).

#pragma once

#include <stdint.h>
#include <string.h>

namespace CMRInet {
namespace testbed {

/// The seam writeCdcLine binds: a CDC console with a bounded write
/// buffer, a clock for the time budget, and a yield for the backoff.
///
/// A test fakes one object; a sketch binds one adapter to Serial,
/// millis(), and delay(). The clock and the yield are part of the seam
/// because the budget split is the logic this header exists to test.
class CdcConsole {
 public:
  virtual ~CdcConsole() = default;

  /// Whether the stream is up (Serial as a bool).
  virtual bool open() const = 0;

  /// Bytes the write buffer can accept now; 0 means full.
  virtual size_t availableForWrite() = 0;

  /// Write up to n bytes; return the count accepted. A 0 return means
  /// full or discarded (setTxTimeoutMs(0) with a full buffer).
  virtual size_t write(const uint8_t* data, size_t n) = 0;

  /// The budget clock (millis on hardware, injected in tests).
  virtual uint32_t nowMs() = 0;

  /// Backoff yield (delay(1) on hardware, a clock bump in tests).
  virtual void yieldMs() = 0;
};

/// Stream one telemetry line to the console, then terminate it.
///
/// One fixed time budget (maxWaitMs), split so the record boundary can
/// never be starved by the body: the body spends maxWaitMs -
/// terminatorWaitMs, the terminator gets terminatorWaitMs reserved, and
/// the body cannot borrow the terminator's slice. The terminator gets
/// the same room check and retry as the body, because the one moment it
/// is written is the moment the buffer is most likely full.
///
/// Returns the bytes written (body + terminator if it landed).
inline size_t writeCdcLine(CdcConsole& console, const char* line,
                           uint32_t maxWaitMs = 250,
                           uint32_t terminatorWaitMs = 50) {
  if (!console.open() || line == nullptr) {
    return 0;
  }

  const uint32_t bodyWaitMs = maxWaitMs - terminatorWaitMs;
  const size_t lineLen = strlen(line);
  const uint32_t bodyStart = console.nowMs();
  size_t written = 0;
  while (console.open() && written < lineLen &&
         (console.nowMs() - bodyStart) < bodyWaitMs) {
    const size_t room = console.availableForWrite();
    if (room == 0) {
      console.yieldMs();
      continue;
    }
    const size_t remaining = lineLen - written;
    const size_t chunk = (remaining < room) ? remaining : room;
    const size_t n = console.write(
        reinterpret_cast<const uint8_t*>(line + written), chunk);
    if (n == 0) {
      console.yieldMs();
      continue;
    }
    written += n;
  }

  // The terminator gets the same room check, the same retry, and its
  // return value inspected — setTxTimeoutMs(0) makes a write
  // discard-and-return when the buffer is full, so an unchecked write is
  // a silent drop (#86).
  //
  // It needs a reserved slice rather than the body's leftovers, because
  // the only way the body exhausts its budget is by spinning on a full
  // buffer. The one moment the terminator gets written is therefore the
  // moment it is most likely to be dropped: the failure correlates
  // exactly with the condition the terminator exists to survive.
  const uint8_t newline = '\n';
  const uint32_t terminatorStart = console.nowMs();
  while (console.open() &&
         (console.nowMs() - terminatorStart) < terminatorWaitMs) {
    if (console.availableForWrite() == 0) {
      console.yieldMs();
      continue;
    }
    if (console.write(&newline, 1) == 1) {
      return written + 1;
    }
    console.yieldMs();
  }
  return written;
}

/// LineWriter trampoline so the shared engine binds the shared logic
/// directly: engine.bind(host, transport, image, version,
/// &writeCdcLineCb, &console). The context is a CdcConsole*.
inline void writeCdcLineCb(void* context, const char* line) {
  CdcConsole* console = static_cast<CdcConsole*>(context);
  writeCdcLine(*console, line);
}

}  // namespace testbed
}  // namespace CMRInet
