#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct ParsedGeneratorParams {
  bool has_UA = false;
  uint8_t UA = 0;
  bool has_period = false;
  uint32_t period_ms = 0;

  bool has_byte = false;
  uint8_t byte_idx = 0;
  bool has_invert = false;
  bool invert = false;

  bool has_in = false;
  uint16_t in_bit = 0;

  bool has_out = false;
  uint16_t out_bit = 0;
  bool has_src_byte = false;
  uint8_t src_byte = 0;
  bool has_src_bit = false;
  uint8_t src_bit = 0;
  bool has_dst_byte = false;
  uint8_t dst_byte = 0;
  bool has_dst_bit = false;
  uint8_t dst_bit = 0;
  bool has_loopback_mode = false;
  bool loopback_mode_write_read = false;

  bool has_stall_ms = false;
  uint32_t stall_ms = 0;

  bool has_mode = false;
  bool mode_busy = false;

  const char* error_code = nullptr; // e.g. "badParam"
  const char* error_val = nullptr;  // e.g. the offending key
};

namespace generator_parser_detail {

// Case-insensitive comparison for user-typed C&C keys and values. A user
// typing "UA 31" or "Period 750" should parse the same as "ua 31" or
// "period 750" -- the keys are not case-significant. gen_name (the
// dispatch key itself) stays case-sensitive: it is chosen programmatically
// by the verb handler, never typed directly by a user.
inline bool iequals(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower(static_cast<unsigned char>(*a)) !=
        tolower(static_cast<unsigned char>(*b)))
      return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// One key's value-apply function: parses val_str into `p`'s field(s) and
// returns false on an out-of-range value (the caller reports "badValue").
using KeyApplyFn = bool (*)(ParsedGeneratorParams& p, const char* val_str);

struct KeyHandler {
  const char* key;
  KeyApplyFn apply;
};

inline bool applyUa(ParsedGeneratorParams& p, const char* v) {
  const unsigned long val = strtoul(v, nullptr, 10);
  if (val > 127UL) return false;
  p.has_UA = true;
  p.UA = static_cast<uint8_t>(val);
  return true;
}

inline bool applyPeriodMs(ParsedGeneratorParams& p, const char* v) {
  p.has_period = true;
  p.period_ms = strtoul(v, nullptr, 10);
  return true;
}

inline bool applyByteIdx(ParsedGeneratorParams& p, const char* v) {
  p.has_byte = true;
  p.byte_idx = static_cast<uint8_t>(strtoul(v, nullptr, 10));
  return true;
}

inline bool applyInvert(ParsedGeneratorParams& p, const char* v) {
  p.has_invert = true;
  // 1/true/yes/on => inverted (active-low walk).
  p.invert = (iequals(v, "1") || iequals(v, "true") || iequals(v, "yes") ||
              iequals(v, "on"));
  return true;
}

inline bool applyIn(ParsedGeneratorParams& p, const char* v) {
  p.has_in = true;
  p.in_bit = static_cast<uint16_t>(strtoul(v, nullptr, 10));
  return true;
}

inline bool applyOut(ParsedGeneratorParams& p, const char* v) {
  p.has_out = true;
  p.out_bit = static_cast<uint16_t>(strtoul(v, nullptr, 10));
  return true;
}

inline bool applySrcByte(ParsedGeneratorParams& p, const char* v) {
  const unsigned long parsed = strtoul(v, nullptr, 10);
  if (parsed > 255UL) return false;
  p.has_src_byte = true;
  p.src_byte = static_cast<uint8_t>(parsed);
  return true;
}

inline bool applySrcBit(ParsedGeneratorParams& p, const char* v) {
  const unsigned long parsed = strtoul(v, nullptr, 10);
  if (parsed > 7UL) return false;
  p.has_src_bit = true;
  p.src_bit = static_cast<uint8_t>(parsed);
  return true;
}

inline bool applyDstByte(ParsedGeneratorParams& p, const char* v) {
  const unsigned long parsed = strtoul(v, nullptr, 10);
  if (parsed > 255UL) return false;
  p.has_dst_byte = true;
  p.dst_byte = static_cast<uint8_t>(parsed);
  return true;
}

inline bool applyDstBit(ParsedGeneratorParams& p, const char* v) {
  const unsigned long parsed = strtoul(v, nullptr, 10);
  if (parsed > 7UL) return false;
  p.has_dst_bit = true;
  p.dst_bit = static_cast<uint8_t>(parsed);
  return true;
}

inline bool applyLoopbackMode(ParsedGeneratorParams& p, const char* v) {
  p.has_loopback_mode = true;
  if (iequals(v, "toggle")) {
    p.loopback_mode_write_read = false;
  } else if (iequals(v, "write_read")) {
    p.loopback_mode_write_read = true;
  } else {
    return false;
  }
  return true;
}

inline bool applyStallMode(ParsedGeneratorParams& p, const char* v) {
  p.has_mode = true;
  if (iequals(v, "yield")) {
    p.mode_busy = false;
  } else if (iequals(v, "busy")) {
    p.mode_busy = true;
  } else {
    return false;
  }
  return true;
}

// clang-format off
constexpr KeyHandler kWalkerKeys[] = {
  {"ua",     applyUa},
  {"period", applyPeriodMs},
  {"byte",   applyByteIdx},
  {"invert", applyInvert},
};
constexpr KeyHandler kToggleKeys[] = {
  {"ua",       applyUa},
  {"in",       applyIn},
  {"out",      applyOut},
  {"src_byte", applySrcByte},
  {"src_bit",  applySrcBit},
  {"dst_byte", applyDstByte},
  {"dst_bit",  applyDstBit},
  {"mode",     applyLoopbackMode},
};
constexpr KeyHandler kStallKeys[] = {
  {"period", applyPeriodMs},
  {"mode",   applyStallMode},
};
// clang-format on

struct GeneratorKind {
  const char* name;
  const KeyHandler* keys;
  size_t keyCount;
  // "enable stall <ms> ..." accepts a leading bare number as stall_ms
  // before the key/value pairs; no other kind has a positional field.
  bool leadingNumericIsStallMs;
};

constexpr GeneratorKind kGeneratorKinds[] = {
    {"walker", kWalkerKeys, sizeof(kWalkerKeys) / sizeof(kWalkerKeys[0]),
     false},
    {"toggleoutfrominput", kToggleKeys,
     sizeof(kToggleKeys) / sizeof(kToggleKeys[0]), false},
    {"stall", kStallKeys, sizeof(kStallKeys) / sizeof(kStallKeys[0]), true},
};
constexpr size_t kGeneratorKindCount =
    sizeof(kGeneratorKinds) / sizeof(kGeneratorKinds[0]);

inline const GeneratorKind* findKind(const char* gen_name) {
  for (size_t i = 0; i < kGeneratorKindCount; ++i) {
    if (strcmp(gen_name, kGeneratorKinds[i].name) == 0) {
      return &kGeneratorKinds[i];
    }
  }
  return nullptr;
}

inline const KeyHandler* findKey(const GeneratorKind& kind, const char* key) {
  for (size_t i = 0; i < kind.keyCount; ++i) {
    if (iequals(key, kind.keys[i].key)) {
      return &kind.keys[i];
    }
  }
  return nullptr;
}

}  // namespace generator_parser_detail

