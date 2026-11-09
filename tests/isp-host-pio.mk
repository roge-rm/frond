# Included after cloudpilot's Makefile, with FROND_68K=1.
ISP_HOST_TEST ?= $(FROND_TEST_ROOT)/build/test-isp-host-pio
$(ISP_HOST_TEST).o: $(FROND_TEST_ROOT)/tests/isp-host-pio.cpp emulator/hardware/EmRegsISPDana.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(ISP_HOST_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(ISP_HOST_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out ../common/libcommon.a,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: isp-host-test
isp-host-test: $(ISP_HOST_TEST)
	$(ISP_HOST_TEST)
