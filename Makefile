# Desktop:  make            LV2 bundle + an x86 build of the VST2 plugin (for the host test)
#           make test       DSP tests and the VST2 host test (x86)
# Force:    make force      the MPC OS plugin, armv7 hard-float (needs arm-linux-gnueabihf-g++)
#           make test-force the same host test, cross-built, against the Force .so under qemu-arm
# Packaging the Force plugin (skin, installer, zip) is mpc/build.sh.
CXX      ?= g++
ARMCXX   ?= arm-linux-gnueabihf-g++
ARMCC    ?= arm-linux-gnueabihf-gcc
QEMU     ?= qemu-arm
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
# Plugin code: no C++ runtime (exceptions, RTTI) so the .so needs only libc/libm; errno-free math inlines better.
PLUGFLAGS = -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-math-errno
# Same ISA as the community's arm32v7/gcc:12 builds (armv7-a, VFPv3-D16, hard float), tuned for the RK3288.
ARMFLAGS  = -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -mthumb -mtune=cortex-a17
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
	$(ARMCC) $(ARMFLAGS) -shared -o $@ build/armhf/sk_vst.o -lm -Wl,--no-undefined -Wl,--as-needed -Wl,-z,relro,-z,now -s

build/armhf/test_vst: tests/test_vst.cpp dsp/ShermanCore.h
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) -o $@ $< -ldl

test-force: build/armhf/$(SO) build/armhf/test_vst
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_vst build/armhf/$(SO)

clean:
	rm -rf build $(BUNDLE)/sherkhan.so

.PHONY: all test force test-force clean
