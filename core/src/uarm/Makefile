-include ../Makefile.local

CFLAGS_COMMON := \
	-Wextra -Wall -Wno-unused-parameter -Wno-unknown-pragmas \
	-Wno-visibility -Wno-constant-conversion -Wno-constant-logical-operand \
	-D_FILE_OFFSET_BITS=64

CFLAGS_EMCC_EXTRA = -g

LDFLAGS_TEST_EXTRA ?= -lgtest_main

LDFLAGS_EMCC = $(EMCC_OPT) -Wno-version-check -Wl,-u,fileno -g \
	--post-js $(WEBIDL_BINDING_JS) \
	-s EXIT_RUNTIME=0 \
	-s MODULARIZE=1 \
	-s ENVIRONMENT="web,worker" \
	-s EXPORT_NAME=createModule \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=33554432 \
	-s WASM_BIGINT=1 \
	-s ALLOW_TABLE_GROWTH=1 \
	-s FILESYSTEM=0 \
	-s BINARYEN_IGNORE_IMPLICIT_TRAPS=1 \
	-s EXPORTED_RUNTIME_METHODS=["wasmTable","callMain","HEAPU8","HEAPU32"]

WEBIDL_BINDING_SRC = $(WEBIDL_BINDING_DIR)/uarm.idl ../common/web/session_file5.idl

INCLUDE = -I./uarm -I. -I../common -I../common/zip -I../argparse

STD_C ?= -std=gnu11

WASMOPT ?= $${EMSDK}/upstream/bin/wasm-opt
WASMOPT_FLAGS = --enable-bulk-memory --enable-bulk-memory-opt  --enable-sign-ext --enable-nontrapping-float-to-int  --inlining-optimizing \
	-fimfs 1000 -ifwl -pii 1000  -aimfs 10 -tnh -lmu -O4

WASMSTRIP ?= wasm-strip
YARN = yarn

SOURCE_C = 								\
	cputil.c							\
	uarm/uae/cpudefs.c 					\
	uarm/uae/cpuemu.c 					\
	uarm/uae/cpustbl.c 					\
	uarm/uae/readcpu.c 					\
	uarm/uae/newcpu.c

