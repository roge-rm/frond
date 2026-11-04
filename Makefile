.PHONY: all clean prepare test test-cpu test-tx-state test-sdl-audio test-fast-audio test-instruction-cache test-pxa-timer benchmark-session test-native-governor test-db-list test-export-checkpoint test-db-backup test-checkpoint-input test-native-database
BUILD_JOBS ?= 4
CPU_TEST_LDLIBS ?= -lcurl -lpthread -lresolv $(shell pkg-config --libs slirp)

# The Fast engine (Dynarmic JIT) is built in on x86-64 and arm64 hosts; FROND_FAST=0 leaves it out.
# Its source is exported into build/ by make dynarmic-source and built once in build/dynarmic.
HOST_ARCH := $(shell uname -m)
FROND_FAST ?= $(if $(filter x86_64 aarch64 arm64,$(HOST_ARCH)),1,0)
DYNARMIC_SRC ?= build/dynarmic-trial/src
JIT_LIBS = build/dynarmic/src/dynarmic/libdynarmic.a build/dynarmic/externals/mcl/src/libmcl.a \
	build/dynarmic/externals/fmt/libfmt.a \
	$(if $(filter x86_64,$(HOST_ARCH)),build/dynarmic/externals/zydis/libZydis.a \
	build/dynarmic/externals/zydis/zycore/libZycore.a)
ifeq ($(FROND_FAST),1)
CORE_JIT_DEPS = build/dynarmic/.built
CORE_JIT_FLAGS = FROND_DYNARMIC_SRC=$(abspath $(DYNARMIC_SRC)) FROND_DYNARMIC_LIBS="$(abspath $(JIT_LIBS))"
CPU_TEST_LDLIBS += $(JIT_LIBS)
endif

all: out/frond desktop

.PHONY: test-memory-stick-media
test: test-memory-stick-controller test-memory-stick-media
test-memory-stick-media: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Ibuild/source/src/uarm/uarm tests/memory-stick-media.cpp -o build/test-memory-stick-media
	./build/test-memory-stick-media

.PHONY: test-card-backing
test: test-card-backing
test-card-backing: prepare
	$(CC) -std=gnu99 -O1 -g -Ibuild/source/src/common -c build/source/src/common/zip/miniz.c \
		-o build/miniz-card-backing.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/common \
		tests/card-backing.cpp tests/card-backing-generations.cpp \
		build/miniz-card-backing.o -o build/test-card-backing
	./build/test-card-backing

.PHONY: test-session-v7
test: test-session-v7
test-session-v7: prepare
	$(CC) -std=gnu99 -O1 -g -Ibuild/source/src/common -c build/source/src/common/zip/miniz.c \
		-o build/miniz-session-v7.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/common \
		tests/session-v7.cpp build/source/src/common/session/session_file5.cpp \
		build/source/src/common/md5.cpp build/source/src/uarm/uarm/sdcard.cpp build/miniz-session-v7.o \
		-o build/test-session-v7
	./build/test-session-v7

prepare: build/source/.prepared

# Check input content on every invocation, including removed patches and the gitlink.
.PHONY: FORCE
FORCE:

build/source/.prepared: FORCE
	python3 tools/prepare.py

out/frond: build/source/.prepared build/source/src/common/libcommon.a $(CORE_JIT_DEPS)
	$(MAKE) -C build/source/src/uarm bin $(CORE_JIT_FLAGS)
	mkdir -p out
	cp build/source/src/uarm/cp-uarm out/frond.new
	mv -f out/frond.new out/frond

# The 68k runner for DragonBall devices, from Cloudpilot's core in src/cloudpilot.
out/frond-68k: build/source/.prepared build/source/src/common/libcommon.a
	$(MAKE) -C build/source/src/cloudpilot FROND_68K=1 frond-68k
	mkdir -p out
	cp build/source/src/cloudpilot/frond-68k out/frond-68k.new
	mv -f out/frond-68k.new out/frond-68k

test: test-tg50-pwm-audio test-pxa-i2s-audio test-build-tools test-audio-results test-earlier-audio prepare test-pxa255-udc test-pxa270-udc test-usb-host-fixture test-usb-enumeration test-usb-bridge test-usb-bridge-soc test-e2-state test-tg50-state test-device-profiles test-uart-lifecycle test-uart-checkpoint test-uart-flow test-uart-dma-peer test-tx-serial test-gameplay-metrics test-mmu-fast-path test-mem-region-bounds test-pxa-power test-compatibility test-developer-run test-xscale-prefetch test-device-controls test-scheduler test-wifi test-host-network test-network-reliability test-keyboard test-network test-sd-image test-sd test-cpu test-arm-conditions test-tx-state test-sdl-audio test-fast-audio test-instruction-cache test-gdbstub test-pace-debug test-pace-step test-pxa-timer test-native-governor test-db-list test-export-checkpoint test-db-backup test-checkpoint-input test-native-database
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


