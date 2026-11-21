FROND_TEST_OBJ ?= $(FROND_TEST_ROOT)/build/out
# Included after cloudpilot's Makefile, with FROND_68K=1.
BAT_TEST = $(FROND_TEST_OBJ)/test-68k-battery
$(BAT_TEST).o: $(FROND_TEST_ROOT)/tests/68k-battery.cpp emulator/hardware/EmSPISlaveADS784x.h emulator/EmBattery.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(BAT_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(BAT_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: battery-test
battery-test: $(BAT_TEST)
	$(BAT_TEST)
