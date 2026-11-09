# Included after cloudpilot's Makefile, with FROND_68K=1.
ISP_ATL_TEST ?= $(FROND_TEST_ROOT)/build/test-isp-host-atl
$(ISP_ATL_TEST).o: $(FROND_TEST_ROOT)/tests/isp-host-atl.cpp emulator/hardware/EmRegsISPDana.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(ISP_ATL_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(ISP_ATL_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out ../common/libcommon.a,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: isp-atl-test
isp-atl-test: $(ISP_ATL_TEST)
	$(ISP_ATL_TEST)
