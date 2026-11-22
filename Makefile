.PHONY: all clean prepare test test-cpu test-tx-state test-sdl-audio test-fast-audio test-instruction-cache test-pxa-timer benchmark-session test-native-governor test-db-list test-export-checkpoint test-db-backup test-checkpoint-input test-native-database
BUILD_JOBS ?= 4
# Everything rebuildable the build makes goes under OBJ_ROOT: the core's objects, Dynarmic, the
# desktop build and the test binaries. out/ holds the binaries you run. The rest of build/ is
# evidence and results, so OBJ_ROOT and out/ can each be a link to a faster disk.
OBJ_ROOT ?= build/out
# A link left pointing at a wiped folder stops the build here.
$(foreach l,$(OBJ_ROOT) out,$(if $(shell test -L '$(l)' && test ! -e '$(l)' && echo x),$(error $(l) is a link to a missing folder ($(shell readlink '$(l)')): recreate that folder, or remove the link)))
$(shell mkdir -p $(OBJ_ROOT))
# The core sub-makes put each folder's objects in FROND_CORE_OBJ/<folder>/.
CORE_OBJ_FLAGS = FROND_CORE_OBJ=$(abspath $(OBJ_ROOT))/core
UARM_OBJ = $(OBJ_ROOT)/core/uarm/.build
COMMON_OBJ = $(OBJ_ROOT)/core/common/.build
COMMON_LIB = $(OBJ_ROOT)/core/common/libcommon.a
COMMON_NET_LIB = $(OBJ_ROOT)/core/common/libcommon-net.a
CLOUDPILOT_OBJ = $(OBJ_ROOT)/core/cloudpilot/.build-frond
# The desktop's CMake build, on the resolved path: moc writes includes relative to it, and those
# go wrong through a link.
DESKTOP_BUILD = $(realpath $(OBJ_ROOT))/desktop
DESKTOP_DEV_PATHS ?= ON
# Test scripts find the test binaries and probes here.
export FROND_OBJ = $(abspath $(OBJ_ROOT))
# libcommon-net.a: the core's .o files include HostInfrared's phone (SlirpIp).
include core/src/Makefile.host

CPU_TEST_LDLIBS ?= $(COMMON_NET_LIB) $(COMMON_LIB) -lpthread $(shell pkg-config --libs slirp) $(PLATFORM_LIBS)

# The Fast engine (Dynarmic JIT) is built in on x86-64 and arm64 hosts; FROND_FAST=0 leaves it out.
# Its source is in vendor/dynarmic and it's built in build/out/dynarmic.
FROND_FAST ?= $(if $(filter x86_64 aarch64 arm64,$(HOST_ARCH)),1,0)
DYNARMIC_SRC ?= vendor/dynarmic
# Boost headers for Dynarmic: found on the default paths on Linux and in MSYS2; Homebrew's prefix is
# passed to cmake on macOS.
DYNARMIC_CMAKE_ARGS ?= $(if $(filter macos,$(HOST_OS)),-DCMAKE_PREFIX_PATH=$(shell brew --prefix 2>/dev/null || echo /usr/local))
JIT_LIBS = $(OBJ_ROOT)/dynarmic/src/dynarmic/libdynarmic.a $(OBJ_ROOT)/dynarmic/externals/mcl/src/libmcl.a \
	$(OBJ_ROOT)/dynarmic/externals/fmt/libfmt.a \
	$(if $(filter x86_64,$(HOST_ARCH)),$(OBJ_ROOT)/dynarmic/externals/zydis/libZydis.a \
	$(OBJ_ROOT)/dynarmic/externals/zydis/zycore/libZycore.a)
ifeq ($(FROND_FAST),1)
CORE_JIT_DEPS = $(OBJ_ROOT)/dynarmic/.built
CORE_JIT_FLAGS = FROND_DYNARMIC_SRC=$(abspath $(DYNARMIC_SRC)) FROND_DYNARMIC_LIBS="$(abspath $(JIT_LIBS))"
CPU_TEST_LDLIBS += $(JIT_LIBS)
endif

all: out/frond desktop

# Every test target runs under a time limit: each recipe line goes through tests/timed-shell.sh,
# which kills the line's process group and fails with a TEST TIMEOUT line naming the target.
# Change the limit for one target with `test-foo: export TEST_TIMEOUT = 900`, or for a run with
# `make TEST_TIMEOUT=600 test`. The builds the tests depend on keep the normal shell.
TEST_TIMEOUT ?= 300
TEST_KILL_GRACE ?= 10
export TEST_KILL_GRACE
test: SHELL = $(CURDIR)/tests/timed-shell.sh
test-%: SHELL = $(CURDIR)/tests/timed-shell.sh
export TEST_TIMEOUT
test: export TEST_NAME = $@
test-%: export TEST_NAME = $@
# make recheck-list shows which checks need running (changed paths, failed, never passed),
# make recheck runs them one by one; both read tests/check-paths.tsv and build/check-passes.tsv.
# CHECKS="test-zodiac-* test-cpu" limits them. Run it as `heavy build make recheck`.
.PHONY: recheck recheck-list
recheck-list:
	@python3 tools/check-record.py list $(CHECKS)
recheck:
	@python3 tools/check-record.py recheck $(CHECKS)
prepare desktop zodiac-boot-probe benchmark-replay device-profiles: SHELL = /bin/sh
out/% build/% $(OBJ_ROOT)/% core/% vendor/%: SHELL = /bin/sh
test-desktop: export TEST_TIMEOUT = 3600
# These two run for several minutes on their own and longer under make -j.
test-zodiac-machine test-zodiac-fast-lean test-zodiac-ymf-ready: export TEST_TIMEOUT = 1800
# Card store checks bound by the disk: 230 to 300 s alone on a busy hard drive.
test-ms-logical-equivalence test-card-backing: export TEST_TIMEOUT = 900
# Two Zodiacs in Exact for the InkStorm chat.
test-zodiac-bluetooth: export TEST_TIMEOUT = 2400
# The limit counts the wait for a `heavy emulator` slot, and the fixture builds that wait up to
# 900 s for one, so the targets that start cores get more. The peripherals test gets it per --rom.
test-card-swap test-core-control test-background-save test-frond-68k-busy test-frond-68k-gdb \
    test-tg50-voicerec test-printer-control test-frond-68k-boot-save test-frond-68k-install \
    test-frond-arm-install test-device-profiles test-frond-68k-auto-off: export TEST_TIMEOUT = 1800
test-frond-68k-peripherals: export TEST_TIMEOUT = $(shell expr 1800 \* $(or $(words $(filter --rom,$(FROND_68K_ROMS))),1))


.PHONY: test-memory-stick-media
test: test-memory-stick-controller test-memory-stick-media
test-memory-stick-media: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/memory-stick-media.cpp -o $(OBJ_ROOT)/test-memory-stick-media
	$(OBJ_ROOT)/test-memory-stick-media

.PHONY: test-ms-logical-equivalence
test: test-ms-logical-equivalence
test-ms-logical-equivalence: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-ms-logical-equivalence.o
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror -UNDEBUG \
		-Icore/src/uarm/uarm -Icore/src/common tests/ms-logical-equivalence.cpp \
		$(OBJ_ROOT)/miniz-ms-logical-equivalence.o -o $(OBJ_ROOT)/test-ms-logical-equivalence
	$(OBJ_ROOT)/test-ms-logical-equivalence

.PHONY: test-card-backing
test: test-card-backing
test-card-backing: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-card-backing.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common \
		tests/card-backing.cpp tests/card-backing-generations.cpp \
		$(OBJ_ROOT)/miniz-card-backing.o -o $(OBJ_ROOT)/test-card-backing
	$(OBJ_ROOT)/test-card-backing

.PHONY: test-card-swap
test: test-card-swap
# Card swap and change counters without a SoC, then on the T|X, E2, Zire 72 and TG50 smoke sessions
# there are (tests/card-swap-rom.py).
test-card-swap: out/frond
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-card-swap.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/card-swap.cpp core/src/common/session/session_file5.cpp core/src/common/md5.cpp \
		core/src/uarm/uarm/sdcard.cpp core/src/uarm/uarm/mmiodev_MemoryStickController.cpp \
		$(OBJ_ROOT)/miniz-card-swap.o $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-card-swap
	$(OBJ_ROOT)/test-card-swap
	python3 tests/card-swap-rom.py

.PHONY: test-card-reinsert
test: test-card-reinsert
# A card put back right after an eject, with no guest time between, on the T|X, E2, Zire 72 and TG50
# smoke sessions there are (tests/card-reinsert-rom.py).
test-card-reinsert: out/frond
	python3 tests/card-reinsert-rom.py

.PHONY: test-card-tool
test: test-card-tool
# The card tool (make, import, export) as a library, then as `frond-core --card` (tests/card-tool.py).
test-card-tool: out/frond
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-card-tool.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/card-tool.cpp core/src/common/session/session_file5.cpp core/src/common/md5.cpp \
		core/src/uarm/uarm/sdcard.cpp core/src/uarm/uarm/mmiodev_MemoryStickController.cpp \
		$(OBJ_ROOT)/miniz-card-tool.o $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-card-tool
	$(OBJ_ROOT)/test-card-tool
	python3 tests/card-tool.py

.PHONY: test-session-v7
test: test-session-v7
test-session-v7: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-session-v7.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/session-v7.cpp core/src/common/session/session_file5.cpp \
		core/src/common/md5.cpp core/src/uarm/uarm/sdcard.cpp $(OBJ_ROOT)/miniz-session-v7.o \
		-o $(OBJ_ROOT)/test-session-v7
	$(OBJ_ROOT)/test-session-v7

.PHONY: test-storage-settle
test: test-storage-settle
# The storage library's settle and verify: both apps' leftovers (read back with mtools), journal
# v2 session and standalone journals cut off at each step, pending lists and side regions.
test-storage-settle: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-storage-settle.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common \
		tests/storage-settle.cpp core/src/common/session/session_file5.cpp \
		core/src/common/md5.cpp core/src/uarm/uarm/sdcard.cpp $(OBJ_ROOT)/miniz-storage-settle.o \
		-o $(OBJ_ROOT)/test-storage-settle
	python3 tests/storage-settle.py $(OBJ_ROOT)/test-storage-settle

.PHONY: test-card-install
test: test-card-install
# The card edit behind a Windows CE install: replacing a file keeps the old one whole if the copy fails.
test-card-install: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-card-install.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/card-install.cpp $(OBJ_ROOT)/miniz-card-install.o -o $(OBJ_ROOT)/test-card-install
	$(OBJ_ROOT)/test-card-install build

.PHONY: test-storage-fat
test: test-storage-fat
# The storage library's FatImage on FAT12: format layouts, FAT32 refused, cut-off changes on a FAT12
# card file, and cards it formats or fills read back with mtools (tests/storage-fat.py).
test-storage-fat: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-storage-fat.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common \
		tests/storage-fat.cpp $(OBJ_ROOT)/miniz-storage-fat.o -o $(OBJ_ROOT)/test-storage-fat
	python3 tests/storage-fat.py $(OBJ_ROOT)/test-storage-fat

.PHONY: test-storage-ms
test: test-storage-ms
# Files on classic Memory Sticks: writes into held and unclaimed blocks read back through
# LogicalMsMedia, unsettled sticks refused, the TG50's format for each size read back with mtools,
# cut-off changes (tests/storage-ms.py).
test-storage-ms: prepare
	$(CC) -std=gnu99 -O1 -g -Icore/src/common -c core/src/common/zip/miniz.c \
		-o $(OBJ_ROOT)/miniz-storage-ms.o
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common \
		tests/storage-ms.cpp $(OBJ_ROOT)/miniz-storage-ms.o -o $(OBJ_ROOT)/test-storage-ms
	python3 tests/storage-ms.py $(OBJ_ROOT)/test-storage-ms

# The core's source is in core/ and builds from there, so there is nothing to prepare.
# Targets still list prepare so they keep their order under -j.
prepare:

.PHONY: FORCE
FORCE:

out/frond: $(COMMON_LIB) $(COMMON_NET_LIB) $(CORE_JIT_DEPS)
	$(MAKE) -C core/src/uarm $(CORE_OBJ_FLAGS) bin $(CORE_JIT_FLAGS)
	mkdir -p out
	cp core/src/uarm/cp-uarm$(EXE_SUFFIX) out/frond$(EXE_SUFFIX).new
	mv -f out/frond$(EXE_SUFFIX).new out/frond$(EXE_SUFFIX)

# The core with the Zodiac developer start: FROND_ZODIAC_STATE=/path/to.state runs the ROM from a
# bring-up state. For tests and bring-up, never packaged.
out/frond-zodiac-developer: out/frond
	$(MAKE) -C core/src/uarm $(CORE_OBJ_FLAGS) zodiac-developer $(CORE_JIT_FLAGS)
	cp core/src/uarm/cp-uarm-zodiac-developer out/frond-zodiac-developer.new
	mv -f out/frond-zodiac-developer.new out/frond-zodiac-developer

# The 68k runner for DragonBall devices, from Cloudpilot's core in src/cloudpilot.
out/frond-68k: $(COMMON_LIB) $(COMMON_NET_LIB)
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 frond-68k$(EXE_SUFFIX)
	mkdir -p out
	cp core/src/cloudpilot/frond-68k$(EXE_SUFFIX) out/frond-68k$(EXE_SUFFIX).new
	mv -f out/frond-68k$(EXE_SUFFIX).new out/frond-68k$(EXE_SUFFIX)

.PHONY: test-sync-engine
test: test-sync-engine
test-sync-engine: $(COMMON_LIB)
	mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/common/sync tests/sync-engine.cpp $(COMMON_LIB) -lpthread \
		-o $(OBJ_ROOT)/test-sync-engine
	timeout 120 $(OBJ_ROOT)/test-sync-engine $(OBJ_ROOT)/sync-engine-scratch

.PHONY: test-sync-pim
test: test-sync-pim
test-sync-pim: $(COMMON_LIB)
	mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/common/sync tests/sync-pim.cpp $(COMMON_LIB) -lpthread \
		-o $(OBJ_ROOT)/test-sync-pim
	timeout 120 $(OBJ_ROOT)/test-sync-pim $(OBJ_ROOT)/sync-pim-scratch

.PHONY: test-sync-airsync
test: test-sync-airsync
test-sync-airsync: $(COMMON_LIB)
	mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/common/sync tests/sync-airsync.cpp $(COMMON_LIB) -lpthread \
		-o $(OBJ_ROOT)/test-sync-airsync
	umask 077 && timeout 120 $(OBJ_ROOT)/test-sync-airsync build/sync-airsync-scratch

.PHONY: test-sync-pwi
test: test-sync-pwi
test-sync-pwi:
	mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/common/sync tests/sync-pwi.cpp core/src/common/sync/PwiNote.cpp \
		-o $(OBJ_ROOT)/test-sync-pwi
	$(OBJ_ROOT)/test-sync-pwi

# Build the PE shims for a Linux desktop that runs installers through Wine.
WINDOWS_SHIM_CXX ?= i686-w64-mingw32-g++
WINDOWS_SHIM_OUT ?= out/installer-shims
.PHONY: installer-shims
installer-shims:
	mkdir -p "$(WINDOWS_SHIM_OUT)"
	$(WINDOWS_SHIM_CXX) -std=c++17 -O2 -static -shared -Wl,--kill-at \
		installer-shims/InstAide.cpp -o "$(WINDOWS_SHIM_OUT)/InstAide.dll"
	$(WINDOWS_SHIM_CXX) -std=c++17 -O2 -static -municode \
		installer-shims/InstallApp.cpp -o "$(WINDOWS_SHIM_OUT)/Instapp.exe"
	cp "$(WINDOWS_SHIM_OUT)/Instapp.exe" "$(WINDOWS_SHIM_OUT)/CEAppMgr.exe"
	$(WINDOWS_SHIM_CXX) -std=c++17 -O2 -static -municode \
		installer-shims/RunInstaller.cpp -ladvapi32 -o "$(WINDOWS_SHIM_OUT)/RunInstaller.exe"

# The sync manager's profiles, install queue and backups from the command line.
out/frond-sync: $(COMMON_LIB) core/src/common/sync/SyncTool.cpp
	mkdir -p out
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Icore/src/common/sync core/src/common/sync/SyncTool.cpp \
		$(COMMON_LIB) -lpthread -o out/frond-sync$(EXE_SUFFIX)

test: test-tg50-pwm-audio test-pxa-i2s-audio test-build-tools test-audio-results test-earlier-audio prepare test-pxa255-udc test-pxa270-udc test-usb-host-fixture test-usb-enumeration test-usb-bridge test-usb-bridge-soc test-e2-state test-tg50-state test-device-profiles test-uart-lifecycle test-uart-checkpoint test-uart-flow test-uart-dma-peer test-tx-serial test-gameplay-metrics test-mmu-fast-path test-mem-region-bounds test-pxa-power test-pxa-clock test-pxa-faster test-compatibility test-developer-run test-xscale-prefetch test-device-controls test-scheduler test-wifi test-host-network test-network-reliability test-keyboard test-network test-sd-image test-sd test-cpu test-arm-conditions test-tx-state test-sdl-audio test-fast-audio test-instruction-cache test-gdbstub test-pace-debug test-pace-step test-pxa-timer test-native-governor test-db-list test-export-checkpoint test-db-backup test-checkpoint-input test-native-database test-audio-metrics test-audio-timing test-tx-baseline test-menu-response test-wm9712-irq test-pxa-ac97-irq test-pace-patch test-battery-power

# The checks that used to be recipe lines of test: itself, each a target so it is recorded.
.PHONY: test-battery-power test-audio-metrics test-audio-timing test-tx-baseline test-menu-response test-wm9712-irq test-pxa-ac97-irq test-pace-patch
test-audio-metrics: prepare
	python3 tests/audio-metrics.py
test-audio-timing: prepare
	python3 tests/audio-timing.py
test-tx-baseline: prepare
	python3 tests/tx-baseline.py
test-menu-response: prepare
	python3 tests/menu-response.py
test-wm9712-irq: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/wm9712-irq.cpp core/src/uarm/uarm/ac97dev_WM9712L.cpp \
		-Wl,--gc-sections -o $(OBJ_ROOT)/test-wm9712-irq
	$(OBJ_ROOT)/test-wm9712-irq
test-battery-power: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/battery-power.cpp core/src/uarm/uarm/ac97dev_WM9712L.cpp core/src/uarm/uarm/sspdev_AD7873.cpp \
		-Wl,--gc-sections -o $(OBJ_ROOT)/test-battery-power
	$(OBJ_ROOT)/test-battery-power
test-pxa-ac97-irq: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-ac97-irq.cpp core/src/uarm/uarm/pxa_AC97.cpp \
		-Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-ac97-irq
	$(OBJ_ROOT)/test-pxa-ac97-irq
