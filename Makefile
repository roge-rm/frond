.PHONY: all clean prepare test test-cpu test-tx-state test-sdl-audio test-fast-audio test-instruction-cache test-pxa-timer benchmark-session test-native-governor test-db-list test-export-checkpoint test-db-backup
BUILD_JOBS ?= 4
CPU_TEST_LDLIBS ?= -lcurl -lpthread -lresolv $(shell pkg-config --libs slirp)
all: out/frond desktop

prepare: build/source/.prepared

# Check input content on every invocation, including removed patches and the gitlink.
.PHONY: FORCE
FORCE:

build/source/.prepared: FORCE
	python3 tools/prepare.py

out/frond: build/source/.prepared build/source/src/common/libcommon.a
	$(MAKE) -C build/source/src/uarm bin
	mkdir -p out
	cp build/source/src/uarm/cp-uarm out/frond.new
	mv -f out/frond.new out/frond

test: test-build-tools prepare test-e2-state test-device-profiles test-uart-lifecycle test-uart-checkpoint test-uart-flow test-uart-dma-peer test-tx-serial test-gameplay-metrics test-mmu-fast-path test-pxa-power test-compatibility test-developer-run test-xscale-prefetch test-device-controls test-scheduler test-wifi test-host-network test-network-reliability test-keyboard test-network test-sd test-cpu test-arm-conditions test-tx-state test-sdl-audio test-fast-audio test-instruction-cache test-gdbstub test-pace-debug test-pace-step test-pxa-timer test-native-governor test-db-list test-export-checkpoint test-db-backup
	python3 tests/audio-metrics.py
	python3 tests/audio-timing.py
	python3 tests/tx-baseline.py
	python3 tests/input-latency.py
	python3 tests/menu-response.py
	python3 tests/guest-latency.py
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/wm9712-irq.cpp build/source/src/uarm/uarm/ac97dev_WM9712L.cpp \
		-Wl,--gc-sections -o build/test-wm9712-irq
	./build/test-wm9712-irq
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-ac97-irq.cpp build/source/src/uarm/uarm/pxa_AC97.cpp \
		-Wl,--gc-sections -o build/test-pxa-ac97-irq
	./build/test-pxa-ac97-irq
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm \
		tests/pace-patch.cpp build/source/src/uarm/uarm/pace_patch.cpp \
		-o build/test-pace-patch
	./build/test-pace-patch

test-cpu: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/arm-alignment.cpp @build/core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-arm-alignment
	./build/test-arm-alignment

test-tx-state: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tx-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tx-session-state.cpp @build/tx-core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tx-session-state
	./build/test-tx-session-state

# Always run the recursive make so edits in the generated source are rebuilt.
.PHONY: out/frond

clean:
	rm -rf build out


test-sdl-audio: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/audio-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm \
		-Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/sdl-audio-buffer.cpp build/source/src/uarm/native/SdlAudioDriver.cpp \
		@build/audio-core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-sdl-audio-buffer
	./build/test-sdl-audio-buffer


test-fast-audio: out/frond
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm \
		tests/fast-audio.cpp build/source/src/uarm/uarm/audio_queue.cpp \
		-pthread -o build/test-fast-audio
	./build/test-fast-audio
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/fast-audio-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/fast-audio-state.cpp @build/fast-audio-core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-fast-audio-state
	./build/test-fast-audio-state

test-instruction-cache: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/cache-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/instruction-cache.cpp @build/cache-core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-instruction-cache
	./build/test-instruction-cache

.PHONY: test-gdbstub
build/test-gdbstub-target: out/frond tests/gdbstub-target.cpp
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/gdbstub-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/gdbstub-target.cpp @build/gdbstub-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-gdbstub-target

test-gdbstub: build/test-gdbstub-target
	python3 tests/gdbstub.py

test-pxa-timer: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-timer.cpp build/source/src/uarm/uarm/pxa_TIMR.cpp \
		-Wl,--gc-sections -o build/test-pxa-timer
	./build/test-pxa-timer

benchmark-session: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/benchmark-core-objects.rsp
	$(CXX) -std=c++17 -O2 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tools/benchmark-session.cpp @build/benchmark-core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/benchmark-session

test-native-governor: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' ! -name cputil.o > build/governor-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/native-governor.cpp @build/governor-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-native-governor
	./build/test-native-governor

test-db-list: prepare
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/common \
		tests/db-list.cpp build/source/src/uarm/uarm/db_list.cpp -o build/test-db-list
	./build/test-db-list

test-export-checkpoint: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/export-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/export-checkpoint.cpp @build/export-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-export-checkpoint
	./build/test-export-checkpoint