.PHONY: test-replay-snapshot
test: test-replay-snapshot
test-replay-snapshot: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/replay-snapshot-core.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm \
		-Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/replay-snapshot.cpp build/source/src/uarm/.build/native/SdlRenderer.o \
		build/source/src/uarm/.build/native/InputReplay.o build/source/src/uarm/.build/native/Silkscreen.o \
		build/source/src/uarm/.build/native/DeviceControls.o \
		@build/replay-snapshot-core.rsp build/source/src/common/libcommon.a \
		$(shell sdl2-config --libs) -lSDL2_image $(CPU_TEST_LDLIBS) -o build/test-replay-snapshot
	./build/test-replay-snapshot

test-sdl-audio: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/audio-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm \
		-Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/sdl-audio-buffer.cpp build/source/src/uarm/native/SdlAudioDriver.cpp \
		@build/audio-core-objects.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-sdl-audio-buffer
	./build/test-sdl-audio-buffer

.PHONY: test-render-link
test: test-render-link
test-render-link: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm \
		tests/render-link.cpp $(shell sdl2-config --libs) -lpthread -o build/test-render-link
	./build/test-render-link

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

.PHONY: test-jit-overshoot
ifeq ($(FROND_FAST),1)
test: test-jit-overshoot
endif
# How far a cpuCycle call runs past its budget in each engine, against cpuMaxOvershootCycles.
test-jit-overshoot: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/jit-overshoot-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/jit-overshoot.cpp @build/jit-overshoot-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-jit-overshoot
	./build/test-jit-overshoot

# Random ARM and Thumb blocks run on both engines and compared, plus directed tests.
# Seeds 1 to 30000 and the directed tests, then 30001 to 40000 with an unchecked limit of 256.
# JIT_DIFF_ARGS="--seeds 1: --duration 3600" for a long run, "--seed N" for one case.
.PHONY: test-jit-differential build/test-jit-differential
ifeq ($(FROND_FAST),1)
test: test-jit-differential
endif
build/test-jit-differential: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/jit-differential-core.rsp
	$(CXX) -std=c++17 -O2 -g \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/jit-differential.cpp @build/jit-differential-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-jit-differential
test-jit-differential: build/test-jit-differential
	@# The core logs every guest data abort to stderr, so that goes to a file.
	./build/test-jit-differential $(JIT_DIFF_ARGS) 2>build/jit-differential.err || \
		{ grep -v ' failed (pc=' build/jit-differential.err | tail -5; exit 1; }
	test -n "$(JIT_DIFF_ARGS)" || ./build/test-jit-differential --seeds 30001:40001 --no-directed --max-unchecked 256 \
		2>build/jit-differential.err || { grep -v ' failed (pc=' build/jit-differential.err | tail -5; exit 1; }
	test -n "$(JIT_DIFF_ARGS)" || ./build/test-jit-differential --seeds 40001:50001 --no-directed --hot 3 \
		2>build/jit-differential.err || { grep -v ' failed (pc=' build/jit-differential.err | tail -5; exit 1; }

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
	+cmake --build build/desktop --target frond-desktop --parallel $(BUILD_JOBS)
	cp build/desktop/frond-desktop out/frond-desktop.new
	mv -f out/frond-desktop.new out/frond-desktop

test-desktop: desktop
	cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
	+cmake --build build/desktop --parallel $(BUILD_JOBS)
	ctest --test-dir build/desktop --output-on-failure

deb: desktop
	python3 tools/package-deb.py

# arm64 package for Raspberry Pi OS trixie, cross built in Docker. Needs Docker only.
# The core is prepared fresh in build/arm64/source and mounted over build/source.
ARM64_JOBS ?= 10
.PHONY: deb-arm64
deb-arm64: build/dynarmic-trial/src/.frond-source
	rm -rf build/arm64/source
	python3 tools/prepare.py --destination $(CURDIR)/build/arm64/source
	docker build -t frond-arm64-cross tools/arm64
	docker run --rm --user $$(id -u):$$(id -g) -e JOBS=$(ARM64_JOBS) \
		-e SOURCE_DATE_EPOCH=$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct)} \
		-v $(CURDIR):/src -v $(CURDIR)/build/arm64/source:/src/build/source \
		-w /src frond-arm64-cross tools/arm64/build.sh

.PHONY: test-sd-image test-sd
test-sd-image: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Ibuild/source/src/uarm/native tests/sd-image.cpp -o build/test-sd-image
	./build/test-sd-image

test-sd: out/frond
	python3 tests/sd-results.py
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/common -Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm \
		tests/sd-session.cpp build/source/src/uarm/uarm/sdcard.cpp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-sd-session
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
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/ir-keyboard-board.cpp @build/keyboard-core-objects.rsp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-ir-keyboard-board
	./build/test-ir-keyboard-board

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
		build/source/src/uarm/native/DeviceControls.cpp build/source/src/uarm/native/SdlRenderer.cpp \
		build/source/src/uarm/native/Silkscreen.cpp @build/controls-core-objects.rsp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -lSDL2_image -o build/test-device-controls
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
	python3 tests/app-compatibility.py

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

.PHONY: test-mem-region-bounds
test-mem-region-bounds: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/mem-bounds-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/mem-region-bounds.cpp @build/mem-bounds-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-mem-region-bounds
	./build/test-mem-region-bounds

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

