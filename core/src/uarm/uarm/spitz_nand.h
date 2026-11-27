#ifndef SPITZ_NAND_H
#define SPITZ_NAND_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "reschedule.h"

struct ArmMem;
struct MemoryBuffer;
struct NAND;
struct SpitzNand;

constexpr size_t spitzNandRawSize = 1024 * 32 * 528;

// Converts separate main and spare runs in a service backup to interleaved NAND pages.
bool spitzImportDbk(const uint8_t* data, size_t size, std::vector<uint8_t>& raw);
// Reads the stock Linux zImage from the Sharp logical boot partition.
bool spitzExtractKernel(const uint8_t* raw, size_t size, std::vector<uint8_t>& kernel);

SpitzNand* spitzNandInit(ArmMem* mem, Reschedule reschedule, uint8_t* raw, size_t size,
                         const MemoryBuffer* pageBuffer);
void spitzNandDestroy(SpitzNand* controller);
NAND* spitzNandGetNand(SpitzNand* controller);

template <typename T>
void spitzNandSave(SpitzNand* controller, T& savestate);

template <typename T>
void spitzNandLoad(SpitzNand* controller, T& loader);

#endif