test-db-backup: out/frond
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common -Ibuild/source/src/common/zip \
		tests/db-backup.cpp build/source/src/uarm/uarm/db_backup.cpp \
		build/source/src/common/libcommon.a -o build/test-db-backup
	./build/test-db-backup

.PHONY: test-arm-conditions benchmark-menu
test-arm-conditions: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/conditions-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/arm-conditions.cpp @build/conditions-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-arm-conditions
	./build/test-arm-conditions

benchmark-menu: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/menu-core.rsp
	$(CXX) -std=c++17 -O2 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tools/benchmark-menu.cpp @build/menu-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/benchmark-menu

.PHONY: desktop test-desktop deb
desktop: out/frond
	cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=RelWithDebInfo
	+cmake --build build/desktop --parallel $(BUILD_JOBS)
	cp build/desktop/frond-desktop out/frond-desktop.new
	mv -f out/frond-desktop.new out/frond-desktop

test-desktop: desktop
	ctest --test-dir build/desktop --output-on-failure

deb: desktop
	python3 tools/package-deb.py

.PHONY: test-sd
test-sd: out/frond
	python3 tests/sd-results.py
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/common -Ibuild/source/src/uarm/native \
		tests/sd-session.cpp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-sd-session
	./build/test-sd-session
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-mmc.cpp build/source/src/uarm/uarm/pxa_MMC.cpp build/source/src/uarm/uarm/vSD.cpp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -Wl,--gc-sections -o build/test-pxa-mmc
	./build/test-pxa-mmc
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-dma.cpp build/source/src/uarm/uarm/pxa_DMA.cpp -Wl,--gc-sections -o build/test-pxa-dma
	./build/test-pxa-dma

.PHONY: test-network
test-network: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/network-simulation.cpp build/source/src/uarm/uarm/NetworkSimulation.cpp build/source/src/uarm/uarm/HostNetwork.cpp \
		build/source/src/common/libcommon.a -lz -pthread -o build/test-network-simulation
	./build/test-network-simulation

.PHONY: test-keyboard
test-keyboard: out/frond
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/ir-keyboard.cpp build/source/src/common/libcommon.a -lz -o build/test-ir-keyboard
	./build/test-ir-keyboard

	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-uart.cpp build/source/src/uarm/uarm/pxa_UART.cpp -Wl,--gc-sections -o build/test-pxa-uart
	./build/test-pxa-uart
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/keyboard-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/host-keyboard.cpp build/source/src/uarm/native/SdlEventHandler.cpp @build/keyboard-core-objects.rsp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -o build/test-host-keyboard
	./build/test-host-keyboard

.PHONY: test-host-network
test-host-network: prepare
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm \
		tests/host-network.cpp build/source/src/uarm/uarm/HostNetwork.cpp -pthread -o build/test-host-network
	./build/test-host-network

.PHONY: test-wifi
test-wifi: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm \
		tests/wifi-router.cpp build/source/src/uarm/uarm/WifiRouter.cpp \
		$(shell pkg-config --cflags --libs slirp) -pthread -o build/test-wifi-router
	./build/test-wifi-router
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/libertas-cf.cpp build/source/src/uarm/uarm/LibertasCF.cpp \
		build/source/src/uarm/uarm/WifiRouter.cpp build/source/src/uarm/uarm/NetworkSimulation.cpp \
		build/source/src/uarm/uarm/HostNetwork.cpp build/source/src/common/libcommon.a \
		$(shell pkg-config --cflags --libs slirp) -pthread -lz -o build/test-libertas-cf
	./build/test-libertas-cf

.PHONY: test-scheduler
test-scheduler: prepare
	$(CXX) -std=c++17 -O2 -Ibuild/source/src/uarm -Ibuild/source/src/uarm/uarm -Ibuild/source/src/common tests/scheduler-order.cpp -o build/test-scheduler-order
	./build/test-scheduler-order

.PHONY: test-device-controls
test-device-controls: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/controls-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/device-controls.cpp build/source/src/uarm/native/SdlEventHandler.cpp \
		build/source/src/uarm/native/DeviceControls.cpp @build/controls-core-objects.rsp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -o build/test-device-controls
	./build/test-device-controls

.PHONY: test-developer-run
test-developer-run:
	python3 tests/developer-run.py

.PHONY: test-xscale-prefetch
test-xscale-prefetch: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/prefetch-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/xscale-prefetch.cpp @build/prefetch-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-xscale-prefetch
	./build/test-xscale-prefetch

.PHONY: test-compatibility
build/test-compatibility-core: out/frond tests/compatibility-core.cpp
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/compatibility-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/compatibility-core.cpp @build/compatibility-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-compatibility-core