.PHONY: test-frond-68k
test: test-frond-68k
test-frond-68k: out/frond-68k build/test-cpu68k-timing build/test-frond-68k-sound
	./build/test-cpu68k-timing
	./build/test-frond-68k-sound
	python3 tests/frond-68k.py

# Piezo, backlight, sleep, serial HotSync, databases, GDB and IrDA on the private IIIx ROM; skips
# without it. Add --rom with FROND_68K_ROMS="--rom A --rom B" for other ROMs.
.PHONY: test-frond-68k-peripherals
test-frond-68k-peripherals: out/frond-68k build/test-frond-68k-sound
	./build/test-frond-68k-sound
	python3 tests/frond-68k-peripherals.py $(FROND_68K_ROMS)

build/test-frond-68k-sound: tests/frond-68k-sound.cpp build/source/.prepared
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/cloudpilot/frond tests/frond-68k-sound.cpp -o $@

CPU68K_UAE = build/source/src/cloudpilot/emulator/uae
build/test-cpu68k-timing: tests/cpu68k-timing.cpp build/source/.prepared
	$(CXX) -std=c++17 -O1 -g -I$(CPU68K_UAE) -Ibuild/source/src/cloudpilot/emulator/hardware \
		-Ibuild/source/src/cloudpilot/emulator -Ibuild/source/src/common -DEMULATION_LEVEL=EMULATION_UNIX \
		tests/cpu68k-timing.cpp $(CPU68K_UAE)/readcpu.cpp $(CPU68K_UAE)/cputiming.cpp \
		build/source/src/cloudpilot/emulator/hardware/EmBusTiming.cpp \
		-x c $(CPU68K_UAE)/cpudefs.c -x none -o $@

.PHONY: device-profiles test-device-profiles
device-profiles: prepare
	mkdir -p out
	$(CXX) -std=c++17 -O2 -Ibuild/source/src/common tools/device-profiles.cpp -o out/frond-device-profiles

test-device-profiles: out/frond device-profiles
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/common -Ibuild/source/src/uarm/uarm \
		tests/device-profiles.cpp build/source/src/uarm/uarm/device_configuration.cpp \
		build/source/src/common/libcommon.a -o build/test-device-profiles
	./build/test-device-profiles
	python3 tests/device-rom.py --device sony-clie-tg50 --synthetic-rejection
	python3 tests/device-input.py
	python3 tools/generate-device-data.py --check
	python3 tests/device-data.py
	python3 tests/device-rom-harness.py
	python3 tests/guest-faults.py
	python3 tests/device-rom.py --device palm-zire-72

# Firmware-free build/package policy and independently reconstructed source.
.PHONY: test-build-tools check-reconstruction
test-build-tools:
	python3 tests/build-tools.py

check-reconstruction:
	python3 tools/check-reconstruction.py

# Every option, command, environment variable, tool and target in the sources is in the developer reference.
.PHONY: check-docs
check-docs: prepare
	python3 tools/check-docs.py

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

.PHONY: test-pxa255-udc
test-pxa255-udc: build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa255-udc.cpp build/source/src/common/libcommon.a -o build/test-pxa255-udc
	./build/test-pxa255-udc

.PHONY: test-pxa270-udc
test-pxa270-udc: build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa270-udc.cpp build/source/src/common/libcommon.a -o build/test-pxa270-udc
	./build/test-pxa270-udc

.PHONY: test-usb-enumeration
test-usb-enumeration: build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/usb-enumeration.cpp build/source/src/common/libcommon.a -o build/test-usb-enumeration
	./build/test-usb-enumeration

.PHONY: test-usb-bridge
test-usb-bridge: build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Wall -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/usb-bridge.cpp build/source/src/uarm/uarm/pxa255_UDC.cpp build/source/src/uarm/uarm/UsbBusHost.cpp build/source/src/uarm/uarm/HostSerial.cpp \
		build/source/src/uarm/uarm/pxa_UART.cpp build/source/src/common/libcommon.a -lz -lpthread -o build/test-usb-bridge
	timeout 300 ./build/test-usb-bridge

.PHONY: test-usb-bridge-soc
test-usb-bridge-soc: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/usb-bridge-soc-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/usb-bridge-soc.cpp @build/usb-bridge-soc-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-usb-bridge-soc
	timeout 300 ./build/test-usb-bridge-soc

# TG50 USB HotSync over a PTY with pilot-link; skips without the TG50 session (make smoke-sessions) or pilot-link.
.PHONY: test-tg50-usb-hotsync
test-tg50-usb-hotsync: out/frond
	python3 tests/tg50-usb-hotsync.py

.PHONY: test-usb-host-fixture test-tx-usb-rom
test-usb-host-fixture: build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/usb-host-fixture.cpp build/source/src/common/libcommon.a -o build/test-usb-host-fixture
	./build/test-usb-host-fixture

# Optional diagnostic: builds only; run explicitly with a private TX session.
# The fixture includes the UDC implementation to inspect hardware-owned state.
test-tx-usb-rom: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' ! -name pxa270_UDC.o > build/usb-rom-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tx-usb-rom.cpp @build/usb-rom-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tx-usb-rom

