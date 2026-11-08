# Included after cloudpilot's Makefile, with FROND_68K=1.
DANA_TEST = $(FROND_TEST_ROOT)/build/test-68k-dana
$(DANA_TEST).o: $(FROND_TEST_ROOT)/tests/68k-dana.cpp emulator/hardware/EmRegsVZDana.h emulator/hardware/EmRegsISPDana.h emulator/hardware/EmHAL.h emulator/hardware/EmSPISlaveSD.h emulator/ExternalStorage.h ../common/CardImage.h ../uarm/uarm/CardBacking.h ../common/dana_input5.h ../common/dana_keyboard5.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(DANA_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(DANA_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out ../common/libcommon.a,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: dana-test
dana-test: $(DANA_TEST)
	$(DANA_TEST)
