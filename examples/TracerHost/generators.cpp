#include "generators.h"

#include <string.h>

#include "GeneratorParser.h"
#include "HostServices.h"

namespace tracer_generators {
namespace {

using CMRInet::app::BitWalkerConfig;
using CMRInet::app::BitWalkerService;
using CMRInet::app::InputToggleConfig;
using CMRInet::app::InputToggleMode;
using CMRInet::app::InputToggleService;
using CMRInet::app::Orchestrator;
using CMRInet::app::StallConfig;
using CMRInet::app::StallMode;
using CMRInet::app::StallService;

constexpr size_t kMaxWalkers = 16;
constexpr size_t kMaxToggles = 8;

BitWalkerService walkers[kMaxWalkers];
bool walkerLive[kMaxWalkers] = {false};
InputToggleService toggles[kMaxToggles];
bool toggleLive[kMaxToggles] = {false};
StallService stallService;
Orchestrator orchestrator;
bool ready = false;

int findWalker(uint8_t ua, uint8_t byteIdx) {
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    if (!walkerLive[i]) continue;
    const BitWalkerConfig& c = walkers[i].config();
    if (c.nodeUA == ua && c.byte == byteIdx) return static_cast<int>(i);
  }
  return -1;
}

int allocWalker() {
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    if (!walkerLive[i]) return static_cast<int>(i);
  }
  return -1;
}

int findToggle(uint8_t ua) {
  for (size_t i = 0; i < kMaxToggles; ++i) {
    if (!toggleLive[i]) continue;
    if (toggles[i].config().inNodeUA == ua) return static_cast<int>(i);
  }
  return -1;
}

int allocToggle() {
  for (size_t i = 0; i < kMaxToggles; ++i) {
    if (!toggleLive[i]) return static_cast<int>(i);
  }
  return -1;
}

// Generator verb recognition and node-scoping, table-driven: adding a
// generator kind means adding a row here, not another cascade branch
// in handleVerb. Each kind's enable/disable/configure bodies stay
// separate (see handleVerb) because they operate on genuinely
// different pool shapes (walker slots keyed by UA+byte, toggle slots
// keyed by UA, one stall config) -- that is real behavioral
// difference, not duplicated knowledge.
struct GeneratorKindEntry {
  const char* name;
  bool nodeScoped;  // requires "UA <n>" in the verb
};
constexpr GeneratorKindEntry kGeneratorKindTable[] = {
    {"walker", true},
    {"toggleoutfrominput", true},
    {"stall", false},
};
constexpr size_t kGeneratorKindTableCount =
    sizeof(kGeneratorKindTable) / sizeof(kGeneratorKindTable[0]);

const GeneratorKindEntry* findGeneratorKind(const char* name) {
  for (size_t i = 0; i < kGeneratorKindTableCount; ++i) {
    if (strcmp(name, kGeneratorKindTable[i].name) == 0) {
      return &kGeneratorKindTable[i];
    }
  }
  return nullptr;
}

void emitGeneratorEvent(const char* event, const char* generator,
                        bool include_ua, uint8_t UA) {
  Serial.print("{\"event\":\"");
  Serial.print(event);
  Serial.print("\",\"generator\":\"");
  Serial.print(generator);
  if (include_ua) {
    Serial.print("\",\"ua\":");
    Serial.print(UA);
    Serial.println("}");
  } else {
    Serial.println("\"}");
  }
}

}  // namespace

void begin() {
  if (ready) return;
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    walkers[i].setEnabled(false);
    orchestrator.add(&walkers[i]);
  }
  for (size_t i = 0; i < kMaxToggles; ++i) {
    toggles[i].setEnabled(false);
    orchestrator.add(&toggles[i]);
  }
  stallService.setEnabled(false);
  orchestrator.add(&stallService);
  ready = true;
}

int serviceCount() { return orchestrator.count(); }

void tick(CMRInet::CMRIHost& host, uint32_t nowMs) {
  orchestrator.tick(host, nowMs);
}

void disableAll() {
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    walkers[i].setEnabled(false);
    walkerLive[i] = false;
  }
  for (size_t i = 0; i < kMaxToggles; ++i) {
    toggles[i].setEnabled(false);
    toggleLive[i] = false;
  }
  stallService.setEnabled(false);
}

