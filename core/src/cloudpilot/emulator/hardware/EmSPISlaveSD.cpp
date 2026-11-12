#include "EmSPISlaveSD.h"

#include "CPCrc.h"
#include "LegacyCardGeometry.h"
#include <stdexcept>
#include "ExternalStorage.h"
#include "savestate/ChunkHelper.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

// #define DUMP_COMMAND_STREAM

namespace {
    constexpr uint32 SAVESTATE_VERSION = 1;

    constexpr uint8 ERR_ILLEGAL_COMMAND = 0x04;
    constexpr uint8 ERR_CARD_IDLE = 0x01;
    constexpr uint8 ERR_PARAMETER = 0x40;

    constexpr uint8 DATA_TOKEN_DEFAULT = 0xfe;
    constexpr uint8 DATA_TOKEN_CMD25 = 0xfc;
    constexpr uint8 DATA_TOKEN_STOP = 0xfd;

    constexpr uint8 DATA_RESPONSE_ACCEPTED = 0xe5;
    constexpr uint8 DATA_RESPONSE_WRITE_FAILED = 0x0d;



}  // namespace

EmSPISlaveSD::EmSPISlaveSD(EmHAL::Slot slot)
    : slot(slot), chunkType(slot == EmHAL::Slot::sdcard ? ChunkType::spiSlaveSD : ChunkType::spiSlaveSD2) {
    if (slot != EmHAL::Slot::sdcard && slot != EmHAL::Slot::sdcard2)
        throw std::invalid_argument("invalid SD slot");
}

CardImage* EmSPISlaveSD::Image() const { return gExternalStorage.GetImageInSlot(slot); }

void EmSPISlaveSD::Reset() {
    if (Image()) savedProtocol = uint32(Image()->GetProtocol());
    spiState = SpiState::notSelected;
    cardState = CardState::idle;
    lastCmd = 0;
    acmd = false;
    cmd12Countdown = 0;
    bufferSize = bufferIndex = currentBlock = 0;
    memset(buffer, 0, sizeof(buffer));
}

void EmSPISlaveSD::Save(Savestate<ChunkType>& savestate) { DoSave(savestate); }

void EmSPISlaveSD::Save(SavestateProbe<ChunkType>& savestateProbe) { DoSave(savestateProbe); }

void EmSPISlaveSD::Load(SavestateLoader<ChunkType>& loader) {
    Chunk* chunk = loader.GetChunk(chunkType);
    if (!chunk) {
        if (slot == EmHAL::Slot::sdcard2) Reset();
        return;
    }

    const uint32 version = chunk->Get32();
    if (version != 1 && version != 2) {
        logPrintf("unable to restore SPISlaveSD: unsupported savestate version\n");
        loader.NotifyError();

        return;
    }

    // Validate the complete cursor before changing the live transaction.
    const uint32 packed = chunk->Get32();
    const uint32 size = chunk->Get32(), index = chunk->Get32();
    const uint32 block = chunk->Get32(), application = chunk->Get32();
    uint8 bytes[sizeof(buffer)];
    chunk->GetBuffer(bytes, sizeof(bytes));
    const uint32 protocol = version == 2 ? chunk->Get32() : 0;
    const uint8 spi = packed >> 24, card = packed >> 16;
    const uint8 command = packed >> 8, countdown = packed;
    bool valid = protocol <= 1 && (version != 2 || protocol == 1) &&
        (!protocol || application == 0) && !chunk->HasError() && chunk->IsAtEnd() &&
        spi <= static_cast<uint8>(SpiState::txBlockWait) &&
        card <= static_cast<uint8>(CardState::multiblockWrite) &&
        command <= 63 && countdown <= 6 && application <= 1 &&
        size <= sizeof(buffer) && index <= size;
    if (spi == static_cast<uint8>(SpiState::rxArg))
        valid = valid && size == 5 && index < size;
    if (spi == static_cast<uint8>(SpiState::rxBlock))
        valid = valid && size == 514 && index < size &&
            (card == static_cast<uint8>(CardState::writeTransaction) ||
             card == static_cast<uint8>(CardState::multiblockWrite));
    if (spi == static_cast<uint8>(SpiState::rxBlockWait))
        valid = valid && (card == static_cast<uint8>(CardState::writeTransaction) ||
                          card == static_cast<uint8>(CardState::multiblockWrite));
    if (spi == static_cast<uint8>(SpiState::txBlockWait))
        valid = valid && card == static_cast<uint8>(CardState::multiblockRead);
    if (spi == static_cast<uint8>(SpiState::txData))
        valid = valid && size > 0 && index < size;
    if (!valid) { loader.NotifyError(); return; }
    savedProtocol = protocol;
    spiState = static_cast<SpiState>(spi);
    cardState = static_cast<CardState>(card);
    lastCmd = command;
    cmd12Countdown = countdown;
    bufferSize = size;
    bufferIndex = index;
    currentBlock = block;
    acmd = application;
    memcpy(buffer, bytes, sizeof(buffer));
}

