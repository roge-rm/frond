#ifndef FROND_IR_KEYBOARD_H
#define FROND_IR_KEYBOARD_H
#include <array>
#include <cstdint>
#include "savestate/savestateAll.h"

// Palm Universal Wireless Keyboard (3169WW): 9600 baud, 8N2 infrared packets.
// Model the bytes at the UART boundary; no guest event injection.
class IrKeyboard {
 public:
    static constexpr uint32_t capacity = 512;
    bool Attached() const { return attached; }
    bool Pending() const { return count != 0; }
    void Attach(bool value) { if (!value) ReleaseAll(); attached=value; }
    bool Key(uint8_t code, bool down) {
        if (!code || code>=128 || (down && !attached) || held[code]==down) return false;
        // Reserve a break-code slot for every held key. Overflow can reject a
        // new make code, but must never lose a release for an accepted press.
        if (down && count+HeldCount()+2>capacity) return false;
        if (count>=capacity) return false;
        queue[(head+count++)%capacity]=code | (down?0:0x80);
        held[code]=down;
        return true;
    }
    void ReleaseAll() { for (uint32_t i=1;i<128;i++) if (held[i]) Key(i,false); }
    void Reset() { head=count=byte=0; nextByteAt=0; queue={}; held={}; }
    uint16_t Read(uint64_t now) {
        if (!count || now<nextByteAt) return 0x100; // UART_CHAR_NONE
        const auto code=queue[head];
        const uint8_t packet[]={0xff,0xc0,code,uint8_t(code^0xff),uint8_t(code^0x67),0xc1};
        const auto value=packet[byte++];
        nextByteAt=now+1145834; // ceil(11 bits / 9600 baud) in guest nanoseconds
        if (byte==6) { byte=0; queue[head]=0; head=(head+1)%capacity; --count; }
        return value;
    }
    template<class S> void Save(S& state) {
        auto* chunk=state.GetChunk(ChunkType::irKeyboard,0);
        SaveChunkHelper helper(*chunk); Transfer(helper);
    }
    void Load(SavestateLoader<ChunkType>& loader) {
        attached=false; Reset();
        if (!loader.HasChunk(ChunkType::irKeyboard)) return;
        auto* chunk=loader.GetChunkOrFail(ChunkType::irKeyboard,0,"infrared keyboard");
        if (!chunk) return;
        LoadChunkHelper helper(*chunk); Transfer(helper);
        bool valid=head<capacity && count<=capacity && byte<6 && (count || !byte);
        for (auto value:held) valid &= value<=1;
        valid &= count+HeldCount()<=capacity;
        if (!valid) { attached=false; Reset(); loader.NotifyError(); }
    }
 private:
    uint32_t head{},count{},byte{};
    bool attached{};
    uint64_t nextByteAt{};
    std::array<uint8_t,capacity> queue{};
    std::array<uint8_t,128> held{};
    uint32_t HeldCount() const { uint32_t n=0; for (auto v:held) n+=bool(v); return n; }
    template<class H> void Transfer(H& h) {
        h.DoBool(attached).Do32(head).Do32(count).Do32(byte).Do64(nextByteAt)
          .DoBuffer(queue.data(),queue.size()).DoBuffer(held.data(),held.size());
    }
};
#endif
