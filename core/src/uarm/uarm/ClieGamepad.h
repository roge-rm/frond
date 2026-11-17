#ifndef FROND_CLIE_GAMEPAD_H
#define FROND_CLIE_GAMEPAD_H
#include <algorithm>
#include <array>
#include <cstdint>
#include "keys.h"
#include "savestate/savestateAll.h"

// Sony PEGA-GC10 game controller on the TG50's interface connector, at the UART boundary.
// Plug-in: the pad pulls the connector's HotSync line (GPIO3) low, SonySys opens the cradle
// port and waits for 55 01 04 + 5 bytes, answers AA + the same 7 bytes, and the ROM's
// GameControllerLib then reads one byte per button change at 9600 8N1: 0x80 | code for a press,
// code for a release. The pad never gets data from the CLIÉ except that reply.
// The SoC owns the wiring (GPIO3, AUX, the FFUART hooks); this holds the pad's own state.
class ClieGamepad {
 public:
    enum class Phase : uint8_t { idle, pulling, identifying, identified };
    static constexpr uint32_t capacity = 64;
    static constexpr uint64_t byteNs = 1041667;            // 10 bits at 9600 baud, in guest ns
    static constexpr uint64_t pullNs = 300000000;          // HotSync line held low
    static constexpr uint64_t idEveryNs = 300000000;       // gaps longer than SonySys's 100-tick flush
    // After the line goes back up. SonySys answers within about 30 ms of an ID it waits for.
    static constexpr uint64_t idGiveUpNs = 1500000000ull;
    static constexpr uint64_t idleDropNs = 25000000000ull; // GameControllerLib drops the pad after 30 s quiet
    // A byte waiting this long on a port that isn't the connector's (IrDA took the UART on an app
    // switch, or it's closed) re-identifies, which has SonySys switch the line back.
    static constexpr uint64_t portWaitNs = 200000000;
    static constexpr uint64_t pollNs = 10000000;
    // Bytes 3-7 aren't read by any driver; the real pad's are unknown.
    static constexpr uint8_t id[8] = {0x55, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t noId = 8;

    // Up, Down, Left, Right, A, B, X, Y, Home, Menu, in KeyId order from keyIdPadUp.
    static int Index(KeyId key) { return key >= keyIdPadUp && key < keyIdPadHold ? int(key - keyIdPadUp) : -1; }
    static uint8_t Code(int index) {
        static constexpr uint8_t codes[10] = {0x7D, 0x7E, 0x60, 0x7B, 0x5F, 0x7C, 0x5E, 0x5D, 0x5B, 0x5C};
        return codes[index];
    }

    bool Attached() const { return attached; }
    bool LineLow() const { return attached && phase == Phase::pulling; }
    Phase GetPhase() const { return phase; }
    bool HoldOn() const { return hold; }
    uint32_t IdsSent() const { return ids; }
    uint32_t Replies() const { return replies; }
    uint32_t BytesSent() const { return sent; }

    void Attach(uint64_t now, bool identify) {
        Clear();
        attached = true;
        if (identify) StartIdentify(now);
    }
    void Detach() { Clear(); }

    // A pad button or the Hold slide. Hold toggles on each press, like a slide. True when there's
    // something new for the UART or the line.
    bool Key(KeyId key, bool down, uint64_t now) {
        if (!attached) return false;
        if (key == keyIdPadHold) {
            if (!down) return false;
            hold = !hold;
            if (hold) {
                for (int i = 0; i < 10; ++i)
                    if (sentDown & (1u << i)) Queue(Code(i));
                sentDown = 0;
            }
            return true;
        }
        const int index = Index(key);
        if (index < 0) return false;
        const uint16_t bit = uint16_t(1u << index);
        if (down) {
            physical |= bit;
            if (hold || (sentDown & bit)) return false;
            if (phase == Phase::idle) { StartIdentify(now); return true; }
            if (!Queue(uint8_t(0x80 | Code(index)))) return false;
            sentDown |= bit;
            return true;
        }
        physical &= uint16_t(~bit);
        if (!(sentDown & bit) || !Queue(Code(index))) return false;
        sentDown &= uint16_t(~bit);
        return true;
    }

    // The CLIÉ slept or reset: its driver has forgotten the pad.
    void Forget() {
        phase = Phase::idle;
        head = count = 0;
        sentDown = 0;
        idPos = noId;
        replyPos = 0;
        portWaitSince = 0;
    }

    void Tick(uint64_t now, bool portReady) {
        if (!attached) return;
        switch (phase) {
            case Phase::idle: break;
            case Phase::pulling:
                if (now >= pullUntil) {
                    phase = Phase::identifying;
                    nextIdAt = now;
                    idDeadline = now + idGiveUpNs;
                }
                break;
            case Phase::identifying:
                // No answer while the connector's port is open: SonySys couldn't open it, as the
                // ROM's driver still holds it from before (a quick unplug and plug in, which
                // doesn't reach SonySys here). That driver still reads the pad.
                if (now >= idDeadline && portReady && idPos == noId) {
                    phase = Phase::identified;
                    lastSentAt = now;
                    portWaitSince = 0;
                } else if (now >= idDeadline) Forget();
                else if (portReady && idPos == noId && now >= nextIdAt) {
                    idPos = 0;
                    nextIdAt = now + idEveryNs;
                    ++ids;
                }
                break;
            case Phase::identified:
                if (count && !portReady) {
                    if (!portWaitSince) portWaitSince = now;
                    else if (now - portWaitSince >= portWaitNs) StartIdentify(now);
                } else portWaitSince = 0;
                if (phase == Phase::identified && !count && now - lastSentAt >= idleDropNs) Forget();
                break;
        }
    }
    // When Tick next has something to do; UINT64_MAX for never.
    uint64_t Wake(uint64_t now) const {
        if (!attached) return UINT64_MAX;
        switch (phase) {
            case Phase::idle: return UINT64_MAX;
            case Phase::pulling: return pullUntil;
            case Phase::identifying:
                return idPos != noId || nextIdAt <= now ? std::min(idDeadline, now + pollNs) : std::min(idDeadline, nextIdAt);
            case Phase::identified: return count ? now + pollNs : lastSentAt + idleDropNs;
        }
        return UINT64_MAX;
    }

    bool Pending() const { return attached && (idPos != noId || (phase == Phase::identified && count)); }
    uint16_t Read(uint64_t now) {
        if (!attached || now < nextByteAt) return 0x100;  // UART_CHAR_NONE
        uint8_t value;
        if (idPos != noId) {
            value = id[idPos++];
            if (idPos == sizeof(id)) idPos = noId;
        } else if (phase == Phase::identified && count) {
            value = queue[head];
            head = (head + 1) % capacity;
            --count;
            lastSentAt = now;
        } else return 0x100;
        ++sent;
        nextByteAt = now + byteNs;
        return value;
    }
    // A byte from the CLIÉ. True when the reply to the ID completed it.
    bool Write(uint8_t value, uint64_t now) {
        if (!attached) return false;
        if (replyPos && value == id[replyPos]) ++replyPos;
        else replyPos = value == 0xAA ? 1 : 0;
        if (replyPos < sizeof(id)) return false;
        replyPos = 0;
        ++replies;
        if (phase != Phase::identifying) return false;
        phase = Phase::identified;
        idPos = noId;
        lastSentAt = now;
        portWaitSince = 0;
        return true;
    }

    // Saved only while attached, so sessions without the pad stay as they were.
    template <class S> void Save(S& state) {
        if (!attached) return;
        auto* chunk = state.GetChunk(ChunkType::sonyTg50Gamepad, version);
        if (!chunk) return;
        SaveChunkHelper helper(*chunk);
        Transfer(helper);
    }
    // Without the chunk (older sessions, the pad not attached) it loads as detached.
    void Load(SavestateLoader<ChunkType>& loader) {
        Clear();
        if (!loader.HasChunk(ChunkType::sonyTg50Gamepad)) return;
        auto* chunk = loader.GetChunkOrFail(ChunkType::sonyTg50Gamepad, version, "CLIÉ game controller");
        if (!chunk) return;
        LoadChunkHelper helper(*chunk);
        Transfer(helper);
        const bool valid = attached && uint8_t(phase) <= uint8_t(Phase::identified) && head < capacity &&
                           count <= capacity && (idPos <= noId) && replyPos < sizeof(id) &&
                           physical < 0x400 && sentDown < 0x400;
        if (!valid) { Clear(); loader.NotifyError(); }
    }

 private:
    static constexpr uint32_t version = 1;
    bool attached{}, hold{};
    Phase phase{Phase::idle};
    uint16_t physical{}, sentDown{};
    uint32_t head{}, count{};
    std::array<uint8_t, capacity> queue{};
    uint8_t idPos{noId}, replyPos{};
    uint64_t nextByteAt{}, pullUntil{}, nextIdAt{}, idDeadline{}, lastSentAt{}, portWaitSince{};
    uint32_t ids{}, replies{}, sent{};

    void Clear() {
        attached = hold = false;
        phase = Phase::idle;
        physical = sentDown = 0;
        head = count = 0;
        queue = {};
        idPos = noId;
        replyPos = 0;
        nextByteAt = pullUntil = nextIdAt = idDeadline = lastSentAt = portWaitSince = 0;
        ids = replies = sent = 0;
    }
    bool Queue(uint8_t value) {
        if (count >= capacity) return false;
        queue[(head + count++) % capacity] = value;
        return true;
    }
    static int IndexOfCode(uint8_t code) {
        for (int i = 0; i < 10; ++i)
            if (Code(i) == (code & 0x7f)) return i;
        return -1;
    }
    // The line goes low; what's still queued and the presses of buttons still held go out once
    // identified. The first byte after an ID must be a press, or the driver drops the pad, so a
    // release whose press went out before is dropped.
    void StartIdentify(uint64_t now) {
        std::array<uint8_t, capacity> old{};
        const uint32_t oldCount = count;
        for (uint32_t i = 0; i < count; ++i) old[i] = queue[(head + i) % capacity];
        Forget();
        phase = Phase::pulling;
        pullUntil = now + pullNs;
        if (hold) return;
        uint16_t queuedDown = 0;
        for (int i = 0; i < 10; ++i) {
            const uint16_t bit = uint16_t(1u << i);
            bool pressedLater = false;
            for (uint32_t j = 0; j < oldCount; ++j) pressedLater |= old[j] == (0x80 | Code(i));
            if ((physical & bit) && !pressedLater) { Queue(uint8_t(0x80 | Code(i))); queuedDown |= bit; }
        }
        for (uint32_t j = 0; j < oldCount; ++j) {
            const int index = IndexOfCode(old[j]);
            if (index < 0) continue;
            const uint16_t bit = uint16_t(1u << index);
            if (old[j] & 0x80) { if (!(queuedDown & bit)) { Queue(old[j]); queuedDown |= bit; } }
            else if (queuedDown & bit) { Queue(old[j]); queuedDown &= uint16_t(~bit); }
        }
        sentDown = queuedDown;
    }
    template <class H> void Transfer(H& h) {
        uint8_t phaseValue = uint8_t(phase);
        h.DoBool(attached).DoBool(hold).Do8(phaseValue).Do16(physical).Do16(sentDown).Do32(head).Do32(count)
            .DoBuffer(queue.data(), queue.size()).Do8(idPos).Do8(replyPos)
            .Do64(nextByteAt).Do64(pullUntil).Do64(nextIdAt).Do64(idDeadline).Do64(lastSentAt).Do64(portWaitSince)
            .Do32(ids).Do32(replies).Do32(sent);
        phase = Phase(phaseValue);
    }
};
#endif