uint16 EmSPISlaveSD::DoExchange(uint16 control, uint16 data) {
    uint8 bits = (control & 0x0f) + 1;

    switch (bits) {
        case 16: {
            uint16 dataOut = DoExchange8(data >> 8) << 8;
            dataOut |= DoExchange8(data);

            return dataOut;
        }

        case 8:
            return DoExchange8(data);

        default:
            // cerr << "unsupported SPI shift width " << (int)bits << endl << flush;

            return 0xff;
    }
}

void EmSPISlaveSD::Enable(void) {
    if (spiState != SpiState::notSelected) return;

    if (gExternalStorage.IsMounted(slot)) {
        spiState = SpiState::rxCmdByte;
        lastCmd = 0;
    }
}

void EmSPISlaveSD::Disable(void) { spiState = SpiState::notSelected; }

template <typename T>
void EmSPISlaveSD::DoSave(T& savestate) {
    typename T::chunkT* chunk = savestate.GetChunk(chunkType);
    if (!chunk) return;

    chunk->Put32(IsMMC() ? 2 : SAVESTATE_VERSION);

    SaveChunkHelper helper(*chunk);
    DoSaveLoad(helper);
    if (IsMMC()) chunk->Put32(1);
}

template <typename T>
void EmSPISlaveSD::DoSaveLoad(T& helper) {
    uint8 spiStateByte = static_cast<uint8>(spiState);
    uint8 cardStateByte = static_cast<uint8>(cardState);

    helper.Do(typename T::Pack8() << spiStateByte << cardStateByte << lastCmd << cmd12Countdown)
        .Do32(bufferSize)
        .Do32(bufferIndex)
        .Do32(currentBlock)
        .DoBool(acmd)
        .DoBuffer(buffer, sizeof(buffer));

    spiState = static_cast<SpiState>(spiStateByte);
    cardState = static_cast<CardState>(cardStateByte);
}

uint8 EmSPISlaveSD::DoExchange8(uint8 data) {
    if (!gExternalStorage.IsMounted(slot)) return 0xff;
    if (cardState == CardState::multiblockRead) HandleCmd12(data);

    switch (spiState) {
        case SpiState::notSelected:
            return 0xff;

        case SpiState::rxCmdByte:
            if ((data & 0xc0) == 0x40) {
                lastCmd = data & 0x3f;
                spiState = SpiState::rxArg;
                BufferStart(5);
            }

            return 0xff;

        case SpiState::rxArg:
            buffer[bufferIndex++] = data;

            if (bufferIndex == bufferSize) {
                DoCmd();
                spiState = SpiState::txData;
            }

            return 0xff;

        case SpiState::txData: {
            uint8 dataOut = buffer[bufferIndex++];
            if (bufferIndex == bufferSize) {
                switch (cardState) {
                    case CardState::writeTransaction:
                    case CardState::multiblockWrite:
                        spiState = SpiState::rxBlockWait;
                        break;

                    case CardState::multiblockRead:
                        spiState = SpiState::txBlockWait;
                        break;

                    default:
                        spiState = SpiState::rxCmdByte;
                }
            }

            return dataOut;
        }

        case SpiState::rxBlockWait:
            if (data == DATA_TOKEN_DEFAULT ||
                (data == DATA_TOKEN_CMD25 && cardState == CardState::multiblockWrite)) {
                spiState = SpiState::rxBlock;
                BufferStart(514);
            };

            if ((data == DATA_TOKEN_STOP && cardState == CardState::multiblockWrite)) {
                cardState = CardState::initialized;
                spiState = SpiState::rxCmdByte;
            }

            return 0xff;

        case SpiState::rxBlock:
            buffer[bufferIndex++] = data;
            if (bufferIndex == bufferSize) {
                if (cardState == CardState::multiblockWrite)
                    FinishWriteMultiblock();
                else
                    FinishWriteSingleBlock();
            }

            return 0xff;

        case SpiState::txBlockWait:
            spiState = SpiState::txData;
            DoReadMulitblock();

            return 0xff;
    }

    EmAssert(false);
    return 0;
}

