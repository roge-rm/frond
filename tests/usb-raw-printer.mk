FROND_TEST_OBJ ?= $(FROND_TEST_ROOT)/build/out
# Included after cloudpilot's Makefile, with FROND_68K=1.
USB_PRINTER_TEST ?= $(FROND_TEST_OBJ)/test-usb-raw-printer
$(USB_PRINTER_TEST).o: $(FROND_TEST_ROOT)/tests/usb-raw-printer.cpp emulator/hardware/EmRegsISPDana.h ../common/UsbRawPrinter.h ../common/RawPrintJob.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(USB_PRINTER_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(USB_PRINTER_TEST).o
	$(CXX_NATIVE) -o $@ $(filter-out $(COMMON_LIB_DIR)libcommon.a,$^) $(COMMON_LIB_DIR)libcommon.a $(LDFLAGS_NATIVE)
.PHONY: usb-printer-test
usb-printer-test: $(USB_PRINTER_TEST)
	$(USB_PRINTER_TEST)
