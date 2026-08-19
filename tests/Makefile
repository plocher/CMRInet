# Desktop unit tests for the CMRInet library.
#
# The library core has no Arduino dependencies, so these tests compile
# the exact library sources with the host compiler.
#
# Run from the repo root:   make -C tests
# Or from this directory:   make

CC       ?= cc
CXX      ?= c++
CFLAGS    = -Wall -Wextra -g -Iunity
CXXFLAGS  = -std=c++11 -Wall -Wextra -Werror -g -I../src -Iunity
BUILD    := build

LIB_SRCS := ../src/CMRIFrameCodec.cpp ../src/CMRIHost.cpp \
            ../src/MockCMRITransport.cpp ../src/SerialCMRITransport.cpp
LIB_HDRS := ../src/CMRIFrameCodec.h ../src/CMRIHost.h ../src/CMRIPacket.h \
            ../src/CMRInet.h ../src/CMRISerialPort.h ../src/CMRITime.h \
            ../src/CMRITransport.h ../src/Esp32UartCMRISerialPort.h \
            ../src/MockCMRITransport.h \
            ../src/RemoteNodeHandle.h ../src/SerialCMRITransport.h \
            ../src/StreamCMRISerialPort.h ../src/SimpleHostMetrics.h

TESTS   := test_codec test_time test_mock_transport test_serial_transport \
           test_host test_tracer test_host_display_metrics
TARGETS := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test clean

all: test

test: $(TARGETS)
	@for t in $(TARGETS); do ./$$t || exit 1; done

$(BUILD)/unity.o: unity/unity.c unity/unity.h unity/unity_internals.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c unity/unity.c -o $@

$(BUILD)/%: %.cpp $(LIB_SRCS) $(LIB_HDRS) $(BUILD)/unity.o
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(LIB_SRCS) $(BUILD)/unity.o -o $@

clean:
	rm -rf $(BUILD)