.PHONY: test-checkpoint-input test-native-database
test-checkpoint-input: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/checkpoint-input-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/checkpoint-input.cpp @build/checkpoint-input-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-checkpoint-input
	./build/test-checkpoint-input

test-native-database: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm -Ibuild/source/src/uarm/uarm -Ibuild/source/src/common -Ibuild/source/src/common/zip \
		tests/native-database.cpp build/source/src/common/libcommon.a -o build/test-native-database
	./build/test-native-database

# Firmware-free native CPU/callback/memory and SoC lifecycle regressions.
.PHONY: test-native-primitives
test: test-native-primitives
test-native-primitives: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/native-primitives-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/native-primitives.cpp @build/native-primitives-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-native-primitives
	./build/test-native-primitives exact-hooks
	./build/test-native-primitives nested-hooks
	./build/test-native-primitives buffers
	./build/test-native-primitives scratch
	./build/test-native-primitives stack-owner
	./build/test-native-primitives runtime-replacement
	./build/test-native-primitives soc-lifecycle

.PHONY: test-debugger-inspect
test: test-debugger-inspect
test-debugger-inspect: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/debugger-inspect-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/debugger-inspect.cpp @build/debugger-inspect-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-debugger-inspect
	./build/test-debugger-inspect

.PHONY: test-lifecycle test-tx-lifecycle-rom
test: test-lifecycle
test-lifecycle: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/lifecycle-core.rsp
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' ! -name cputil.o > build/audio-lifecycle-core.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/audio-lifecycle.cpp build/source/src/uarm/native/SdlAudioDriver.cpp \
		@build/audio-lifecycle-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-audio-lifecycle
	./build/test-audio-lifecycle
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/peripheral-lifecycle.cpp @build/lifecycle-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-peripheral-lifecycle
	./build/test-peripheral-lifecycle
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/sd-lifecycle.cpp build/source/src/uarm/uarm/pxa_MMC.cpp build/source/src/uarm/uarm/vSD.cpp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -Wl,--gc-sections -o build/test-sd-lifecycle
	./build/test-sd-lifecycle

# Optional: build only; invoke explicitly with private sessions and a host deadline.
test-tx-lifecycle-rom: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tx-lifecycle-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tx-lifecycle-rom.cpp @build/tx-lifecycle-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tx-lifecycle-rom

.PHONY: test-host-serial
test: test-host-serial
test-host-serial: prepare build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Wall -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/host-serial.cpp build/source/src/uarm/uarm/pxa_UART.cpp build/source/src/uarm/uarm/HostSerial.cpp \
		build/source/src/common/libcommon.a -lz -lpthread -o build/test-host-serial
	timeout 120 ./build/test-host-serial

.PHONY: test-audio-results
test-audio-results:
	python3 tests/audio-results.py

.PHONY: test-earlier-audio
test-earlier-audio: out/frond
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/early-audio-ledger.cpp -o build/test-early-audio-ledger
	./build/test-early-audio-ledger
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/early-audio.cpp build/source/src/uarm/uarm/audio_queue.cpp -pthread -o build/test-early-audio
	./build/test-early-audio
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/early-audio-checkpoint.cpp build/source/src/uarm/uarm/audio_queue.cpp -pthread -o build/test-early-audio-checkpoint
	./build/test-early-audio-checkpoint
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/patch-dispatch.cpp build/source/src/uarm/uarm/patch_dispatch.cpp build/source/src/uarm/.build/cputil.o build/source/src/common/libcommon.a -lz -o build/test-patch-dispatch
	./build/test-patch-dispatch

.PHONY: test-irda-endpoint
test: test-irda-endpoint
test-irda-endpoint: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/irda-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/irda-endpoint.cpp @build/irda-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-irda-endpoint
	./build/test-irda-endpoint

.PHONY: test-irda-results
test: test-irda-results
test-irda-results:
	python3 tests/irda-results.py

.PHONY: test-irda-link
test: test-irda-link
test-irda-link:
	python3 tests/irda-link.py

.PHONY: test-bluetooth-link
test: test-bluetooth-link
test-bluetooth-link:
	python3 tests/bluetooth-link.py

.PHONY: test-pty-link
test: test-pty-link
test-pty-link:
	python3 tests/pty-link.py

.PHONY: test-bluetooth
test: test-bluetooth
test-bluetooth: out/frond
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/bluetooth-uart-messaging.cpp -o build/test-bluetooth-uart-messaging
	./build/test-bluetooth-uart-messaging
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/bluetooth-bootstrap.cpp build/source/src/common/libcommon.a -lz -o build/test-bluetooth-bootstrap
	./build/test-bluetooth-bootstrap
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm \
		tests/bluetooth-backend.cpp build/source/src/uarm/uarm/BluetoothBackend.cpp -o build/test-bluetooth-backend
	./build/test-bluetooth-backend
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/bluetooth-controller.cpp build/source/src/uarm/uarm/uartdev_bcm2035.cpp \
		build/source/src/uarm/uarm/BluetoothBackend.cpp build/source/src/common/libcommon.a -lz -o build/test-bluetooth-controller
	./build/test-bluetooth-controller
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/bluetooth-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm \
		-Ibuild/source/src/uarm -Ibuild/source/src/common tests/bluetooth-soc.cpp @build/bluetooth-core.rsp \
		build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-bluetooth-soc
	./build/test-bluetooth-soc

