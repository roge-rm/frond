-include ../Makefile.local

CFLAGS_COMMON = -Wextra -Wall -Wno-unused-parameter
STD_C = -std=gnu99
INCLUDE = -I.

SOURCE_C = 							\
	zip/miniz.c 					\
	zip/zip.c					\
	sync/SqliteBuild.c \
	sync/SynceRapi.c

SOURCE_CXX = 						\
	CardImage.cpp 					\
	CardVolume.cpp 					\
	CPCrc.cpp 						\
	RndisHost.cpp					\
	GunzipContext.cpp 				\
	GzipContext.cpp 				\
	CreateZipContext.cpp 			\
	ZipfileWalker.cpp 				\
	FileUtil.cpp 					\
	Logging.cpp						\
	encoding.cpp					\
	md5.cpp							\
	rom_info5.cpp					\
	session/rle.cpp					\
	session/session_file5.cpp		\
	savestate/Chunk.cpp				\
	savestate/ChunkProbe.cpp		\
	modem/HayesModem.cpp			\
	modem/PppServer.cpp				\
	ir/Sir.cpp						\
	ir/IrLap.cpp					\
	ir/IrLmp.cpp					\
	ir/Ias.cpp						\
	ir/TinyTp.cpp					\
	ir/IrComm.cpp					\
	ir/IrPhone.cpp				\
	ir/IrPrinter.cpp			\
	sync/PimRecord.cpp			\
	sync/PimSync.cpp				\
	sync/PalmDb.cpp				\
	sync/InstallerFiles.cpp \
	sync/SyncStore.cpp			\
	sync/Dlp.cpp				\
	sync/HotSync.cpp			\
	sync/Link.cpp				\
	sync/SyncManager.cpp \
	sync/ActiveSync.cpp \
	sync/ActiveSyncFiles.cpp \
	sync/ActiveSyncPartnership.cpp \
	sync/AirSync.cpp \
	sync/Wbxml.cpp

SOURCE_C_NATIVE = $(SOURCE_C)
SOURCE_CXX_NATIVE = 				\
	$(SOURCE_CXX)					\
	Cli.cpp

LIBRARY_NATIVE = libcommon.a
# libslirp behind PPP and Wi-Fi, a library of its own so only the cores link libslirp.
LIBRARY_NET = libcommon-net.a
OBJECTS_NET = $(BUILDDIR_NATIVE)/net/SlirpIp.o $(BUILDDIR_NATIVE)/net/WifiRouter.o $(BUILDDIR_NATIVE)/net/DesktopLink.o
GARBAGE_EXTRA = $(LIBRARY_NET)

include ../Makefile.common

bin: $(LIBRARY_NATIVE)

net: $(LIBRARY_NET)

$(OBJECTS_NET): CXXFLAGS_NATIVE_EXTRA += $(shell pkg-config --cflags slirp)

$(LIBRARY_NET): $(OBJECTS_NET)
	$(AR_NATIVE) cru $@ $^
	$(RANLIB_NATIVE) $@

.PHONY: bin net clean