/// Parse the key/value tail of a generator verb (everything after the
/// generator name) against the table for `gen_name`. Table-driven: adding
/// a key to a generator means adding a row to that generator's table in
/// generator_parser_detail, not another branch in this function.
inline ParsedGeneratorParams parseGeneratorParams(char* args, const char* gen_name) {
  using namespace generator_parser_detail;
  ParsedGeneratorParams p;
  char* saveptr = nullptr;
  char* token = strtok_r(args, " ", &saveptr);
  if (!token) return p;

  const GeneratorKind* kind = findKind(gen_name);
  if (kind == nullptr) return p;  // caller already validated gen_name

  if (kind->leadingNumericIsStallMs &&
      isdigit(static_cast<unsigned char>(token[0]))) {
    p.has_stall_ms = true;
    p.stall_ms = strtoul(token, nullptr, 10);
    token = strtok_r(nullptr, " ", &saveptr);
  }

  while (token != nullptr) {
    const char* key = token;
    char* val_str = strtok_r(nullptr, " ", &saveptr);
    if (!val_str) {
      p.error_code = "missingValue";
      p.error_val = key;
      return p;
    }

    const KeyHandler* handler = findKey(*kind, key);
    if (handler == nullptr) {
      p.error_code = "unknownKey";
      p.error_val = key;
      return p;
    }
    if (!handler->apply(p, val_str)) {
      p.error_code = "badValue";
      p.error_val = val_str;
      return p;
    }

    token = strtok_r(nullptr, " ", &saveptr);
  }
  if (p.has_src_byte != p.has_src_bit) {
    p.error_code = "missingValue";
    p.error_val = p.has_src_byte ? "src_bit" : "src_byte";
    return p;
  }
  if (p.has_dst_byte != p.has_dst_bit) {
    p.error_code = "missingValue";
    p.error_val = p.has_dst_byte ? "dst_bit" : "dst_byte";
    return p;
  }
  return p;
}
