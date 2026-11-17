# SK TI Link (Access Virus TI over USB, for MPC OS).
#   make            x86 build of the plugin and the host test
#   make test       host test against a fake Virus (x86), plus the sequencer ABI check
#   make force      armv7 hard-float with NEON (Force and the other Gen1 MPCs)
#   make arm64      arm64 (the 64-bit MPCs)
#   make test-force / test-arm64   the host test against the ARM builds under qemu
# Packaging: mpc/build.sh. Regenerate the parameter table and pages: python3 gen.py
CXX      ?= g++
ARMCXX   ?= arm-linux-gnueabihf-g++
ARMCC    ?= arm-linux-gnueabihf-gcc
A64CXX   ?= aarch64-linux-gnu-g++
A64CC    ?= aarch64-linux-gnu-gcc
QEMU     ?= qemu-arm
QEMU64   ?= qemu-aarch64
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
PLUGFLAGS = -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-math-errno
ARMFLAGS  = -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mthumb -mtune=cortex-a17 -U_TIME_BITS
A64FLAGS  = -march=armv8-a -mtune=cortex-a55 -mno-outline-atomics
LINKMPC   = -lm -Wl,--no-undefined -Wl,--as-needed -Wl,-z,relro,-z,now -s
SO       := sk_ti_link.so
SRC      := sk_ti.cpp ti_table.h

all: build/x86/$(SO) build/x86/test_ti

build/x86/$(SO): $(SRC)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PLUGFLAGS) -c -o build/x86/sk_ti.o sk_ti.cpp
	$(CXX:g++=gcc) -shared -o $@ build/x86/sk_ti.o -lm -Wl,--no-undefined -Wl,--as-needed

build/x86/test_ti: tests/test_ti.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< -ldl

build/x86/abi_check: tests/abi_check.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $<

test: build/x86/$(SO) build/x86/test_ti build/x86/abi_check
	./build/x86/abi_check
	./build/x86/test_ti build/x86/$(SO)

force: build/armhf/$(SO)
build/armhf/$(SO): $(SRC)
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) $(PLUGFLAGS) -c -o build/armhf/sk_ti.o sk_ti.cpp
	$(ARMCC) $(ARMFLAGS) -shared -o $@ build/armhf/sk_ti.o $(LINKMPC)
build/armhf/test_ti: tests/test_ti.cpp
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) -o $@ $< -ldl
# The armhf cross sysroot may lack the kernel's sound headers: they are architecture independent, take the host's.
build/armhf/abi_check: tests/abi_check.cpp
	@mkdir -p $(@D)/uapi && cp -r /usr/include/sound $(@D)/uapi/
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) -idirafter $(@D)/uapi -o $@ $<
test-force: build/armhf/$(SO) build/armhf/test_ti build/armhf/abi_check
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/abi_check
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_ti build/armhf/$(SO)

arm64: build/arm64/$(SO)
build/arm64/$(SO): $(SRC)
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) $(PLUGFLAGS) -c -o build/arm64/sk_ti.o sk_ti.cpp
	$(A64CC) $(A64FLAGS) -shared -o $@ build/arm64/sk_ti.o $(LINKMPC)
build/arm64/test_ti: tests/test_ti.cpp
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) -o $@ $< -ldl
build/arm64/abi_check: tests/abi_check.cpp
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) -o $@ $<
test-arm64: build/arm64/$(SO) build/arm64/test_ti build/arm64/abi_check
	$(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/abi_check
	$(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/test_ti build/arm64/$(SO)

clean:
	rm -rf build

.PHONY: all test force arm64 test-force test-arm64 clean
