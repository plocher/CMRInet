// CMRIProfile.h — the compile-time geometry profile.
//
// The single source of truth for the library's geometry (Design
// v1.1 D8). Exactly one knob is configured here; everything else
// in the library is derived from it. Consumer headers include
// this header and consume the macro; they define no values of
// their own. To support a new board, add a fork to the chain
// below. That is the whole customization surface.
//
// The knob: CMRINET_MAX_PAYLOAD_BYTES — the IO-image ceiling.
// The most NI/NO bytes a node may configure (CMRINode.h), the
// most an IOBuffer stores (IOBuffer.h), and the most an R or T
// body carries.
//
// The derivations — computed, never configured:
//   body ceiling  = payload ceiling
//     CMRIPacket.h kMaxBody. The largest body any message type
//     carries is the IO image itself: T carries the NO bytes, R
//     carries the NI bytes, and the largest I body is USIC's 20.
//     One ceiling serves packet and image alike.
//   max wire frame = max payload + the codec's framing overhead
//     CMRIPacket.h kMaxWireFrame = 6 + 2 * body: two SYN + STX +
//     UA + MT + ETX, plus the worst-case DLE escape of two wire
//     bytes per data byte (rules 2.1.1, 2.1.6). The codec owns
//     that arithmetic; nothing here configures a frame size.
//
// Value rationale:
//   256 — the protocol ceiling (E7), on the desktop test hosts:
//     256 logical body bytes counted AFTER DLE removal. The
//     framing rides outside that budget (no length byte — the
//     body runs to ETX), so a full 256-byte body is a legal
//     518-byte wire frame that conforming receivers must accept.
//     The ceiling matches the spec's theoretical maximum node:
//     64 I/O cards (DIN32/DOUT32 class) at 4 bytes per card.
//     Not 255: the boundary tests must pin the exact ceiling.
//     The suite's codec tests are symbolic in kMaxBody, so the
//     desktop fork is what exercises the E7 maximum at all.
//     VALIDATION: Interop v1.1 E7: the body length limit of 256
//     counts data bytes after DLE removal.
//   118 — JMRI's reply-image ceiling for fielded nodes (interop
//     E7, rule 2.3.6): any geometry accepted under it also works
//     under the dominant fielded Host. Fielded reality never
//     approaches the protocol ceiling: the largest Node ever
//     built (a full SUSIC backplane, 32 cards of 32-bit IO,
//     4 bytes each) is 128; bench work on a fielded platform
//     raises that fork's value deliberately.
//   20  — the cpNode family: IO image 18 bytes max (8 expander
//     ports + 2 onboard), rounded up to cover the largest init
//     body (USIC's 20). Keeps every escaped wire frame
//     (6 + 2 * 20 = 46 bytes) inside the 64-byte AVR TX buffer,
//     so every send is one gapless write (rule 2.1.5); any value
//     through 29 holds that invariant.
//
// Why a profile header and not a per-sketch define: the knob
// must be identical in every translation unit. Library sources
// and the sketch .ino compile separately. A sketch TU that sees
// a different ceiling than the library TUs gives CMRIPacket,
// IOBuffer, and the engines two different layouts across the
// API, and memory corrupts. The platform macros (__AVR__,
// RAMEND, ESP_PLATFORM, ARDUINO, the host macros) are the same
// in every TU of one build, so fork-conditional values stay
// consistent with no build-flag ceremony.
//
// The knob is defined unconditionally here, so nothing else can
// define it: a sketch-local or stray -D pre-definition cannot
// shadow the profile — the profile value wins in every TU, and
// the conflicting definition surfaces as a macro-redefinition
// warning (a hard error under the sketch-lint gate). Layout
// consistency is enforced by construction, not by discipline.
//
// Not a profile knob: the receive queue depth
// (CMRINET_SERIAL_RX_QUEUE, transport/serial.h). It is 4 on
// every platform — a property of the polled strategy (one
// exchange at a time), not of the target's memory — and its RAM
// cost scales with the knob by itself, because slots are
// CMRIPackets. It lives with its only consumer.
//
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time
// knobs, so a '328-class build shrinks packet buffers. The
// small-AVR fork is the shipped default for the small-RAM
// targets D8 names.

#pragma once

#if defined(__AVR__)
// RAMEND must be visible in every TU, whether or not Arduino.h
// was included first: the library's own translation units reach
// this header through CMRIPacket.h without ever seeing
// Arduino.h. Without this include the fork below would silently
// miss on library TUs and split the geometry — and the layout —
// across one build.
#include <avr/io.h>
#endif

// ---- platform forks ----------------------------------------------------
//
// One fork per platform family, each assigning the one knob. The
// chain is closed: a platform with no fork is a hard compile
// error, never a silent fallback. A new target (a future BBLeo
// board, say) gets its own fork here, with its own rationale.

#if defined(__AVR__)
    #if defined(RAMEND) && (RAMEND < 0x1000)
        // ATmega328P / ATmega168 / ATmega32U4: 2 KB SRAM. The
        // cpNode-family ceiling. The default 118 would need
        // about 2.7 KB of static RAM alone and does not fit.
        #define CMRINET_MAX_PAYLOAD_BYTES 20
    #else
        // Larger AVR parts (ATmega1284P, ATmega2560): the JMRI
        // ceiling.
        #define CMRINET_MAX_PAYLOAD_BYTES 118
    #endif
#elif defined(ESP_PLATFORM)
    // The ESP32 family, Arduino core or bare ESP-IDF has lots of memory,
    // so we choose the JMRI ceiling.
    #define CMRINET_MAX_PAYLOAD_BYTES 118
#elif defined(ARDUINO)
    // Every other Arduino core. Correct by construction: the
    // common small-SRAM parts of the ecosystem are the AVR parts,
    // caught above.
    #define CMRINET_MAX_PAYLOAD_BYTES 118
#elif defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    // The desktop test hosts: the protocol ceiling. The suite is
    // the conformance instrument — the codec boundary, overflow,
    // and worst-case-escaping tests are symbolic in kMaxBody, so
    // this fork is what exercises the E7 maximum (256-byte body,
    // 518-byte frame) at all. RAM is free here; the JMRI-interop
    // ceiling (118) belongs to the fielded forks above.
    #define CMRINET_MAX_PAYLOAD_BYTES 256
#else
    #error "CMRInet: no geometry fork for this platform. Add one to the chain in CMRIProfile.h."
#endif