bool handleVerb(char* cmd, void (*lazyBeginFn)()) {
  begin();

  char* saveptr = nullptr;
  char* action = strtok_r(cmd, " ", &saveptr);
  if (!action) return false;

  const bool is_enable = (strcmp(action, "enable") == 0);
  const bool is_disable = (strcmp(action, "disable") == 0);
  const bool is_configure = (strcmp(action, "configure") == 0);
  if (!is_enable && !is_disable && !is_configure) return false;

  char* gen_name = strtok_r(nullptr, " ", &saveptr);
  if (!gen_name) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"");
    Serial.print(action);
    Serial.println(": missing generator\"}");
    return true;
  }

  const GeneratorKindEntry* kindEntry = findGeneratorKind(gen_name);
  if (kindEntry == nullptr) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"unknown generator '");
    Serial.print(gen_name);
    Serial.println("'\"}");
    return true;
  }
  const bool is_walker = (strcmp(gen_name, "walker") == 0);
  const bool is_toggle = (strcmp(gen_name, "toggleoutfrominput") == 0);
  const bool is_stall = (strcmp(gen_name, "stall") == 0);
  const bool node_scoped = kindEntry->nodeScoped;

  ParsedGeneratorParams p;
  char* args = strtok_r(nullptr, "", &saveptr);
  if (args != nullptr) {
    p = parseGeneratorParams(args, gen_name);
    if (p.error_code) {
      Serial.print("{\"event\":\"error\",\"error\":\"");
      Serial.print(p.error_code);
      Serial.print("\",\"message\":\"Invalid parameter '");
      Serial.print(p.error_val);
      Serial.println("'\"}");
      return true;
    }
  }

  uint8_t target_ua = 0;
  if (node_scoped) {
    if (!p.has_UA) {
      Serial.println(
          "{\"event\":\"error\",\"error\":\"badVerb\","
          "\"message\":\"node-scoped service needs UA <n>\"}");
      return true;
    }
    target_ua = p.UA;
  }

  if (is_stall) {
    if (is_disable) {
      stallService.setEnabled(false);
      emitGeneratorEvent("disable", "stall", false, 0);
      return true;
    }
    StallConfig cfg = stallService.config();
    if (p.has_stall_ms) cfg.stallMs = p.stall_ms;
    if (p.has_period) cfg.periodMs = p.period_ms;
    if (p.has_mode)
      cfg.mode = p.mode_busy ? StallMode::kBusy : StallMode::kYield;
    stallService.setConfig(cfg);
    if (is_enable)
      stallService.setEnabled(cfg.stallMs != 0);
    emitGeneratorEvent(action, "stall", false, 0);
    return true;
  }

  if (is_walker) {
    // period/byte/invert are independent knobs with no named presets;
    // an omitted key falls back to these defaults.
    bool invert = false;
    uint8_t byteIdx = 3;
    uint32_t period = 250;
    if (p.has_invert) invert = p.invert;
    if (p.has_byte) byteIdx = p.byte_idx;
    if (p.has_period) period = p.period_ms;

    if (is_disable) {
      if (p.has_byte) {
        const int idx = findWalker(target_ua, byteIdx);
        if (idx >= 0) {
          walkers[idx].setEnabled(false);
          walkerLive[idx] = false;
        }
      } else {
        for (size_t i = 0; i < kMaxWalkers; ++i) {
          if (!walkerLive[i]) continue;
          if (walkers[i].config().nodeUA != target_ua) continue;
          walkers[i].setEnabled(false);
          walkerLive[i] = false;
        }
      }
      emitGeneratorEvent("disable", "walker", true, target_ua);
      return true;
    }

    int idx = findWalker(target_ua, byteIdx);
    if (idx < 0) idx = allocWalker();
    if (idx < 0) {
      Serial.println(
          "{\"event\":\"error\",\"error\":\"noSlot\","
          "\"message\":\"walker pool full\"}");
      return true;
    }

    BitWalkerConfig cfg;
    cfg.nodeUA = target_ua;
    cfg.byte = byteIdx;
    cfg.startBit = 0;
    cfg.bitsCount = 8;
    cfg.periodMs = period;
    cfg.inverted = invert;
    walkers[idx].setConfig(cfg);
    walkerLive[idx] = true;
    if (is_enable) {
      lazyBeginFn();
      walkers[idx].setEnabled(true);
    }
    emitGeneratorEvent(action, "walker", true, target_ua);
    return true;
  }

  if (is_toggle) {
    if (is_disable) {
      const int idx = findToggle(target_ua);
      if (idx >= 0) {
        toggles[idx].setEnabled(false);
        toggleLive[idx] = false;
      }
      emitGeneratorEvent("disable", gen_name, true, target_ua);
      return true;
    }

    int idx = findToggle(target_ua);
    if (idx < 0) idx = allocToggle();
    if (idx < 0) {
      Serial.println(
          "{\"event\":\"error\",\"error\":\"noSlot\","
          "\"message\":\"toggle pool full\"}");
      return true;
    }

    InputToggleConfig cfg = toggles[idx].config();
    cfg.inNodeUA = target_ua;
    cfg.outNodeUA = target_ua;
    if (p.has_in) {
      cfg.inByte = static_cast<uint8_t>(p.in_bit / 8u);
      cfg.inBit = static_cast<uint8_t>(p.in_bit % 8u);
    }
    if (p.has_out) {
      cfg.outByte = static_cast<uint8_t>(p.out_bit / 8u);
      cfg.outBit = static_cast<uint8_t>(p.out_bit % 8u);
    }
    if (p.has_src_byte && p.has_src_bit) {
      cfg.inByte = p.src_byte;
      cfg.inBit = p.src_bit;
    }
    if (p.has_dst_byte && p.has_dst_bit) {
      cfg.outByte = p.dst_byte;
      cfg.outBit = p.dst_bit;
    }
    if (p.has_loopback_mode) {
      cfg.mode = p.loopback_mode_write_read
          ? InputToggleMode::kLevelFollow
          : InputToggleMode::kToggleOnRise;
    }
    toggles[idx].setConfig(cfg);
    toggleLive[idx] = true;
    if (is_enable) {
      lazyBeginFn();
      toggles[idx].setEnabled(true);
    }
    emitGeneratorEvent(action, gen_name, true, target_ua);
    return true;
  }

  return true;
}

size_t writeStatusItem(void* /*context*/, int index, char* buffer, size_t cap) {
  CMRInet::app::Service* service = orchestrator.serviceAt(index);
  if (service == nullptr) return 0;
  return service->writeStatus(buffer, cap);
}

}  // namespace tracer_generators
