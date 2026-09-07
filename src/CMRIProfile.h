// CMRIProfile.h — compile-time target profiles for the geometry knobs.
//
// The single source of truth for the library's geometry-knob defaults
// (Design v1.1 D8). This header (1) selects exactly one platform
// profile per build and (2) defines every knob default for that
// profile. Knob headers include this header and consume the macros;
// they define no defaults of their own. To add a knob, add one line
// per profile block below. To add a platform, extend the selection
// chain (and add a profile block only if it needs its own numbers).
//
// Profile selection, in precedence order:
//   1. Forced on the command line: -DCMRINET_PROFILE_AVR_MINI or
//      -DCMRINET_PROFILE_STOCK. Exactly one; forcing both is an
//      error.
//   2. Auto-detected from the platform macros:
//        - AVR with less than 4 KB of SRAM (RAMEND < 0x1000:
//          ATmega328P, ATmega168, ATmega32U4) -> AVR_MINI.
//        - every other AVR part, every Arduino core, ESP-IDF, and
//          the desktop hosts (macOS, Linux, Windows) -> STOCK.
//          Stock is correct by construction for any non-AVR Arduino
//          core: the small-SRAM parts in the ecosystem are the AVR
//          parts, caught above.
//        - anything else -> hard compile error. The profile set is
//          closed: a new platform makes a deliberate profile
//          decision here (or forces one with -D) instead of
//          inheriting a silent fallback.
//
// Why a profile header and not a per-sketch define: the knob macros
// must be identical in every translation unit. Library sources and
// the sketch .ino compile separately. A sketch TU that sees a
// different CMRINET_MAX_BODY than the library TUs gives CMRIPacket,
// IOBuffer, and the engines two different layouts across the API,
// and memory corrupts. The platform macros (__AVR__, RAMEND,
// ARDUINO, the host macros) are the same in every TU of one build,
// so profile-conditional defaults stay consistent with no build-flag
// ceremony.
//
// Customization happens on exactly one axis: profile selection.
// Force a profile with -DCMRINET_PROFILE_AVR_MINI or
// -DCMRINET_PROFILE_STOCK (build-global, exactly one), or edit the
// value table below. The knob macros are defined unconditionally
// here, so nothing else can define them: a sketch-local or stray
// -D pre-definition cannot shadow the profile — the profile value
// wins in every TU, and the conflicting definition surfaces as a
// macro-redefinition warning (a hard error under the sketch-lint
// gate). Layout consistency is enforced by construction, not by
// discipline.
//
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time
// knobs, so a '328-class build shrinks packet buffers. The mini
// profile is the shipped default set for the small-RAM AVR targets
// D8 names.

#pragma once

// ---- profile selection ------------------------------------------------

#if defined(CMRINET_PROFILE_AVR_MINI) && defined(CMRINET_PROFILE_STOCK)
#error "CMRInet: CMRINET_PROFILE_AVR_MINI and CMRINET_PROFILE_STOCK are both defined; force exactly one profile."
#endif

#if defined(__AVR__)
// RAMEND, in every TU whether or not Arduino.h was included first.
#include <avr/io.h>
#endif

#if !defined(CMRINET_PROFILE_AVR_MINI) && !defined(CMRINET_PROFILE_STOCK)
#if defined(__AVR__) && defined(RAMEND) && (RAMEND < 0x1000)
// AVR parts with less than 4 KB of SRAM. RAMEND < 0x1000 means less
// than 4 KB.
#define CMRINET_PROFILE_AVR_MINI 1
#elif defined(__AVR__) || defined(ARDUINO) || defined(ESP_PLATFORM) || \
    defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
// Larger AVR parts (ATmega1284P, ATmega2560), every other Arduino
// core, ESP-IDF without Arduino, and the desktop test hosts.
#define CMRINET_PROFILE_STOCK 1
#else
#error "CMRInet: no platform profile for this target. Add the platform to the selection chain in CMRIProfile.h, or force a profile with -DCMRINET_PROFILE_AVR_MINI or -DCMRINET_PROFILE_STOCK."
#endif
#endif

// ---- knob defaults: one block per profile -----------------------------
//
// Every knob is defined unconditionally in its profile block: the
// profile is the single source for the value, and a conflicting
// pre-definition is a macro-redefinition warning instead of a
// silent shadow. Value rationale lives beside the values.

#if defined(CMRINET_PROFILE_AVR_MINI)

// Logical body ceiling (CMRIPacket.h). 24 covers the cpNode-family
// IO image (18 bytes max: 8 expander ports + 2 onboard) and the
// largest init body (20 bytes, USIC), and keeps every escaped wire
// frame (6 + 2 * 24 = 54 bytes) inside the 64-byte AVR TX buffer,
// so every send is one gapless write (rule 2.1.5). A value above 29
// breaks that invariant on small-RAM AVR parts.
#define CMRINET_MAX_BODY 24

// Received-packet queue depth (transport/serial.h). Slots are
// CMRIPackets, and the mini CMRINET_MAX_BODY shrinks each slot:
// depth costs less than buffer size, so the queue keeps the stock
// depth. First growth-loop target when bring-up measurements show
// RAM headroom.
#define CMRINET_SERIAL_RX_QUEUE 4

// IO image capacity (IOBuffer.h). The cpNode-family image reaches at
// most 18 bytes.
#define CMRINET_IO_BUFFER_MAX_BYTES 20

// Node input/output image ceilings (CMRINode.h). Same 18-byte
// family ceiling.
#define CMRINET_NODE_MAX_INPUT_BYTES 20
#define CMRINET_NODE_MAX_OUTPUT_BYTES 20

#elif defined(CMRINET_PROFILE_STOCK)

// VALIDATION: Interop v1.1 E7: the default is 256 logical body
// bytes, counted after DLE removal.
#define CMRINET_MAX_BODY 256

// The polled strategy consumes replies one exchange at a time, so a
// small queue suffices.
#define CMRINET_SERIAL_RX_QUEUE 4

// JMRI's reply-image ceiling for fielded nodes.
#define CMRINET_IO_BUFFER_MAX_BYTES 118

#define CMRINET_NODE_MAX_INPUT_BYTES 118
#define CMRINET_NODE_MAX_OUTPUT_BYTES 118

#else
#error "CMRInet: no platform profile defined — the CMRIProfile.h selection chain has a gap."
#endif
