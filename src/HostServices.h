// HostServices.h — Host sketch overlay (not protocol core).
//
// Orchestrator and services for SimpleHost and TracerHost.
// Same include layout as SimpleHostMetrics.h.
// Protocol engines do not include this file.
//
// VALIDATION: docs/adr/0005-orchestration-and-services.md

#pragma once

#include <stdint.h>
#include <string.h>

#include "CMRInet.h"

namespace CMRInet {
namespace app {

/// Base service — tick once per host loop.
class Service {
 public:
  virtual ~Service() = default;
  virtual void tick(CMRIHost& host, uint32_t now) = 0;
};

/// Fixed-capacity orchestrator (no heap after setup for SimpleHost;
/// TracerHost may add at setup only and toggle enable at runtime).
class Orchestrator {
 public:
  static constexpr int kMaxServices = 32;

  Orchestrator() : serviceCount_(0) {}

  bool add(Service* service) {
    if (service == nullptr || serviceCount_ >= kMaxServices) {
      return false;
    }
    services_[serviceCount_++] = service;
    return true;
  }

  void tick(CMRIHost& host, uint32_t now) {
    for (int i = 0; i < serviceCount_; ++i) {
      services_[i]->tick(host, now);
    }
  }

  int count() const { return serviceCount_; }

 private:
  Service* services_[kMaxServices];
  int serviceCount_;
};

// ---- Bit walker ------------------------------------------------------------

struct BitWalkerConfig {
  uint8_t nodeUA = 0;
  uint8_t byte = 0;
  uint8_t startBit = 0;
  uint8_t bitsCount = 8;
  uint32_t periodMs = 250;
  bool inverted = false;  // true: active-low walk (Tracer fastwalker style)
};

/// Walks one bit of one output byte. Enable/disable and reconfigure at runtime.
class BitWalkerService : public Service {
 public:
  explicit BitWalkerService(const BitWalkerConfig& config = BitWalkerConfig())
      : config_(config),
        enabled_(true),
        currentBit_(0),
        lastBit_(-1),
        lastStepMs_(0) {}

  void setConfig(const BitWalkerConfig& config) { config_ = config; }
  const BitWalkerConfig& config() const { return config_; }

  void setEnabled(bool on) {
    enabled_ = on;
    if (on) {
      currentBit_ = 0;
      lastBit_ = -1;
      lastStepMs_ = 0;
    }
  }
  bool enabled() const { return enabled_; }

  void tick(CMRIHost& host, uint32_t now) override {
    if (!enabled_) return;
    RemoteNodeHandle* node = host.node(config_.nodeUA);
    if (node == nullptr ||
        node->state() != RemoteNodeState::kOnline) {
      return;
    }
    if (config_.byte >= node->outputLength() || config_.bitsCount == 0) {
      return;
    }
    if (now - lastStepMs_ < config_.periodMs && lastStepMs_ != 0) {
      return;
    }
    if (lastBit_ == -1) {
      currentBit_ = config_.startBit;
    } else {
      // Clear previous
      const bool clearVal = config_.inverted ? true : false;
      node->setOutputBit(config_.byte, static_cast<uint8_t>(lastBit_), clearVal);
    }
    // Set current
    const bool setVal = config_.inverted ? false : true;
    node->setOutputBit(config_.byte, currentBit_, setVal);

    lastBit_ = static_cast<int16_t>(currentBit_);
    currentBit_ = static_cast<uint8_t>(
        ((currentBit_ + 1 - config_.startBit) % config_.bitsCount) +
        config_.startBit);
    lastStepMs_ = now;
  }

 private:
  BitWalkerConfig config_;
  bool enabled_;
  uint8_t currentBit_;
  int16_t lastBit_;
  uint32_t lastStepMs_;
};

// ---- Input → output --------------------------------------------------------

enum class InputToggleMode : uint8_t {
  kLevelFollow,   // on change, out := in (SimpleHost / write_read)
  kToggleOnRise,  // on rising edge, toggle out
};

struct InputToggleConfig {
  uint8_t inNodeUA = 0;
  uint8_t inByte = 0;
  uint8_t inBit = 0;
  uint8_t outNodeUA = 0;
  uint8_t outByte = 0;
  uint8_t outBit = 0;
  InputToggleMode mode = InputToggleMode::kLevelFollow;
};

class InputToggleService : public Service {
 public:
  explicit InputToggleService(const InputToggleConfig& config = InputToggleConfig())
      : config_(config), enabled_(true), lastIn_(false), haveLast_(false) {}

  void setConfig(const InputToggleConfig& config) { config_ = config; }
  const InputToggleConfig& config() const { return config_; }

  void setEnabled(bool on) {
    enabled_ = on;
    haveLast_ = false;
  }
  bool enabled() const { return enabled_; }

  void tick(CMRIHost& host, uint32_t /*now*/) override {
    if (!enabled_) return;
    RemoteNodeHandle* inNode = host.node(config_.inNodeUA);
    RemoteNodeHandle* outNode = host.node(config_.outNodeUA);
    if (inNode == nullptr || outNode == nullptr ||
        inNode->state() != RemoteNodeState::kOnline ||
        outNode->state() != RemoteNodeState::kOnline) {
      return;
    }
    const bool inVal = inNode->inputBit(config_.inByte, config_.inBit);
    if (!haveLast_) {
      lastIn_ = inVal;
      haveLast_ = true;
      if (config_.mode == InputToggleMode::kLevelFollow) {
        outNode->setOutputBit(config_.outByte, config_.outBit, inVal);
      }
      return;
    }
    if (config_.mode == InputToggleMode::kLevelFollow) {
      if (inVal != lastIn_) {
        outNode->setOutputBit(config_.outByte, config_.outBit, inVal);
      }
    } else {
      // toggle on rise
      if (inVal && !lastIn_) {
        const bool cur = outNode->outputBit(config_.outByte, config_.outBit);
        outNode->setOutputBit(config_.outByte, config_.outBit, !cur);
      }
    }
    lastIn_ = inVal;
  }

 private:
  InputToggleConfig config_;
  bool enabled_;
  bool lastIn_;
  bool haveLast_;
};

// ---- Host-loop stall (Tracer stimulus only) --------------------------------

enum class StallMode : uint8_t { kYield, kBusy };

struct StallConfig {
  uint32_t stallMs = 0;
  uint32_t periodMs = 150;
  StallMode mode = StallMode::kYield;
};

class StallService : public Service {
 public:
  explicit StallService(const StallConfig& config = StallConfig())
      : config_(config), enabled_(false), lastMs_(0) {}

  void setConfig(const StallConfig& config) { config_ = config; }
  const StallConfig& config() const { return config_; }

  void setEnabled(bool on) {
    enabled_ = on && config_.stallMs != 0;
    lastMs_ = 0;
  }
  bool enabled() const { return enabled_; }

  void tick(CMRIHost& /*host*/, uint32_t now) override {
    if (!enabled_ || config_.stallMs == 0) return;
    if (lastMs_ != 0 && now - lastMs_ < config_.periodMs) return;
    if (config_.mode == StallMode::kYield) {
#ifdef ARDUINO
      delay(config_.stallMs);
#else
      (void)now;
#endif
    } else {
#ifdef ARDUINO
      const uint32_t start = millis();
      while (millis() - start < config_.stallMs) {
      }
#else
      (void)now;
#endif
    }
#ifdef ARDUINO
    lastMs_ = millis();
#else
    lastMs_ = now;
#endif
  }

 private:
  StallConfig config_;
  bool enabled_;
  uint32_t lastMs_;
};

}  // namespace app
}  // namespace CMRInet
