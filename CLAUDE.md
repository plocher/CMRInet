# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## High-Level Architecture and Concepts

This library implements the CMRInet protocol suite, designed around the concept of an **I/O Image Service**. It is structured around two primary logical entities: the `CMRIHost` and the `CMRINode` Engines, which operate over pluggable transports.

**1. Core Seams and Contracts:**
*   **Image Seam (Top-level Contract):** This is the library's highest layer. Components here operate on abstract concepts like I/O images, freshness, and node health. This seam is strategy-neutral.
*   **Packet Seam (Carrier Boundary):** This layer is the carrier boundary, allowing CMRInet to ride different physical transports (RS-485, TCP, Mock).
*   **Layer Model:** The architecture flows from high-level sketches (Image Service) down to raw bytes (Serial Rendering: codec, TXEN, SYN preamble).

**2. Component Types (Ubiquitous Language):**
*   **Engines (`CMRIHost`, `CMRINode`):** Components that implement the core **image contract** and represent a specific protocol strategy (e.g., Polled CMRInet). They are the core stateful components.
*   **Observers/Wrappers (Shell):** Components that observe or drive an Engine. They do not implement the image contract and handle command-and-control, telemetry rendering, or status display.
*   **Strategy:** The exchange discipline (e.g., Polled CMRInet). This library contains the only current strategy.

**3. Repository Structure:**
*   `src/`: Contains the core implementation of the codec, the polled Host and Node Engines, strategy-neutral handle types, and the shared testbed shell.
*   `src/transport/`: Houses all transport implementations (e.g., `serial.h`, `mock.h`). The umbrella `CMRITransport.h` manages the seam, and sketches include the chosen implementation.
*   `docs/DESIGN.md`: The authoritative source for the architectural decisions (D1-D17), layer model, and seam contracts.
*   `examples/`: Contains front-door tutorials (`SimpleHost.ino`, `SimpleNode.ino`) and specialized testbed tools (`XiaoHostTracer.ino`, `XiaoSniffer.ino`).
*   `tests/`: Location for unit tests run on the desktop testbed, independent of Arduino dependencies.

**4. Naming Convention:**
*   Units are named using the spec: **Host** and **Node**, never master or slave.
*   Naming follows the grammar: The head noun is last. Qualifiers stack in front.

## Bench and Hardware Testing (Experimental)

The repository contains a specialized set of Python scripts (`extras/bench/`) used for interacting with and validating the CMRInet hardware over USB. These tools are essential for reproducing bugs and verifying design decisions on a physical testbed.

**1. Setup:**
The environment must be set up using the provided shell script:
```shell
extras/bench/setup.sh
```
This creates a virtual environment (`.venv`) and installs dependencies like `pyserial`. All subsequent scripts must be run using the interpreter within this environment:
```shell
extras/bench/.venv/bin/python extras/bench/<script>.py
```

**2. Operational Commands:**
*   **Bench Management:** Use \`extras/bench/bench\` to list live devices, resolve roles (Host, Sniffer, Dongle), and define bench sets using \`bench.json\`.
    *   List roles: \`extras/bench/bench list\`
    *   Resolve device path: \`extras/bench/bench resolve --role Host\`
*   **Execution:** Specific scripts for simultaneous capture, decoding, and validation are provided:
    *   **Frame Decoding:** \`extras/bench/dongle_decode.py\` decodes raw bytes from the RS485 dongle.
    *   **Three-Witness Capture:** \`extras/bench/three.py\` performs simultaneous capture across Xiao sniffers and the dongle for A/B comparison.
    *   **Flash and Probe:** \`extras/bench/flash_and_probe.sh\` flashes a sketch (e.g., \`XiaoHostTracer\` or \`SimpleHost\`), executes a full witness capture cycle, and prints a VERDICT block to report test success or failure.

**3. Workflow Note:**
A typical bug reproduction cycle involves using \`extras/bench/flash_and_probe.sh\` to run a target sketch and observing the output's VERDICT block to determine if the expected fault is reproduced.