SOURCE_CXX =		 					\
	uarm/SoC.cpp 						\
	uarm/soc_PXA.cpp					\
	uarm/soc_pv.cpp						\
	uarm/soc_IMX1.cpp					\
	uarm/imx1_boot_probe.cpp			\
	uarm/imx1_bootstrap.cpp				\
	uarm/imx1_watchdog.cpp				\
	uarm/imx1_rtc.cpp					\
	uarm/imx1_aitc.cpp					\
	uarm/imx1_cgm.cpp					\
	uarm/imx1_gpio.cpp					\
	uarm/imx1_sysctrl.cpp				\
	uarm/imx1_eim.cpp					\
	uarm/imx1_timer.cpp					\
	uarm/imx1_i2c.cpp					\
	uarm/imx1_asp.cpp					\
	uarm/imx1_pwm.cpp					\
	uarm/pv_ic.cpp						\
	uarm/pv_timer.cpp					\
	uarm/pv_uart.cpp					\
	uarm/pv_hypercall_interface.cpp		\
	uarm/pv_display.cpp					\
	uarm/pv_keys.cpp					\
	uarm/pv_rtc.cpp 					\
	uarm/pv_audio.cpp 					\
	uarm/pv_touch.cpp 					\
	uarm/pv_sysctl.cpp					\
	uarm/pv_storage.cpp					\
	uarm/device_configuration.cpp 		\
	uarm/patch68k.cpp 					\
	uarm/peephole.cpp					\
	uarm/pace_patch.cpp					\
	uarm/app_launcher.cpp 				\
	uarm/timeutil.cpp					\
	uarm/keys.cpp 						\
	uarm/gdbstub.cpp 					\
	uarm/ROM.cpp						\
	uarm/RAM.cpp						\
	uarm/memory_buffer.cpp				\
	uarm/mem.cpp						\
	uarm/syscall.cpp					\
	uarm/sdcard.cpp						\
	uarm/patches.cpp					\
	MainLoop.cpp						\
	uarm/patch_dispatch.cpp				\
	uarm/arm920_dcache.cpp					\
	uarm/icache.cpp						\
	uarm/CPU.cpp						\
	uarm/pace.cpp 						\
	uarm/MMU.cpp						\
	uarm/MPU.cpp						\
	uarm/pxa270_WMMX.cpp				\
	uarm/pxa270_KPC.cpp					\
	uarm/pxa270_IMC.cpp					\
	uarm/pxa270_UDC.cpp					\
	uarm/pxa_TIMR.cpp 					\
	uarm/pxa_IC.cpp						\
	uarm/pxa_LCD.cpp					\
	uarm/pxa_DMA.cpp					\
	uarm/pxa_UART.cpp 					\
	uarm/pxa_SSP.cpp					\
	uarm/pxa_GPIO.cpp 					\
	uarm/pxa_AC97.cpp 					\
	uarm/pxa_I2S.cpp					\
	uarm/pxa_I2C.cpp					\
	uarm/pxa_MemCtrl.cpp				\
	uarm/pxa_PwrClk.cpp 				\
	uarm/pxa_PWM.cpp					\
	uarm/pxa_MMC.cpp					\
	uarm/pxa_RTC.cpp					\
	uarm/pxa255_DSP.cpp					\
	uarm/pxa255_UDC.cpp					\
	uarm/ac97dev_WM9712L.cpp			\
	uarm/nand.cpp 						\
	uarm/mmiodev_DirectNAND.cpp			\
	uarm/uartdev_bcm2035.cpp			\
	uarm/vSD.cpp						\
	uarm/devicePalmTungstenE2.cpp 		\
	uarm/deviceSonyTG50.cpp \
	uarm/mmiodev_TG50uc.cpp \
	uarm/sspdev_AD7873.cpp \
	uarm/i2sdev_AK4534.cpp \
	uarm/i2cdev_AN32502A.cpp \
	uarm/i2cdev_OV9640.cpp \
	uarm/pxa27x_QCI.cpp \
	uarm/camera_pattern.cpp \
	uarm/mmiodev_MemoryStickController.cpp \
	uarm/cp15mmu.cpp 					\
	uarm/cp15mpu.cpp 					\
	uarm/memcpy.cpp						\
	uarm/native_boundary.cpp \
	uarm/syscall_dispatch.cpp			\
	uarm/db_installer.cpp				\
	uarm/audio_queue.cpp				\
	uarm/db_list.cpp					\
	uarm/db_backup.cpp					\
	uarm/LibertasCF.cpp \
	uarm/HostNetwork.cpp			\
	uarm/HostSerial.cpp			\
	uarm/HostInfrared.cpp \
	uarm/BcspTransport.cpp					\
	uarm/CsrBcspRadio.cpp					\
	uarm/BluetoothBackend.cpp \
	uarm/UsbBusHost.cpp			\
	uarm/UsbHostBridge.cpp			\
	uarm/NetworkSimulation.cpp			\
	uarm/system_state.cpp

SOURCE_C_NATIVE = $(SOURCE_C)
SOURCE_CXX_NATIVE =						\
	$(SOURCE_CXX)						\
	native/Silkscreen.cpp				\
	native/DeviceControls.cpp			\
	native/SdlRenderer.cpp				\
	native/SdlEventHandler.cpp			\
	native/SdlAudioDriver.cpp			\
	native/Commands.cpp					\
	native/InputReplay.cpp				\
	native/main.cpp

SOURCE_C_EMCC = $(SOURCE_C)
SOURCE_CXX_EMCC = 						\
	$(SOURCE_CXX)						\
	web/main.cpp						\
	web/uarm.cpp						\
	web/bridge.cpp

SOURCE_C_TEST = $(SOURCE_C)

SOURCE_CXX_TEST =						\
	$(SOURCE_CXX)						\
	test/scheduler.cpp 					\
	test/queue.cpp

# The infrared phone's and Wi-Fi's slirp side (common/net), ahead of libcommon.a.
OBJECTS_EXTRA_NATIVE = ../common/libcommon-net.a ../common/libcommon.a
OBJECTS_EXTRA_TEST = ../common/libcommon-net.a ../common/libcommon.a
OBJECTS_EXTRA_EMCC = ../common/libcommon-wasm.a

# Fast engine: FROND_DYNARMIC_SRC is a patched Dynarmic source tree and FROND_DYNARMIC_LIBS its
# built static libraries, in link order. Without them the core builds with the interpreter only.
ifneq ($(FROND_DYNARMIC_SRC),)
SOURCE_CXX_NATIVE += uarm/jit_dynarmic.cpp
OBJECTS_EXTRA_NATIVE += $(FROND_DYNARMIC_LIBS)
endif

