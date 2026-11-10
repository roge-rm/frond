# Included after cloudpilot's Makefile, with FROND_68K=1.
VZ_TEST = $(FROND_TEST_ROOT)/build/test-68k-vz-registers
$(VZ_TEST).o: $(FROND_TEST_ROOT)/tests/68k-vz-registers.cpp emulator/hardware/EmRegsVZ.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(VZ_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(VZ_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: vz-register-test
vz-register-test: $(VZ_TEST)
	$(VZ_TEST)