test-compatibility: build/test-compatibility-core
	./build/test-compatibility-core
	python3 tests/compatibility.py

.PHONY: test-tx-power-rom
test-tx-power-rom: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/power-rom-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tx-power-rom.cpp @build/power-rom-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tx-power-rom

# Share one recursive library build across emulator and standalone tests,
# and always check for edits in generated common source.
.PHONY: build/source/src/common/libcommon.a
build/source/src/common/libcommon.a: build/source/.prepared
	$(MAKE) -C build/source/src/common bin

.PHONY: test-network-reliability
test-network-reliability: prepare
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm \
		tests/network-reliability.cpp build/source/src/uarm/uarm/HostNetwork.cpp \
		-pthread -o build/test-network-reliability
	./build/test-network-reliability
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm \
		tests/network-dns.cpp build/source/src/uarm/uarm/HostNetwork.cpp \
		-Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo -pthread -o build/test-network-dns
	./build/test-network-dns

.PHONY: test-pxa-power
test-pxa-power: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/power-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-power.cpp @build/power-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-pxa-power
	./build/test-pxa-power

.PHONY: test-pace-debug
test-pace-debug: build/test-gdbstub-target
	python3 tests/pace-debug.py
	python3 tests/pace-report.py

.PHONY: benchmark-replay
benchmark-replay: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' ! -name timeutil.o > build/replay-core.rsp
	$(CXX) -std=c++17 -O2 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tools/benchmark-replay.cpp @build/replay-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) $(shell pkg-config --libs sdl2 SDL2_image) -o build/benchmark-replay

.PHONY: test-mmu-fast-path
test-mmu-fast-path: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/mmu-core.rsp
	$(CXX) -std=c++17 -O2 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/mmu-fast-path.cpp @build/mmu-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-mmu-fast-path
	./build/test-mmu-fast-path

.PHONY: test-gameplay-metrics
test-gameplay-metrics: benchmark-replay
	python3 tests/gameplay-metrics.py

# Capability regressions report missing transport explicitly; no host endpoint or firmware.
.PHONY: test-tx-serial
test-tx-serial: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tx-serial-model.cpp build/source/src/uarm/uarm/pxa_UART.cpp \
		build/source/src/uarm/uarm/pxa270_UDC.cpp build/source/src/common/libcommon.a \
		-lz -o build/test-tx-serial-model
	./build/test-tx-serial-model
	python3 tests/tx-serial.py

.PHONY: device-profiles test-device-profiles
device-profiles: prepare
	mkdir -p out
	$(CXX) -std=c++17 -O2 -Ibuild/source/src/common tools/device-profiles.cpp -o out/frond-device-profiles

test-device-profiles: out/frond device-profiles
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/common -Ibuild/source/src/uarm/uarm \
		tests/device-profiles.cpp build/source/src/uarm/uarm/device_configuration.cpp \
		build/source/src/common/libcommon.a -o build/test-device-profiles
	./build/test-device-profiles
	python3 tests/device-input.py

# Firmware-free build/package policy and independently reconstructed source.
.PHONY: test-build-tools check-reconstruction
test-build-tools:
	python3 tests/build-tools.py

check-reconstruction:
	python3 tools/check-reconstruction.py

.PHONY: test-pace-step
test-pace-step: build/test-gdbstub-target
	python3 tests/pace-step.py
	python3 tests/watchpoint-contract.py

.PHONY: test-uart-lifecycle
test-uart-lifecycle: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/uart-lifecycle.cpp build/source/src/uarm/uarm/pxa_UART.cpp \
		build/source/src/common/libcommon.a -lz -o build/test-uart-lifecycle
	./build/test-uart-lifecycle

.PHONY: test-uart-checkpoint
test-uart-checkpoint: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/uart-checkpoint-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/uart-checkpoint.cpp @build/uart-checkpoint-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-uart-checkpoint
	./build/test-uart-checkpoint

.PHONY: test-e2-state
test-e2-state: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/e2-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/e2-state.cpp @build/e2-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-e2-state
	./build/test-e2-state

.PHONY: test-uart-flow
test-uart-flow: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/uart-flow.cpp build/source/src/uarm/uarm/pxa_UART.cpp \
		build/source/src/common/libcommon.a -lz -o build/test-uart-flow
	./build/test-uart-flow

.PHONY: test-uart-dma-peer
build/test-uart-dma-peer: out/frond tests/uart-dma-peer.cpp
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/uart-dma-peer-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/uart-dma-peer.cpp @build/uart-dma-peer-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-uart-dma-peer

test-uart-dma-peer: build/test-uart-dma-peer
	./build/test-uart-dma-peer
