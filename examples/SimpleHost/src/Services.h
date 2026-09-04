/*
 * Services.h - Service definitions and orchestrator for CMRInet
 * 
 * This file contains the service base class, concrete service implementations,
 * and orchestrator for managing multiple behaviors in CMRInet sketches.
 * 
 * Services provide specific behaviors like bit walking, input toggling, etc.
 * The Orchestrator manages collections of services and executes them.
 */

#ifndef SERVICES_H
#define SERVICES_H

#include <Arduino.h>
#include "CMRInet.h"

// Base Service class - all services must inherit from this
class Service {
public:
    virtual ~Service() = default;
    virtual void tick(CMRInet::CMRIHost& host,  uint32_t now) = 0;  // Pure virtual - must implement
};

// Simple orchestrator that manages and executes services
class Orchestrator {
private:
    static constexpr int MAX_SERVICES = 10;
    Service* services_[MAX_SERVICES];
    int serviceCount_;
    
public:
    Orchestrator() : serviceCount_(0) {}
    
    bool add(Service* service) {
        if (serviceCount_ >= MAX_SERVICES) {
            return false;
        }
        services_[serviceCount_] = service;
        serviceCount_++;
        return true;
    }
    
    void tick(CMRInet::CMRIHost& host, uint32_t now) {
        for (int i = 0; i < serviceCount_; i++) {
            services_[i]->tick(host, now);
        }
    }
};

// Service implementations
// Configuration structure for bitwalker service
struct BitWalkerConfig {
    uint8_t nodeUA;
    uint8_t byte;
    uint8_t startBit;
    uint8_t bitsCount;
    uint32_t periodMs;
    bool inverted;  
};

class BitWalkerService : public Service {
private:
    BitWalkerConfig config_;
    uint8_t currentBit;
    int16_t lastBit;  // last bit that was set
    uint32_t lastStepMs;

public:
    BitWalkerService(const BitWalkerConfig& config) : 
        config_(config), currentBit(0), lastBit(-1), lastStepMs(0) {}

    void tick(CMRInet::CMRIHost& host, uint32_t now) override {
        // Only process if we have a node and it's online
        CMRInet::RemoteNodeHandle* node = host.node(config_.nodeUA);
        if (node == nullptr ||
            node->state() != CMRInet::RemoteNodeState::kOnline) {
            return;
        }

        // Bitwalk: every periodMs, set one bit of byte, clearing the previous
        if (now - lastStepMs >= config_.periodMs) {
            // Clear previous bit (if not first time...)
            if (lastBit == -1) {
                // First time, no previous bit to clear
                currentBit = config_.startBit;
            } else {
                node->setOutputBit(config_.byte, lastBit, config_.inverted ? true : false);
            }
            // Set current bit to true
            node->setOutputBit(config_.byte, currentBit, config_.inverted ? false : true);
                        
            // Advance to next bit
            lastBit = currentBit;
            currentBit = ((currentBit + 1 - config_.startBit) % config_.bitsCount) + config_.startBit;
            lastStepMs = now;
        }
    }
};

// Configuration for the input toggle service
struct InputToggleConfig {
    uint8_t inNodeUA;
    uint8_t inByte;
    uint8_t inBit;
    uint8_t outNodeUA;
    uint8_t outByte;
    uint8_t outBit;
    bool lastValue;
};

class InputToggleService : public Service {
private:
    InputToggleConfig config_;
    
public:
    InputToggleService(const InputToggleConfig& config) : config_(config) {}
    void tick(CMRInet::CMRIHost& host, uint32_t now) override {
        CMRInet::RemoteNodeHandle* inNode  = host.node(config_.inNodeUA);
        CMRInet::RemoteNodeHandle* outNode = host.node(config_.outNodeUA);

        if (inNode == nullptr || outNode == nullptr ||
            (inNode->state() != CMRInet::RemoteNodeState::kOnline) ||
            (outNode->state() != CMRInet::RemoteNodeState::kOnline)) {
            return;
        }
        
        // Toggle an output based on an input.
        const bool in0 = inNode->inputBit(config_.inByte, config_.inBit);
        if (in0 != config_.lastValue) {
            outNode->setOutputBit(config_.outByte, config_.outBit,in0);
        }
        config_.lastValue = in0;
    }
};

// Global orchestrator instance for use by sketches
extern Orchestrator g_orchestrator;

#endif // SERVICES_H