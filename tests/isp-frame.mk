# Included after cloudpilot's Makefile, with FROND_68K=1.
ISP_FRAME_TEST ?= $(FROND_TEST_ROOT)/build/test-isp-frame
$(ISP_FRAME_TEST).o: $(FROND_TEST_ROOT)/tests/isp-frame.cpp emulator/hardware/EmRegsISPDana.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(ISP_FRAME_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(ISP_FRAME_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out ../common/libcommon.a,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: isp-frame-test
isp-frame-test: $(ISP_FRAME_TEST)
	$(ISP_FRAME_TEST)
