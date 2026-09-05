// tracerconsole.h — TracerHost's C&C surface.
//
// The sketch-local extension of the shared TracerShell verb vocabulary
// (src/testbed/TracerShell.h owns status/roster/node management); this
// class owns everything needed to move verb/telemetry bytes reliably
// over a narrow, lossy CDC link and to recognize the local verbs
// TracerShell doesn't:
//   - the CdcConsole byte-I/O adapter (#99, the seam writeCdcLine binds to)
//   - verb line framing (readVerb)
//   - the #47/#112 ring-buffer capture mode (run/dump/reset) -- private
//     to this class (see capture.h): a buffering strategy for dense
//     telemetry that would otherwise overwhelm the CDC line, the same
//     kind of problem writeCdcLine's own chunking solves for outbound
//     JSON lines. Not an engine-adjacent behavior.
//   - the local verb dispatch table (enable/disable/configure, node's
//     lazy-begin hook, display, reboot, run, dump, reset)
//   - deferred host.begin(): this surface decides when the engine is
//     allowed to start driving the wire.
//
// What this class does NOT own: what a dispatched verb actually does
// once it reaches a behavior module. Walker/toggle/stall stimulus
// (generators.h) and OLED rendering (display.h) stay separate --
// engine-adjacent behaviors, callable with no knowledge that C&C
// exists, not C&C plumbing. This class calls out to them; it never
// reimplements them.

//
// C&C: verbs on the CDC stream, JSON lines back. Every verb that acts
// on a node names its UA — the shell holds no bound node (Design v1.2
// D5), so there is no implicit target:
//   status | status <UA>
//   quiesce <UA> | resume <UA> | forcetx <UA>
//   setbit <UA> <bit> <0|1> | writeoutputs <UA> <hex>
//   node add <UA> <type> ...     type C|M|N|X + type-specific INIT
//                                C: <in> <out> [opts1 [opts2]]
//                                M: [ns [ct0..ct5]]
//                                N|X: <ns> <in> <out> [ct x ns]
//   node delete <UA>
//   node geometry <UA> <in> <out>   (CPNODE NI/NO reshape)
//   node enable <UA> | node disable <UA>
//   enable|disable|configure <generator> [UA <n>] ...
//
// Node membership is only via node add/delete — this sketch seeds none.
//   run <secs> | dump | reset | reboot | display <1|2> <text> | quit
// After quit the image emits "final" with all activity quiesced

#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "CMRIHost.h"
#include "CMRIPacket.h"
#include "testbed/TracerShell.h"
#include "testbed/CdcLineWriter.h"

#include "capture.h"
#include "display.h"

class TracerConsole : public CMRInet::testbed::CdcConsole {
 public:
  // ---- CdcConsole seam for writeCdcLine (#99) ----
  bool open() const override { return static_cast<bool>(Serial); }
  size_t availableForWrite() override { return Serial.availableForWrite(); }
  size_t write(const uint8_t* data, size_t n) override {
    return Serial.write(data, n);
  }
  uint32_t nowMs() override { return millis(); }
  void yieldMs() override { delay(1); }

  /// Wire the console to the engine it fronts and the shell/display it
  /// coordinates with. Registers itself as the host's trace listener
  /// (the run/dump ring is private to this class, per capture.h).
  void bind(CMRInet::CMRIHost& host, CMRInet::testbed::TracerShell& engine,
            TracerDisplay& display);

  /// Drive one loop iteration's worth of C&C: 
  /// -- keep the shell's clock current,
  /// -- tick the engine and generator stimulus,
  /// -- service the capture session's expiry,
  /// -- read and dispatch at most one verb.
  ///
  /// `quit` does not special-case this call. It quiesces every node and
  /// disables every generator (parkForQuit_), so host.tick() has nothing
  /// to poll and returns near-instantly on its own 

  void tick(uint32_t nowMs);

 private:
  static void onTrace_(void* context, bool transmit,
                        const CMRInet::CMRIPacket& packet);
  void handleTrace_(bool transmit, const CMRInet::CMRIPacket& packet);

  static void lazyBeginThunk_();
  void lazyBegin();

  /// Quiesce every live node and disable every generator
  void parkForQuit_();

  bool readVerb(char* out, size_t len);
  void handleDisplayVerb(char* verbCopy);
  void handleRebootVerb();
  void handleRunVerb(char* verbCopy);
  void handleResetVerb();

  static TracerConsole* instance_;  // for the plain-function-pointer thunk

  CMRInet::CMRIHost* host_ = nullptr;
  CMRInet::testbed::TracerShell* engine_ = nullptr;
  TracerDisplay* display_ = nullptr;

  TracerCapture capture_;
  bool hostBegun_ = false;
  bool finished_ = false;

  char lineBuf_[128] = {0};
  size_t lineUsed_ = 0;
};
