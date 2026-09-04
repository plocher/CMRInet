# 005-orchestration-and-services

## Status

Accepted

## Context

The CMRInet library needs a way to support flexible, configurable behaviors that can be orchestrated together. This is needed for:

1. Bench testing with various exercisers (bit walkers, loopbacks, etc.)
2. Layout control systems with distributed control points
3. Interlocking and safety logic implementations
4. Test-driven development of complex automation scenarios

The need arises from the requirement to have a clean separation between CMRInet's core communication infrastructure and application-level control logic, while supporting the ability to:
- Define arbitrary numbers of behaviors with different configurations
- Run multiple behaviors simultaneously in a single sketch
- Manage these behaviors in a clean, orchestrated fashion
- Work within Arduino resource constraints (memory, compile-time overhead)

## Decision

We will implement a simple orchestration and services pattern that follows these principles:

### Service Architecture

Services are concrete implementations of specific behaviors that inherit from a base Service interface.

The base `Service` class provides a minimal interface:
```cpp
class Service {
public:
    virtual ~Service() = default;
    virtual void tick(uint32_t now) = 0;  // Pure virtual - must implement
};
```

Concrete services implement specific behaviors:
```cpp
class BitWalkerService : public Service {
public:
    void tick(uint32_t now) override {
        // Implementation that walks bits
    }
};
```

### Configuration Pattern

Services must support configuration to handle the variety of use cases:
- Different nodes/UA numbers
- Different bytes/bits to control  
- Different timing intervals
- Different operational modes

Configuration should follow a simple pattern:
```cpp
// Fixed-size arrays for Arduino compatibility
static constexpr int MAX_BITS = 8;
struct BitWalkerConfig {
    uint8_t nodeUA;
    uint8_t byte;
    uint8_t bits[MAX_BITS];
    int bitsCount;
    uint32_t periodMs;
    uint8_t currentStep;
    uint32_t lastStepMs;
};

class BitWalkerService : public Service {
private:
    BitWalkerConfig config_;
    
public:
    BitWalkerService(const BitWalkerConfig& config) : config_(config) {}
    void tick(uint32_t now) override {
        // Use config_ to drive behavior
    }
};
```

### Orchestrator Pattern

The `Orchestrator` is a simple manager that coordinates services:

```cpp
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
    
    void tick(uint32_t now) {
        for (int i = 0; i < serviceCount_; i++) {
            services_[i]->tick(now);
        }
    }
};
```

## Consequences

### Positive
- Clean separation of concerns between communication infrastructure (CMRInet) and application logic
- Extensible architecture that supports diverse use cases
- Flexible configuration of individual service behaviors  
- Predictable memory usage and performance for Arduino environments
- Easy to unit test individual service components
- Aligns with well-established design patterns

### Negative
- Adds some complexity to the library structure
- Requires some boilerplate in service implementations  
- The orchestration adds an extra layer of abstraction

### Neutral
- Follows established patterns in larger-scale systems
- Doesn't affect existing CMRInet APIs or behavior
- Minimal impact on compile times or resource usage compared to the alternatives

## Implementation Notes

This architecture is intended as a clean overlay that can be added to existing sketches (like SimpleHost) without modifying the core CMRInet library structure. The pattern can later be extracted into its own project/repo when needed.

The configuration approach uses simple structs that can be easily serialized or deserialized, and the service pattern uses minimal inheritance that's appropriate for Arduino resource constraints.

The example usage pattern in SimpleHost would show how to:
1. Define various service configurations
2. Instantiate services with specific configurations  
3. Register services with an Orchestrator
4. Call the orchestrator's tick method from the main loop