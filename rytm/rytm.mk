# SK Rytm (Overbridge for the Analog Rytm MKII on MPC OS). Included by the top Makefile.
#   make rytm            x86 builds of both plugins (for the host test) + the engine simulation test
#   make rytm-test       engine simulation test + VST2 host test of both plugins (x86)
#   make rytm-arm        armv7 (Force and Gen1 MPCs) and arm64 builds of both plugins
#   make rytm-test-arm   the tests against the ARM builds under qemu
RYTM_SRC  = rytm/rytm_vst.cpp rytm/ob_usb.h rytm/ob_core.h
# -z nodelete: the shared engine's threads run this code, so it must never be unloaded (see ob_usb.h).
# libpthread.so.0 by name: on MPC OS with glibc < 2.34 the pthread functions live there (with glibc 2.34+ it is an
# empty compatibility library); see the .symver lines in ob_usb.h.
RYTM_LINK = -lm -Wl,--no-as-needed -l:libpthread.so.0 -Wl,--as-needed -Wl,--no-undefined -Wl,-z,relro,-z,now,-z,nodelete -s
RYTM_SOS  = build/x86/sk_rytm_track.so build/x86/sk_rytm_send.so

rytm: $(RYTM_SOS) build/x86/test_rytm_core

build/x86/sk_rytm_%.so: $(RYTM_SRC)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PLUGFLAGS) $(if $(filter send,$*),-DSK_RYTM_SEND) -c -o build/x86/rytm_$*.o rytm/rytm_vst.cpp
	$(CXX:g++=gcc) -shared -o $@ build/x86/rytm_$*.o -lm -Wl,--no-undefined -Wl,-z,nodelete -lpthread

build/armhf/sk_rytm_%.so: $(RYTM_SRC)
	@mkdir -p $(@D)
	$(ARMCXX) $(CXXFLAGS) $(ARMFLAGS) $(PLUGFLAGS) $(if $(filter send,$*),-DSK_RYTM_SEND) -c -o build/armhf/rytm_$*.o rytm/rytm_vst.cpp
	$(ARMCC) $(ARMFLAGS) -shared -o $@ build/armhf/rytm_$*.o $(RYTM_LINK)

build/arm64/sk_rytm_%.so: $(RYTM_SRC)
	@mkdir -p $(@D)
	$(A64CXX) $(CXXFLAGS) $(A64FLAGS) $(PLUGFLAGS) $(if $(filter send,$*),-DSK_RYTM_SEND) -c -o build/arm64/rytm_$*.o rytm/rytm_vst.cpp
	$(A64CC) $(A64FLAGS) -shared -o $@ build/arm64/rytm_$*.o $(RYTM_LINK)

build/%/test_rytm_core: tests/test_rytm_core.cpp rytm/ob_core.h
	@mkdir -p $(@D)
	$(if $(filter armhf,$*),$(ARMCXX) $(ARMFLAGS),$(if $(filter arm64,$*),$(A64CXX) $(A64FLAGS),$(CXX))) $(CXXFLAGS) -o $@ $< -lpthread

build/%/test_rytm_vst: tests/test_rytm_vst.cpp
	@mkdir -p $(@D)
	$(if $(filter armhf,$*),$(ARMCXX) $(ARMFLAGS),$(if $(filter arm64,$*),$(A64CXX) $(A64FLAGS),$(CXX))) $(CXXFLAGS) -o $@ $< -ldl -lpthread

rytm-test: rytm build/x86/test_rytm_vst
	./build/x86/test_rytm_core
	./build/x86/test_rytm_vst build/x86/sk_rytm_track.so build/x86/sk_rytm_send.so

rytm-arm: build/armhf/sk_rytm_track.so build/armhf/sk_rytm_send.so build/arm64/sk_rytm_track.so build/arm64/sk_rytm_send.so

rytm-test-arm: rytm-arm build/armhf/test_rytm_core build/armhf/test_rytm_vst build/arm64/test_rytm_core build/arm64/test_rytm_vst
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_rytm_core
	$(QEMU) -L /usr/arm-linux-gnueabihf ./build/armhf/test_rytm_vst build/armhf/sk_rytm_track.so build/armhf/sk_rytm_send.so
	$(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/test_rytm_core
	$(QEMU64) -L /usr/aarch64-linux-gnu ./build/arm64/test_rytm_vst build/arm64/sk_rytm_track.so build/arm64/sk_rytm_send.so

.PHONY: rytm rytm-test rytm-arm rytm-test-arm
.PRECIOUS: build/%/test_rytm_core build/%/test_rytm_vst
