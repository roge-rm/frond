-include ../Makefile.local

CFLAGS_COMMON = -Wextra -Wall -Wno-unused-parameter
STD_C = -std=gnu99
INCLUDE = -I.

SOURCE_C = 							\
	zip/miniz.c 					\
	zip/zip.c

SOURCE_CXX = 						\
	CardImage.cpp 					\
	CardVolume.cpp 					\
	CPCrc.cpp 						\
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
	ir/IrPrinter.cpp

SOURCE_C_NATIVE = $(SOURCE_C)
SOURCE_CXX_NATIVE = 				\
	$(SOURCE_CXX)					\
	Cli.cpp

SOURCE_C_EMCC = $(SOURCE_C)
SOURCE_CXX_EMCC = $(SOURCE_CXX)

SOURCE_C_TEST = $(SOURCE_C)
SOURCE_CXX_TEST = 					\
	$(SOURCE_CXX) 					\
	test/Crc.cpp 					\
	test/GunzipContext.cpp 			\
	test/GzipContext.cpp			\
	test/SaveChunkHelper.cpp		\
	test/LoadChunkHelper.cpp		\
	test/Savestate.cpp				\
	test/SavestateChunk.cpp			\
	test/SavestateChunkProbe.cpp	\
	test/SavestateLoader.cpp		\
	test/SavestateProbe.cpp			\
	test/Encoding.cpp				\
	test/main.cpp

LIBRARY_NATIVE = libcommon.a
# libslirp behind PPP and Wi-Fi, a library of its own so only the cores link libslirp.
LIBRARY_NET = libcommon-net.a
OBJECTS_NET = $(BUILDDIR_NATIVE)/net/SlirpIp.o $(BUILDDIR_NATIVE)/net/WifiRouter.o
GARBAGE_EXTRA = $(LIBRARY_NET)
LIBRARY_EMCC = libcommon-wasm.a
BINARY_TEST = test/test

include ../Makefile.common

bin: $(LIBRARY_NATIVE)

net: $(LIBRARY_NET)

$(OBJECTS_NET): CXXFLAGS_NATIVE_EXTRA += $(shell pkg-config --cflags slirp)

$(LIBRARY_NET): $(OBJECTS_NET)
	$(AR_NATIVE) cru $@ $^
	$(RANLIB_NATIVE) $@

emscripten: $(LIBRARY_EMCC)

test: $(BINARY_TEST)
	$(BINARY_TEST)

.PHONY: bin net emscripten clean test