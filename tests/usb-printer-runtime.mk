FROND_TEST_OBJ ?= $(FROND_TEST_ROOT)/build/out
# Included after cloudpilot's Makefile, with FROND_68K=1.
USB_RUNTIME_TEST ?= $(FROND_TEST_OBJ)/test-usb-printer-runtime
$(USB_RUNTIME_TEST).o: $(FROND_TEST_ROOT)/tests/usb-printer-runtime.cpp emulator/hardware/EmRegsISPDana.h ../common/UsbRawPrinter.h ../common/RawPrintJob.h ../common/RawPrintFile.h emulator/hardware/UsbPrinterRuntime.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(USB_RUNTIME_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(USB_RUNTIME_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out $(COMMON_LIB_DIR)libcommon.a,$^) $(COMMON_LIB_DIR)libcommon.a $(LDFLAGS_NATIVE)
.PHONY: usb-runtime-test
usb-runtime-test: $(USB_RUNTIME_TEST)
	$(USB_RUNTIME_TEST) $(USB_RUNTIME_OUTPUT)
