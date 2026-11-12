#ifndef FROND_IR_SIR_H
#define FROND_IR_SIR_H

// SIR framing for IrLAP (IrDA Physical Layer 1.4, section 5): extra BOFs (0xff), BOF (0xc0),
// the frame with 0xc0, 0xc1 and 0x7d escaped as 0x7d then the byte xor 0x20, the FCS-16 low
// byte first, then EOF (0xc1).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace frondIr {

using Bytes = std::vector<uint8_t>;

// CRC-CCITT as IrLAP uses it, starting at 0xffff. A frame with its FCS gives fcsGood.
uint16_t Fcs16(const uint8_t* data, size_t size, uint16_t fcs = 0xffff);
constexpr uint16_t fcsGood = 0xf0b8;

// Appends one wrapped frame (address, control and information, no FCS) to out.
void SirWrap(const uint8_t* frame, size_t size, unsigned extraBofs, std::vector<uint8_t>& out);
inline void SirWrap(const Bytes& frame, unsigned extraBofs, std::vector<uint8_t>& out) {
    SirWrap(frame.data(), frame.size(), extraBofs, out);
}

// Takes raw SIR bytes and gives whole frames with a good FCS, without the FCS.
class SirUnwrapper {
   public:
    // The largest frame taken: 2048 bytes of information, address, control and FCS.
    static constexpr size_t maxFrame = 2048 + 4;

    // True when byte ended a good frame, which is then in Frame().
    bool Feed(uint8_t byte);
    const Bytes& Frame() const { return done; }
    // Frames dropped for a bad FCS, a short frame or one too long.
    unsigned BadFrames() const { return bad; }
    void Reset();

   private:
    bool inFrame{false}, escaped{false};
    Bytes frame, done;
    unsigned bad{0};
};

}  // namespace frondIr

#endif
