# Desktop unit tests for the CMRInet library.
#
# The library core has no Arduino dependencies (DESIGN.md D6/D7), so these
# tests compile the exact library sources with the host compiler.
#
# Run from the repo root:   make -C tests
# Or from this directory:   make

CC       ?= cc
CXX      ?= c++
CFLAGS    = -Wall -Wextra -g -Iunity
CXXFLAGS  = -std=c++11 -Wall -Wextra -Werror -g -I../src -Iunity
BUILD    := build

TARGET   := $(BUILD)/test_codec

.PHONY: all test clean

all: test

test: $(TARGET)
	./$(TARGET)

$(BUILD)/unity.o: unity/unity.c unity/unity.h unity/unity_internals.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c unity/unity.c -o $@

$(TARGET): test_codec.cpp ../src/CMRIFrameCodec.cpp ../src/CMRIFrameCodec.h \
           ../src/CMRIPacket.h ../src/CMRInet.h $(BUILD)/unity.o
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) test_codec.cpp ../src/CMRIFrameCodec.cpp \
	    $(BUILD)/unity.o -o $@

clean:
	rm -rf $(BUILD)
