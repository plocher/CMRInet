// CMRIProfile.h — compile-time target profiles for the geometry knobs.
//
// The library's geometry ceilings (body size, queue depth, IO image
// size) are compile-time knobs (Design v1.1 D8). Each knob header
// defines its own default. This header selects the default set for
// the target: small-RAM AVR parts get the mini profile, everything
// else gets the stock profile.
//
// Why a profile header and not a per-sketch define: the knob macros
// must be identical in every translation unit. Library sources and
// the sketch .ino compile separately. A sketch TU that sees a
// different CMRINET_MAX_BODY than the library TUs gives CMRIPacket,
// IOBuffer, and the engines two different layouts across the API,
// and memory corrupts. The platform macros (__AVR__, RAMEND) are the
// same in every TU of one build, so profile-conditional defaults
// stay consistent with no build-flag ceremony.
//
// Overrides stay possible, and they must be build-global. Example
// for arduino-cli:
//   --build-property "compiler.cpp.extra_flags=-DCMRINET_MAX_BODY=32"
// That flag reaches the sketch and the library sources together. A
// sketch-local #define before the include changes only the sketch
// TU. Do not use one.
//
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time
// knobs, so a '328-class build shrinks packet buffers. The mini
// profile is the shipped default set for the small-RAM AVR targets
// D8 names.

#pragma once

#if defined(__AVR__)
// RAMEND, in every TU whether or not Arduino.h was included first.
#include <avr/io.h>
#endif

// Mini profile: AVR parts with less than 4 KB of SRAM (ATmega328P,
// ATmega168, ATmega32U4). RAMEND < 0x1000 means less than 4 KB.
// Parts with 4 KB or more (ATmega1284P, ATmega2560) and every
// non-AVR platform keep the stock defaults.
#if defined(__AVR__) && defined(RAMEND) && (RAMEND < 0x1000)
#define CMRINET_PROFILE_AVR_MINI 1
#endif
