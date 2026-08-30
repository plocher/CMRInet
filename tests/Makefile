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
            ../src/CMRINode.cpp ../src/transport/mock.cpp \
            ../src/transport/serial.cpp
LIB_HDRS := ../src/CMRIFrameCodec.h ../src/CMRIHost.h ../src/CMRINode.h \
            ../src/CMRIPacket.h ../src/CMRInet.h ../src/CMRITime.h \
            ../src/CMRITransport.h ../src/ConformanceFault.h \
            ../src/IOBuffer.h \
            ../src/transport/mock.h ../src/transport/serial.h \
            ../src/transport/serialPort.h ../src/transport/serialStream.h \
            ../src/transport/serialESP32.h ../src/RemoteNodeHandle.h \
            ../src/SimpleHostMetrics.h

TESTS   := test_codec test_time test_mock_transport test_serial_transport \
           test_host test_node test_tracer test_host_display_metrics \
           test_xiao_generators test_conformance_fault test_cdc_line
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
