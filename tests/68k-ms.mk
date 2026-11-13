RC_TEST = $(FROND_TEST_ROOT)/build/test-runtime-cards-ms
MS_TEST = $(FROND_TEST_ROOT)/build/test-68k-ms
$(MS_TEST).o: $(FROND_TEST_ROOT)/tests/68k-ms.cpp emulator/hardware/EmRegsMB86189.h emulator/hardware/MemoryStickBackend.h ../common/CardImage.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(MS_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(MS_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: ms-test
ms-test: $(MS_TEST) $(RC_TEST)
	$(MS_TEST)
	$(RC_TEST)

$(RC_TEST).o: $(FROND_TEST_ROOT)/tests/runtime-cards-ms.cpp frond/RuntimeCards.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(RC_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(RC_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(OBJECTS_EXTRA_NATIVE) $(LDFLAGS_NATIVE)
