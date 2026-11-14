# Included after cloudpilot's Makefile, with FROND_68K=1.
HOST_TRANSPORT_TEST = $(FROND_TEST_ROOT)/build/test-68k-host-transport
$(HOST_TRANSPORT_TEST).o: $(FROND_TEST_ROOT)/tests/68k-host-transport.cpp frond/HostTransport.h frond/HostLink.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -Ifrond -c -o $@ $<
$(HOST_TRANSPORT_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(HOST_TRANSPORT_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: host-transport-test
host-transport-test: $(HOST_TRANSPORT_TEST)
	$(HOST_TRANSPORT_TEST)
