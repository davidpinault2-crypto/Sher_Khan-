# Desktop:  make            LV2 bundle + an x86 build of the VST2 plugin (for the host test)
#           make test       DSP tests and the VST2 host test (x86)
# MPC:      make force      the plugin for the Force and the other Gen1 MPCs: armv7 hard-float with NEON
#                           (needs arm-linux-gnueabihf-g++)
#           make arm64      the plugin for the arm64 MPCs (needs aarch64-linux-gnu-g++)
#           make test-force the DSP tests and the host test, cross-built, against the armv7 .so under qemu-arm
#                           (the host test twice: with NEON, as on the device, and the plain path)
#           make test-arm64 the same against the arm64 .so under qemu-aarch64
# Packaging (skin, Akai SDK folder, installer, zips) is mpc/build.sh.
# SK Rytm (Overbridge for the Analog Rytm MKII): see rytm/rytm.mk (make rytm-test, make rytm-arm) and rytm/build.sh.
CXX      ?= g++
ARMCXX   ?= arm-linux-gnueabihf-g++
ARMCC    ?= arm-linux-gnueabihf-gcc
QEMU     ?= qemu-arm
A64CXX   ?= aarch64-linux-gnu-g++
A64CC    ?= aarch64-linux-gnu-gcc
QEMU64   ?= qemu-aarch64
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
# Plugin code: no C++ runtime (exceptions, RTTI) so the .so needs only libc/libm; errno-free math inlines better.
PLUGFLAGS = -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-math-errno
# The ISAs of Akai's MPC Plugin Builder SDK: armv7-a with NEON and VFPv4 (every Gen1 MPC's RK3288 has both: 32
# double registers and fused multiply-add for the scalar code too), tuned for its Cortex-A17; armv8-a for the arm64
# MPCs, tuned for the Cortex-A55.
# -U_TIME_BITS: Ubuntu's armhf compilers default to 64-bit time_t, whose symbols need glibc 2.34; MPC OS 2.x has 2.32
# (Akai's SDK targets 2.31). -mno-outline-atomics: plain armv8.0 atomics inline, without libgcc's run-time CPU check
# (a static constructor and a __getauxval import).
ARMFLAGS  = -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mthumb -mtune=cortex-a17 -U_TIME_BITS
A64FLAGS  = -march=armv8-a -mtune=cortex-a55 -mno-outline-atomics
LINKMPC   = -lm -Wl,--no-undefined -Wl,--as-needed -Wl,-z,relro,-z,now -s
BUNDLE   := lv2/sherkhan.lv2
SO       := sk_filterbank.so

all: $(BUNDLE)/sherkhan.so build/x86/$(SO)

$(BUNDLE)/sherkhan.so: lv2/sherkhan.cpp dsp/ShermanCore.h
	$(CXX) $(CXXFLAGS) $(PLUGFLAGS) -shared -o $@ $<

build/x86/$(SO): mpc/sk_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PLUGFLAGS) -c -o build/x86/sk_vst.o $<
	$(CXX:g++=gcc) -shared -o $@ build/x86/sk_vst.o -lm -Wl,--no-undefined -Wl,--as-needed

build/test_core: tests/test_core.cpp dsp/ShermanCore.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

build/x86/test_vst: tests/test_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $< -ldl

test: build/test_core build/x86/test_vst build/x86/$(SO)
	./build/test_core
	./build/x86/test_vst build/x86/$(SO)

# ---- Force / MPC OS (armv7 hard-float) ----
force: build/armhf/$(SO)

build/armhf/$(SO): mpc/sk_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) $(PLUGFLAGS) -c -o build/armhf/sk_vst.o $<
	$(ARMCC) $(ARMFLAGS) -shared -o $@ build/armhf/sk_vst.o $(LINKMPC)

build/armhf/test_vst: tests/test_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) -o $@ $< -ldl

build/armhf/test_core: tests/test_core.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) -o $@ $<

test-force: build/armhf/$(SO) build/armhf/test_vst build/armhf/test_core
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_core
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_vst build/armhf/$(SO)
	SK_NO_NEON=1 $(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_vst build/armhf/$(SO)

# ---- arm64 MPCs ----
arm64: build/arm64/$(SO)

build/arm64/$(SO): mpc/sk_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) $(PLUGFLAGS) -c -o build/arm64/sk_vst.o $<
	$(A64CC) $(A64FLAGS) -shared -o $@ build/arm64/sk_vst.o $(LINKMPC)

build/arm64/test_vst: tests/test_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) -o $@ $< -ldl

build/arm64/test_core: tests/test_core.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) -o $@ $<

test-arm64: build/arm64/$(SO) build/arm64/test_vst build/arm64/test_core
	$(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/test_core
	$(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/test_vst build/arm64/$(SO)
	SK_NO_NEON=1 $(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/test_vst build/arm64/$(SO)

include rytm/rytm.mk

clean:
	rm -rf build $(BUNDLE)/sherkhan.so

.PHONY: all test force test-force arm64 test-arm64 clean
