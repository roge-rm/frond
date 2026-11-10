SD_TEST = $(FROND_TEST_ROOT)/build/test-68k-sd
$(SD_TEST).o: $(FROND_TEST_ROOT)/tests/68k-sd.cpp emulator/hardware/EmSPISlaveSD.h emulator/ExternalStorage.h emulator/hardware/EmHAL.h ../common/CardImage.h ../uarm/uarm/CardBacking.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(SD_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(SD_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: sd-test
sd-test: $(SD_TEST)
	$(SD_TEST)
