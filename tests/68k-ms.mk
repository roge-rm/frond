MS_TEST = $(FROND_TEST_ROOT)/build/test-68k-ms
$(MS_TEST).o: $(FROND_TEST_ROOT)/tests/68k-ms.cpp emulator/hardware/EmRegsMB86189.h emulator/hardware/MemoryStickBackend.h ../common/CardImage.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(MS_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(MS_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: ms-test
ms-test: $(MS_TEST)
	$(MS_TEST)
