# Included after cloudpilot's Makefile, with FROND_68K=1.
ISP_ROOT_TEST ?= $(FROND_TEST_ROOT)/build/test-isp-host-root
$(ISP_ROOT_TEST).o: $(FROND_TEST_ROOT)/tests/isp-host-root.cpp emulator/hardware/EmRegsISPDana.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(ISP_ROOT_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(ISP_ROOT_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out ../common/libcommon.a,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: isp-root-test
isp-root-test: $(ISP_ROOT_TEST)
	$(ISP_ROOT_TEST)
