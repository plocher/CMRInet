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

inline ParsedGeneratorParams parseGeneratorParams(char* args, const char* gen_name) {
  ParsedGeneratorParams p;
  char* saveptr = nullptr;
  char* token = strtok_r(args, " ", &saveptr);
  if (!token) return p;

  // Case-insensitive comparison for user-typed C&C keys and values.
  // A user typing "UA 31" or "Period 750" should parse the same as
  // "ua 31" or "period 750" — the keys are not case-significant.
  // gen_name is kept case-sensitive: it is dispatched programmatically
  // by the verb handler, not typed by the user.
  auto iequals = [](const char* a, const char* b) -> bool {
    while (*a && *b) {
      if (tolower(static_cast<unsigned char>(*a)) !=
          tolower(static_cast<unsigned char>(*b)))
        return false;
      ++a; ++b;
    }
    return *a == *b;
  };

  if (strcmp(gen_name, "stall") == 0) {
    if (isdigit(token[0])) {
      p.has_stall_ms = true;
      p.stall_ms = strtoul(token, nullptr, 10);
      token = strtok_r(nullptr, " ", &saveptr);
    }
  }

  while (token != nullptr) {
    const char* key = token;
    char* val_str = strtok_r(nullptr, " ", &saveptr);
    if (!val_str) {
      p.error_code = "missingValue";
      p.error_val = key;
      return p;
    }

    const bool node_scoped_gen =
        (strcmp(gen_name, "fastwalker") == 0 ||
         strcmp(gen_name, "slowwalker") == 0 ||
         strcmp(gen_name, "toggleoutfrominput") == 0);
    if (node_scoped_gen && iequals(key, "ua")) {
      const unsigned long UA_val = strtoul(val_str, nullptr, 10);
      if (UA_val > 127UL) {
        p.error_code = "badValue";
        p.error_val = val_str;
        return p;
      }
      p.has_UA = true;
      p.UA = static_cast<uint8_t>(UA_val);
      token = strtok_r(nullptr, " ", &saveptr);
      continue;
    }

    if (strcmp(gen_name, "fastwalker") == 0 || strcmp(gen_name, "slowwalker") == 0) {
      if (iequals(key, "period")) {
        p.has_period = true;
        p.period_ms = strtoul(val_str, nullptr, 10);
      } else if (iequals(key, "byte")) {
        p.has_byte = true;
        p.byte_idx = strtoul(val_str, nullptr, 10);
      } else {
        p.error_code = "unknownKey";
        p.error_val = key;
        return p;
      }
    } else if (strcmp(gen_name, "toggleoutfrominput") == 0) {
      if (iequals(key, "in")) {
        p.has_in = true;
        p.in_bit = strtoul(val_str, nullptr, 10);
      } else if (iequals(key, "out")) {
        p.has_out = true;
        p.out_bit = strtoul(val_str, nullptr, 10);
      } else if (iequals(key, "src_byte")) {
        const unsigned long parsed = strtoul(val_str, nullptr, 10);
        if (parsed > 255UL) {
          p.error_code = "badValue";
          p.error_val = val_str;
          return p;
        }
        p.has_src_byte = true;
        p.src_byte = static_cast<uint8_t>(parsed);
      } else if (iequals(key, "src_bit")) {
        const unsigned long parsed = strtoul(val_str, nullptr, 10);
        if (parsed > 7UL) {
          p.error_code = "badValue";
          p.error_val = val_str;
          return p;
        }
        p.has_src_bit = true;
        p.src_bit = static_cast<uint8_t>(parsed);
      } else if (iequals(key, "dst_byte")) {
        const unsigned long parsed = strtoul(val_str, nullptr, 10);
        if (parsed > 255UL) {
          p.error_code = "badValue";
          p.error_val = val_str;
          return p;
        }
        p.has_dst_byte = true;
        p.dst_byte = static_cast<uint8_t>(parsed);
      } else if (iequals(key, "dst_bit")) {
        const unsigned long parsed = strtoul(val_str, nullptr, 10);
        if (parsed > 7UL) {
          p.error_code = "badValue";
          p.error_val = val_str;
          return p;
        }
        p.has_dst_bit = true;
        p.dst_bit = static_cast<uint8_t>(parsed);
      } else if (iequals(key, "mode")) {
        p.has_loopback_mode = true;
        if (iequals(val_str, "toggle")) {
          p.loopback_mode_write_read = false;
        } else if (iequals(val_str, "write_read")) {
          p.loopback_mode_write_read = true;
        } else {
          p.error_code = "badValue";
          p.error_val = val_str;
          return p;
        }
      } else {
        p.error_code = "unknownKey";
        p.error_val = key;
        return p;
      }
    } else if (strcmp(gen_name, "stall") == 0) {
      if (iequals(key, "period")) {
        p.has_period = true;
        p.period_ms = strtoul(val_str, nullptr, 10);
      } else if (iequals(key, "mode")) {
        p.has_mode = true;
        if (iequals(val_str, "yield")) p.mode_busy = false;
        else if (iequals(val_str, "busy")) p.mode_busy = true;
        else {
          p.error_code = "badValue";
          p.error_val = val_str;
          return p;
        }
      } else {
        p.error_code = "unknownKey";
        p.error_val = key;
        return p;
      }
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
