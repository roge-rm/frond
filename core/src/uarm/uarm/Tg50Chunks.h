// TG50 board and chip state use separate versioned chunks.
#ifndef FROND_TG50_CHUNKS_H
#define FROND_TG50_CHUNKS_H
#include "savestate/ChunkTypeUarm.h"
constexpr ChunkType tg50UcChunk = ChunkType::sonyTg50Uc;
constexpr ChunkType tg50TouchChunk = ChunkType::sonyTg50Touch;
constexpr ChunkType tg50CodecChunk = ChunkType::sonyTg50Codec;
constexpr ChunkType tg50PowerChunk = ChunkType::sonyTg50Power;
constexpr ChunkType tg50MsChunk = ChunkType::sonyTg50Ms;
constexpr ChunkType tg50BoardChunk = ChunkType::sonyTg50Board;
#endif