void EmSPISlaveSD::BufferStart(uint32 count) {
    bufferSize = count;
    bufferIndex = 0;
}

void EmSPISlaveSD::BufferAddBlock(uint8 token, uint8* data, size_t size) {
    BufferAdd(token);

    memcpy(buffer + bufferSize, data, size);
    bufferSize += size;

    uint16 crc16 = crc::sdCRC16(data, size);
    BufferAdd(crc16 >> 8, crc16);
}

void EmSPISlaveSD::DoCmd() {
    if (acmd && !IsMMC()) return DoAcmd();
    acmd = false;

#ifdef DUMP_COMMAND_STREAM
    cerr << "received CMD" << (int)lastCmd << endl << flush;
#endif

    if (IsMMC() && (lastCmd==17 || lastCmd==18 || lastCmd==24 || lastCmd==25) && (Param() & 511)) {
        PrepareR1(0x20 | (cardState==CardState::idle ? ERR_CARD_IDLE : 0));
        return;
    }
    switch (lastCmd) {
        // Reset
        case 0:
            cardState = CardState::idle;
            acmd = false;
            cmd12Countdown = 0;
            PrepareR1(ERR_CARD_IDLE);

            return;

        // Initialize (MMC)
        case 1:
            PrepareR1(0x00);
            cardState = CardState::initialized;

            return;

        // SDIO stuff. We do not support this.
        case 5:
            PrepareR1(ERR_ILLEGAL_COMMAND);

            return;

        // Read CSD
        case 9:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else {
                DoReadCSD();
            }

            return;

        // Read CID
        case 10:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else {
                DoReadCID();
            }

            return;

        // Read status register
        case 13:
            BufferStart(0);
            if (cardState == CardState::idle) {
                BufferAdd(0xff, ERR_CARD_IDLE, 0);
            } else {
                BufferAdd(0xff, 0, 0);
            }

            return;

        // Set block length. We only support 512 bytes.
        case 16:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else if (Param() != 512) {
                PrepareR1(ERR_PARAMETER);
            } else {
                PrepareR1(0x00);
            }

            return;

        // Single block read
        case 17:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else {
                DoReadSingleBlock();
            }

            return;

        // Multiblock read
        case 18:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else if (Param() >> 9 >= Image()->BlocksTotal()) {
                PrepareR1(ERR_PARAMETER);
            } else {
                cardState = CardState::multiblockRead;
                cmd12Countdown = 0;
                currentBlock = Param() >> 9;
                PrepareR1(0);
            }

            return;

        // Single block write
        case 24:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else if (Param() >> 9 >= Image()->BlocksTotal()) {
                PrepareR1(ERR_PARAMETER);
            } else {
                currentBlock = Param() >> 9;

                PrepareR1(0x00);
                cardState = CardState::writeTransaction;
            }

            return;

        // Multiblock write
        case 25:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else if (Param() >> 9 >= Image()->BlocksTotal()) {
                PrepareR1(ERR_PARAMETER);
            } else {
                cardState = CardState::multiblockWrite;
                currentBlock = Param() >> 9;
                PrepareR1(0);
            }

            return;

        // Escape to ACMD
        case 55:
            if (IsMMC()) { PrepareR1(ERR_ILLEGAL_COMMAND | (cardState == CardState::idle ? ERR_CARD_IDLE : 0)); return; }
            PrepareR1(0x00);
            acmd = true;

            return;

        // Read OCR
        case 58: {
            BufferStart(0);
            BufferAdd(0xff, IsMMC() && cardState == CardState::idle ? 1 : 0,
                      IsMMC() && cardState != CardState::idle ? 0x80 : 0, 0xff, 0x80, 0x00);

            return;
        }

        default:
            cerr << "unsupported SD CMD" << (int)lastCmd << endl << flush;
            PrepareR1(ERR_ILLEGAL_COMMAND | (IsMMC() && cardState == CardState::idle ? ERR_CARD_IDLE : 0));

            return;
    }
}

void EmSPISlaveSD::DoAcmd() {
#ifdef DUMP_COMMAND_STREAM
    cerr << "received ACMD" << (int)lastCmd << endl << flush;
#endif

    acmd = false;

    switch (lastCmd) {
        case 13:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else {
                DoReadSDStatus();
            }

            return;

        // Set preerease block count. The spec flags the content of blocks
        // that have been marked for erasure but not subsequently written as
        // undefined, so we just ignore this command.
        case 23:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else {
                PrepareR1(0x00);
            }

            return;

        // Init (SD style)
        case 41:
            PrepareR1(0x00);
            cardState = CardState::initialized;

            return;

        // Read SCR
        case 51:
            if (cardState == CardState::idle) {
                PrepareR1(ERR_CARD_IDLE);
            } else {
                DoReadSCR();
            }

            return;

        default:
            cerr << "unsupported SD ACMD" << (int)lastCmd << endl << flush;
            PrepareR1(ERR_ILLEGAL_COMMAND);

            return;
    }
}

