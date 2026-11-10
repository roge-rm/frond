# Included after cloudpilot's Makefile, with FROND_68K=1.
BUS_STATE_TEST ?= $(FROND_TEST_ROOT)/build/test-68k-bus-state
$(BUS_STATE_TEST).o: $(FROND_TEST_ROOT)/tests/68k-bus-state.cpp emulator/hardware/EmBusTimingSavestate.h emulator/hardware/EmBusTiming.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(BUS_STATE_TEST): $(BUS_STATE_TEST).o $(BUILDDIR_NATIVE)/emulator/hardware/EmBusTiming.o ../common/libcommon.a
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: bus-state-test
bus-state-test: $(BUS_STATE_TEST)
	$(BUS_STATE_TEST)