.PHONY: test-bluetooth-results
test: test-bluetooth-results
test-bluetooth-results:
	python3 tests/bluetooth-results.py

.PHONY: test-peripheral-qualification
test: test-peripheral-qualification
test-peripheral-qualification:
	python3 tests/peripheral-qualification.py

.PHONY: smoke smoke-sessions
smoke:
	python3 tests/smoke.py run
smoke-sessions:
	python3 tests/smoke.py sessions

.PHONY: generate-device-data check-device-data
generate-device-data: prepare
	python3 tools/generate-device-data.py
check-device-data: prepare
	python3 tools/generate-device-data.py --check

.PHONY: test-tg50-state
test-tg50-state: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-state.cpp @build/tg50-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tg50-state
	./build/test-tg50-state
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-touch.cpp build/source/src/uarm/uarm/sspdev_AD7873.cpp \
		build/source/src/uarm/.build/cputil.o build/source/src/common/libcommon.a -lz -o build/test-tg50-touch
	./build/test-tg50-touch

# TG50 sound replay with a guest-time consumer and forced backpressure, in Exact and Fast.
.PHONY: test-tg50-audio-replay
test: test-tg50-audio-replay
test-tg50-audio-replay: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-audio-replay-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-audio-replay.cpp @build/tg50-audio-replay-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tg50-audio-replay
	ulimit -c 0; ./build/test-tg50-audio-replay exact
	ulimit -c 0; ./build/test-tg50-audio-replay fast

# Negative check: admission is told a smaller bound than the JIT keeps to, and the reservation
# debug counter has to report overruns.
.PHONY: test-audio-reservation-negative
ifeq ($(FROND_FAST),1)
test: test-audio-reservation-negative
endif
test-audio-reservation-negative: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-audio-replay-core.rsp
	$(CXX) -std=c++17 -O1 -g -DFROND_BROKEN_BOUND -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-audio-replay.cpp @build/tg50-audio-replay-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -Wl,--wrap=_Z21cpuMaxOvershootCyclesPK6ArmCpu -o build/test-audio-reservation-negative
	ulimit -c 0; FROND_AUDIO_RESERVATION_STATS=1 ./build/test-audio-reservation-negative broken-bound

# Needs a private TG50 session with the Voice Recorder ready to play: TG50_PREVIEW_SESSION=path.
.PHONY: test-tg50-preview-replay
test-tg50-preview-replay: out/frond
	test -n "$(TG50_PREVIEW_SESSION)" || { echo "set TG50_PREVIEW_SESSION"; exit 1; }
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' ! -name timeutil.o > build/tg50-preview-replay-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-preview-replay.cpp @build/tg50-preview-replay-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tg50-preview-replay
	ulimit -c 0; ./build/test-tg50-preview-replay exact "$(TG50_PREVIEW_SESSION)"
	ulimit -c 0; ./build/test-tg50-preview-replay fast "$(TG50_PREVIEW_SESSION)"

.PHONY: device-session-probe
device-session-probe: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/device-session-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/device-session.cpp @build/device-session-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-device-session

.PHONY: test-pxa-i2s-audio
test-pxa-i2s-audio: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-i2s-audio.cpp build/source/src/uarm/uarm/pxa_I2S.cpp \
		build/source/src/uarm/uarm/i2sdev_AK4534.cpp build/source/src/uarm/uarm/audio_queue.cpp \
		build/source/src/common/savestate/Chunk.cpp build/source/src/common/savestate/ChunkProbe.cpp \
		build/source/src/common/Logging.cpp -Wl,--gc-sections -o build/test-pxa-i2s-audio
	./build/test-pxa-i2s-audio
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa-i2s-dma.cpp build/source/src/uarm/uarm/pxa_I2S.cpp \
		build/source/src/uarm/uarm/pxa_DMA.cpp -Wl,--gc-sections -o build/test-pxa-i2s-dma
	./build/test-pxa-i2s-dma

.PHONY: test-tg50-pace
test: test-tg50-pace
test-tg50-pace: prepare
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm \
		tests/tg50-pace.cpp build/source/src/uarm/uarm/pace_patch.cpp -o build/test-tg50-pace
	./build/test-tg50-pace

.PHONY: test-tg50-pwm-audio
test-tg50-pwm-audio: prepare
	$(CXX) -std=c++17 -O2 -Ibuild/source/src/uarm/uarm tests/tg50-pwm-audio.cpp -o build/test-tg50-pwm-audio
	./build/test-tg50-pwm-audio

.PHONY: test-tg50-call-budget
test: test-tg50-call-budget
test-tg50-call-budget: prepare
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/tg50-call-budget.cpp -o build/test-tg50-call-budget
	./build/test-tg50-call-budget