void EmSPISlaveSD::DoReadSingleBlock() {
    uint8 block[512];

    BufferStart(0);
    if (Image()->Read(block, Param() >> 9, 1) != 1) {
        PrepareR1(ERR_PARAMETER);
        return;
    }

    PrepareR1(0x00);
    BufferAdd(0xff);

    BufferAddBlock(DATA_TOKEN_DEFAULT, block, 512);
}

void EmSPISlaveSD::DoReadMulitblock() {
    uint8 block[512];

    BufferStart(0);
    if (Image()->Read(block, currentBlock, 1) != 1) {
        BufferAdd(0x08);  // Data error token: out of range.
        cardState = CardState::initialized;
        return;
    }
    ++currentBlock;

    BufferAddBlock(DATA_TOKEN_DEFAULT, block, 512);
}

void EmSPISlaveSD::DoReadCSD() {
    // Block size is hardcoded to 512 bytes
    const uint32 blLen = 0x09, sectorSize = 0;
    uint32 cSizeMult, cSize;

    if (!legacyCardLayout(Image()->BlocksTotal(), cSizeMult, cSize)) {
        PrepareR1(ERR_PARAMETER);
        return;
    }

    uint8 csd[16]{};

    csd[0] = 0x00;
    csd[1] = 0x0e;                  // TAAC
    csd[2] = 0x00;                  // NSAC
    csd[3] = 0x5A;                  // TRAN_SPEED
    csd[4] = 0x5B;                  // CCC[4..11]
    csd[5] = blLen | 0x50;          // CCC[0..3], READ_BL_LEN
    csd[6] = (cSize >> 10) | 0x80;  // READ_BL_PARTIAL(1), WRITE_BLK_MISALIGN, READ_BLK_MISALIGN,
                                    // DSR_IMP, RESERVED, C_SIZE[10..11]
    csd[7] = cSize >> 2;            // C_SIZE[2..9]
    csd[8] = (cSize << 6) | 0x2D;   // C_SIZE[0..1], VDD_R_CURR_MIN (35mA), VDD_R_CURR_MAX(45mA)
    csd[9] =
        (cSizeMult >> 1) | 0xD8;  // VDD_W_CURR_MIN (60mA), VDD_W_CURR_MAX(80mA), C_SIZE_MULT[1..2]
    csd[10] = (sectorSize >> 1) |
              ((cSizeMult << 7) & 0xff);  // C_SIZE_MULT[0], ERASE_BLK_EN, SECTOR_SIZE[1..7]
    csd[11] = (sectorSize << 7) & 0xff;   // SECTOR_SIZE[0], WP_GRP_SIZE
    csd[12] = (blLen >> 2) | 8;     // WP_GRP_ENABLE, RESERVED, R2W_FACTOR(2), WRITE_BL_LEN[2..3]
    csd[13] = (blLen << 6) & 0xff;  // WRITE_BL_LEN[0..1], WRITE_BL_PARTIAL, RESERVED
    csd[14] = 0x00;  // FILE_FORMAT_GRP, COPY, PERM_WRITE_PROTECT, TMP_WRITE_PROTECT, FILE_FORMAT,
                     // RESERVED

    if (IsMMC()) {
        csd[0] = 0x8c; // CSD 1.2, MMC 3.3 (SanDisk manual 80-36-00320, table 3-10).
        csd[1] = 0x0f;
        csd[3] = 0x2a;
        csd[4] = 0x01; // Basic, block read and block write classes.
        csd[5] = 0x59;
        csd[6] &= 0x03; // Fixed aligned 512-byte blocks.
        csd[10] &= 0x80;
        csd[11] = 0;
        csd[14] = Image()->IsReadOnly() ? 0x10 : 0;
    }
    PrepareR1(0x00);
    BufferAdd(0xff);
    if (IsMMC()) csd[15] = (crc::sdCRC7(csd, 15) << 1) | 1;
    BufferAddBlock(DATA_TOKEN_DEFAULT, csd, IsMMC() ? 16 : 15);
}

