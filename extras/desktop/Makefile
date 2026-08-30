# Desktop Host harness for the CMRInet library (macOS / Linux).
#
# Builds cmri_tracer: CMRIHost over SerialCMRITransport over the POSIX
# byte port. The library core has no Arduino dependencies, so this
# compiles the exact library sources that run on hardware.
#
# Run from the repo root:   make -C extras/desktop
# Or from this directory:   make

CXX      ?= c++
CXXFLAGS  = -std=c++11 -Wall -Wextra -Werror -g -I../../src -I.
BUILD    := build

LIB_SRCS := ../../src/CMRIFrameCodec.cpp ../../src/CMRIHost.cpp \
            ../../src/transport/serial.cpp

all: $(BUILD)/cmri_tracer

$(BUILD)/cmri_tracer: cmri_tracer.cpp PosixCMRISerialPort.cpp \
                      PosixCMRISerialPort.h \
                      ../../src/testbed/TracerShell.h $(LIB_SRCS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) cmri_tracer.cpp PosixCMRISerialPort.cpp \
	    $(LIB_SRCS) -o $@

.PHONY: all clean
clean:
	rm -rf $(BUILD)