test-pace-patch: prepare
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm \
		tests/pace-patch.cpp core/src/uarm/uarm/pace_patch.cpp \
		-o $(OBJ_ROOT)/test-pace-patch
	$(OBJ_ROOT)/test-pace-patch

test-cpu: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/arm-alignment.cpp @$(OBJ_ROOT)/core-objects.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-arm-alignment
	$(OBJ_ROOT)/test-arm-alignment

test-tx-state: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tx-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tx-session-state.cpp @$(OBJ_ROOT)/tx-core-objects.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tx-session-state
	$(OBJ_ROOT)/test-tx-session-state

# Always run the recursive make so edits in core/ are rebuilt.
.PHONY: out/frond

# Empties OBJ_ROOT and out/ and leaves them in place, as either can be a link. The rest of build/
# is evidence and stays.
clean:
	find $(OBJ_ROOT)/ out/ -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
	-$(MAKE) -C core/src/common $(CORE_OBJ_FLAGS) clean
	-$(MAKE) -C core/src/uarm $(CORE_OBJ_FLAGS) clean
	-$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 clean
	rm -f core/src/uarm/cp-uarm-zodiac-developer
	rm -rf core/src/*/.build core/src/*/.build-* core/src/*/.deps core/src/*/.deps-*


.PHONY: test-tg50-sd-refusal
test: test-tg50-sd-refusal
test-tg50-sd-refusal: out/frond
	python3 tests/tg50-sd-refusal.py

.PHONY: test-replay-snapshot
test: test-replay-snapshot
test-replay-snapshot: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/replay-snapshot-core.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm \
		-Icore/src/uarm -Icore/src/common \
		tests/replay-snapshot.cpp $(UARM_OBJ)/native/SdlRenderer.o \
		$(UARM_OBJ)/native/InputReplay.o $(UARM_OBJ)/native/Silkscreen.o \
		$(UARM_OBJ)/native/DeviceControls.o \
		@$(OBJ_ROOT)/replay-snapshot-core.rsp $(COMMON_LIB) \
		$(shell sdl2-config --libs) -lSDL2_image $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-replay-snapshot
	$(OBJ_ROOT)/test-replay-snapshot

test-sdl-audio: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/audio-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm \
		-Icore/src/uarm -Icore/src/common \
		tests/sdl-audio-buffer.cpp core/src/uarm/native/SdlAudioDriver.cpp \
		@$(OBJ_ROOT)/audio-core-objects.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-sdl-audio-buffer
	$(OBJ_ROOT)/test-sdl-audio-buffer

.PHONY: test-dana-keyboard-art
test: test-dana-keyboard-art
DANA_ART_SOURCE ?= core
DANA_ART_BUILD ?= build
# Any other source folder is a copy of core/ made first. The : keeps the line count fixed for check-record.py.
test-dana-keyboard-art: $(if $(filter core,$(DANA_ART_SOURCE)),prepare)
	$(if $(filter-out core,$(DANA_ART_SOURCE)),python3 tools/copy-core.py $(DANA_ART_SOURCE),:)
	mkdir -p $(DANA_ART_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(shell pkg-config --cflags sdl2 SDL2_image) \
		-I$(DANA_ART_SOURCE)/src/uarm/native -I$(DANA_ART_SOURCE)/src/uarm/uarm \
		-I$(DANA_ART_SOURCE)/src/common \
		tests/dana-keyboard-art.cpp $(DANA_ART_SOURCE)/src/uarm/native/DeviceControls.cpp \
		$(shell pkg-config --libs sdl2 SDL2_image) -o $(DANA_ART_BUILD)/test-dana-keyboard-art
	$(DANA_ART_BUILD)/test-dana-keyboard-art

.PHONY: test-dana-view
test: test-dana-view
DANA_VIEW_SOURCE ?= core
DANA_VIEW_BUILD ?= build
# Any other source folder is a copy of core/ made first. The : keeps the line count fixed for check-record.py.
test-dana-view: $(if $(filter core,$(DANA_VIEW_SOURCE)),prepare)
	$(if $(filter-out core,$(DANA_VIEW_SOURCE)),python3 tools/copy-core.py $(DANA_VIEW_SOURCE),:)
	mkdir -p $(DANA_VIEW_BUILD)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(shell pkg-config --cflags sdl2 SDL2_image) \
		-I$(DANA_VIEW_SOURCE)/src/uarm/native -I$(DANA_VIEW_SOURCE)/src/uarm/uarm \
		-I$(DANA_VIEW_SOURCE)/src/common \
		tests/dana-view.cpp $(DANA_VIEW_SOURCE)/src/uarm/native/DeviceControls.cpp \
		$(shell pkg-config --libs sdl2 SDL2_image) -o $(DANA_VIEW_BUILD)/test-dana-view
	SDL_VIDEODRIVER=dummy $(DANA_VIEW_BUILD)/test-dana-view

.PHONY: test-render-link
test: test-render-link
test-render-link: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/render-link.cpp $(shell sdl2-config --libs) -lpthread -o $(OBJ_ROOT)/test-render-link
	$(OBJ_ROOT)/test-render-link

test: test-shortcut-keys test-dana-host-keys
test-shortcut-keys: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native tests/shortcut-keys.cpp -o $(OBJ_ROOT)/test-shortcut-keys
	$(OBJ_ROOT)/test-shortcut-keys

test-dana-host-keys: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/common \
		tests/dana-host-keys.cpp -o $(OBJ_ROOT)/test-dana-host-keys
	$(OBJ_ROOT)/test-dana-host-keys

test-fast-audio: out/frond
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm \
		tests/fast-audio.cpp core/src/uarm/uarm/audio_queue.cpp \
		-pthread -o $(OBJ_ROOT)/test-fast-audio
	$(OBJ_ROOT)/test-fast-audio
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/fast-audio-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/fast-audio-state.cpp @$(OBJ_ROOT)/fast-audio-core-objects.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-fast-audio-state
	$(OBJ_ROOT)/test-fast-audio-state

test-instruction-cache: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/cache-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/instruction-cache.cpp @$(OBJ_ROOT)/cache-core-objects.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-instruction-cache
	$(OBJ_ROOT)/test-instruction-cache

.PHONY: test-gdbstub
$(OBJ_ROOT)/test-gdbstub-target: out/frond tests/gdbstub-target.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/gdbstub-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/gdbstub-target.cpp @$(OBJ_ROOT)/gdbstub-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-gdbstub-target

test-gdbstub: $(OBJ_ROOT)/test-gdbstub-target
	python3 tests/gdbstub.py

test-pxa-timer: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-timer.cpp core/src/uarm/uarm/pxa_TIMR.cpp \
		-Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-timer
	$(OBJ_ROOT)/test-pxa-timer

benchmark-session: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/benchmark-core-objects.rsp
	$(CXX) -std=c++17 -O2 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tools/benchmark-session.cpp @$(OBJ_ROOT)/benchmark-core-objects.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/benchmark-session

test-native-governor: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name cputil.o > $(OBJ_ROOT)/governor-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/native-governor.cpp @$(OBJ_ROOT)/governor-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-native-governor
	$(OBJ_ROOT)/test-native-governor

test-db-list: prepare
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common \
		tests/db-list.cpp core/src/uarm/uarm/db_list.cpp -o $(OBJ_ROOT)/test-db-list
	$(OBJ_ROOT)/test-db-list

test-export-checkpoint: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/export-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/export-checkpoint.cpp @$(OBJ_ROOT)/export-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-export-checkpoint
	$(OBJ_ROOT)/test-export-checkpoint

test-db-backup: out/frond
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common -Icore/src/common/zip \
		tests/db-backup.cpp core/src/uarm/uarm/db_backup.cpp \
		$(COMMON_LIB) -o $(OBJ_ROOT)/test-db-backup
	$(OBJ_ROOT)/test-db-backup

.PHONY: test-jit-overshoot
ifeq ($(FROND_FAST),1)
test: test-jit-overshoot
endif
# How far a cpuCycle call runs past its budget in each engine, against cpuMaxOvershootCycles.
test-jit-overshoot: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/jit-overshoot-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/jit-overshoot.cpp @$(OBJ_ROOT)/jit-overshoot-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-jit-overshoot
	$(OBJ_ROOT)/test-jit-overshoot

# Random ARM and Thumb blocks run on both engines and compared, plus directed tests.
# Seeds 1 to 30000 and the directed tests, then 30001 to 40000 with an unchecked limit of 256.
# JIT_DIFF_ARGS="--seeds 1: --duration 3600" for a long run, "--seed N" for one case.
.PHONY: test-jit-differential $(OBJ_ROOT)/test-jit-differential
ifeq ($(FROND_FAST),1)
test: test-jit-differential
endif
$(OBJ_ROOT)/test-jit-differential: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/jit-differential-core.rsp
	$(CXX) -std=c++17 -O2 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/jit-differential.cpp @$(OBJ_ROOT)/jit-differential-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-jit-differential
test-jit-differential: $(OBJ_ROOT)/test-jit-differential
	@# The core logs every guest data abort to stderr, so that goes to a file.
	$(OBJ_ROOT)/test-jit-differential $(JIT_DIFF_ARGS) 2>$(OBJ_ROOT)/jit-differential.err || \
		{ grep -v ' failed (pc=' $(OBJ_ROOT)/jit-differential.err | tail -5; exit 1; }
	test -n "$(JIT_DIFF_ARGS)" || $(OBJ_ROOT)/test-jit-differential --seeds 30001:40001 --no-directed --max-unchecked 256 \
		2>$(OBJ_ROOT)/jit-differential.err || { grep -v ' failed (pc=' $(OBJ_ROOT)/jit-differential.err | tail -5; exit 1; }
	test -n "$(JIT_DIFF_ARGS)" || $(OBJ_ROOT)/test-jit-differential --seeds 40001:50001 --no-directed --hot 3 \
		2>$(OBJ_ROOT)/jit-differential.err || { grep -v ' failed (pc=' $(OBJ_ROOT)/jit-differential.err | tail -5; exit 1; }

# The JIT compile thread's bookkeeping on its own, with asserts on.
.PHONY: test-jit-compile-thread
ifeq ($(FROND_FAST),1)
test: test-jit-compile-thread
endif
test-jit-compile-thread:
	$(CXX) -std=c++20 -O1 -g -Ivendor/dynarmic/src -Ivendor/dynarmic/externals/mcl/include \
		-Ivendor/dynarmic/externals/fmt/include -DFMT_HEADER_ONLY \
		tests/jit-compile-thread.cpp -pthread -o $(OBJ_ROOT)/test-jit-compile-thread
	$(OBJ_ROOT)/test-jit-compile-thread

# iWMMXt on the PXA27x: each instruction against a per-lane model.
.PHONY: test-pxa270-wmmx
test: test-pxa270-wmmx
test-pxa270-wmmx: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/wmmx-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa270-wmmx.cpp @$(OBJ_ROOT)/wmmx-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa270-wmmx
	$(OBJ_ROOT)/test-pxa270-wmmx

.PHONY: test-arm-conditions benchmark-menu
test-arm-conditions: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/conditions-core.rsp
	$(CXX) -std=c++17 -O1 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/arm-conditions.cpp @$(OBJ_ROOT)/conditions-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-arm-conditions
	$(OBJ_ROOT)/test-arm-conditions

benchmark-menu: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/menu-core.rsp
	$(CXX) -std=c++17 -O2 -g \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tools/benchmark-menu.cpp @$(OBJ_ROOT)/menu-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/benchmark-menu

.PHONY: desktop test-desktop deb
desktop: out/frond
	cmake -S desktop -B $(DESKTOP_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFROND_PREPARED_SOURCE=$(CURDIR)/core -DFROND_DEV_PATHS=$(DESKTOP_DEV_PATHS)
	+cmake --build $(DESKTOP_BUILD) --target frond-desktop --parallel $(BUILD_JOBS)
	cp $(DESKTOP_BUILD)/frond-desktop out/frond-desktop.new
	mv -f out/frond-desktop.new out/frond-desktop

test-desktop: desktop
	cmake -S desktop -B $(DESKTOP_BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFROND_PREPARED_SOURCE=$(CURDIR)/core -DBUILD_TESTING=ON -DFROND_DEV_PATHS=$(DESKTOP_DEV_PATHS)
	+cmake --build $(DESKTOP_BUILD) --parallel $(BUILD_JOBS)
	ctest --test-dir $(DESKTOP_BUILD) --output-on-failure

# Packages leave out the checkout lookup for btvirt and the phone peer.
deb: DESKTOP_DEV_PATHS := OFF
deb: desktop out/frond-68k build/bluetooth-provider/emulator/btvirt bluetooth-opp-peer
	python3 tools/package-deb.py

# This Frond's Bluetooth radio. The pinned BlueZ source is fetched once with
# python3 tools/build-bluetooth-provider.py --fetch.
build/bluetooth-provider/emulator/btvirt: tools/bluez-bdaddr.patch
	python3 tools/build-bluetooth-provider.py --jobs $(BUILD_JOBS)

# arm64 package for Raspberry Pi OS trixie, cross built in Docker. Needs Docker and the BlueZ
# checkout from tools/build-bluetooth-provider.py --fetch (btvirt is cross built from it).
# core/ is copied fresh to build/arm64/source and mounted over core/, away from the host's objects.
ARM64_JOBS ?= 10
.PHONY: deb-arm64
deb-arm64:
# btvirt is cross built from the BlueZ checkout as it stands, so it must carry the current patch.
	git -C $(BLUETOOTH_PROVIDER) diff --no-ext-diff --no-color HEAD | cmp -s - tools/bluez-bdaddr.patch || \
		{ echo "$(BLUETOOTH_PROVIDER) doesn't carry tools/bluez-bdaddr.patch: run python3 tools/build-bluetooth-provider.py" >&2; exit 1; }
	rm -rf build/arm64/source
	python3 tools/copy-core.py $(CURDIR)/build/arm64/source
	docker build -t frond-arm64-cross tools/arm64
	docker run --rm --user $$(id -u):$$(id -g) -e JOBS=$(ARM64_JOBS) \
		-e SOURCE_DATE_EPOCH=$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct)} \
		-v $(CURDIR):/src -v $(CURDIR)/build/arm64/source:/src/core \
		-w /src frond-arm64-cross tools/arm64/build.sh

# AppImages for x86-64 and arm64, built in Docker on Debian 12 so they run on glibc 2.36 and newer
# (tools/appimage/). Needs Docker and the BlueZ checkout from tools/build-bluetooth-provider.py --fetch.
# core/ is copied fresh to build/appimage/ARCH/source. The arm64 one is cross built.
APPIMAGE_JOBS ?= 10
define build-appimage
	git -C $(BLUETOOTH_PROVIDER) diff --no-ext-diff --no-color HEAD | cmp -s - tools/bluez-bdaddr.patch || \
		{ echo "$(BLUETOOTH_PROVIDER) doesn't carry tools/bluez-bdaddr.patch: run python3 tools/build-bluetooth-provider.py" >&2; exit 1; }
	rm -rf build/appimage/$(1)/source
	python3 tools/copy-core.py $(CURDIR)/build/appimage/$(1)/source
	docker build -t frond-appimage-$(2) --build-arg ARCH=$(2) tools/appimage
	docker run --rm --user $$(id -u):$$(id -g) -e JOBS=$(APPIMAGE_JOBS) -e ARCH=$(1) \
		-e SOURCE_DATE_EPOCH=$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct)} \
		-v $(CURDIR):/src -w /src frond-appimage-$(2) tools/appimage/build.sh
	SOURCE_DATE_EPOCH=$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct)} python3 tools/appimage/make-appimage.py --arch $(1)
endef
.PHONY: appimage appimage-arm64
appimage:
	$(call build-appimage,x86_64,amd64)
appimage-arm64:
	$(call build-appimage,aarch64,arm64)

.PHONY: test-palm-sdio-wifi
test-palm-sdio-wifi: $(COMMON_NET_LIB) $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/palm-sdio-wifi.cpp core/src/uarm/uarm/PalmSdioWifi.cpp \
		core/src/uarm/uarm/SdioCard.cpp core/src/uarm/uarm/vSD.cpp \
		core/src/uarm/uarm/NetworkSimulation.cpp core/src/uarm/uarm/HostNetwork.cpp \
		$(COMMON_NET_LIB) $(COMMON_LIB) $(shell pkg-config --libs slirp) -pthread \
		-Wl,--gc-sections -o $(OBJ_ROOT)/test-palm-sdio-wifi
	$(OBJ_ROOT)/test-palm-sdio-wifi

.PHONY: test-palm-prism-frame
test-palm-prism-frame:
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/uarm/uarm tests/palm-prism-frame.cpp \
		-o $(OBJ_ROOT)/test-palm-prism-frame
	$(OBJ_ROOT)/test-palm-prism-frame

.PHONY: test-pxa-mmc
test-pxa-mmc: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-mmc.cpp core/src/uarm/uarm/pxa_MMC.cpp core/src/uarm/uarm/vSD.cpp \
		core/src/uarm/uarm/SdioCard.cpp \
		$(COMMON_LIB) -Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-mmc
	$(OBJ_ROOT)/test-pxa-mmc

.PHONY: test-sd-image test-sd
test-sd-image: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/native tests/sd-image.cpp -o $(OBJ_ROOT)/test-sd-image
	$(OBJ_ROOT)/test-sd-image

test-sd: out/frond test-pxa-mmc
	python3 tests/sd-results.py
	$(CXX) -std=c++17 -O1 -g -Icore/src/common -Icore/src/uarm/native -Icore/src/uarm/uarm \
		tests/sd-session.cpp core/src/uarm/uarm/sdcard.cpp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-sd-session
	$(OBJ_ROOT)/test-sd-session
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-dma.cpp core/src/uarm/uarm/pxa_DMA.cpp -Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-dma
	$(OBJ_ROOT)/test-pxa-dma

.PHONY: test-network
test-network: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/network-simulation.cpp core/src/uarm/uarm/NetworkSimulation.cpp core/src/uarm/uarm/HostNetwork.cpp \
		$(COMMON_LIB) -lz -pthread -o $(OBJ_ROOT)/test-network-simulation
	$(OBJ_ROOT)/test-network-simulation

.PHONY: test-keyboard
test-keyboard: out/frond
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/ir-keyboard.cpp $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-ir-keyboard
	$(OBJ_ROOT)/test-ir-keyboard

	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-uart.cpp core/src/uarm/uarm/pxa_UART.cpp -Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-uart
	$(OBJ_ROOT)/test-pxa-uart
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/keyboard-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/host-keyboard.cpp core/src/uarm/native/SdlEventHandler.cpp @$(OBJ_ROOT)/keyboard-core-objects.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -o $(OBJ_ROOT)/test-host-keyboard
	$(OBJ_ROOT)/test-host-keyboard
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/ir-keyboard-board.cpp @$(OBJ_ROOT)/keyboard-core-objects.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-ir-keyboard-board
	$(OBJ_ROOT)/test-ir-keyboard-board