BINARY_NATIVE = cp-uarm$(EXE_SUFFIX)
BINARY_EMCC = uarm_web.wasm
BINARY_TEST = test/test

PROCESSED_BINARY_WASM = uarm_web_processed.wasm
OPTIMIZED_BINARY_WASM = uarm_web_optimized.wasm

GARBAGE_EXTRA = 						\
	$(PROCESSED_BINARY_WASM)			\
	$(PROCESSED_BINARY_WASM).s			\
	$(OPTIMIZED_BINARY_WASM)

include ../Makefile.common

bin: $(BINARY_NATIVE)

# The Zodiac developer build, for tests and bring-up: the same core, with main.cpp also taking a
# bring-up state file from FROND_ZODIAC_STATE. Its object follows main.o for the header dependencies.
BINARY_ZODIAC_DEVELOPER = cp-uarm-zodiac-developer$(EXE_SUFFIX)
MAIN_ZODIAC_DEVELOPER = $(BUILDDIR_NATIVE)/native/main-zodiac-developer.o
$(MAIN_ZODIAC_DEVELOPER): native/main.cpp $(BUILDDIR_NATIVE)/native/main.o
	$(CXX_NATIVE) $(STD_CXX) $(CXXFLAGS_COMMON) $(CXXFLAGS_NATIVE) $(CXXFLAGS_NATIVE_EXTRA) -DFROND_ZODIAC_DEVELOPER $(INCLUDE) $(INCLUDE_EXTRA) -c -o $@ $<
$(BINARY_ZODIAC_DEVELOPER): $(filter-out $(BUILDDIR_NATIVE)/native/main.o,$(OBJECTS_NATIVE)) $(MAIN_ZODIAC_DEVELOPER)
	$(LD_NATIVE) -o $@ $^ $(LDFLAGS_NATIVE) $(LDFLAGS_NATIVE_EXTRA)
zodiac-developer: $(BINARY_ZODIAC_DEVELOPER)

test: $(BINARY_TEST)
	$(BINARY_TEST)

emscripten: $(OPTIMIZED_BINARY_WASM)

$(PROCESSED_BINARY_WASM): $(BINARY_EMCC)
	if test -z "$(DEVELOP)"; then $(YARN) --cwd ./tools/build-jump-table && $(YARN) --cwd ./tools/build-jump-table build-jump-table "$$(pwd)/$^" "$$(pwd)/$@"; else cp $^ $@; fi

$(OPTIMIZED_BINARY_WASM): $(PROCESSED_BINARY_WASM)
	if test -n "$(DEVELOP)"; then cp $^ $@; else $(WASMOPT) $(WASMOPT_FLAGS) -o $@ $^; fi
	if test -z "$(DEVELOP)"; then $(WASMSTRIP) $@; fi

.PHONY: bin emscripten clean test zodiac-developer

# Native virtual wireless router
CXXFLAGS_NATIVE_EXTRA += $(shell pkg-config --cflags slirp)
LDFLAGS_NATIVE_EXTRA += $(shell pkg-config --libs slirp)
CXXFLAGS_TEST_EXTRA += $(shell pkg-config --cflags slirp)
LDFLAGS_TEST_EXTRA += $(shell pkg-config --libs slirp)


ifneq ($(FROND_DYNARMIC_SRC),)
CXXFLAGS_NATIVE_EXTRA += -DFROND_DYNARMIC
$(BUILDDIR_NATIVE)/uarm/jit_dynarmic.o: STD_CXX = -std=c++20
# Only Dynarmic's headers: the core's include folders hide system ones (uarm/syscall.h).
$(BUILDDIR_NATIVE)/uarm/jit_dynarmic.o: INCLUDE = -I$(FROND_DYNARMIC_SRC)/src \
	-I$(FROND_DYNARMIC_SRC)/externals/mcl/include -I$(FROND_DYNARMIC_SRC)/externals/fmt/include
endif

# CPU.o and icache.o change with FROND_DYNARMIC, so switching it rebuilds them.
JIT_STAMP := $(BUILDDIR_NATIVE)/.jit-$(if $(FROND_DYNARMIC_SRC),on,off)
$(BUILDDIR_NATIVE)/uarm/CPU.o $(BUILDDIR_NATIVE)/uarm/icache.o: $(JIT_STAMP)
$(JIT_STAMP):
	mkdir -p $(BUILDDIR_NATIVE) && rm -f $(BUILDDIR_NATIVE)/.jit-* && touch $@
