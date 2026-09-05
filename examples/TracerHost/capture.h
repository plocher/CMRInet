// capture.h — TracerHost's #47/#112 ring-buffer capture mode.
//
// `run <secs>` arms a fixed-size RAM ring that records every I/T/P/R
// packet trace instead of streaming it live over CDC (streaming can't
// keep up with dense full-T traffic without dropping records); `dump`
// prints the ring; `reset` clears it. This is a private implementation
// detail of the C&C surface (tracerconsole.h/.cpp owns the one instance) --
// a buffering strategy for coping with a narrow/lossy CDC link, the
// same kind of concern as writeCdcLine's own chunking, not an
// engine-adjacent behavior like a walker or the OLED panel. Kept as
// its own file for readability, not reuse.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "CMRIPacket.h"

class TracerCapture {
 public:
  static constexpr size_t kCapacity = 12000;

  /// Record one packet while a capture is active; no-op otherwise.
  /// `nowMs` is the trace-time clock (callers pass millis() at the
  /// moment the packet was observed, not a cached loop-tick value).
  void record(bool transmit, const CMRInet::CMRIPacket& packet, uint32_t nowMs);

  bool active() const { return active_; }

  /// Arm a capture for `secs` seconds and print "BEGIN CAPTURE ...".
  /// `pollsSentNow` anchors the poll-count delta the end-of-capture
  /// summary reports.
  void start(uint32_t nowMs, uint32_t secs, uint32_t pollsSentNow);

  /// Call once per loop(). While active, advances the loop-iteration
  /// counter; if the capture is due to end, prints the
  /// "END CAPTURE ..." summary and returns true (the caller still owns
  /// turning off TracerShell::setBackoffTraceOnly -- this class knows
  /// nothing about the shell).
  bool tick(uint32_t nowMs, uint32_t pollsSentNow);

  /// Print "BEGIN DUMP" .. one "PKT ..." line per record .. "END DUMP".
  void dump() const;

  /// Clear the ring and every counter (the `reset` verb).
  void reset();

 private:
  struct RingRecord {
    uint32_t t_ms;
    uint8_t UA;
    uint8_t mt;
    uint8_t flags;  // bit 0 = TX, bit 1 = invalid wire-UA at capture
    uint8_t len;
  } __attribute__((packed));
  static constexpr uint8_t kFlagTx = 0x01u;
  static constexpr uint8_t kFlagInvalidUa = 0x02u;

  RingRecord ring_[kCapacity];
  size_t ringUsed_ = 0;
  bool active_ = false;
  uint32_t endMs_ = 0;
  uint32_t startMs_ = 0;
  uint32_t pollsAtStart_ = 0;
  uint32_t loopIterations_ = 0;
  uint32_t itFrames_ = 0;
  uint32_t invalidUaRecords_ = 0;
};
