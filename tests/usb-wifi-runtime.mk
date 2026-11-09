# Included after cloudpilot's Makefile, with FROND_68K=1.
USB_WIFI_TEST ?= $(FROND_TEST_ROOT)/build/test-usb-wifi-runtime
$(USB_WIFI_TEST).o: $(FROND_TEST_ROOT)/tests/usb-wifi-runtime.cpp emulator/hardware/EmRegsISPDana.h ../common/UsbPrism2Adapter.h ../common/net/WifiRouter.h emulator/hardware/UsbWifiRuntime.h emulator/hardware/UsbPrinterRuntime.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(USB_WIFI_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(USB_WIFI_TEST).o
	$(CXX_NATIVE) -o $@ $(USB_WIFI_TEST).o $(filter-out ../common/libcommon.a $(USB_WIFI_TEST).o,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: usb-wifi-test
usb-wifi-test: $(USB_WIFI_TEST)
	$(USB_WIFI_TEST)

# The Dana Wireless's built-in module, on the same core objects.
DANA_MODULE_TEST ?= $(FROND_TEST_ROOT)/build/test-dana-wifi-module
$(DANA_MODULE_TEST).o: $(FROND_TEST_ROOT)/tests/dana-wifi-module.cpp emulator/hardware/EmRegsISPDana.h ../common/UsbPrism2Adapter.h ../common/net/WifiRouter.h ../common/dana_wireless5.h emulator/hardware/UsbWifiRuntime.h emulator/hardware/UsbPrinterRuntime.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(DANA_MODULE_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(DANA_MODULE_TEST).o
	$(CXX_NATIVE) -o $@ $(DANA_MODULE_TEST).o $(filter-out ../common/libcommon.a $(DANA_MODULE_TEST).o,$^) ../common/libcommon.a $(LDFLAGS_NATIVE)
.PHONY: dana-module-test
dana-module-test: $(DANA_MODULE_TEST)
	$(DANA_MODULE_TEST)
