# Included after cloudpilot's Makefile, with FROND_68K=1.
EZ_UART_SPI_TEST = $(FROND_TEST_ROOT)/build/test-68k-ez-uart-spi
$(EZ_UART_SPI_TEST).o: $(FROND_TEST_ROOT)/tests/68k-ez-uart-spi.cpp emulator/hardware/EmRegsEZ.h emulator/hardware/EmRegsVZ.h emulator/hardware/EmUARTDragonball.h
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(INCLUDE) -c -o $@ $<
$(EZ_UART_SPI_TEST): $(filter-out $(BUILDDIR_NATIVE)/frond/main.o,$(OBJECTS_NATIVE)) $(EZ_UART_SPI_TEST).o
	$(CXX_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE)
.PHONY: ez-uart-spi-test
ez-uart-spi-test: $(EZ_UART_SPI_TEST)
	$(EZ_UART_SPI_TEST)
