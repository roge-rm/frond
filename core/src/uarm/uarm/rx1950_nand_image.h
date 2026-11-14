#ifndef FROND_RX1950_NAND_IMAGE_H
#define FROND_RX1950_NAND_IMAGE_H

// The rx1950's NAND contents (S3cNand's page layout) made from the bootloader and CEOS.nbf, enough for
// eboot to find and load the OS. The full flash image, its store and the accepted hashes are R2's.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Block 0 page 9 holds the board's settings, which eboot copies into the driver globals; neither dump has
// it, so it reads erased unless debugMessages is set, which clears dwDbgMsgFlags so the kernel keeps its
// debug text (see tools/rx1950-boot.cpp).
struct Rx1950NandOptions {
    bool debugMessages = false;
};

bool rx1950NandImage(const uint8_t* eboot, size_t ebootSize, const uint8_t* nbf, size_t nbfSize,
                     std::vector<uint8_t>& flash, std::string& error, const Rx1950NandOptions& options = {});

#endif