void EmSPISlaveSD::DoReadCID() {
    uint8 cid[16];

    cid[0] = 'D';  // MID
    cid[1] = 't';  // OID
    cid[2] = 'y';
    cid[3] = 'C';  // PNM
    cid[4] = 'l';
    cid[5] = 'p';
    cid[6] = 'l';
    cid[7] = 't';
    cid[8] = 32;    // PRV
    const uint32 serial = Image()->Serial();
    cid[9] = serial >> 24;  // PSN
    cid[10] = serial >> 16;
    cid[11] = serial >> 8;
    cid[12] = serial;
    cid[13] = 9;   // MDT.month = sept
    cid[14] = 22;  // MDT.year = 2022
    cid[15] = crc::sdCRC7(cid, 15) << 1;

    if (IsMMC()) {
        // Virtual identity, six-byte MMC PNM and MMC date/serial positions (table 3-9).
        cid[0] = 0xff; cid[1] = 'F'; cid[2] = 'R';
        memcpy(cid + 3, "FRNDMC", 6);
        cid[9] = 0x10;
        cid[10] = serial >> 24; cid[11] = serial >> 16;
        cid[12] = serial >> 8; cid[13] = serial;
        cid[14] = 0x91; // September 1998, MMC year offset 1997.
        cid[15] = (crc::sdCRC7(cid, 15) << 1) | 1;
    }
    PrepareR1(0x00);
    BufferAdd(0xff);
    BufferAddBlock(DATA_TOKEN_DEFAULT, cid, sizeof(cid));
}

void EmSPISlaveSD::DoReadSCR() {
    uint8 scr[8];

    scr[0] = 0x01;  // SCR_STRUCTURE, SD_SPEC
    scr[1] = 0x25;  // DATA_STAT_AFTER_ERASE, SD_SECURITY, SD_BUS_WIDTHS
    scr[2] = 0x00;  // SD_SPEC3, EX_ SECURITY, SD_SPEC4, SD_SPECX[2..3]
    scr[3] = 0x03;  // SD_SPECX[0..1], RESERVED, CMD_SUPPORT (CMD23, CMD20)
    scr[4] = 0;     // rserved for manufacturer
    scr[5] = 0;     // rserved for manufacturer
    scr[6] = 0;     // rserved for manufacturer
    scr[7] = 0;     // rserved for manufacturer

    PrepareR1(0);
    BufferAdd(0xff);
    BufferAddBlock(DATA_TOKEN_DEFAULT, scr, 8);
}

void EmSPISlaveSD::DoReadSDStatus() {
    uint8 sdStatus[64];
    memset(sdStatus, 0, sizeof(sdStatus));

    BufferStart(0);
    BufferAdd(0xff, 0x00, 0x00, 0xff);
    BufferAddBlock(DATA_TOKEN_DEFAULT, sdStatus, 64);
}

void EmSPISlaveSD::FinishWriteSingleBlock() {
    BufferStart(0);

    if (Image()->Write(buffer, currentBlock) == 1) {
        BufferAdd(DATA_RESPONSE_ACCEPTED, 0xff);
    } else {
        BufferAdd(DATA_RESPONSE_WRITE_FAILED);
    }

    cardState = CardState::initialized;
    spiState = SpiState::txData;
}

void EmSPISlaveSD::FinishWriteMultiblock() {
    BufferStart(0);

    if (Image()->Write(buffer, currentBlock) == 1) {
        ++currentBlock;
        BufferAdd(DATA_RESPONSE_ACCEPTED, 0xff);
    } else {
        BufferAdd(DATA_RESPONSE_WRITE_FAILED);
    }

    spiState = SpiState::txData;
}

void EmSPISlaveSD::HandleCmd12(uint8 data) {
    if (cmd12Countdown > 0 && --cmd12Countdown == 0) {
        cardState = CardState::initialized;
        spiState = SpiState::txData;
        BufferStart(0);
        PrepareR1(0);

        return;
    }

    if (data == 0x4c) {
        cmd12Countdown = 6;
    }
}

uint32 EmSPISlaveSD::Param() const {
    return (uint32(buffer[0]) << 24) | (uint32(buffer[1]) << 16) | (uint32(buffer[2]) << 8) | buffer[3];
}

void EmSPISlaveSD::PrepareR1(uint8 flags) {
    BufferStart(0);
    BufferAdd(0xff, flags & 0x7f);
}

bool EmSPISlaveSD::IsSizeRepresentable(size_t blocksTotal) {
    uint32 cSizeMult, cSize;

    return legacyCardLayout(blocksTotal, cSizeMult, cSize);
}