.PHONY: test-memory-stick-controller
test-memory-stick-controller: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/memory-stick-controller.cpp build/source/src/uarm/uarm/mmiodev_MemoryStickController.cpp \
		build/source/src/common/libcommon.a -lz -o build/test-memory-stick-controller
	./build/test-memory-stick-controller

.PHONY: test-memory-stick-pro
test: test-memory-stick-pro
test-memory-stick-pro: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/uarm/native -Ibuild/source/src/common \
		tests/memory-stick-pro.cpp build/source/src/uarm/uarm/mmiodev_MemoryStickController.cpp \
		build/source/src/common/libcommon.a -lz -o build/test-memory-stick-pro
	./build/test-memory-stick-pro

.PHONY: test-tg50-serial-configuration test-microwire64x16
test: test-tg50-serial-configuration test-microwire64x16
test-microwire64x16: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Ibuild/source/src/uarm/uarm tests/microwire64x16.cpp -o build/test-microwire64x16
	./build/test-microwire64x16

test-tg50-serial-configuration: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-serial-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-serial-configuration.cpp @build/tg50-serial-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-tg50-serial-configuration
	./build/test-tg50-serial-configuration

# FrondBench, a Palm benchmark for calibrating emulated CPU speed.
PALM_SDK ?= $(HOME)/Projects/palm/palm-os-sdk/sdk-5r3
FRONDBENCH_DIR := build/frondbench
FRONDBENCH_SRC := tools/frondbench
.PHONY: frondbench
frondbench: $(FRONDBENCH_DIR)/FrondBench.prc
$(FRONDBENCH_DIR)/FrondBench.prc: $(FRONDBENCH_SRC)/frondbench.c $(FRONDBENCH_SRC)/frondbench.h $(FRONDBENCH_SRC)/frondbench.rcp $(FRONDBENCH_SRC)/frondbench.def
	mkdir -p $(FRONDBENCH_DIR)
	m68k-palmos-gcc -O2 -Wall -Wno-unknown-pragmas -I$(PALM_SDK)/include \
		$(addprefix -I,$(shell find $(PALM_SDK)/include -mindepth 1 -type d)) \
		-c $(FRONDBENCH_SRC)/frondbench.c -o $(FRONDBENCH_DIR)/frondbench.o
	m68k-palmos-gcc -L$(PALM_SDK)/lib/m68k-palmos-coff $(FRONDBENCH_DIR)/frondbench.o -o $(FRONDBENCH_DIR)/frondbench
	cd $(FRONDBENCH_SRC) && pilrc -q -ro frondbench.rcp $(CURDIR)/$(FRONDBENCH_DIR)/frondbench.ro
	build-prc -o $@ $(FRONDBENCH_SRC)/frondbench.def $(FRONDBENCH_DIR)/frondbench $(FRONDBENCH_DIR)/frondbench.ro

# Emulation speed: Bike or Die replay and FrondBench through benchmark-replay.
# BIKE_SESSION and FRONDBENCH_SESSION are session files you supply; a workload without one is skipped.
# SPEED_HOST=user@host runs on that machine in ~/frond-speed/ (SPEED_SSH_KEY and SPEED_REAL_MS are optional).
BIKE_SESSION ?=
FRONDBENCH_SESSION ?=
SPEED_MIPS ?= 100
SPEED_HOST ?=
.PHONY: speed-check
speed-check: benchmark-replay
	BIKE_SESSION=$(BIKE_SESSION) FRONDBENCH_SESSION=$(FRONDBENCH_SESSION) SPEED_MIPS=$(SPEED_MIPS) SPEED_HOST=$(SPEED_HOST) SPEED_BENCH=$(SPEED_BENCH) SPEED_SSH_KEY=$(SPEED_SSH_KEY) SPEED_REAL_MS=$(SPEED_REAL_MS) python3 tools/speed-check.py

