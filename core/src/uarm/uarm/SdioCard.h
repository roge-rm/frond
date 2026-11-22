#ifndef FROND_SDIO_CARD_H
#define FROND_SDIO_CARD_H

#include <array>

#include "vSD.h"

// The card model supplies the read-only FBR identity and CIS through function 0,
// and implements the registers and FIFOs of its I/O functions.
class SdioFunctions {
   public:
    virtual bool Read(unsigned function, uint32_t address, uint8_t& value) = 0;
    virtual bool Write(unsigned function, uint32_t address, uint8_t value) = 0;
    virtual void Reset() = 0;

   protected:
    ~SdioFunctions() = default;
};

// Native SD bus, I/O-only card. The owner supplies the functions and their
// lifetime, powers the card on insertion and reports changes to its IRQ sources.
class SdioCard {
   public:
    SdioCard(SdioFunctions& functions, unsigned count, uint32_t cisAddress);
    void Reset();
    void SetPending(uint8_t functions);
    bool IrqAsserted() const;
    SdReplyType Command(uint8_t command, uint32_t arg, uint8_t* reply);
    bool AcceptsTransfer(bool write, uint16_t length, uint16_t blocks) const;
    SdDataReplyType ReadBlock(void* data, uint32_t length);
    SdDataReplyType WriteBlock(const void* data, uint32_t length);
    void AbortTransfer();

   private:
    bool Read(unsigned function, uint32_t address, uint8_t& value);
    bool Write(unsigned function, uint32_t address, uint8_t value);
    uint32_t Direct(uint32_t arg);
    uint32_t Extended(uint32_t arg);

    SdioFunctions& functions;
    const unsigned count;
    const uint32_t cisAddress;
    const uint8_t functionMask;
    uint8_t enabled = 0, interruptEnable = 0, pending = 0, busWidth = 0;
    std::array<uint16_t, 8> blockSize{};
    bool ready = false, addressed = false, selected = false;
    struct Transfer {
        uint32_t address;
        uint16_t length, blocks;
        unsigned function;
        bool write, increment;
    } transfer{};
};

#endif