.PHONY: test-host-network
test-host-network: prepare
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common \
		tests/host-network.cpp core/src/uarm/uarm/HostNetwork.cpp -pthread -o $(OBJ_ROOT)/test-host-network
	$(OBJ_ROOT)/test-host-network

.PHONY: test-modem
test: test-modem
test-modem: prepare $(COMMON_LIB) $(COMMON_NET_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Icore/src/common tests/modem.cpp \
		$(COMMON_NET_LIB) $(COMMON_LIB) \
		$(shell pkg-config --cflags --libs slirp) -o $(OBJ_ROOT)/test-modem
	$(OBJ_ROOT)/test-modem

.PHONY: test-rndis-host
test: test-rndis-host
test-rndis-host: prepare $(COMMON_LIB) $(COMMON_NET_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -UNDEBUG -Icore/src/common tests/rndis-host.cpp \
		$(COMMON_NET_LIB) $(COMMON_LIB) \
		$(shell pkg-config --cflags --libs slirp) -o $(OBJ_ROOT)/test-rndis-host
	$(OBJ_ROOT)/test-rndis-host

.PHONY: test-ir-peer
test: test-ir-peer
test-ir-peer: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Icore/src/common tests/ir-peer.cpp \
		$(COMMON_LIB) -o $(OBJ_ROOT)/test-ir-peer
	$(OBJ_ROOT)/test-ir-peer

.PHONY: test-ir-printer
test: test-ir-printer
# The IR printer's IrLPT and IrCOMM jobs from a scripted IrLAP primary (tests/ir-printer.cpp).
test-ir-printer: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Icore/src/common tests/ir-printer.cpp \
		$(COMMON_LIB) -o $(OBJ_ROOT)/test-ir-printer
	$(OBJ_ROOT)/test-ir-printer

# Always checked, like libcommon.a, so edits in core/src/common/net are rebuilt.
.PHONY: $(COMMON_NET_LIB)
$(COMMON_NET_LIB):
	$(MAKE) -C core/src/common $(CORE_OBJ_FLAGS) net

.PHONY: test-wifi
test-wifi: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/common/net \
		tests/wifi-router.cpp core/src/common/net/WifiRouter.cpp $(COMMON_LIB) \
		$(shell pkg-config --cflags --libs slirp) -pthread -o $(OBJ_ROOT)/test-wifi-router
	$(OBJ_ROOT)/test-wifi-router
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/libertas-cf.cpp core/src/uarm/uarm/LibertasCF.cpp \
		core/src/common/net/WifiRouter.cpp core/src/uarm/uarm/NetworkSimulation.cpp \
		core/src/uarm/uarm/HostNetwork.cpp $(COMMON_LIB) \
		$(shell pkg-config --cflags --libs slirp) -pthread -lz -o $(OBJ_ROOT)/test-libertas-cf
	$(OBJ_ROOT)/test-libertas-cf

# The shared Ethernet router from libcommon-net.a, as both cores link it.
.PHONY: test-net-wifi-router
test: test-net-wifi-router
test-net-wifi-router: prepare $(COMMON_NET_LIB) $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Icore/src/common tests/net-wifi-router.cpp \
		$(COMMON_NET_LIB) $(COMMON_LIB) $(shell pkg-config --cflags --libs slirp) -pthread \
		-o $(OBJ_ROOT)/test-net-wifi-router
	$(OBJ_ROOT)/test-net-wifi-router

.PHONY: test-scheduler
test-scheduler: prepare
	$(CXX) -std=c++17 -O2 -Icore/src/uarm -Icore/src/uarm/uarm -Icore/src/common tests/scheduler-order.cpp -o $(OBJ_ROOT)/test-scheduler-order
	$(OBJ_ROOT)/test-scheduler-order

.PHONY: test-device-controls
test-device-controls: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/controls-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/device-controls.cpp core/src/uarm/native/SdlEventHandler.cpp \
		core/src/uarm/native/InputReplay.cpp \
		core/src/uarm/native/DeviceControls.cpp core/src/uarm/native/SdlRenderer.cpp \
		core/src/uarm/native/Silkscreen.cpp @$(OBJ_ROOT)/controls-core-objects.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -lSDL2_image -o $(OBJ_ROOT)/test-device-controls
	$(OBJ_ROOT)/test-device-controls

# Fingers on the buttons and pen, ARM (T|X, E2) and 68k (IIIx): several held at once, no private sessions.
.PHONY: test-68k-frame-bounds
test: test-68k-frame-bounds
test-68k-frame-bounds: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/cloudpilot/emulator tests/68k-frame-bounds.cpp -o $(OBJ_ROOT)/test-68k-frame-bounds
	$(OBJ_ROOT)/test-68k-frame-bounds

.PHONY: test-multi-touch
test: test-multi-touch
test-multi-touch: out/frond out/frond-68k
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/multi-touch-core-objects.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common -Icore/src/cloudpilot/frond \
		tests/multi-touch.cpp core/src/uarm/native/SdlEventHandler.cpp \
		core/src/uarm/native/DeviceControls.cpp @$(OBJ_ROOT)/multi-touch-core-objects.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -o $(OBJ_ROOT)/test-multi-touch
	$(OBJ_ROOT)/test-multi-touch

.PHONY: test-developer-run
test-developer-run:
	python3 tests/developer-run.py

.PHONY: test-xscale-prefetch
test-xscale-prefetch: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/prefetch-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/xscale-prefetch.cpp @$(OBJ_ROOT)/prefetch-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-xscale-prefetch
	$(OBJ_ROOT)/test-xscale-prefetch

.PHONY: test-compatibility
$(OBJ_ROOT)/test-compatibility-core: out/frond tests/compatibility-core.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/compatibility-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/compatibility-core.cpp @$(OBJ_ROOT)/compatibility-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-compatibility-core

test-compatibility: $(OBJ_ROOT)/test-compatibility-core
	$(OBJ_ROOT)/test-compatibility-core
	python3 tests/compatibility.py
	python3 tests/app-compatibility.py

.PHONY: test-tx-power-rom
test-tx-power-rom: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/power-rom-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tx-power-rom.cpp @$(OBJ_ROOT)/power-rom-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tx-power-rom

# Share one recursive library build across emulator and standalone tests,
# and always check for edits in generated common source.
.PHONY: $(COMMON_LIB)
$(COMMON_LIB):
	$(MAKE) -C core/src/common $(CORE_OBJ_FLAGS) bin

.PHONY: test-network-reliability
test-network-reliability: prepare
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common \
		tests/network-reliability.cpp core/src/uarm/uarm/HostNetwork.cpp \
		-pthread -o $(OBJ_ROOT)/test-network-reliability
	$(OBJ_ROOT)/test-network-reliability
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common \
		tests/network-dns.cpp core/src/uarm/uarm/HostNetwork.cpp \
		-Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo -pthread -o $(OBJ_ROOT)/test-network-dns
	$(OBJ_ROOT)/test-network-dns

.PHONY: test-pxa-power
test-pxa-power: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/power-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-power.cpp @$(OBJ_ROOT)/power-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa-power
	$(OBJ_ROOT)/test-pxa-power

test-pxa-clock: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/clock-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-clock.cpp @$(OBJ_ROOT)/clock-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa-clock
	$(OBJ_ROOT)/test-pxa-clock

# Faster speed on the PXA: the core clock times the multiplier, guest time on the guest's clock, Original as
# before, the speed setting's names and MainLoop's switch.
.PHONY: test-pxa-faster
test-pxa-faster: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/faster-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-faster.cpp @$(OBJ_ROOT)/faster-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa-faster
	$(OBJ_ROOT)/test-pxa-faster

# The ARM runner's --speed and control socket speed command on a T|X smoke session (FROND_ARM_SESSION, skips
# without one).
.PHONY: test-arm-speed
test-arm-speed: out/frond
	python3 tests/arm-speed.py

# Faster speed on frond-68k (IIIx ROM, skips without it): N times the cycles a guest second, the speed
# command and option.
.PHONY: test-frond-68k-speed
test-frond-68k-speed: out/frond-68k
	python3 tests/frond-68k-speed.py

.PHONY: test-pace-debug
test-pace-debug: $(OBJ_ROOT)/test-gdbstub-target
	python3 tests/pace-debug.py
	python3 tests/pace-report.py

.PHONY: benchmark-replay
benchmark-replay: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name timeutil.o > $(OBJ_ROOT)/replay-core.rsp
	$(CXX) -std=c++17 -O2 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tools/benchmark-replay.cpp @$(OBJ_ROOT)/replay-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) $(shell pkg-config --libs sdl2 SDL2_image) -o $(OBJ_ROOT)/benchmark-replay

.PHONY: test-mmu-fast-path
test-mmu-fast-path: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/mmu-core.rsp
	$(CXX) -std=c++17 -O2 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/mmu-fast-path.cpp @$(OBJ_ROOT)/mmu-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-mmu-fast-path
	$(OBJ_ROOT)/test-mmu-fast-path

# The XScale D-cache's stale window after the MMU goes off, on both engines.
.PHONY: test-xscale-stale-dcache
test: test-xscale-stale-dcache
test-xscale-stale-dcache: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/stale-dcache-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/xscale-stale-dcache.cpp @$(OBJ_ROOT)/stale-dcache-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-xscale-stale-dcache
	$(OBJ_ROOT)/test-xscale-stale-dcache 2>$(OBJ_ROOT)/xscale-stale-dcache.err

# The cost model's data cache in sessions: a warm cache saved mid-run gives the same Exact counts.
.PHONY: test-xscale-cost-session
test: test-xscale-cost-session
test-xscale-cost-session: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/cost-session-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/xscale-cost-session.cpp @$(OBJ_ROOT)/cost-session-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-xscale-cost-session
	$(OBJ_ROOT)/test-xscale-cost-session

.PHONY: test-mem-region-bounds
test-mem-region-bounds: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/mem-bounds-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/mem-region-bounds.cpp @$(OBJ_ROOT)/mem-bounds-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-mem-region-bounds
	$(OBJ_ROOT)/test-mem-region-bounds

.PHONY: test-gameplay-metrics
test-gameplay-metrics: benchmark-replay
	python3 tests/gameplay-metrics.py

# Capability regressions report missing transport explicitly; no host endpoint or firmware.
.PHONY: test-tx-serial
test-tx-serial: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tx-serial-model.cpp core/src/uarm/uarm/pxa_UART.cpp \
		core/src/uarm/uarm/pxa270_UDC.cpp $(COMMON_LIB) \
		-lz -o $(OBJ_ROOT)/test-tx-serial-model
	$(OBJ_ROOT)/test-tx-serial-model
	python3 tests/tx-serial.py

.PHONY: test-frond-68k
test: test-frond-68k
test-frond-68k: out/frond-68k $(OBJ_ROOT)/test-cpu68k-timing $(OBJ_ROOT)/test-frond-68k-sound
	$(OBJ_ROOT)/test-cpu68k-timing
	$(OBJ_ROOT)/test-frond-68k-sound
	python3 tests/frond-68k.py

# Piezo, backlight, sleep, serial HotSync, databases, GDB and IrDA on the private IIIx ROM; skips
# without it. Add --rom with FROND_68K_ROMS="--rom A --rom B" for other ROMs.
.PHONY: test-frond-68k-peripherals
test-frond-68k-peripherals: out/frond-68k $(OBJ_ROOT)/test-frond-68k-sound
	$(OBJ_ROOT)/test-frond-68k-sound
	python3 tests/frond-68k-peripherals.py $(FROND_68K_ROMS)

# Left idle after a cold boot, the private IIIx and m515 ROMs turn off at the OS's 2 minute auto-off.
# Needs FROND_DEVICE_IMAGES.
.PHONY: test-frond-68k-auto-off
test-frond-68k-auto-off: out/frond-68k
	python3 tests/frond-68k-auto-off.py

# Console database commands while a guest app is busy for 45 s on the private IIIx 3.1 ROM: they
# wait for its event loop or fail with an error, and the core runs on. Skips without the ROM, or
# m68k-palmos-gcc and the 3.5 SDK in ../palm-os-sdk.
.PHONY: test-frond-68k-busy
test-frond-68k-busy: out/frond-68k
	python3 tests/frond-68k-busy.py

# Installs at the launcher and with other apps in front on the private IIIx 3.1 ROM: the launcher
# lists a new app at once, other apps stay in front, and sync notifies go to the creator app unless
# it's in front. Skips without the ROM, or m68k-palmos-gcc and the 3.5 SDK in ../palm-os-sdk.
.PHONY: test-frond-68k-install
test-frond-68k-install: out/frond-68k
	python3 tests/frond-68k-install.py

# The same installs on the T|X, E2, Zire 72 and TG50 smoke sessions (FROND_ARM_SESSIONS, a folder of
# machine folders, else build/smoke/machines); a device without one is skipped. Needs the probes as above.
.PHONY: test-frond-arm-install
test-frond-arm-install: out/frond
	python3 tests/frond-arm-install.py

.PHONY: test-native-budget
test-native-budget:
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm tests/native-budget.cpp -o $(OBJ_ROOT)/test-native-budget
	$(OBJ_ROOT)/test-native-budget

.PHONY: test-frond-arm-big-install
test-frond-arm-big-install: out/frond
	python3 tests/frond-arm-big-install.py

# A guest-ready check wakes nothing; a host call wakes the T|X's UI task, and a refused one on the Zire 72
# wakes it at most once (GDB breakpoint on EvtWakeup). Same sessions as above.
.PHONY: test-host-call-wake
test-host-call-wake: out/frond
	python3 tests/host-call-wake.py

# Host calls on the T|X with system patches in its native trap table, as UDMH makes them: hooks over
# the GDB stub that chain to the ROM routines. Same sessions as above.
.PHONY: test-patched-traps
test-patched-traps: out/frond
	python3 tests/patched-traps.py

# A session folder that doesn't load (a card file the session doesn't describe) is refused with a line, not a crash.
.PHONY: test-session-folder-card
test-session-folder-card: out/frond
	python3 tests/session-folder-card.py

# The battery command on both runners; needs FROND_DEVICE_IMAGES and an ARM smoke machine, else that side is skipped.
.PHONY: test-battery-control
test-battery-control: out/frond out/frond-68k
	python3 tests/battery-control.py

# m68k-palmos-gdb 5.3 and gdb-multiarch on the debug Hello app (build/hello-debug) on the private
# OS 3.5 ROM; skips without them.
# The desktop control socket on both runners; needs a private ARM smoke session and IIIx ROM, else
# that side is skipped.
.PHONY: test-core-control
test-core-control: out/frond out/frond-68k
	python3 tests/core-control.py

# Desktop saves written in the background on both runners: byte for byte against the synchronous
# save at the same instant, the guest running on meanwhile, and the stall each way. Needs the Zire 72
# smoke session and IIIx ROM, else that side is skipped.
.PHONY: test-background-save
test-background-save: out/frond out/frond-68k
	python3 tests/background-save.py

# The printer commands (irprinter and the Dana's printer) through both runners' control sockets; needs
# a private ARM smoke session and the Dana ROM, else that side is skipped.
.PHONY: test-printer-control
test-printer-control: out/frond out/frond-68k
	python3 tests/printer-control.py

.PHONY: test-frond-68k-gdb
test-frond-68k-gdb: out/frond-68k
	python3 tests/frond-68k-gdb.py
	python3 tests/frond-68k-gdb.py --multiarch

.PHONY: test-frond-68k-boot-save
test-frond-68k-boot-save: out/frond-68k
	python3 tests/frond-68k-boot-save.py

$(OBJ_ROOT)/test-frond-68k-sound: tests/frond-68k-sound.cpp core/src/cloudpilot/frond/Sound.h
	$(CXX) -std=c++17 -O1 -g -Icore/src/cloudpilot/frond tests/frond-68k-sound.cpp -o $@

CPU68K_UAE = core/src/cloudpilot/emulator/uae
$(OBJ_ROOT)/test-cpu68k-timing: tests/cpu68k-timing.cpp
	$(CXX) -std=c++17 -O1 -g -I$(CPU68K_UAE) -Icore/src/cloudpilot/emulator/hardware \
		-Icore/src/cloudpilot/emulator -Icore/src/common -DEMULATION_LEVEL=EMULATION_UNIX \
		tests/cpu68k-timing.cpp $(CPU68K_UAE)/readcpu.cpp $(CPU68K_UAE)/cputiming.cpp \
		core/src/cloudpilot/emulator/hardware/EmBusTiming.cpp \
		-x c $(CPU68K_UAE)/cpudefs.c -x none -o $@

.PHONY: device-profiles test-device-profiles
device-profiles: prepare
	mkdir -p out
	$(CXX) -std=c++17 -O2 -Icore/src/common tools/device-profiles.cpp -o out/frond-device-profiles

test-device-profiles: out/frond device-profiles
	$(CXX) -std=c++17 -O1 -g -Icore/src/common -Icore/src/uarm/uarm \
		tests/device-profiles.cpp core/src/uarm/uarm/device_configuration.cpp \
		$(COMMON_LIB) -o $(OBJ_ROOT)/test-device-profiles
	$(OBJ_ROOT)/test-device-profiles
	python3 tests/device-rom.py --device sony-clie-tg50 --synthetic-rejection
	python3 tests/device-input.py
	python3 tools/generate-device-data.py --check
	python3 tests/device-data.py
	python3 tests/device-rom-harness.py
	python3 tests/e3-boot.py
	python3 tests/e2-denvfs-boot.py
	python3 tests/guest-faults.py
	python3 tests/device-rom.py --device palm-zire-72

# Firmware-free build/package policy.
.PHONY: test-build-tools
test-build-tools:
	python3 tests/build-tools.py

# Experimental board probe, excluded from the public runners and firmware-free by default.
.PHONY: zodiac-boot-probe test-zodiac-boot-probe
zodiac-boot-probe: $(OBJ_ROOT)/zodiac-boot-probe
# Kept in every state the probe saves: the commit and a + for uncommitted changes.
ZODIAC_STATE_BUILD_NOTE = $(shell git rev-parse --short HEAD 2>/dev/null)$(shell git diff --quiet HEAD 2>/dev/null || echo +)
$(OBJ_ROOT)/zodiac-boot-probe: out/frond tools/zodiac-boot-probe.cpp tools/zodiac-state-file.h
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name 'imx1_*.o' > $(OBJ_ROOT)/zodiac-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -DFROND_STATE_BUILD_NOTE='"$(ZODIAC_STATE_BUILD_NOTE)"' \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tools/zodiac-boot-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -lz -o $@

test-zodiac-boot-probe: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-boot-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-boot-probe
	$(OBJ_ROOT)/test-zodiac-boot-probe
	python3 tests/zodiac-diagnostic-cli.py

ZODIAC_MACHINE_SOURCES = core/src/uarm/uarm/imx1_boot_probe.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp

.PHONY: test-zodiac-live-controls
test-zodiac-live-controls: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-live-controls.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-live-controls
	$(OBJ_ROOT)/test-zodiac-live-controls

.PHONY: test-zodiac-normal-runtime
# The normal runtime's view turn, pen, stick and EEPROM identity, with no firmware.
test-zodiac-normal-runtime: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-runtime-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-normal-runtime.cpp @$(OBJ_ROOT)/zodiac-runtime-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-normal-runtime
	$(OBJ_ROOT)/test-zodiac-normal-runtime
	FROND_ZODIAC_STOP_ON_EXCEPTION=1 ZODIAC_TEST_STOP=1 $(OBJ_ROOT)/test-zodiac-normal-runtime
	ZODIAC_TEST_CARD=1 $(OBJ_ROOT)/test-zodiac-normal-runtime
	ZODIAC_TEST_PULL=1 $(OBJ_ROOT)/test-zodiac-normal-runtime
	ZODIAC_TEST_SLEEP=1 $(OBJ_ROOT)/test-zodiac-normal-runtime

.PHONY: test-pxa-battery-pins
test: test-pxa-battery-pins
# Battery, headphone and SD switch inputs on the PXA boards, firmware-free.
test-pxa-battery-pins: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/pxa-battery-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-battery-pins.cpp @$(OBJ_ROOT)/pxa-battery-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa-battery-pins
	$(OBJ_ROOT)/test-pxa-battery-pins

.PHONY: test-zodiac-usb-function
test: test-zodiac-usb-function
test-zodiac-usb-function:
	mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-usb-function.cpp -o $(OBJ_ROOT)/test-zodiac-usb-function
	$(OBJ_ROOT)/test-zodiac-usb-function

.PHONY: test-zodiac-battery
test: test-zodiac-battery
# The Zodiac's battery setting on the ASP U input and its charger pin, saved states unchanged, the cradle and the
# USB bridge's cable through it, made-up ROM.
test-zodiac-battery: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-battery-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/zodiac-battery.cpp @$(OBJ_ROOT)/zodiac-battery-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-battery
	$(OBJ_ROOT)/test-zodiac-battery
	$(OBJ_ROOT)/test-zodiac-battery halted
	$(OBJ_ROOT)/test-zodiac-battery waiting

.PHONY: test-zodiac-guest-calls
# Hard reset and guest calls on the real ROM from a bring-up state with a 68k app open (ZODIAC_ROM, ZODIAC_STATE),
# Exact and Fast: install FrondBench, list it and export it back.
# Skipped without them.
ZODIAC_STATE ?=
test-zodiac-guest-calls: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-guest-calls-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common -Icore/src/common/zip \
		tests/zodiac-guest-calls.cpp @$(OBJ_ROOT)/zodiac-guest-calls-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-guest-calls
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" $(OBJ_ROOT)/test-zodiac-guest-calls hard-reset-yes
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" ZODIAC_TEST_FAST=1 $(OBJ_ROOT)/test-zodiac-guest-calls hard-reset-yes
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" $(OBJ_ROOT)/test-zodiac-guest-calls
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" ZODIAC_TEST_FAST=1 $(OBJ_ROOT)/test-zodiac-guest-calls

.PHONY: test-zodiac-resume
# Save and resume on the real ROM from a bring-up state (ZODIAC_ROM, ZODIAC_STATE), Exact and sync Fast: the run that
# saved and the save loaded again end byte for byte the same. Skipped without them.
test-zodiac-resume: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-resume-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/zodiac-resume.cpp @$(OBJ_ROOT)/zodiac-resume-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-resume
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" $(OBJ_ROOT)/test-zodiac-resume
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" ZODIAC_TEST_FAST=1 $(OBJ_ROOT)/test-zodiac-resume

.PHONY: test-zodiac-bluetooth
# The Zodiac's Bluetooth HCI shim on the real ROM from a bring-up state (ZODIAC_ROM, ZODIAC_STATE): radio on and an
# OPP send to bluetooth-opp-peer through btvirt, Fast then Exact. Skipped without them or the peers.
$(OBJ_ROOT)/test-zodiac-bluetooth: out/frond tests/zodiac-bluetooth.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-bluetooth-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common -Icore/src/common/zip \
		tests/zodiac-bluetooth.cpp @$(OBJ_ROOT)/zodiac-bluetooth-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-bluetooth
test-zodiac-bluetooth: $(OBJ_ROOT)/test-zodiac-bluetooth
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" python3 tests/zodiac-bluetooth.py --fast 1 \
		--btvirt $(BLUETOOTH_PROVIDER)/emulator/btvirt
	ZODIAC_ROM="$(ZODIAC_ROM)" ZODIAC_STATE="$(ZODIAC_STATE)" python3 tests/zodiac-bluetooth.py --fast 0 \
		--btvirt $(BLUETOOTH_PROVIDER)/emulator/btvirt

.PHONY: test-zodiac-session-cards
# A Zodiac session with a card in each slot saved and loaded natively, on a made-up ROM.
test-zodiac-session-cards: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-session-cards-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/zodiac-session-cards.cpp @$(OBJ_ROOT)/zodiac-session-cards-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-session-cards
	$(OBJ_ROOT)/test-zodiac-session-cards

.PHONY: test-zodiac-machine
# Reuse qualified generated guests, comparing one-shot and sliced execution.
test-zodiac-machine: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-machine.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-machine
	$(OBJ_ROOT)/test-zodiac-machine
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-machine-cli.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-zodiac-machine-cli
	FROND_ZODIAC_PROBE=$(OBJ_ROOT)/test-zodiac-machine-cli python3 tests/zodiac-graphics-plain-copy.py
	FROND_ZODIAC_PROBE=$(OBJ_ROOT)/test-zodiac-machine-cli FROND_ZODIAC_PROBE_ENGINE=fast python3 tests/zodiac-graphics-plain-copy.py
	FROND_ZODIAC_PROBE=$(OBJ_ROOT)/test-zodiac-machine-cli python3 tests/zodiac-gpio-event.py
	python3 tests/zodiac-machine-frame.py

# Experimental ARM920T instruction checks and optional private-ROM trace comparison.
.PHONY: test-zodiac-jit
ifeq ($(FROND_FAST),1)
test: test-zodiac-jit
endif
$(OBJ_ROOT)/test-zodiac-jit: out/frond tests/zodiac-jit.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name 'imx1_*.o' > $(OBJ_ROOT)/zodiac-jit-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-jit.cpp core/src/uarm/uarm/imx1_boot_probe.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-jit-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $@
test-zodiac-jit: $(OBJ_ROOT)/test-zodiac-jit
	$(OBJ_ROOT)/test-zodiac-jit

# The ARM920T's FCSE on the interpreter: the PID remap for fetches and data, FAR, MVA keyed caches.
.PHONY: test-arm920-fcse
test: test-arm920-fcse
$(OBJ_ROOT)/test-arm920-fcse: out/frond tests/arm920-fcse.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/arm920-fcse-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -UNDEBUG -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/arm920-fcse.cpp @$(OBJ_ROOT)/arm920-fcse-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $@
test-arm920-fcse: $(OBJ_ROOT)/test-arm920-fcse
	$(OBJ_ROOT)/test-arm920-fcse

# Every option, command, environment variable, tool and target in the sources is in the developer reference.
.PHONY: check-docs
check-docs: prepare
	python3 tools/check-docs.py

.PHONY: test-pace-step
test-pace-step: $(OBJ_ROOT)/test-gdbstub-target
	python3 tests/pace-step.py
	python3 tests/watchpoint-contract.py

.PHONY: test-uart-lifecycle
test-uart-lifecycle: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/uart-lifecycle.cpp core/src/uarm/uarm/pxa_UART.cpp \
		$(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-uart-lifecycle
	$(OBJ_ROOT)/test-uart-lifecycle

.PHONY: test-uart-checkpoint
test-uart-checkpoint: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/uart-checkpoint-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/uart-checkpoint.cpp @$(OBJ_ROOT)/uart-checkpoint-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-uart-checkpoint
	$(OBJ_ROOT)/test-uart-checkpoint

.PHONY: test-e2-state
test-e2-state: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/e2-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/e2-state.cpp @$(OBJ_ROOT)/e2-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-e2-state
	$(OBJ_ROOT)/test-e2-state

.PHONY: test-hotsync-line
test: test-hotsync-line
test-hotsync-line: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/hotsync-line-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/hotsync-line.cpp @$(OBJ_ROOT)/hotsync-line-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-hotsync-line
	$(OBJ_ROOT)/test-hotsync-line

.PHONY: test-uart-flow
test-uart-flow: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/uart-flow.cpp core/src/uarm/uarm/pxa_UART.cpp \
		$(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-uart-flow
	$(OBJ_ROOT)/test-uart-flow

.PHONY: test-uart-dma-peer
$(OBJ_ROOT)/test-uart-dma-peer: out/frond tests/uart-dma-peer.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/uart-dma-peer-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/uart-dma-peer.cpp @$(OBJ_ROOT)/uart-dma-peer-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-uart-dma-peer

test-uart-dma-peer: $(OBJ_ROOT)/test-uart-dma-peer
	$(OBJ_ROOT)/test-uart-dma-peer

.PHONY: test-pxa255-udc
test-pxa255-udc: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa255-udc.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-pxa255-udc
	$(OBJ_ROOT)/test-pxa255-udc

.PHONY: test-pxa270-udc
test-pxa270-udc: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa270-udc.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-pxa270-udc
	$(OBJ_ROOT)/test-pxa270-udc

.PHONY: test-usb-enumeration
test-usb-enumeration: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/usb-enumeration.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-usb-enumeration
	$(OBJ_ROOT)/test-usb-enumeration

.PHONY: test-usb-bridge
test-usb-bridge: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/usb-bridge.cpp core/src/uarm/uarm/pxa255_UDC.cpp core/src/uarm/uarm/UsbBusHost.cpp core/src/uarm/uarm/HostSerial.cpp \
		core/src/uarm/uarm/pxa_UART.cpp $(COMMON_LIB) -lz -lpthread -o $(OBJ_ROOT)/test-usb-bridge
	timeout 300 $(OBJ_ROOT)/test-usb-bridge

.PHONY: test-usb-bridge-soc
test-usb-bridge-soc: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/usb-bridge-soc-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/usb-bridge-soc.cpp @$(OBJ_ROOT)/usb-bridge-soc-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-usb-bridge-soc
	timeout 300 $(OBJ_ROOT)/test-usb-bridge-soc

# TG50 Voice Recorder launch, whose NULL read runs on the ROM PACE (patch 0835); skips without the TG50
# smoke session (tests/tg50-voicerec.py).
.PHONY: test-tg50-voicerec
test: test-tg50-voicerec
test-tg50-voicerec: out/frond
	python3 tests/tg50-voicerec.py

# CLIÉ game controller: the pad model alone, then on the TG50: plug-in ID, buttons, Hold, live detach and re-attach, save and reload;
# skips without the TG50 smoke session (tests/clie-gamepad.py).
.PHONY: test-clie-gamepad
test: test-clie-gamepad
test-clie-gamepad: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/clie-gamepad-model.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-clie-gamepad-model
	$(OBJ_ROOT)/test-clie-gamepad-model
ifeq ($(FROND_TG50_SESSION),)
	@echo "test-clie-gamepad: TG50 session part skipped, no FROND_TG50_SESSION"
else
	python3 tests/clie-gamepad.py
endif

# TG50 USB HotSync over a PTY with pilot-link; skips without the TG50 session (make smoke-sessions) or pilot-link.
.PHONY: test-tg50-usb-hotsync
test-tg50-usb-hotsync: out/frond
	python3 tests/tg50-usb-hotsync.py

.PHONY: test-usb-host-fixture test-tx-usb-rom
test-usb-host-fixture: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/usb-host-fixture.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-usb-host-fixture
	$(OBJ_ROOT)/test-usb-host-fixture

# Optional diagnostic: builds only; run explicitly with a private TX session.
# The fixture includes the UDC implementation to inspect hardware-owned state.
test-tx-usb-rom: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name pxa270_UDC.o > $(OBJ_ROOT)/usb-rom-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tx-usb-rom.cpp @$(OBJ_ROOT)/usb-rom-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tx-usb-rom

.PHONY: test-checkpoint-input test-native-database
test-checkpoint-input: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/checkpoint-input-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/checkpoint-input.cpp @$(OBJ_ROOT)/checkpoint-input-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-checkpoint-input
	$(OBJ_ROOT)/test-checkpoint-input

.PHONY: test-nand-store-fail
test: test-nand-store-fail
test-nand-store-fail: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/nand-store-fail-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/nand-store-fail.cpp @$(OBJ_ROOT)/nand-store-fail-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-nand-store-fail
	$(OBJ_ROOT)/test-nand-store-fail

# Writes made during a background save, replayed when a NAND or card moves onto its saved file.
.PHONY: test-save-replay
test: test-save-replay
test-save-replay: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/save-replay-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/save-replay.cpp @$(OBJ_ROOT)/save-replay-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-save-replay
	$(OBJ_ROOT)/test-save-replay

test-native-database: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm -Icore/src/uarm/uarm -Icore/src/common -Icore/src/common/zip \
		tests/native-database.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-native-database
	$(OBJ_ROOT)/test-native-database

# Firmware-free native CPU/callback/memory and SoC lifecycle regressions.
.PHONY: test-native-primitives
test: test-native-primitives
test-native-primitives: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/native-primitives-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/native-primitives.cpp @$(OBJ_ROOT)/native-primitives-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-native-primitives
	$(OBJ_ROOT)/test-native-primitives exact-hooks
	$(OBJ_ROOT)/test-native-primitives nested-hooks
	$(OBJ_ROOT)/test-native-primitives buffers
	$(OBJ_ROOT)/test-native-primitives scratch
	$(OBJ_ROOT)/test-native-primitives stack-owner
	$(OBJ_ROOT)/test-native-primitives runtime-replacement
	$(OBJ_ROOT)/test-native-primitives soc-lifecycle

.PHONY: test-debugger-inspect
test: test-debugger-inspect
test-debugger-inspect: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/debugger-inspect-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/debugger-inspect.cpp @$(OBJ_ROOT)/debugger-inspect-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-debugger-inspect
	$(OBJ_ROOT)/test-debugger-inspect

.PHONY: test-lifecycle test-tx-lifecycle-rom
test: test-lifecycle
test-lifecycle: out/frond test-sd-lifecycle
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/lifecycle-core.rsp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name cputil.o > $(OBJ_ROOT)/audio-lifecycle-core.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/audio-lifecycle.cpp core/src/uarm/native/SdlAudioDriver.cpp \
		@$(OBJ_ROOT)/audio-lifecycle-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-audio-lifecycle
	$(OBJ_ROOT)/test-audio-lifecycle
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/peripheral-lifecycle.cpp @$(OBJ_ROOT)/lifecycle-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-peripheral-lifecycle
	$(OBJ_ROOT)/test-peripheral-lifecycle

.PHONY: test-sd-lifecycle
test-sd-lifecycle: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/sd-lifecycle.cpp core/src/uarm/uarm/pxa_MMC.cpp core/src/uarm/uarm/vSD.cpp \
		core/src/uarm/uarm/SdioCard.cpp \
		$(COMMON_LIB) -Wl,--gc-sections -o $(OBJ_ROOT)/test-sd-lifecycle
	$(OBJ_ROOT)/test-sd-lifecycle

# Optional: build only; invoke explicitly with private sessions and a host deadline.
test-tx-lifecycle-rom: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tx-lifecycle-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tx-lifecycle-rom.cpp @$(OBJ_ROOT)/tx-lifecycle-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tx-lifecycle-rom

.PHONY: test-host-serial
test: test-xscale-cost
.PHONY: test-xscale-cost
test-xscale-cost:
	mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Icore/src/uarm/uarm tests/xscale-cost.cpp -o $(OBJ_ROOT)/test-xscale-cost
	$(OBJ_ROOT)/test-xscale-cost

test: test-host-serial
test-host-serial: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/host-serial.cpp core/src/uarm/uarm/pxa_UART.cpp core/src/uarm/uarm/HostSerial.cpp \
		$(COMMON_LIB) -lz -lpthread -o $(OBJ_ROOT)/test-host-serial
	timeout 120 $(OBJ_ROOT)/test-host-serial

.PHONY: test-audio-results
test-audio-results:
	python3 tests/audio-results.py

.PHONY: test-earlier-audio
test-earlier-audio: out/frond
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm tests/early-audio-ledger.cpp -o $(OBJ_ROOT)/test-early-audio-ledger
	$(OBJ_ROOT)/test-early-audio-ledger
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm tests/early-audio.cpp core/src/uarm/uarm/audio_queue.cpp -pthread -o $(OBJ_ROOT)/test-early-audio
	$(OBJ_ROOT)/test-early-audio
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm tests/early-audio-checkpoint.cpp core/src/uarm/uarm/audio_queue.cpp -pthread -o $(OBJ_ROOT)/test-early-audio-checkpoint
	$(OBJ_ROOT)/test-early-audio-checkpoint
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/patch-dispatch.cpp core/src/uarm/uarm/patch_dispatch.cpp $(UARM_OBJ)/cputil.o $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-patch-dispatch
	$(OBJ_ROOT)/test-patch-dispatch

.PHONY: test-irda-endpoint
test: test-irda-endpoint
test-irda-endpoint: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/irda-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/irda-endpoint.cpp @$(OBJ_ROOT)/irda-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-irda-endpoint
	$(OBJ_ROOT)/test-irda-endpoint

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
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm tests/bluetooth-uart-messaging.cpp -o $(OBJ_ROOT)/test-bluetooth-uart-messaging
	$(OBJ_ROOT)/test-bluetooth-uart-messaging
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/bluetooth-bootstrap.cpp $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-bluetooth-bootstrap
	$(OBJ_ROOT)/test-bluetooth-bootstrap
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common \
		tests/bluetooth-backend.cpp core/src/uarm/uarm/BluetoothBackend.cpp -o $(OBJ_ROOT)/test-bluetooth-backend
	$(OBJ_ROOT)/test-bluetooth-backend
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/bluetooth-controller.cpp core/src/uarm/uarm/uartdev_bcm2035.cpp \
		core/src/uarm/uarm/BluetoothBackend.cpp $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-bluetooth-controller
	$(OBJ_ROOT)/test-bluetooth-controller
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/bluetooth-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native -Icore/src/uarm/uarm \
		-Icore/src/uarm -Icore/src/common tests/bluetooth-soc.cpp @$(OBJ_ROOT)/bluetooth-core.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-bluetooth-soc
	$(OBJ_ROOT)/test-bluetooth-soc

.PHONY: test-bluetooth-results
test: test-bluetooth-results
test-bluetooth-results:
	python3 tests/bluetooth-results.py

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
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tg50-state.cpp @$(OBJ_ROOT)/tg50-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-state
	$(OBJ_ROOT)/test-tg50-state
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tg50-touch.cpp core/src/uarm/uarm/sspdev_AD7873.cpp \
		$(UARM_OBJ)/cputil.o $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-tg50-touch
	$(OBJ_ROOT)/test-tg50-touch

# TG50 sound replay with a guest-time consumer and forced backpressure, in Exact and Fast.
.PHONY: test-tg50-audio-replay
test: test-tg50-audio-replay
test-tg50-audio-replay: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-audio-replay-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tg50-audio-replay.cpp @$(OBJ_ROOT)/tg50-audio-replay-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-audio-replay
	ulimit -c 0; $(OBJ_ROOT)/test-tg50-audio-replay exact
	ulimit -c 0; $(OBJ_ROOT)/test-tg50-audio-replay fast

.PHONY: device-session-probe
device-session-probe: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/device-session-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/device-session.cpp @$(OBJ_ROOT)/device-session-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-device-session

.PHONY: test-savestate-layout
test: test-savestate-layout
test-savestate-layout: prepare
	$(CXX) -std=c++17 -O1 -g -Icore/src/common -Icore/src/common/savestate \
		tests/savestate-layout.cpp core/src/common/savestate/Chunk.cpp \
		core/src/common/savestate/ChunkProbe.cpp core/src/common/Logging.cpp \
		-o $(OBJ_ROOT)/test-savestate-layout
	$(OBJ_ROOT)/test-savestate-layout

.PHONY: test-pxa-i2s-audio
test-pxa-i2s-audio: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-i2s-audio.cpp core/src/uarm/uarm/pxa_I2S.cpp \
		core/src/uarm/uarm/i2sdev_AK4534.cpp core/src/uarm/uarm/audio_queue.cpp \
		core/src/common/savestate/Chunk.cpp core/src/common/savestate/ChunkProbe.cpp \
		core/src/common/Logging.cpp -Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-i2s-audio
	$(OBJ_ROOT)/test-pxa-i2s-audio
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa-i2s-dma.cpp core/src/uarm/uarm/pxa_I2S.cpp \
		core/src/uarm/uarm/pxa_DMA.cpp -Wl,--gc-sections -o $(OBJ_ROOT)/test-pxa-i2s-dma
	$(OBJ_ROOT)/test-pxa-i2s-dma

.PHONY: test-tg50-pace
test: test-tg50-pace
test-tg50-pace: prepare
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm \
		tests/tg50-pace.cpp core/src/uarm/uarm/pace_patch.cpp -o $(OBJ_ROOT)/test-tg50-pace
	$(OBJ_ROOT)/test-tg50-pace

.PHONY: test-tg50-pwm-audio
test-tg50-pwm-audio: prepare
	$(CXX) -std=c++17 -O2 -Icore/src/uarm/uarm tests/tg50-pwm-audio.cpp -o $(OBJ_ROOT)/test-tg50-pwm-audio
	$(OBJ_ROOT)/test-tg50-pwm-audio

.PHONY: test-tg50-call-budget
test: test-tg50-call-budget
test-tg50-call-budget: prepare
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm tests/tg50-call-budget.cpp -o $(OBJ_ROOT)/test-tg50-call-budget
	$(OBJ_ROOT)/test-tg50-call-budget

.PHONY: test-memory-stick-controller
test-memory-stick-controller: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/memory-stick-controller.cpp core/src/uarm/uarm/mmiodev_MemoryStickController.cpp \
		core/src/uarm/uarm/sdcard.cpp $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-memory-stick-controller
	$(OBJ_ROOT)/test-memory-stick-controller

.PHONY: test-memory-stick-pro
test: test-memory-stick-pro
test-memory-stick-pro: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common \
		tests/memory-stick-pro.cpp core/src/uarm/uarm/mmiodev_MemoryStickController.cpp \
		core/src/uarm/uarm/sdcard.cpp $(COMMON_LIB) -lz -o $(OBJ_ROOT)/test-memory-stick-pro
	$(OBJ_ROOT)/test-memory-stick-pro

.PHONY: test-tg50-serial-configuration test-microwire64x16
test: test-tg50-serial-configuration test-microwire64x16
test-microwire64x16: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm tests/microwire64x16.cpp -o $(OBJ_ROOT)/test-microwire64x16
	$(OBJ_ROOT)/test-microwire64x16

test-tg50-serial-configuration: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-serial-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tg50-serial-configuration.cpp @$(OBJ_ROOT)/tg50-serial-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-serial-configuration
	$(OBJ_ROOT)/test-tg50-serial-configuration

# FrondBench, a Palm benchmark for calibrating emulated CPU speed.
PALM_SDK ?= $(HOME)/Projects/palm/palm-os-sdk/sdk-5r3
FRONDBENCH_DIR := $(OBJ_ROOT)/frondbench
FRONDBENCH_SRC := tools/frondbench
.PHONY: frondbench
frondbench: $(FRONDBENCH_DIR)/FrondBench.prc
$(FRONDBENCH_DIR)/FrondBench.prc: $(FRONDBENCH_SRC)/frondbench.c $(FRONDBENCH_SRC)/frondbench.h $(FRONDBENCH_SRC)/frondbench.rcp $(FRONDBENCH_SRC)/frondbench.def
	mkdir -p $(FRONDBENCH_DIR)
	m68k-palmos-gcc -O2 -Wall -Wno-unknown-pragmas -I$(PALM_SDK)/include \
		$(addprefix -I,$(shell find $(PALM_SDK)/include -mindepth 1 -type d)) \
		-c $(FRONDBENCH_SRC)/frondbench.c -o $(FRONDBENCH_DIR)/frondbench.o
	m68k-palmos-gcc -L$(PALM_SDK)/lib/m68k-palmos-coff $(FRONDBENCH_DIR)/frondbench.o -o $(FRONDBENCH_DIR)/frondbench
	cd $(FRONDBENCH_SRC) && pilrc -q -ro frondbench.rcp $(abspath $(FRONDBENCH_DIR))/frondbench.ro
	build-prc -o $@ $(FRONDBENCH_SRC)/frondbench.def $(FRONDBENCH_DIR)/frondbench $(FRONDBENCH_DIR)/frondbench.ro
	python3 $(FRONDBENCH_SRC)/fixdates.py $@

# The committed tools/frondbench/FrondBench.prc is what the apps ship. This rebuilds it and compares;
# it is skipped with a message when the Palm tools are missing.
.PHONY: check-frondbench
check-frondbench:
	@if ! command -v m68k-palmos-gcc >/dev/null || ! command -v pilrc >/dev/null || ! command -v build-prc >/dev/null || [ ! -d "$(PALM_SDK)/include" ]; then \
		echo "check-frondbench: skipped, needs m68k-palmos-gcc, pilrc, build-prc and PALM_SDK"; \
	else \
		$(MAKE) -B frondbench && cmp $(FRONDBENCH_DIR)/FrondBench.prc $(FRONDBENCH_SRC)/FrondBench.prc \
			&& echo "check-frondbench: ok, $$(sha256sum $(FRONDBENCH_SRC)/FrondBench.prc | cut -c1-16)"; \
	fi

# Zodiac TwGfx test program (tools/zodiac-twgfx): every TwGfx call and option, and the Window Manager's drawing
# modes and patterns, run headless through benchmark-replay on Fast and Exact. The table (ok, the ROM's error,
# refused with Frond's stop text, or crashed) is checked against tests/zodiac-twgfx.golden; pass
# ZODIAC_TWGFX_ARGS=--update to rewrite it, or --engines fast for Fast only. Needs arm-palmos-gcc,
# m68k-palmos-gcc, build-prc, mtools and the Zodiac 1.1 ROM (ZODIAC_ROM, or under FROND_DEVICE_IMAGES);
# skipped without them. The PRC, card, states and table go in build/out/zodiac-twgfx.
ZODIAC_TWGFX_DIR := $(OBJ_ROOT)/zodiac-twgfx
ZODIAC_ROM ?= $(if $(FROND_DEVICE_IMAGES),$(FROND_DEVICE_IMAGES)/Tapwave/Zodiac-2/Tapwave-Zodiac-2-v1.1.rom)
ZODIAC_TWGFX_ARGS ?=
.PHONY: zodiac-twgfx test-zodiac-twgfx
# About 5 minutes for both engines.
test-zodiac-twgfx: export TEST_TIMEOUT = 1200
zodiac-twgfx:
	python3 tools/zodiac-twgfx/build.py $(ZODIAC_TWGFX_DIR)
test-zodiac-twgfx:
	@if ! command -v arm-palmos-gcc >/dev/null || ! command -v m68k-palmos-gcc >/dev/null || ! command -v build-prc >/dev/null \
		|| ! command -v mformat >/dev/null || [ ! -f "$(ZODIAC_ROM)" ]; then \
		echo "test-zodiac-twgfx: skipped, needs arm-palmos-gcc, m68k-palmos-gcc, build-prc, mtools and ZODIAC_ROM"; \
	else \
		$(MAKE) benchmark-replay zodiac-twgfx && python3 tools/zodiac-twgfx/run.py --bench $(OBJ_ROOT)/benchmark-replay \
			--rom "$(ZODIAC_ROM)" --fixture tests/fixtures/zodiac-doom2.replay --prc $(ZODIAC_TWGFX_DIR)/TwGfxTest.prc \
			--work $(ZODIAC_TWGFX_DIR) --golden tests/zodiac-twgfx.golden $(ZODIAC_TWGFX_ARGS); \
	fi

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

# Guest instruction, cycle and frame counts and hashes against tools/speed-baseline.json, one run each.
# Wall-clock rounds only for recipes whose counts moved. COUNT_ARGS adds options (--only, --update, --against).
COUNT_ARGS ?=
.PHONY: count-check
count-check: benchmark-replay
	python3 tools/speed-check.py --counts $(COUNT_ARGS)

# Dynarmic for the Fast engine. FROND_JIT=1 or 0 picks the engine at run time over --engine.
# Its source, with Frond's changes, is in vendor/dynarmic. Ninja runs every time so edits there
# are rebuilt; the stamp only moves when libdynarmic.a changed. A build folder configured from
# another source path is wiped first.
$(OBJ_ROOT)/dynarmic/.built: FORCE
	grep -qx 'CMAKE_HOME_DIRECTORY:INTERNAL=$(abspath $(DYNARMIC_SRC))' $(OBJ_ROOT)/dynarmic/CMakeCache.txt 2>/dev/null || rm -rf $(OBJ_ROOT)/dynarmic
	[ -f $(OBJ_ROOT)/dynarmic/build.ninja ] || cmake -S $(DYNARMIC_SRC) -B $(OBJ_ROOT)/dynarmic -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
		-DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF $(DYNARMIC_CMAKE_ARGS)
	ninja -C $(OBJ_ROOT)/dynarmic -j$(BUILD_JOBS) dynarmic
	[ -f $@ ] && [ ! $(OBJ_ROOT)/dynarmic/src/dynarmic/libdynarmic.a -nt $@ ] || touch $@

# Older names: benchmark-replay and out/frond have the JIT built in now.
.PHONY: benchmark-replay-jit frond-jit
benchmark-replay-jit: benchmark-replay
	cp $(OBJ_ROOT)/benchmark-replay $(OBJ_ROOT)/benchmark-replay-jit
frond-jit: out/frond
	cp out/frond $(OBJ_ROOT)/frond-jit

.PHONY: test-tg50-serial-board
test: test-tg50-serial-board
test-tg50-serial-board: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-serial-board-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/tg50-serial-board.cpp @$(OBJ_ROOT)/tg50-serial-board-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-serial-board
	$(OBJ_ROOT)/test-tg50-serial-board

test: test-pxa255-udc-dma test-pxa255-udc-dma-state

.PHONY: test-pxa255-udc-dma
test-pxa255-udc-dma: $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-class-memaccess -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa255-udc-dma.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-pxa255-udc-dma
	$(OBJ_ROOT)/test-pxa255-udc-dma

.PHONY: test-pxa255-udc-dma-state
test-pxa255-udc-dma-state: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/pxa255-udc-dma-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa255-udc-dma-state.cpp @$(OBJ_ROOT)/pxa255-udc-dma-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa255-udc-dma-state
	$(OBJ_ROOT)/test-pxa255-udc-dma-state

.PHONY: test-pxa255-udc-clock
test: test-pxa255-udc-clock
test-pxa255-udc-clock: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/pxa255-udc-clock-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/pxa255-udc-clock.cpp @$(OBJ_ROOT)/pxa255-udc-clock-core.rsp $(COMMON_LIB) \
		$(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-pxa255-udc-clock
	$(OBJ_ROOT)/test-pxa255-udc-clock

.PHONY: test-tg50-microphone-adc
test-tg50-microphone-adc: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tg50-microphone-adc.cpp core/src/uarm/uarm/pxa_I2S.cpp \
		core/src/uarm/uarm/i2sdev_AK4534.cpp core/src/uarm/uarm/audio_queue.cpp \
		core/src/common/savestate/Chunk.cpp core/src/common/savestate/ChunkProbe.cpp \
		core/src/common/Logging.cpp -Wl,--gc-sections -o $(OBJ_ROOT)/test-tg50-microphone-adc
	$(OBJ_ROOT)/test-tg50-microphone-adc

.PHONY: test-tg50-microphone-state
test-tg50-microphone-state: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-microphone-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/tg50-microphone-state.cpp @$(OBJ_ROOT)/tg50-microphone-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-microphone-state
	$(OBJ_ROOT)/test-tg50-microphone-state

.PHONY: test-tg50-microphone-dma
test-tg50-microphone-dma: prepare
	$(CXX) -std=c++17 -O1 -g -ffunction-sections -fdata-sections \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/tg50-microphone-dma.cpp core/src/uarm/uarm/pxa_I2S.cpp \
		core/src/uarm/uarm/pxa_DMA.cpp core/src/uarm/uarm/i2sdev_AK4534.cpp \
		core/src/uarm/uarm/audio_queue.cpp core/src/common/savestate/Chunk.cpp \
		core/src/common/savestate/ChunkProbe.cpp core/src/common/Logging.cpp \
		-Wl,--gc-sections -o $(OBJ_ROOT)/test-tg50-microphone-dma
	$(OBJ_ROOT)/test-tg50-microphone-dma

test: test-tg50-microphone-adc test-tg50-microphone-state test-tg50-microphone-dma

.PHONY: test-z72-microphone
test: test-z72-microphone
test-z72-microphone: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/z72-microphone-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/z72-microphone.cpp @$(OBJ_ROOT)/z72-microphone-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-z72-microphone
	$(OBJ_ROOT)/test-z72-microphone

.PHONY: test-z72-camera
test: test-z72-camera
test-z72-camera: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/z72-camera-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/z72-camera.cpp @$(OBJ_ROOT)/z72-camera-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-z72-camera
	$(OBJ_ROOT)/test-z72-camera

# Zire 72 camera host sources (native/HostCamera.h): a picture file and pushed frames through the
# QCI at each size, the conversions and the lifecycle.
.PHONY: test-z72-camera-host
test: test-z72-camera-host
test-z72-camera-host: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/z72-camera-host-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/uarm/native -Icore/src/common $(shell sdl2-config --cflags) tests/z72-camera-host.cpp @$(OBJ_ROOT)/z72-camera-host-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) $(shell sdl2-config --libs) -lSDL2_image -o $(OBJ_ROOT)/test-z72-camera-host
	$(OBJ_ROOT)/test-z72-camera-host

# iWMMXt shifts, rotates, TMCRR and TMRRC against a reference.
.PHONY: test-iwmmxt-shift
test: test-iwmmxt-shift
test-iwmmxt-shift: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/iwmmxt-shift-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/iwmmxt-shift.cpp @$(OBJ_ROOT)/iwmmxt-shift-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-iwmmxt-shift
	$(OBJ_ROOT)/test-iwmmxt-shift

# The iWMMXt instructions the Zire 72 video encoder runs, against a plain reference.
.PHONY: test-iwmmxt-ops
test: test-iwmmxt-ops
test-iwmmxt-ops: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/iwmmxt-ops-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/iwmmxt-ops.cpp @$(OBJ_ROOT)/iwmmxt-ops-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-iwmmxt-ops
	$(OBJ_ROOT)/test-iwmmxt-ops

# Zire 72 Camera on the real ROM shows and photographs the built-in test pattern at each size
# (tests/z72-camera.py), with Z72_SESSION as for test-z72-voice-memo. Needs ffmpeg.
.PHONY: test-z72-camera-rom
test-z72-camera-rom: out/frond
ifeq ($(Z72_SESSION),)
	@echo "test-z72-camera-rom: skipped, no Z72_SESSION"
else
	mkdir -p build/z72-camera
	python3 tests/z72-camera.py run --core out/frond --session $(Z72_SESSION) \
		--output build/z72-camera/run-$(shell date +%Y%m%d-%H%M%S)
endif

# Zire 72 Voice Memo on the real ROM records a fake 1 kHz microphone (tests/z72-voice-memo.py).
# Z72_SESSION is a Zire 72 session you supply, at the launcher with the setup Reminder up (the
# smoke session); without it the test is skipped. Each run goes in a new folder.
Z72_SESSION ?=
Z72_MIC_DIR = $(OBJ_ROOT)/z72-voice-memo
.PHONY: test-z72-voice-memo z72-test-mic
test-z72-voice-memo: out/frond
ifeq ($(Z72_SESSION),)
	@echo "test-z72-voice-memo: skipped, no Z72_SESSION"
else
	$(MAKE) z72-test-mic
	python3 tests/z72-voice-memo.py run --core $(Z72_MIC_DIR)/frond-test-mic --session $(Z72_SESSION) \
		--output build/z72-voice-memo/run-$(shell date +%Y%m%d-%H%M%S)
endif

# The core with the fake 1 kHz microphone, for the Voice Memo and video tests.
z72-test-mic: out/frond
	mkdir -p $(Z72_MIC_DIR)
	python3 tests/z72-voice-memo.py source core/src/uarm/native/main.cpp $(Z72_MIC_DIR)/main.cpp
	$(CXX) -std=c++17 -O2 -g $(shell pkg-config --cflags sdl2 SDL2_image slirp) $(if $(filter 1,$(FROND_FAST)),-DFROND_DYNARMIC) \
		-Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		-Icore/src/common/zip -Icore/src/argparse -c $(Z72_MIC_DIR)/main.cpp -o $(Z72_MIC_DIR)/main.o
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/main.o' > $(Z72_MIC_DIR)/core.rsp
	$(CXX) -o $(Z72_MIC_DIR)/frond-test-mic $(Z72_MIC_DIR)/main.o @$(Z72_MIC_DIR)/core.rsp $(COMMON_LIB) \
		$(shell pkg-config --libs sdl2 SDL2_image slirp) $(READLINE_LIBS) $(CPU_TEST_LDLIBS)

# Zire 72 Camera on the real ROM records a 320 x 240 clip of the test pattern and the fake
# microphone to an SD card, plays it in Media and records across a save and load
# (tests/z72-video.py), with Z72_SESSION as for test-z72-voice-memo. Needs ffmpeg and mtools.
.PHONY: test-z72-video-rom
test-z72-video-rom: out/frond
ifeq ($(Z72_SESSION),)
	@echo "test-z72-video-rom: skipped, no Z72_SESSION"
else
	$(MAKE) z72-test-mic
	mkdir -p build/z72-video
	python3 tests/z72-video.py run --core $(Z72_MIC_DIR)/frond-test-mic --session $(Z72_SESSION) \
		--output build/z72-video/run-$(shell date +%Y%m%d-%H%M%S)
endif

.PHONY: test-host-microphone
test-host-microphone: prepare
	$(CXX) -std=c++17 -O2 -g -Icore/src/uarm/native -Icore/src/uarm/uarm \
		$(shell sdl2-config --cflags) tests/host-microphone.cpp $(shell sdl2-config --libs) -pthread -o $(OBJ_ROOT)/test-host-microphone
	SDL_AUDIODRIVER=dummy $(OBJ_ROOT)/test-host-microphone

.PHONY: test-host-microphone-state
test-host-microphone-state: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/host-microphone-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		$(shell sdl2-config --cflags) tests/host-microphone-state.cpp @$(OBJ_ROOT)/host-microphone-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-host-microphone-state
	$(OBJ_ROOT)/test-host-microphone-state

.PHONY: test-csr-bcsp
test: test-csr-bcsp
test-csr-bcsp: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -Icore/src/uarm/uarm tests/bcsp-transport.cpp core/src/uarm/uarm/BcspTransport.cpp -o $(OBJ_ROOT)/test-csr-bcsp-transport
	$(OBJ_ROOT)/test-csr-bcsp-transport
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -Icore/src/uarm/uarm -Icore/src/common tests/csr-backpressure.cpp core/src/uarm/uarm/BluetoothBackend.cpp -o $(OBJ_ROOT)/test-csr-backpressure
	$(OBJ_ROOT)/test-csr-backpressure
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/csr-bcsp-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/csr-bcsp-board.cpp @$(OBJ_ROOT)/csr-bcsp-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-csr-bcsp-board
	$(OBJ_ROOT)/test-csr-bcsp-board
	$(OBJ_ROOT)/test-csr-bcsp-board --lifecycle

.PHONY: test-csr-bcsp-lifetime
test-csr-bcsp-lifetime: prepare
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Icore/src/uarm/uarm -Icore/src/common tests/csr-bcsp-lifetime.cpp core/src/uarm/uarm/CsrBcspRadio.cpp core/src/uarm/uarm/BcspTransport.cpp core/src/uarm/uarm/BluetoothBackend.cpp -o $(OBJ_ROOT)/test-csr-bcsp-lifetime
	$(OBJ_ROOT)/test-csr-bcsp-lifetime

.PHONY: test-csr-peer-services
test: test-csr-peer-services
test-csr-peer-services: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -Icore/src/uarm/uarm tests/csr-peer-services.cpp core/src/uarm/uarm/BcspTransport.cpp -o $(OBJ_ROOT)/test-csr-peer-services
	$(OBJ_ROOT)/test-csr-peer-services
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common tests/csr-peer-radio-state.cpp core/src/uarm/uarm/CsrBcspRadio.cpp core/src/uarm/uarm/BcspTransport.cpp core/src/uarm/uarm/BluetoothBackend.cpp -o $(OBJ_ROOT)/test-csr-peer-radio-state
	$(OBJ_ROOT)/test-csr-peer-radio-state
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/csr-peer-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/csr-peer-state.cpp @$(OBJ_ROOT)/csr-peer-core.rsp $(COMMON_LIB) -lSDL2 -lSDL2_image $(READLINE_LIBS) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-csr-peer-state
	$(OBJ_ROOT)/test-csr-peer-state

.PHONY: bluetooth-opp-peer test-bluetooth-opp-peer test-bluetooth-dun
# The checkout tools/build-bluetooth-provider.py --fetch made and built.
BLUETOOTH_PROVIDER ?= build/bluetooth-provider
bluetooth-opp-peer: prepare $(COMMON_LIB) $(COMMON_NET_LIB) $(BLUETOOTH_PROVIDER)/emulator/btvirt
	python3 tools/build-bluetooth-opp-peer.py --source $(BLUETOOTH_PROVIDER)

test-bluetooth-opp-peer: prepare $(COMMON_LIB) $(COMMON_NET_LIB) $(BLUETOOTH_PROVIDER)/emulator/btvirt
	python3 tools/build-bluetooth-opp-peer.py --source $(BLUETOOTH_PROVIDER) --test

# The DUN phone through btvirt: a scripted client dials and brings LCP and IPCP up.
test-bluetooth-dun: test-bluetooth-opp-peer
	python3 tests/bluetooth-dun.py --btvirt $(BLUETOOTH_PROVIDER)/emulator/btvirt

.PHONY: test-csr-controller-lifecycle
test: test-csr-controller-lifecycle
test-csr-controller-lifecycle: out/frond
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm tests/csr-controller-reset.cpp -o $(OBJ_ROOT)/test-csr-controller-reset
	$(OBJ_ROOT)/test-csr-controller-reset
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/common tests/csr-controller-lifecycle.cpp core/src/uarm/uarm/CsrBcspRadio.cpp core/src/uarm/uarm/BcspTransport.cpp core/src/uarm/uarm/BluetoothBackend.cpp -o $(OBJ_ROOT)/test-csr-controller-lifecycle
	$(OBJ_ROOT)/test-csr-controller-lifecycle
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm tests/csr-peer-services.cpp core/src/uarm/uarm/BcspTransport.cpp -o $(OBJ_ROOT)/test-csr-peer-services
	$(OBJ_ROOT)/test-csr-peer-services
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/common tests/csr-peer-radio-state.cpp core/src/uarm/uarm/CsrBcspRadio.cpp core/src/uarm/uarm/BcspTransport.cpp core/src/uarm/uarm/BluetoothBackend.cpp -o $(OBJ_ROOT)/test-csr-peer-radio-state
	$(OBJ_ROOT)/test-csr-peer-radio-state
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/csr-controller-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/csr-peer-state.cpp @$(OBJ_ROOT)/csr-controller-core.rsp $(COMMON_LIB) -lSDL2 -lSDL2_image $(READLINE_LIBS) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-csr-peer-state
	$(OBJ_ROOT)/test-csr-peer-state
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/csr-bcsp-board.cpp @$(OBJ_ROOT)/csr-controller-core.rsp $(COMMON_LIB) -lSDL2 -lSDL2_image $(READLINE_LIBS) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-csr-bcsp-board
	$(OBJ_ROOT)/test-csr-bcsp-board --lifecycle
	$(OBJ_ROOT)/test-csr-bcsp-board

.PHONY: test-tg50-audio-queue
test-tg50-audio-queue: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-audio-queue-core.rsp
	$(CXX) -std=c++17 -O1 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/tg50-audio-queue.cpp @$(OBJ_ROOT)/tg50-audio-queue-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-audio-queue
	ulimit -c 0; $(OBJ_ROOT)/test-tg50-audio-queue

.PHONY: test-tg50-audio-lifecycle
test-tg50-audio-lifecycle: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/tg50-audio-lifecycle-core.rsp
	$(CXX) -std=c++17 -O1 -g $(shell sdl2-config --cflags) -Icore/src/uarm/native -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common tests/tg50-audio-lifecycle.cpp core/src/uarm/native/SdlAudioDriver.cpp @$(OBJ_ROOT)/tg50-audio-lifecycle-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-tg50-audio-lifecycle
	ulimit -c 0; $(OBJ_ROOT)/test-tg50-audio-lifecycle

# Link protocol library (link/): pairing, Noise KK, records and the channel mux. Monocypher is
# vendored unchanged in link/monocypher/ and built as C.
LINK_SOURCES := link/crypto.cpp link/noise.cpp link/wire.cpp link/pairing.cpp link/messages.cpp link/mux.cpp link/link.cpp
LINK_CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Werror

$(OBJ_ROOT)/link/monocypher.o: link/monocypher/monocypher.c link/monocypher/monocypher.h
	mkdir -p $(OBJ_ROOT)/link
	$(CC) -std=c99 -O2 -g -c $< -o $@

$(OBJ_ROOT)/link/libfrondlink.a: $(LINK_SOURCES) $(wildcard link/*.h) $(OBJ_ROOT)/link/monocypher.o
	mkdir -p $(OBJ_ROOT)/link
	for f in $(LINK_SOURCES); do $(CXX) $(LINK_CXXFLAGS) -c $$f -o $(OBJ_ROOT)/link/$$(basename $$f .cpp).o || exit 1; done
	rm -f $@
	ar rcs $@ $(patsubst link/%.cpp,$(OBJ_ROOT)/link/%.o,$(LINK_SOURCES)) $(OBJ_ROOT)/link/monocypher.o

.PHONY: test-link-protocol
test: test-link-protocol
test-link-protocol: $(OBJ_ROOT)/link/libfrondlink.a
	$(CXX) $(LINK_CXXFLAGS) tests/link-protocol.cpp $(OBJ_ROOT)/link/libfrondlink.a -o $(OBJ_ROOT)/test-link-protocol
	$(OBJ_ROOT)/test-link-protocol

# Infrared between two T|X cores joined by two link hubs on loopback: the IR probe beams both ways,
# then again after one Palm sleeps and wakes. Needs a T|X smoke session (FROND_ARM_SESSIONS, else
# build/smoke/machines) and the IR probe (FROND_IR_PROBE, else pilrc and the SDK), else it skips.
.PHONY: test-link-irda-cores
test-link-irda-cores: out/frond desktop
	python3 tests/link-irda-cores.py

.PHONY: test-e2-family-peripherals
test: test-e2-family-peripherals
test-e2-family-peripherals: out/frond
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/e2-family-peripherals-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/e2-family-peripherals.cpp @$(OBJ_ROOT)/e2-family-peripherals-core.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-e2-family-peripherals
	timeout 120 $(OBJ_ROOT)/test-e2-family-peripherals

.PHONY: test-zodiac-mmio
test-zodiac-mmio: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/zodiac-mmio.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-zodiac-mmio
	$(OBJ_ROOT)/test-zodiac-mmio
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -DFROND_TEST_FAST=$(FROND_FAST) \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-mmio-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp @$(OBJ_ROOT)/zodiac-core.rsp \
		$(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-mmio-probe
	$(OBJ_ROOT)/test-zodiac-mmio-probe

# Firmware-free GPIO scenario checks through the command-line probe.
.PHONY: test-zodiac-gpio-input
.PHONY: test-zodiac-gpio-event
test-zodiac-gpio-event: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-gpio-event.py

.PHONY: test-zodiac-trace-window
test-zodiac-trace-window: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-trace-window.py

test-zodiac-gpio-input: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-gpio-input.py

.PHONY: test-zodiac-armv4-probe
test-zodiac-armv4-probe: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-armv4-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-armv4-probe
	$(OBJ_ROOT)/test-zodiac-armv4-probe

# ARMv4T multiply flags and probe guards, with ordinary generated-code checks.
.PHONY: test-zodiac-multiply
test-zodiac-multiply: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-multiply.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-multiply
	$(OBJ_ROOT)/test-zodiac-multiply

# ARMv4 SWP and SWPB, Exact and Fast, and their probe guards.
.PHONY: test-zodiac-swap
test-zodiac-swap: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-swap.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-swap
	$(OBJ_ROOT)/test-zodiac-swap

# ARM920 privileged User-bank LDM/STM, bank selection and aborts.
.PHONY: test-zodiac-user-bank
test-zodiac-user-bank: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-user-bank.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-user-bank
	$(OBJ_ROOT)/test-zodiac-user-bank

# ARM920 LDM exception return, bank ordering, aborts and fallback.
.PHONY: test-zodiac-ldm-return
test-zodiac-ldm-return: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-ldm-return.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-ldm-return
	$(OBJ_ROOT)/test-zodiac-ldm-return

# The TwGlue log on a small ARM program.
.PHONY: test-zodiac-twglue-log
test-zodiac-twglue-log: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-twglue-log.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-twglue-log
	$(OBJ_ROOT)/test-zodiac-twglue-log

# ARM920 word stores of PC, base-restored abort and interpreter fallback.
.PHONY: test-zodiac-store-pc
test-zodiac-store-pc: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-store-pc.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-store-pc
	$(OBJ_ROOT)/test-zodiac-store-pc

# ARM920 aligned ARM linkage stub branches.
.PHONY: test-zodiac-arm-add-pc
test-zodiac-arm-add-pc: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-arm-add-pc.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-arm-add-pc
	$(OBJ_ROOT)/test-zodiac-arm-add-pc

# Explicit contrast-only LCDC policy, without an internal panel or DMA.
.PHONY: test-zodiac-lcdc-contrast
test-zodiac-lcdc-contrast: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-lcdc-contrast.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-lcdc-contrast
	$(OBJ_ROOT)/test-zodiac-lcdc-contrast

# SSI1 pin mux, idle configuration and guarded transport.
.PHONY: test-zodiac-ssi-startup
test-zodiac-ssi-startup: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-ssi-startup.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-ssi-startup
	$(OBJ_ROOT)/test-zodiac-ssi-startup

# Disabled i.MX1 UART startup and guarded peripheral access.
.PHONY: test-zodiac-pcm-transport
test-zodiac-pcm-transport: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-pcm-transport.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-pcm-transport
	$(OBJ_ROOT)/test-zodiac-pcm-transport

# The Zodiac's PCM sound out: words Exact and Fast alike, frame rate, a state saved mid-stream, PWM volume, D25
# amplifier, the host stage and its gain ramp, and the sink SocIMX1 uses, Exact and Fast host frames alike.
.PHONY: test-zodiac-sound
test-zodiac-sound: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-sound.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-sound
	$(OBJ_ROOT)/test-zodiac-sound

# The stick turned to the guest's frame for each screen orientation.
.PHONY: test-zodiac-stick-view
test-zodiac-stick-view:
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/uarm \
		tests/zodiac-stick-view.cpp -o $(OBJ_ROOT)/test-zodiac-stick-view
	$(OBJ_ROOT)/test-zodiac-stick-view

# The Zodiac's rumble motor and LED on the YMF768's bank 1 register 48, on the model and through the machine.
.PHONY: test-zodiac-rumble
test-zodiac-rumble: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-rumble.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-rumble
	$(OBJ_ROOT)/test-zodiac-rumble

# The Zodiac's YMF768 on CS4: register answers, FIFO and interrupt, pitch, a stored note sequence, FM on guest
# time, saves that change nothing, bad states refused, Exact and Fast alike with a save mid-note, a guest run by
# the B15 interrupt with idle skips, a refused B15 and an older state without the chip.
.PHONY: test-zodiac-ymf768
test-zodiac-ymf768: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-ymf768.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-ymf768
	$(OBJ_ROOT)/test-zodiac-ymf768

# i.MX1 UART control at startup and refused modes.
.PHONY: test-zodiac-uart-startup
test-zodiac-uart-startup: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-uart-startup.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-uart-startup
	$(OBJ_ROOT)/test-zodiac-uart-startup

# Idle i.MX1 DMA reset, control and guarded channels.
.PHONY: test-zodiac-dma-startup
test-zodiac-dma-startup: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-dma-startup.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-dma-startup
	$(OBJ_ROOT)/test-zodiac-dma-startup

# Disabled i.MX1 DMA channel allocation and configuration.
.PHONY: test-zodiac-dma-channel
test-zodiac-dma-channel: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-dma-channel.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-dma-channel
	$(OBJ_ROOT)/test-zodiac-dma-channel

# Host-side SMF reader for the Zodiac music hooks, with ASan and UBSan.
.PHONY: test-zodiac-smf
test: test-zodiac-smf
test-zodiac-smf:
	@mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Icore/src/uarm/uarm tests/zodiac-smf.cpp -o $(OBJ_ROOT)/test-zodiac-smf
	$(OBJ_ROOT)/test-zodiac-smf

# ARM software exception entry, return and FIQ mask preservation.
.PHONY: test-zodiac-exception
test-zodiac-exception: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-exception.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-exception
	$(OBJ_ROOT)/test-zodiac-exception

# IRQ/FIQ boundaries, pending levels, generated batches and Timer 2 acknowledgement.
.PHONY: test-zodiac-irq
test-zodiac-irq: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-irq.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-irq
	$(OBJ_ROOT)/test-zodiac-irq

.PHONY: test-zodiac-thumb-probe
test-zodiac-thumb-probe: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-thumb-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-thumb-probe
	$(OBJ_ROOT)/test-zodiac-thumb-probe

.PHONY: test-zodiac-thumb-add-pc
test-zodiac-thumb-add-pc: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-thumb-add-pc.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-thumb-add-pc
	$(OBJ_ROOT)/test-zodiac-thumb-add-pc

# Directed ARM920 translation, abort and instruction-cache checks in both engines.
.PHONY: test-zodiac-mmu
$(OBJ_ROOT)/test-zodiac-mmu: out/frond tests/zodiac-mmu.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-mmu-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-mmu.cpp @$(OBJ_ROOT)/zodiac-mmu-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $@
test-zodiac-mmu: $(OBJ_ROOT)/test-zodiac-mmu
	$(OBJ_ROOT)/test-zodiac-mmu

# ARM920 data-cache and backing-memory visibility checks in both engines.
.PHONY: test-zodiac-dcache
$(OBJ_ROOT)/test-zodiac-dcache: out/frond tests/zodiac-dcache.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' > $(OBJ_ROOT)/zodiac-dcache-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-dcache.cpp @$(OBJ_ROOT)/zodiac-dcache-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $@
test-zodiac-dcache: $(OBJ_ROOT)/test-zodiac-dcache
	$(OBJ_ROOT)/test-zodiac-dcache

# Every i.MX1 test target.
.PHONY: test-imx1
test-imx1: test-imx1-aitc test-imx1-clocks-gpio test-imx1-live-clocks test-imx1-gpio-idle test-imx1-gpio-pad test-imx1-gpio-active test-imx1-mpll test-imx1-spll test-imx1-eim test-imx1-timer test-imx1-uart test-imx1-timer2 test-imx1-sysctrl test-imx1-mmcsd test-imx1-mmcsd-card test-imx1-memory-dma test-imx1-i2c-read-window test-imx1-i2c test-imx1-asp test-imx1-pwm test-imx1-rtc test-imx1-watchdog test-imx1-mpll-retune test-imx1-rtc-live-writes test-imx1-run-cache test-imx1-steps-until
test: test-imx1

# The UART cache flag survives board assignment.
.PHONY: test-imx1-run-cache
test-imx1-run-cache: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-run-cache.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp -o $(OBJ_ROOT)/test-imx1-run-cache
	$(OBJ_ROOT)/test-imx1-run-cache

# Standalone i.MX1 interrupt-controller state and strict bootstrap integration.
.PHONY: test-imx1-aitc
test-imx1-aitc: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-aitc.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp -o $(OBJ_ROOT)/test-imx1-aitc
	$(OBJ_ROOT)/test-imx1-aitc

.PHONY: test-imx1-clocks-gpio
test-imx1-clocks-gpio: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-clocks-gpio.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-clocks-gpio
	$(OBJ_ROOT)/test-imx1-clocks-gpio

.PHONY: test-imx1-steps-until
# imx1StepsUntil's 64-bit and divided-denominator branches against its 128-bit count.
test-imx1-steps-until: prepare
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-steps-until.cpp -o $(OBJ_ROOT)/test-imx1-steps-until
	$(OBJ_ROOT)/test-imx1-steps-until

.PHONY: test-imx1-live-clocks
test-imx1-live-clocks: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-live-clocks.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-live-clocks
	$(OBJ_ROOT)/test-imx1-live-clocks

.PHONY: test-imx1-gpio-idle
test-imx1-gpio-idle: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-gpio-idle.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-gpio-idle
	$(OBJ_ROOT)/test-imx1-gpio-idle

.PHONY: test-imx1-gpio-pad
test-imx1-gpio-pad: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-gpio-pad.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-gpio-pad
	$(OBJ_ROOT)/test-imx1-gpio-pad

.PHONY: test-imx1-gpio-active
test-imx1-gpio-active: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-gpio-active.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-gpio-active
	$(OBJ_ROOT)/test-imx1-gpio-active

.PHONY: test-imx1-mpll
test-imx1-mpll: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-mpll.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-mpll
	$(OBJ_ROOT)/test-imx1-mpll

.PHONY: test-imx1-spll
test-imx1-spll: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-spll.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-spll
	$(OBJ_ROOT)/test-imx1-spll

# Standalone EIM configuration and strict bootstrap integration.
.PHONY: test-imx1-eim
test-imx1-eim: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-eim.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-eim
	$(OBJ_ROOT)/test-imx1-eim

.PHONY: test-imx1-timer
test-imx1-timer: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-timer.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-timer
	$(OBJ_ROOT)/test-imx1-timer

.PHONY: test-imx1-uart
test-imx1-uart: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/common \
		tests/imx1-uart.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-uart
	$(OBJ_ROOT)/test-imx1-uart

.PHONY: test-imx1-timer2
test-imx1-timer2: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-timer2.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-timer2
	$(OBJ_ROOT)/test-imx1-timer2

.PHONY: test-zodiac-timer-probe
test-zodiac-timer-probe: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -DFROND_TEST_FAST=$(FROND_FAST) \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-timer-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-timer-probe
	$(OBJ_ROOT)/test-zodiac-timer-probe

# Named System Control configuration and strict bootstrap integration.
.PHONY: test-imx1-sysctrl
test-imx1-sysctrl: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-sysctrl.cpp core/src/uarm/uarm/imx1_sysctrl.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-sysctrl
	$(OBJ_ROOT)/test-imx1-sysctrl

.PHONY: test-imx1-mmcsd
test-imx1-mmcsd: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-mmcsd.cpp core/src/uarm/uarm/imx1_sysctrl.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-mmcsd
	$(OBJ_ROOT)/test-imx1-mmcsd

.PHONY: test-imx1-i2c test-imx1-i2c-read-window test-zodiac-i2c-image
test-imx1-i2c-read-window: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/common \
		tests/imx1-i2c-read-window.cpp core/src/uarm/uarm/imx1_i2c.cpp -o $(OBJ_ROOT)/test-imx1-i2c-read-window
	$(OBJ_ROOT)/test-imx1-i2c-read-window

test-zodiac-i2c-image: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-i2c-image.py

test-imx1-i2c: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-i2c.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp -o $(OBJ_ROOT)/test-imx1-i2c
	$(OBJ_ROOT)/test-imx1-i2c

.PHONY: test-imx1-asp
test-imx1-asp: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-asp.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp -o $(OBJ_ROOT)/test-imx1-asp
	$(OBJ_ROOT)/test-imx1-asp

.PHONY: test-zodiac-asp-input
test-zodiac-asp-input: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-asp-input.py

.PHONY: test-imx1-pwm
test-imx1-pwm: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-pwm.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp \
		core/src/uarm/uarm/imx1_asp.cpp -o $(OBJ_ROOT)/test-imx1-pwm
	$(OBJ_ROOT)/test-imx1-pwm

# Diagnostic transport capture only, outside make test and public board dispatch.
.PHONY: test-zodiac-cs4-capture
test-zodiac-cs4-capture: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm tests/zodiac-cs4-capture.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-cs4-capture
	$(OBJ_ROOT)/test-zodiac-cs4-capture
	python3 tests/zodiac-cs4-capture.py

.PHONY: test-imx1-rtc test-zodiac-rtc-input
test-imx1-rtc: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-rtc.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp \
		core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp \
		core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp \
		core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-rtc
	$(OBJ_ROOT)/test-imx1-rtc

test-zodiac-rtc-input: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-rtc-input.py

.PHONY: test-imx1-watchdog
test-imx1-watchdog: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/imx1-watchdog.cpp core/src/uarm/uarm/imx1_watchdog.cpp \
		core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp \
		core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp \
		core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp \
		core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp \
		core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-imx1-watchdog
	$(OBJ_ROOT)/test-imx1-watchdog

# Probe-only bounded W4200 reset echo, outside public board dispatch and make test.
.PHONY: test-zodiac-cs5-reset
test-zodiac-cs5-reset: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm tests/zodiac-cs5-reset.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-cs5-reset
	$(OBJ_ROOT)/test-zodiac-cs5-reset
	python3 tests/zodiac-cs5-reset.py

# Configured virtual W4200 aperture, with full identity/revision reads guarded.
.PHONY: test-zodiac-cs5-configuration
test-zodiac-cs5-configuration: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-cs5-configuration.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-cs5-configuration
	$(OBJ_ROOT)/test-zodiac-cs5-configuration

# Explicit virtual graphics personality, with the strict identity boundary retained.
.PHONY: test-zodiac-virtual-graphics
test-zodiac-virtual-graphics: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-virtual-graphics.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-virtual-graphics
	$(OBJ_ROOT)/test-zodiac-virtual-graphics
	python3 tests/zodiac-virtual-graphics.py

.PHONY: test-zodiac-graphics-gpio
test-zodiac-graphics-gpio: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-gpio.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-gpio
	$(OBJ_ROOT)/test-zodiac-graphics-gpio
	python3 tests/zodiac-graphics-slot-gpio.py

.PHONY: test-zodiac-graphics-memory
test-zodiac-graphics-memory: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-memory.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-memory
	$(OBJ_ROOT)/test-zodiac-graphics-memory

.PHONY: test-zodiac-graphics-controller
test-zodiac-graphics-controller: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-controller.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-controller
	$(OBJ_ROOT)/test-zodiac-graphics-controller

.PHONY: test-zodiac-graphics-engine
test-zodiac-graphics-engine: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-engine.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-engine
	$(OBJ_ROOT)/test-zodiac-graphics-engine

.PHONY: test-zodiac-graphics-commands
test-zodiac-graphics-commands: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-commands.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-commands
	$(OBJ_ROOT)/test-zodiac-graphics-commands

.PHONY: test-zodiac-graphics-worker
# The Fast fill worker thread against fills on the calling thread, and the same with ThreadSanitizer.
test-zodiac-graphics-worker: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-worker.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -pthread -o $(OBJ_ROOT)/test-zodiac-graphics-worker
	$(OBJ_ROOT)/test-zodiac-graphics-worker
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=thread \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-worker.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -pthread -o $(OBJ_ROOT)/test-zodiac-graphics-worker-tsan
	TSAN_OPTIONS=halt_on_error=1 setarch -R $(OBJ_ROOT)/test-zodiac-graphics-worker-tsan

.PHONY: test-zodiac-graphics-gpu
# The host GPU path against the CPU model; skipped (77) on a host with no usable GPU.
test-zodiac-graphics-gpu: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-gpu.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-gpu
	$(OBJ_ROOT)/test-zodiac-graphics-gpu; status=$$?; [ $$status = 0 ] || [ $$status = 77 ]

.PHONY: test-zodiac-graphics-a4
test-zodiac-graphics-a4: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-a4.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-a4
	$(OBJ_ROOT)/test-zodiac-graphics-a4
	python3 tests/zodiac-graphics-a4.py

.PHONY: test-zodiac-graphics-packed-mono
test-zodiac-graphics-packed-mono: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-graphics-packed-mono.py

.PHONY: test-zodiac-graphics-plain-copy
test-zodiac-graphics-plain-copy: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-plain-copy.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-plain-copy
	$(OBJ_ROOT)/test-zodiac-graphics-plain-copy
	python3 tests/zodiac-graphics-plain-copy.py

.PHONY: test-zodiac-graphics-display
test-zodiac-graphics-display: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-graphics-display.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-display
	$(OBJ_ROOT)/test-zodiac-graphics-display

# The W4200 video overlay as player code drives it; the ROM driver and the IDCT still refused.
.PHONY: test-zodiac-video
test-zodiac-video: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/common tests/zodiac-video.cpp \
		core/src/uarm/uarm/imx1_eim.cpp -o $(OBJ_ROOT)/test-zodiac-video
	$(OBJ_ROOT)/test-zodiac-video

.PHONY: test-zodiac-graphics-clocks
test-zodiac-graphics-clocks: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm tests/zodiac-graphics-clocks.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-clocks
	$(OBJ_ROOT)/test-zodiac-graphics-clocks

# CS5 asynchronous external acknowledge and guest-clock transaction checks.
.PHONY: test-zodiac-cs5-bus
test-zodiac-cs5-bus: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/common tests/zodiac-cs5-bus.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp \
		core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp \
		core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp -o $(OBJ_ROOT)/test-zodiac-cs5-bus
	$(OBJ_ROOT)/test-zodiac-cs5-bus
	python3 tests/zodiac-cs5-bus.py

.PHONY: test-zodiac-graphics-interrupts
test-zodiac-graphics-interrupts: zodiac-boot-probe prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm \
		tests/zodiac-graphics-interrupts.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-interrupts
	$(OBJ_ROOT)/test-zodiac-graphics-interrupts
	python3 tests/zodiac-graphics-interrupts.py

.PHONY: test-zodiac-graphics-acquisition
.PHONY: test-zodiac-graphics-sd
# Slot 2 (the W4200 SD block): the empty slot's answers, then a card: detect and its interrupt, init, PIO
# transfers, errors and a state saved mid-transfer.
test-zodiac-graphics-sd: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-graphics-sd.cpp core/src/uarm/uarm/vSD.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-zodiac-graphics-sd
	$(OBJ_ROOT)/test-zodiac-graphics-sd

test-zodiac-graphics-acquisition: zodiac-boot-probe prepare test-zodiac-live-controls
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm tests/zodiac-graphics-acquisition.cpp core/src/uarm/uarm/imx1_gpio.cpp -o $(OBJ_ROOT)/test-zodiac-graphics-acquisition
	$(OBJ_ROOT)/test-zodiac-graphics-acquisition
	python3 tests/zodiac-graphics-acquisition.py

# Explicit equal-frequency MCU PLL reprogramming and complete CPU handoff.
.PHONY: test-imx1-mpll-retune
test-imx1-mpll-retune: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -DFROND_TEST_FAST=$(FROND_FAST) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/imx1-mpll-retune.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-imx1-mpll-retune
	$(OBJ_ROOT)/test-imx1-mpll-retune

# RTC interrupt configuration with an active watchdog.
.PHONY: test-imx1-rtc-live-writes
test-imx1-rtc-live-writes: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -DFROND_TEST_FAST=$(FROND_FAST) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/imx1-rtc-live-writes.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-imx1-rtc-live-writes
	$(OBJ_ROOT)/test-imx1-rtc-live-writes

# Independent SSI serial and bus-clock divider transitions.
.PHONY: test-zodiac-cscr-ssi-clocks
test-zodiac-cscr-ssi-clocks: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -DFROND_TEST_FAST=$(FROND_FAST) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-cscr-ssi-clocks.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-cscr-ssi-clocks
	$(OBJ_ROOT)/test-zodiac-cscr-ssi-clocks

# Stop mode: both PLLs off, CLK32 only, wake on the GPIO lines and the RTC.
.PHONY: test-zodiac-stop-mode
test-zodiac-stop-mode: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -DFROND_TEST_FAST=$(FROND_FAST) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-stop-mode.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-stop-mode
	$(OBJ_ROOT)/test-zodiac-stop-mode

# MCU PLL shutdown and qualified WFI wake.
.PHONY: test-zodiac-idle-wake
test-zodiac-idle-wake: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -DFROND_TEST_FAST=$(FROND_FAST) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-idle-wake.cpp core/src/uarm/uarm/imx1_boot_probe.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-idle-wake
	$(OBJ_ROOT)/test-zodiac-idle-wake
	python3 tests/zodiac-idle-wake-cli.py

.PHONY: test-imx1-mmcsd-card
test-imx1-mmcsd-card: prepare $(COMMON_LIB)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/imx1-mmcsd-card.cpp core/src/uarm/uarm/vSD.cpp core/src/uarm/uarm/imx1_sysctrl.cpp \
		core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp \
		core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp \
		core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		$(COMMON_LIB) -o $(OBJ_ROOT)/test-imx1-mmcsd-card
	$(OBJ_ROOT)/test-imx1-mmcsd-card

.PHONY: test-imx1-memory-dma
test-imx1-memory-dma: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/imx1-memory-dma.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-imx1-memory-dma
	$(OBJ_ROOT)/test-imx1-memory-dma

.PHONY: test-zodiac-mmcsd-empty
test-zodiac-mmcsd-empty: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-mmcsd-empty.py

.PHONY: test-zodiac-asp-pen
test-zodiac-asp-pen: $(OBJ_ROOT)/zodiac-asp-pen-probe
	python3 tests/zodiac-asp-pen.py
# Links with the list build/out/zodiac-boot-probe wrote; writing it again here would empty it under
# a parallel link that reads it.
$(OBJ_ROOT)/zodiac-asp-pen-probe: $(OBJ_ROOT)/zodiac-boot-probe tests/zodiac-asp-pen-probe.cpp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-asp-pen-probe.cpp core/src/uarm/uarm/imx1_boot_probe.cpp core/src/uarm/uarm/imx1_watchdog.cpp core/src/uarm/uarm/imx1_rtc.cpp core/src/uarm/uarm/imx1_bootstrap.cpp core/src/uarm/uarm/imx1_aitc.cpp core/src/uarm/uarm/imx1_cgm.cpp core/src/uarm/uarm/imx1_gpio.cpp core/src/uarm/uarm/imx1_sysctrl.cpp core/src/uarm/uarm/imx1_eim.cpp core/src/uarm/uarm/imx1_timer.cpp core/src/uarm/uarm/imx1_i2c.cpp core/src/uarm/uarm/imx1_asp.cpp core/src/uarm/uarm/imx1_pwm.cpp \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $@

.PHONY: test-zodiac-machine-state
test-zodiac-machine-state: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-machine-state.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-machine-state
	$(OBJ_ROOT)/test-zodiac-machine-state

.PHONY: test-zodiac-ymf-ready
# The Zodiac boot's chip power cycle, sleep and wake end their YMF768 power waits in a few tries, alike in Exact and
# Fast and through a load mid-wait. Needs the Zodiac ROM in FROND_DEVICE_IMAGES, else skipped (minutes in Exact).
test-zodiac-ymf-ready: benchmark-replay
	python3 tests/zodiac-ymf-ready.py

.PHONY: test-zodiac-fast-lean
# The Fast engine's lean path against Exact: TLB flush on CP15 writes, the stop pc and an unsupported access.
test-zodiac-fast-lean: $(OBJ_ROOT)/zodiac-boot-probe test-imx1-steps-until
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-fast-lean.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-fast-lean
	$(OBJ_ROOT)/test-zodiac-fast-lean
	ZODIAC_FAST_LEAN_RING=1 FROND_ZODIAC_CROSS_END=0 FROND_ZODIAC_KEEP_CHECKS=0 $(OBJ_ROOT)/test-zodiac-fast-lean

.PHONY: test-zodiac-code-hooks
# Code hooks on their own: set on cached ARM and Thumb code, every pass stopped, cleared code runs lean and compiled.
test-zodiac-code-hooks: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common -Itests \
		tests/zodiac-code-hooks.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-code-hooks
	$(OBJ_ROOT)/test-zodiac-code-hooks

.PHONY: test-zodiac-music-hook
# Music hooks: the Tapwave Midi Library found (and on ZODIAC_ROM, the 1.1 ROM's), a guest's calls caught with their
# arguments in Exact and both Fast paths, the guest's run unchanged.
test-zodiac-music-hook: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common -Itests \
		tests/zodiac-music-hook.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-music-hook
	ZODIAC_ROM="$(ZODIAC_ROM)" $(OBJ_ROOT)/test-zodiac-music-hook

.PHONY: test-zodiac-guest-exceptions
test-zodiac-guest-exceptions: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-guest-exceptions.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-guest-exceptions
	$(OBJ_ROOT)/test-zodiac-guest-exceptions

.PHONY: test-zodiac-state-fingerprint
# Compiles imx1_boot_probe.cpp for Android (libc++, arm64 and x86_64) with the NDK, whose static_assert checks the
# state layout fingerprint is the desktop's, so states move between hosts. Skipped without an NDK.
ZODIAC_NDK ?= $(lastword $(sort $(wildcard $(or $(ANDROID_HOME),$(HOME)/Android/Sdk)/ndk/*)))
test: test-zodiac-state-fingerprint
test-zodiac-state-fingerprint: prepare
	@if [ -z "$(ZODIAC_NDK)" ]; then echo "test-zodiac-state-fingerprint: no NDK, skipped"; exit 0; fi; \
	for t in aarch64 x86_64; do \
		$(ZODIAC_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ --target=$$t-linux-android26 -std=c++17 \
			-fsyntax-only -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
			core/src/uarm/uarm/imx1_boot_probe.cpp || exit 1; \
		echo "zodiac state fingerprint $$t-android: compiles, fingerprint static_assert holds"; \
	done

.PHONY: test-zodiac-machine-state-reject test-zodiac-machine-state-cli
test-zodiac-machine-state-reject: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-machine-state-reject.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-machine-state-reject
	$(OBJ_ROOT)/test-zodiac-machine-state-reject

# Two probe processes: one saves, the other loads and has to end like an uninterrupted run.
test-zodiac-machine-state-cli: $(OBJ_ROOT)/zodiac-boot-probe
	python3 tests/zodiac-machine-state-cli.py

.PHONY: test-zodiac-bounded
test-zodiac-bounded: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-bounded.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-bounded
	$(OBJ_ROOT)/test-zodiac-bounded

# Faster speed on the Zodiac: FCLK times the multiplier in the clock model and on a running machine.
.PHONY: test-zodiac-faster
test-zodiac-faster: $(OBJ_ROOT)/zodiac-boot-probe
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror \
		-Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/zodiac-faster.cpp $(ZODIAC_MACHINE_SOURCES) \
		@$(OBJ_ROOT)/zodiac-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -o $(OBJ_ROOT)/test-zodiac-faster
	$(OBJ_ROOT)/test-zodiac-faster

# The Zodiac checks not listed elsewhere in the suite. Left out for time: arm-add-pc, ldm-return and fast-lean;
# cs4-capture is a diagnostic kept outside make test.
.PHONY: test-zodiac
test-zodiac: test-zodiac-armv4-probe test-zodiac-faster test-zodiac-asp-input test-zodiac-asp-pen test-zodiac-boot-probe test-zodiac-bounded test-zodiac-cs5-bus test-zodiac-cs5-configuration test-zodiac-cs5-reset test-zodiac-cscr-ssi-clocks test-zodiac-dcache test-zodiac-dma-channel test-zodiac-dma-startup test-zodiac-exception test-zodiac-gpio-event test-zodiac-gpio-input test-zodiac-guest-exceptions test-zodiac-graphics-a4 test-zodiac-graphics-acquisition test-zodiac-graphics-clocks test-zodiac-graphics-commands test-zodiac-graphics-controller test-zodiac-graphics-display test-zodiac-graphics-engine test-zodiac-graphics-gpio test-zodiac-graphics-gpu test-zodiac-graphics-worker test-zodiac-graphics-interrupts test-zodiac-graphics-memory test-zodiac-graphics-packed-mono test-zodiac-graphics-plain-copy test-zodiac-graphics-sd test-zodiac-i2c-image test-zodiac-idle-wake test-zodiac-irq test-zodiac-lcdc-contrast test-zodiac-live-controls test-zodiac-machine test-zodiac-machine-state test-zodiac-machine-state-cli test-zodiac-machine-state-reject test-zodiac-mmcsd-empty test-zodiac-mmio test-zodiac-mmu test-zodiac-multiply test-zodiac-music-hook test-zodiac-normal-runtime test-zodiac-pcm-transport test-zodiac-rtc-input test-zodiac-rumble test-zodiac-session-cards test-zodiac-stick-view test-zodiac-sound test-zodiac-ssi-startup test-zodiac-stop-mode test-zodiac-store-pc test-zodiac-swap test-zodiac-thumb-add-pc test-zodiac-thumb-probe test-zodiac-timer-probe test-zodiac-trace-window test-zodiac-uart-startup test-zodiac-user-bank test-zodiac-video test-zodiac-virtual-graphics test-zodiac-ymf768
test: test-zodiac

# The 68k tests link from cloudpilot's .build-frond objects. out/frond-68k builds them once first,
# so their sub-makes running side by side only link and never rewrite an object another one is reading.
.PHONY: test-mediaq-usb
test: test-mediaq-usb
test-mediaq-usb: prepare
	mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O1 -g -Icore/src/common -Icore/src/cloudpilot/emulator/hardware tests/mediaq-usb.cpp -o $(OBJ_ROOT)/test-mediaq-usb
	$(OBJ_ROOT)/test-mediaq-usb

.PHONY: test-68k-vz-registers
test: test-68k-vz-registers
test-68k-vz-registers: prepare out/frond-68k
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/68k-vz-registers.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) vz-register-test

.PHONY: test-68k-battery
test: test-68k-battery
test-68k-battery: prepare out/frond-68k
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/68k-battery.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) battery-test

.PHONY: test-68k-ez-uart-spi
test: test-68k-ez-uart-spi
test-68k-ez-uart-spi: prepare out/frond-68k
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/68k-ez-uart-spi.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) ez-uart-spi-test

.PHONY: test-68k-host-transport
test: test-68k-host-transport
test-68k-host-transport: prepare out/frond-68k
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/68k-host-transport.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) host-transport-test

.PHONY: test-68k-dana
test: test-68k-dana
test-68k-dana: prepare out/frond-68k
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/68k-dana.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) dana-test

.PHONY: test-dana-input
test: test-dana-input
test-dana-input: prepare
	$(CXX) -std=c++17 -Wall -Wextra -Icore/src/common tests/dana-input.cpp -o $(OBJ_ROOT)/test-dana-input
	$(OBJ_ROOT)/test-dana-input

.PHONY: test-68k-sd
test-68k-sd: prepare
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -j4 -f Makefile \
		-f $(CURDIR)/tests/68k-sd.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) sd-test

.PHONY: test-68k-ms
test-68k-ms: prepare
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -j4 -f Makefile \
		-f $(CURDIR)/tests/68k-ms.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) ms-test

# frond-68k linked cards moving into the cards library (move-linked, settle-linked, card swap).
.PHONY: test-68k-card-swap
test: test-68k-card-swap
test-68k-card-swap: prepare out/frond
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -j4 -f Makefile \
		-f $(CURDIR)/tests/68k-ms.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) FROND=$(CURDIR)/out/frond card-swap-test

.PHONY: test-68k-card-provider
test-68k-card-provider: prepare
	$(MAKE) -C core/src/common $(CORE_OBJ_FLAGS) bin
	$(CXX) -std=c++17 -O1 -Icore/src/common -Icore/src/uarm/uarm \
		tests/68k-card-provider.cpp $(COMMON_LIB) -o $(OBJ_ROOT)/test-68k-card-provider
	$(OBJ_ROOT)/test-68k-card-provider $(OBJ_ROOT)/provider-runtime-check

.PHONY: test-dana-card-inventory
test-dana-card-inventory: prepare
	$(CXX) -std=c++17 -O2 -Icore/src/common tests/dana-card-inventory.cpp -o $(OBJ_ROOT)/test-dana-card-inventory
	$(OBJ_ROOT)/test-dana-card-inventory

.PHONY: test-68k-bus-state
test: test-68k-bus-state
test-68k-bus-state: prepare out/frond-68k
	$(MAKE) -C core/src/cloudpilot $(CORE_OBJ_FLAGS) FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/68k-bus-state.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) bus-state-test

.PHONY: test-raw-print-job
RAW_PRINT_BUILD ?= $(OBJ_ROOT)/raw-print-job
RAW_PRINT_SOURCE ?= $(RAW_PRINT_BUILD)/source
test-raw-print-job:
	python3 tools/copy-core.py $(RAW_PRINT_SOURCE)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I$(RAW_PRINT_SOURCE)/src/common tests/raw-print-job.cpp -o $(RAW_PRINT_BUILD)/test-raw-print-job
	$(RAW_PRINT_BUILD)/test-raw-print-job

.PHONY: test-raw-print-file
RAW_PRINT_FILE_BUILD ?= $(OBJ_ROOT)/raw-print-file
test-raw-print-file:
	python3 tools/copy-core.py $(RAW_PRINT_FILE_BUILD)/source
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I$(RAW_PRINT_FILE_BUILD)/source/src/common tests/raw-print-file.cpp -Wl,--wrap=write,--wrap=unlinkat -o $(RAW_PRINT_FILE_BUILD)/test-raw-print-file
	mkdir -p $(RAW_PRINT_FILE_BUILD)/output
	$(RAW_PRINT_FILE_BUILD)/test-raw-print-file $(abspath $(RAW_PRINT_FILE_BUILD)/output)

ISP_HOST_AREA ?= $(abspath $(OBJ_ROOT))/isp-host-pio
.PHONY: test-isp-host-pio
test-isp-host-pio:
	python3 tools/copy-core.py $(ISP_HOST_AREA)/source
	$(MAKE) -C $(ISP_HOST_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(ISP_HOST_AREA)/source/src/cloudpilot FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/isp-host-pio.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) \
		ISP_HOST_TEST=$(ISP_HOST_AREA)/test-isp-host-pio isp-host-test

ISP_ATL_AREA ?= $(abspath $(OBJ_ROOT))/isp-host-atl
.PHONY: test-isp-host-atl
test-isp-host-atl:
	python3 tools/copy-core.py $(ISP_ATL_AREA)/source
	$(MAKE) -C $(ISP_ATL_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(ISP_ATL_AREA)/source/src/cloudpilot FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/isp-host-atl.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) \
		ISP_ATL_TEST=$(ISP_ATL_AREA)/test-isp-host-atl isp-atl-test

ISP_IRQ_AREA ?= $(abspath $(OBJ_ROOT))/isp-host-irq
.PHONY: test-isp-host-irq
test-isp-host-irq:
	python3 tools/copy-core.py $(ISP_IRQ_AREA)/source
	$(MAKE) -C $(ISP_IRQ_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(ISP_IRQ_AREA)/source/src/cloudpilot FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/isp-host-irq.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) \
		ISP_IRQ_TEST=$(ISP_IRQ_AREA)/test-isp-host-irq isp-irq-test

ISP_ROOT_AREA ?= $(abspath $(OBJ_ROOT))/isp-host-root
.PHONY: test-isp-host-root
test-isp-host-root:
	python3 tools/copy-core.py $(ISP_ROOT_AREA)/source
	$(MAKE) -C $(ISP_ROOT_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(ISP_ROOT_AREA)/source/src/cloudpilot FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/isp-host-root.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) \
		ISP_ROOT_TEST=$(ISP_ROOT_AREA)/test-isp-host-root isp-root-test

.PHONY: test-usb-raw-printer
USB_PRINTER_AREA ?= $(OBJ_ROOT)/usb-raw-printer
test-usb-raw-printer:
	python3 tools/copy-core.py $(USB_PRINTER_AREA)/source
	$(MAKE) -C $(USB_PRINTER_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(USB_PRINTER_AREA)/source/src/cloudpilot -f Makefile -f $(CURDIR)/tests/usb-raw-printer.mk FROND_68K=1 FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) USB_PRINTER_TEST=$(abspath $(USB_PRINTER_AREA))/test-usb-raw-printer usb-printer-test

.PHONY: test-usb-prism2-adapter
USB_PRISM2_AREA ?= $(OBJ_ROOT)/usb-prism2-adapter
test-usb-prism2-adapter:
	python3 tools/copy-core.py $(USB_PRISM2_AREA)/source
	g++ -std=c++17 -Wall -Wextra -I$(USB_PRISM2_AREA)/source/src/common -o $(USB_PRISM2_AREA)/test-usb-prism2-adapter tests/usb-prism2-adapter.cpp
	$(USB_PRISM2_AREA)/test-usb-prism2-adapter

.PHONY: test-usb-printer-runtime
USB_RUNTIME_AREA ?= $(OBJ_ROOT)/usb-printer-runtime
test-usb-printer-runtime:
	python3 tools/copy-core.py $(USB_RUNTIME_AREA)/source
	$(MAKE) -C $(USB_RUNTIME_AREA)/source/src/common libcommon.a net
	mkdir -p $(USB_RUNTIME_AREA)/output
	$(MAKE) -C $(USB_RUNTIME_AREA)/source/src/cloudpilot -f Makefile -f $(CURDIR)/tests/usb-printer-runtime.mk FROND_68K=1 FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) USB_RUNTIME_TEST=$(abspath $(USB_RUNTIME_AREA))/test-usb-printer-runtime USB_RUNTIME_OUTPUT=$(abspath $(USB_RUNTIME_AREA))/output usb-runtime-test

.PHONY: test-usb-wifi-runtime
USB_WIFI_AREA ?= $(OBJ_ROOT)/usb-wifi-runtime
test-usb-wifi-runtime:
	python3 tools/copy-core.py $(USB_WIFI_AREA)/source
	$(MAKE) -C $(USB_WIFI_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(USB_WIFI_AREA)/source/src/cloudpilot -f Makefile -f $(CURDIR)/tests/usb-wifi-runtime.mk FROND_68K=1 FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) USB_WIFI_TEST=$(abspath $(USB_WIFI_AREA))/test-usb-wifi-runtime usb-wifi-test

# The Dana Wireless's built-in Wi-Fi module on root port 1, built in the same area.
.PHONY: test-dana-wifi-module
test-dana-wifi-module:
	python3 tools/copy-core.py $(USB_WIFI_AREA)/source
	$(MAKE) -C $(USB_WIFI_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(USB_WIFI_AREA)/source/src/cloudpilot -f Makefile -f $(CURDIR)/tests/usb-wifi-runtime.mk FROND_68K=1 FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) DANA_MODULE_TEST=$(abspath $(USB_WIFI_AREA))/test-dana-wifi-module dana-module-test

ISP_FRAME_AREA ?= $(abspath $(OBJ_ROOT))/isp-frame
.PHONY: test-isp-frame
test-isp-frame:
	python3 tools/copy-core.py $(ISP_FRAME_AREA)/source
	$(MAKE) -C $(ISP_FRAME_AREA)/source/src/common libcommon.a net
	$(MAKE) -C $(ISP_FRAME_AREA)/source/src/cloudpilot FROND_68K=1 -f Makefile \
		-f $(CURDIR)/tests/isp-frame.mk FROND_TEST_ROOT=$(CURDIR) FROND_TEST_OBJ=$(abspath $(OBJ_ROOT)) \
		ISP_FRAME_TEST=$(ISP_FRAME_AREA)/test-isp-frame isp-frame-test

.PHONY: test-print-text
test: test-print-text
test-print-text:
	mkdir -p $(OBJ_ROOT)/print-text
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror tests/print-text.cpp -o $(OBJ_ROOT)/print-text/test-print-text
	$(OBJ_ROOT)/print-text/test-print-text

# The S3C2442 machine (the HP iPAQ rx1950): its blocks, its NAND image and the boot tool, which runs the
# bootloader and keeps the UART output. The core builds them too; the tools here take their own copies.
S3C2442_SOURCES = core/src/uarm/uarm/s3c2442_intc.cpp core/src/uarm/uarm/s3c2442_clock.cpp \
	core/src/uarm/uarm/s3c2442_timer.cpp core/src/uarm/uarm/s3c2442_rtc.cpp core/src/uarm/uarm/s3c2442_gpio.cpp \
	core/src/uarm/uarm/s3c2442_uart.cpp core/src/uarm/uarm/s3c2442_lcd.cpp core/src/uarm/uarm/s3c2442_nand.cpp \
	core/src/uarm/uarm/s3c2442_sdi.cpp core/src/uarm/uarm/s3c2442_usbd.cpp \
	core/src/uarm/uarm/s3c2442_adc.cpp core/src/uarm/uarm/s3c2442_audio_bus.cpp \
	core/src/uarm/uarm/acx100.cpp core/src/uarm/uarm/wifi_access_point.cpp
S3C2442_HEADERS = core/src/uarm/uarm/s3c2442_blocks.h core/src/uarm/uarm/s3c2442_nand.h \
	core/src/uarm/uarm/s3c2442_machine.h core/src/uarm/uarm/rx1950_nand_image.h core/src/uarm/uarm/rx1950_board.h \
	core/src/uarm/uarm/rx1950_power.h core/src/uarm/uarm/s3c2442_pen.h core/src/uarm/uarm/uda1380.h \
	core/src/uarm/uarm/acx100.h core/src/uarm/uarm/wifi_access_point.h
.PHONY: rx1950-boot test-s3c2442 test-s3c2442-machine test-acx100 test-rx1950-boot
rx1950-boot: $(OBJ_ROOT)/rx1950-boot
$(OBJ_ROOT)/rx1950-boot: out/frond tools/rx1950-boot.cpp $(S3C2442_SOURCES) $(S3C2442_HEADERS) \
		core/src/uarm/uarm/s3c2442_machine.cpp core/src/uarm/uarm/rx1950_nand_image.cpp zodiac-rom/ZodiacRom.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name 's3c2442_*' ! -name 'acx100*' ! -name 'wifi_access_point*' ! -name 'rx1950_*' ! -name 'soc_S3C2442*' ! -name 'UsbHostBridge*' > $(OBJ_ROOT)/rx1950-core.rsp
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common -Izodiac-rom \
		tools/rx1950-boot.cpp core/src/uarm/uarm/s3c2442_machine.cpp core/src/uarm/uarm/rx1950_nand_image.cpp zodiac-rom/ZodiacRom.cpp \
		$(S3C2442_SOURCES) @$(OBJ_ROOT)/rx1950-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -lz -o $@

# Each block's registers, timer ticks, interrupt priority and sub-source masking, UART output.
test: test-s3c2442
test-s3c2442: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -UNDEBUG -Icore/src/uarm/uarm \
		tests/s3c2442-blocks.cpp $(S3C2442_SOURCES) core/src/uarm/uarm/rx1950_nand_image.cpp -o $(OBJ_ROOT)/test-s3c2442
	$(OBJ_ROOT)/test-s3c2442

test: test-s3c2442-machine
test-s3c2442-machine: out/frond tests/s3c2442-machine.cpp $(S3C2442_SOURCES) $(S3C2442_HEADERS) core/src/uarm/uarm/s3c2442_machine.cpp
	find $(UARM_OBJ) -name '*.o' ! -path '*/native/*' ! -name 's3c2442_*' ! -name 'acx100*' ! -name 'wifi_access_point*' ! -name 'rx1950_*' ! -name 'soc_S3C2442*' ! -name 'UsbHostBridge*' > $(OBJ_ROOT)/rx1950-core.rsp
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -UNDEBUG -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/s3c2442-machine.cpp core/src/uarm/uarm/s3c2442_machine.cpp \
		$(S3C2442_SOURCES) @$(OBJ_ROOT)/rx1950-core.rsp $(COMMON_LIB) $(CPU_TEST_LDLIBS) -lz -o $(OBJ_ROOT)/test-s3c2442-machine
	$(OBJ_ROOT)/test-s3c2442-machine

test: test-acx100
test-acx100: prepare
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -UNDEBUG -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
		tests/acx100.cpp core/src/uarm/uarm/acx100.cpp core/src/uarm/uarm/wifi_access_point.cpp -o $(OBJ_ROOT)/test-acx100
	$(OBJ_ROOT)/test-acx100

# The rx1950's bootloader and OS to the kernel's first serial lines, when the firmware is here.
test-rx1950-boot: $(OBJ_ROOT)/rx1950-boot
	python3 tests/rx1950-boot.py $(OBJ_ROOT)/rx1950-boot

.PHONY: test-linked-card-lock test-linked-card-lock-wine
test-linked-card-lock:
	bash tests/linked-card-lock.sh linux

test-linked-card-lock-wine:
	bash tests/linked-card-lock.sh wine

.PHONY: test-sony-ma2-fm
test: test-sony-ma2-fm
test-sony-ma2-fm:
	@mkdir -p $(OBJ_ROOT)
	$(CXX) -std=c++17 -O2 -Icore/src/common/audio \
		tests/sony-ma2-fm.cpp core/src/common/audio/SonyMa2Fm.cpp -o $(OBJ_ROOT)/test-sony-ma2-fm
	$(OBJ_ROOT)/test-sony-ma2-fm
