# Native build:          make
# Cross-compile for Force (armhf, see README):  make CXX=arm-linux-gnueabihf-g++
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -fPIC -fvisibility=hidden -ffast-math
BUNDLE   := lv2/sherkhan.lv2

all: $(BUNDLE)/sherkhan.so

$(BUNDLE)/sherkhan.so: lv2/sherkhan.cpp dsp/ShermanCore.h
	$(CXX) $(CXXFLAGS) -shared -o $@ $<

build/test_core: tests/test_core.cpp dsp/ShermanCore.h
	@mkdir -p build
	$(CXX) -O2 -std=c++17 -Wall -Wextra -o $@ $<

test: build/test_core
	./build/test_core

clean:
	rm -rf build $(BUNDLE)/sherkhan.so

.PHONY: all test clean