# Dynarmic for the Fast engine. FROND_JIT=1 or 0 picks the engine at run time over --engine.
# The pinned source is exported from DYNARMIC_DONOR, a Dynarmic clone holding the revisions in
# patches/dynarmic/source-lock.json, and patched. Without a donor, tools/dynarmic-source.py fetches
# those revisions into build/dynarmic-donor once. make dynarmic-source redoes the export.
DYNARMIC_DONOR ?= build/dynarmic-donor
build/dynarmic-trial/src/.frond-source: patches/dynarmic/source-lock.json $(wildcard patches/dynarmic/*.patch) tools/dynarmic-source.py
	python3 tools/dynarmic-source.py $(DYNARMIC_DONOR) build/dynarmic-trial/src
	rm -f build/dynarmic/.built

.PHONY: dynarmic-source
dynarmic-source:
	python3 tools/dynarmic-source.py $(DYNARMIC_DONOR) build/dynarmic-trial/src
	rm -f build/dynarmic/.built

build/dynarmic/.built: $(if $(filter build/dynarmic-trial/src,$(DYNARMIC_SRC)),build/dynarmic-trial/src/.frond-source)
	cmake -S $(DYNARMIC_SRC) -B build/dynarmic -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
		-DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
	ninja -C build/dynarmic -j$(BUILD_JOBS) dynarmic
	touch $@

# Older names: benchmark-replay and out/frond have the JIT built in now.
.PHONY: benchmark-replay-jit frond-jit
benchmark-replay-jit: benchmark-replay
	cp build/benchmark-replay build/benchmark-replay-jit
frond-jit: out/frond
	cp out/frond build/frond-jit

.PHONY: test-tg50-serial-board
test: test-tg50-serial-board
test-tg50-serial-board: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-serial-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/tg50-serial-board.cpp @build/tg50-serial-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-tg50-serial-board
	./build/test-tg50-serial-board

test: test-pxa255-udc-dma test-pxa255-udc-dma-state

.PHONY: test-pxa255-udc-dma
test-pxa255-udc-dma: build/source/src/common/libcommon.a
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-class-memaccess -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa255-udc-dma.cpp build/source/src/common/libcommon.a -o build/test-pxa255-udc-dma
	./build/test-pxa255-udc-dma

.PHONY: test-pxa255-udc-dma-state
test-pxa255-udc-dma-state: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/pxa255-udc-dma-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa255-udc-dma-state.cpp @build/pxa255-udc-dma-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-pxa255-udc-dma-state
	./build/test-pxa255-udc-dma-state

.PHONY: test-pxa255-udc-clock
test: test-pxa255-udc-clock
test-pxa255-udc-clock: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/pxa255-udc-clock-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/pxa255-udc-clock.cpp @build/pxa255-udc-clock-core.rsp build/source/src/common/libcommon.a \
		$(CPU_TEST_LDLIBS) -o build/test-pxa255-udc-clock
	./build/test-pxa255-udc-clock

.PHONY: test-tg50-microphone-adc
test-tg50-microphone-adc: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-microphone-adc.cpp build/source/src/uarm/uarm/pxa_I2S.cpp \
		build/source/src/uarm/uarm/i2sdev_AK4534.cpp build/source/src/uarm/uarm/audio_queue.cpp \
		build/source/src/common/savestate/Chunk.cpp build/source/src/common/savestate/ChunkProbe.cpp \
		build/source/src/common/Logging.cpp -Wl,--gc-sections -o build/test-tg50-microphone-adc
	./build/test-tg50-microphone-adc

.PHONY: test-tg50-microphone-state
test-tg50-microphone-state: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-microphone-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/tg50-microphone-state.cpp @build/tg50-microphone-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-tg50-microphone-state
	./build/test-tg50-microphone-state

.PHONY: test-tg50-microphone-dma
test-tg50-microphone-dma: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		tests/tg50-microphone-dma.cpp build/source/src/uarm/uarm/pxa_I2S.cpp \
		build/source/src/uarm/uarm/pxa_DMA.cpp build/source/src/uarm/uarm/i2sdev_AK4534.cpp \
		build/source/src/uarm/uarm/audio_queue.cpp build/source/src/common/savestate/Chunk.cpp \
		build/source/src/common/savestate/ChunkProbe.cpp build/source/src/common/Logging.cpp \
		-Wl,--gc-sections -o build/test-tg50-microphone-dma
	./build/test-tg50-microphone-dma

test: test-tg50-microphone-adc test-tg50-microphone-state test-tg50-microphone-dma

.PHONY: test-host-microphone
test-host-microphone: prepare
	$(CXX) -std=c++17 -O2 -g -Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm \
		$(shell sdl2-config --cflags) tests/host-microphone.cpp $(shell sdl2-config --libs) -pthread -o build/test-host-microphone
	SDL_AUDIODRIVER=dummy ./build/test-host-microphone

.PHONY: test-host-microphone-state
test-host-microphone-state: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/host-microphone-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common \
		$(shell sdl2-config --cflags) tests/host-microphone-state.cpp @build/host-microphone-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-host-microphone-state
	./build/test-host-microphone-state

.PHONY: test-sony-dac-preview
test-sony-dac-preview: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Ibuild/source/src/uarm/uarm tests/sony-dac-preview.cpp -o build/test-sony-dac-preview
	./build/test-sony-dac-preview

.PHONY: test-sony-dac-host
test-sony-dac-host: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/sony-dac-host.cpp build/source/src/uarm/uarm/pxa_I2S.cpp build/source/src/uarm/uarm/pxa_DMA.cpp build/source/src/uarm/uarm/i2sdev_AK4534.cpp build/source/src/uarm/uarm/audio_queue.cpp build/source/src/common/savestate/Chunk.cpp build/source/src/common/savestate/ChunkProbe.cpp build/source/src/common/Logging.cpp -Wl,--gc-sections -o build/test-sony-dac-host
	./build/test-sony-dac-host

.PHONY: test-csr-bcsp
test: test-csr-bcsp
test-csr-bcsp: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -Ibuild/source/src/uarm/uarm tests/bcsp-transport.cpp build/source/src/uarm/uarm/BcspTransport.cpp -o build/test-csr-bcsp-transport
	./build/test-csr-bcsp-transport
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -Ibuild/source/src/uarm/uarm tests/csr-backpressure.cpp build/source/src/uarm/uarm/BluetoothBackend.cpp -o build/test-csr-backpressure
	./build/test-csr-backpressure
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/csr-bcsp-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/csr-bcsp-board.cpp @build/csr-bcsp-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-csr-bcsp-board
	./build/test-csr-bcsp-board
	./build/test-csr-bcsp-board --lifecycle

.PHONY: test-csr-bcsp-lifetime
test-csr-bcsp-lifetime: prepare
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ibuild/source/src/uarm/uarm tests/csr-bcsp-lifetime.cpp build/source/src/uarm/uarm/CsrBcspRadio.cpp build/source/src/uarm/uarm/BcspTransport.cpp build/source/src/uarm/uarm/BluetoothBackend.cpp -o build/test-csr-bcsp-lifetime
	./build/test-csr-bcsp-lifetime

.PHONY: test-csr-peer-services
test: test-csr-peer-services
test-csr-peer-services: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -Ibuild/source/src/uarm/uarm tests/csr-peer-services.cpp build/source/src/uarm/uarm/BcspTransport.cpp -o build/test-csr-peer-services
	./build/test-csr-peer-services
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/csr-peer-radio-state.cpp build/source/src/uarm/uarm/CsrBcspRadio.cpp build/source/src/uarm/uarm/BcspTransport.cpp build/source/src/uarm/uarm/BluetoothBackend.cpp -o build/test-csr-peer-radio-state
	./build/test-csr-peer-radio-state
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/csr-peer-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/csr-peer-state.cpp @build/csr-peer-core.rsp build/source/src/common/libcommon.a -lSDL2 -lSDL2_image -lreadline $(CPU_TEST_LDLIBS) -o build/test-csr-peer-state
	./build/test-csr-peer-state

.PHONY: bluetooth-opp-peer test-bluetooth-opp-peer
bluetooth-opp-peer:
	python3 tools/build-bluetooth-opp-peer.py

test-bluetooth-opp-peer:
	python3 tools/build-bluetooth-opp-peer.py --test

.PHONY: test-csr-controller-lifecycle
test: test-csr-controller-lifecycle
test-csr-controller-lifecycle: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Ibuild/source/src/uarm/uarm tests/csr-controller-reset.cpp -o build/test-csr-controller-reset
	./build/test-csr-controller-reset
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Ibuild/source/src/uarm/uarm tests/csr-controller-lifecycle.cpp build/source/src/uarm/uarm/CsrBcspRadio.cpp build/source/src/uarm/uarm/BcspTransport.cpp build/source/src/uarm/uarm/BluetoothBackend.cpp -o build/test-csr-controller-lifecycle
	./build/test-csr-controller-lifecycle
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Ibuild/source/src/uarm/uarm tests/csr-peer-services.cpp build/source/src/uarm/uarm/BcspTransport.cpp -o build/test-csr-peer-services
	./build/test-csr-peer-services
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm tests/csr-peer-radio-state.cpp build/source/src/uarm/uarm/CsrBcspRadio.cpp build/source/src/uarm/uarm/BcspTransport.cpp build/source/src/uarm/uarm/BluetoothBackend.cpp -o build/test-csr-peer-radio-state
	./build/test-csr-peer-radio-state
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/csr-controller-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/csr-peer-state.cpp @build/csr-controller-core.rsp build/source/src/common/libcommon.a -lSDL2 -lSDL2_image -lreadline $(CPU_TEST_LDLIBS) -o build/test-csr-peer-state
	./build/test-csr-peer-state
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/csr-bcsp-board.cpp @build/csr-controller-core.rsp build/source/src/common/libcommon.a -lSDL2 -lSDL2_image -lreadline $(CPU_TEST_LDLIBS) -o build/test-csr-bcsp-board
	./build/test-csr-bcsp-board --lifecycle
	./build/test-csr-bcsp-board

.PHONY: test-tg50-audio-queue
test-tg50-audio-queue: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-audio-queue-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/tg50-audio-queue.cpp @build/tg50-audio-queue-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-tg50-audio-queue
	ulimit -c 0; ./build/test-tg50-audio-queue

.PHONY: test-tg50-audio-lifecycle
test-tg50-audio-lifecycle: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/tg50-audio-lifecycle-core.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) -Ibuild/source/src/uarm/native -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/tg50-audio-lifecycle.cpp build/source/src/uarm/native/SdlAudioDriver.cpp @build/tg50-audio-lifecycle-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-tg50-audio-lifecycle
	ulimit -c 0; ./build/test-tg50-audio-lifecycle

.PHONY: test-sony-original-guards
test-sony-original-guards: out/frond
	find build/source/src/uarm/.build -name '*.o' ! -path '*/native/*' > build/sony-original-guards-core.rsp
	$(CXX) -std=c++17 -O1 -g -Ibuild/source/src/uarm/uarm -Ibuild/source/src/uarm -Ibuild/source/src/common tests/sony-original-guards.cpp @build/sony-original-guards-core.rsp build/source/src/common/libcommon.a $(CPU_TEST_LDLIBS) -o build/test-sony-original-guards
	ulimit -c 0; ./build/test-sony-original-guards
