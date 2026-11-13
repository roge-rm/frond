#include "EmRegsUsbD12.h"

#include <algorithm>
#include <cstring>

#include "EmCommon.h"
#include "savestate/ChunkHelper.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

namespace {
    // v1 was Cloudpilot's 4-byte stub; v2 is the whole chip.
    constexpr uint32 SAVESTATE_VERSION = 2;

    // Commands (PDIUSBD12 datasheet, section 11). 0x40-0x45 is Read Last Transaction Status when
    // read and Set Endpoint Status when written; 0xF0 is Read Buffer or Write Buffer the same way.
    constexpr uint8 cmdSetAddress = 0xd0, cmdSetEndpointEnable = 0xd8, cmdSetMode = 0xf3, cmdSetDma = 0xfb,
                    cmdReadInterrupts = 0xf4, cmdBuffer = 0xf0, cmdAckSetup = 0xf1, cmdClearBuffer = 0xf2,
                    cmdValidateBuffer = 0xfa, cmdFrame = 0xf5, cmdChipId = 0xfd;

    // Last transaction status bits.
    constexpr uint8 ltsSuccess = 0x01, ltsSetup = 0x20, ltsData1 = 0x40, ltsOverwritten = 0x80;

    // Interrupt register bits above the six endpoint bits.
    constexpr uint16 intBusReset = 0x40, intSuspend = 0x80, intDmaEot = 0x100;
}

EmRegsUsbD12::EmRegsUsbD12(emuptr base) : base(base) {}

void EmRegsUsbD12::Initialize() {
    EmRegs::Initialize();
    Reset(true);
}

// The ROM resets the chip through PJ5 on every boot, so any reset gives power-on state.
void EmRegsUsbD12::Reset(Bool hardwareReset) {
    EmRegs::Reset(hardwareReset);
    const bool wasListening = Listening();
    ep = {};
    mode[0] = mode[1] = 0;
    dma = 0;
    address = pendingAddress = 0;
    addressEnabled = true;
    addressPending = endpointsEnabled = false;
    setupLocked[0] = setupLocked[1] = false;
    setupPacket = controlActive = false;
    interrupts = frame = 0;
    command = dataIndex = selected = 0;
    ListeningChanged(wasListening);
    UpdateLine();
}

void EmRegsUsbD12::SetSubBankHandlers() {
    EmRegs::SetSubBankHandlers();
    SetHandler((ReadFunction)&EmRegsUsbD12::Read, (WriteFunction)&EmRegsUsbD12::Write, base, 4);
}

uint8* EmRegsUsbD12::GetRealAddress(emuptr address) { return regs + (address - base); }

uint32 EmRegsUsbD12::Read(emuptr address, int size) {
    // Data at +0/+1; reading the command port gives nothing.
    if ((address - base) & 2) return size == 1 ? 0xff : 0xffff;
    const uint8 value = ReadData();
    return size == 1 ? value : (uint32(value) << 8) | value;
}

void EmRegsUsbD12::Write(emuptr address, int, uint32 value) {
    if ((address - base) & 2)
        Command(uint8(value));
    else
        WriteData(uint8(value));
    // A guest write may have freed or filled a buffer: the host retries a NAKed token soon.
    if (notify) notify(notifyContext);
}

void EmRegsUsbD12::Command(uint8 value) {
    command = value;
    dataIndex = 0;
    if (value < kEndpoints) {
        selected = value;
        return;
    }
    Endpoint& e = ep[selected];
    switch (value) {
        case cmdAckSetup:
            if (selected < 2) setupLocked[selected] = false;
            break;
        case cmdClearBuffer:
            // OUT endpoints only; a SETUP holds EP0 OUT until Acknowledge Setup.
            if (selected % 2 || (selected == 0 && setupLocked[0]) || !e.count) break;
            e.head = (e.head + 1) % Buffers(selected);
            --e.count;
            if (selected == 0) setupPacket = false;
            break;
        case cmdValidateBuffer:
            if (!(selected % 2) || (selected == 1 && setupLocked[1]) || Full(selected)) break;
            {
                const unsigned slot = (e.head + e.count) % Buffers(selected);
                memcpy(e.data[slot], e.stage, e.stageSize);
                e.size[slot] = e.stageSize;
                ++e.count;
                e.stageSize = 0;
            }
            break;
        default:
            break;
    }
}

uint8 EmRegsUsbD12::ReadData() {
    const uint8 index = dataIndex++;
    if (command < kEndpoints) {
        // Select Endpoint: bit 0 full (OUT: a packet waits; IN: no room), bit 1 stalled.
        const Endpoint& e = ep[command];
        const bool full = command % 2 ? Full(command) : e.count > 0;
        return (full ? 1 : 0) | (e.stalled ? 2 : 0);
    }
    if ((command & 0xc0) == 0x40 && (command & 0x3f) < kEndpoints) {
        // Read Last Transaction Status clears the endpoint's interrupt bit.
        const unsigned i = command & 0x3f;
        Endpoint& e = ep[i];
        const uint8 status = e.status;
        e.statusUnread = false;
        e.status &= ~ltsOverwritten;
        interrupts &= ~(1u << i);
        UpdateLine();
        return status;
    }
    if ((command & 0xc0) == 0x80 && (command & 0x3f) < kEndpoints) {
        // Read Endpoint Status: setup, buffer 0 and 1 full, data toggle, stalled.
        const unsigned i = command & 0x3f;
        const Endpoint& e = ep[i];
        return (i == 0 && setupPacket && e.count ? 0x04 : 0) | (e.count >= 1 ? 0x08 : 0) |
               (e.count >= 2 ? 0x10 : 0) | (e.toggle ? 0x20 : 0) | (e.stalled ? 0x80 : 0);
    }
    switch (command) {
        case cmdReadInterrupts:
            // Reading clears bus reset, suspend change and DMA end of transfer; the endpoint bits
            // wait for their transaction status to be read.
            if (index == 0) {
                const uint8 low = uint8(interrupts);
                interrupts &= ~(intBusReset | intSuspend);
                UpdateLine();
                return low;
            }
            if (index == 1) {
                const uint8 high = uint8(interrupts >> 8);
                interrupts &= ~intDmaEot;
                UpdateLine();
                return high;
            }
            return 0;
        case cmdBuffer: {
            // Read Buffer: a reserved byte, the length, then the bytes of the oldest packet.
            const Endpoint& e = ep[selected];
            if (selected % 2 || !e.count) return 0;
            const unsigned slot = e.head;
            if (index == 0) return 0;
            if (index == 1) return e.size[slot];
            return unsigned(index - 2) < e.size[slot] ? e.data[slot][index - 2] : 0;
        }
        case cmdFrame:
            return index == 0 ? uint8(frame) : index == 1 ? uint8(frame >> 8) : 0;
        case cmdSetDma:
            return dma;
        case cmdChipId:
            return index == 0 ? 0x12 : index == 1 ? 0x10 : 0;
        default:
            return 0;
    }
}

void EmRegsUsbD12::WriteData(uint8 value) {
    const uint8 index = dataIndex++;
    if ((command & 0xc0) == 0x40 && (command & 0x3f) < kEndpoints) {
        // Set Endpoint Status: bit 0 stalls; any write starts the endpoint again at DATA0.
        Endpoint& e = ep[command & 0x3f];
        e.stalled = value & 1;
        e.toggle = false;
        return;
    }
    switch (command) {
        case cmdSetAddress:
            if (index == 0) {
                // SET_ADDRESS answers its status stage at the old address: the new one waits for it.
                if (controlActive) {
                    pendingAddress = value;
                    addressPending = true;
                } else {
                    ApplyAddress(value);
                }
            }
            break;
        case cmdSetEndpointEnable:
            if (index == 0) endpointsEnabled = value & 1;
            break;
        case cmdSetMode:
            if (index < 2) {
                const bool wasListening = Listening();
                mode[index] = value;
                ListeningChanged(wasListening);
            }
            break;
        case cmdSetDma:
            if (index == 0) dma = value;
            break;
        case cmdBuffer: {
            // Write Buffer: a reserved byte, the length, then the bytes, into an IN endpoint.
            if (!(selected % 2)) break;
            Endpoint& e = ep[selected];
            if (index == 1) e.stageSize = std::min<unsigned>(value, Capacity(selected));
            if (index >= 2 && unsigned(index - 2) < Capacity(selected)) e.stage[index - 2] = value;
            break;
        }
        default:
            break;
    }
}

void EmRegsUsbD12::ApplyAddress(uint8 value) {
    address = value & 0x7f;
    addressEnabled = value & 0x80;
}

void EmRegsUsbD12::Complete(unsigned index, bool setup) {
    Endpoint& e = ep[index];
    const bool overwritten = e.statusUnread;
    e.status = ltsSuccess | (setup ? ltsSetup : 0) | (e.toggle ? ltsData1 : 0) | (overwritten ? ltsOverwritten : 0);
    e.statusUnread = true;
    e.toggle = !e.toggle;
    Interrupt(1u << index);
}

void EmRegsUsbD12::Interrupt(uint16 bits) {
    interrupts |= bits;
    UpdateLine();
}

void EmRegsUsbD12::UpdateLine() {
    const bool asserted = interrupts != 0;
    if (asserted == line) return;
    line = asserted;
    if (onInterrupt) onInterrupt(line);
}

void EmRegsUsbD12::ListeningChanged(bool wasListening) {
    if (!wasListening && Listening()) ++enableCount;
}

void EmRegsUsbD12::SetCable(bool attached) {
    const bool wasListening = Listening();
    cable = attached;
    if (onCable) onCable(attached);
    ListeningChanged(wasListening);
}

void EmRegsUsbD12::BusReset() {
    for (auto& e : ep) {
        e.head = e.count = e.stageSize = 0;
        e.stalled = e.toggle = e.statusUnread = false;
        e.status = 0;
    }
    setupLocked[0] = setupLocked[1] = false;
    setupPacket = controlActive = addressPending = false;
    address = 0;
    addressEnabled = true;
    interrupts = (interrupts & (intSuspend | intDmaEot)) | intBusReset;
    UpdateLine();
}

bool EmRegsUsbD12::HostReset() {
    if (!Listening()) return false;
    BusReset();
    return true;
}

UsbPacketResult EmRegsUsbD12::HostSetup(const uint8* data) {
    if (!Listening()) return R::Timeout;
    // A SETUP is always taken: it replaces EP0 OUT, flushes EP0 IN, clears a protocol stall and
    // holds both until Acknowledge Setup.
    Endpoint& out = ep[0];
    memcpy(out.data[0], data, 8);
    out.size[0] = 8;
    out.head = 0;
    out.count = 1;
    out.toggle = false;
    out.stalled = ep[1].stalled = false;
    ep[1].count = ep[1].head = 0;
    ep[1].toggle = true;
    setupLocked[0] = setupLocked[1] = true;
    setupPacket = controlActive = true;
    Complete(0, true);
    return R::Ack;
}

UsbPacketResult EmRegsUsbD12::HostOut(unsigned index, const uint8* data, unsigned size) {
    if (!Listening()) return R::Timeout;
    if (index >= 2 && !endpointsEnabled) return R::Timeout;
    if (size > Capacity(index)) return R::Unsupported;
    Endpoint& e = ep[index];
    if (e.stalled) return R::Stall;
    if ((index == 0 && setupLocked[0]) || Full(index)) return R::Nak;
    const unsigned slot = (e.head + e.count) % Buffers(index);
    if (size) memcpy(e.data[slot], data, size);
    e.size[slot] = size;
    ++e.count;
    if (index == 0) setupPacket = false;
    Complete(index, false);
    return R::Ack;
}

UsbPacketResult EmRegsUsbD12::HostIn(unsigned index, uint8* data, unsigned* size, bool take) {
    if (!Listening()) return R::Timeout;
    if (index >= 2 && !endpointsEnabled) return R::Timeout;
    Endpoint& e = ep[index];
    if (e.stalled) return R::Stall;
    if ((index == 1 && setupLocked[1]) || !e.count) return R::Nak;
    const unsigned slot = e.head;
    *size = e.size[slot];
    if (!take) return R::Ack;
    if (*size) memcpy(data, e.data[slot], *size);
    e.head = (e.head + 1) % Buffers(index);
    --e.count;
    Complete(index, false);
    if (index == 1) {
        controlActive = false;
        if (addressPending) {
            addressPending = false;
            ApplyAddress(pendingAddress);
        }
    }
    return R::Ack;
}

UsbDevicePort EmRegsUsbD12::Port() {
    // USB endpoint number to D12 index: OUT 0/1/2 -> 0/2/4, IN 0/1/2 -> 1/3/5.
    static const UsbDevicePort::Operations ops{
        [](void* p) { return Self(p).HostReset(); },
        [](void* p) {
            Self(p).frame = (Self(p).frame + 1) & 0x7ff;
            return false;
        },
        [](void* p) { return Self(p).Listening(); },
        [](void* p) { return Self(p).Listening(); },
        [](void*) { return false; },
        [](void* p) { return Self(p).enableCount; },
        [](void* p) { return uint8(Self(p).addressEnabled ? Self(p).address : 0xff); },
        [](void* p, void (*notify)(void*), void* context) {
            Self(p).notify = notify;
            Self(p).notifyContext = context;
        },
        [](void* p, const uint8_t* data) { return Self(p).HostSetup(data); },
        [](void* p, const uint8_t* data, unsigned size) { return Self(p).HostOut(0, data, size); },
        [](void* p, uint8_t* data, unsigned* size) { return Self(p).HostIn(1, data, size, true); },
        [](void* p, uint8_t endpoint, const uint8_t* data, unsigned size) {
            endpoint &= 0x7f;
            return endpoint > 2 ? R::Timeout : Self(p).HostOut(endpoint * 2, data, size);
        },
        [](void* p, uint8_t endpoint, uint8_t* data, unsigned* size) {
            endpoint &= 0x7f;
            return endpoint > 2 ? R::Timeout : Self(p).HostIn(endpoint * 2 + 1, data, size, true);
        },
        [](void* p, uint8_t endpoint, unsigned* size) {
            endpoint &= 0x7f;
            return endpoint > 2 ? R::Timeout : Self(p).HostIn(endpoint * 2 + 1, nullptr, size, false);
        },
        [](void* p, uint8_t endpoint, unsigned) {
            endpoint &= 0x7f;
            return endpoint > 2 || !Self(p).Full(endpoint * 2);
        }};
    return {this, ops};
}

void EmRegsUsbD12::Save(Savestate<ChunkType>& savestate) { DoSave(savestate); }
void EmRegsUsbD12::Save(SavestateProbe<ChunkType>& savestate) { DoSave(savestate); }

void EmRegsUsbD12::Load(SavestateLoader<ChunkType>& loader) {
    if (!loader.HasChunk(ChunkType::regsUsbClie)) return;
    Chunk* chunk = loader.GetChunk(ChunkType::regsUsbClie);
    if (!chunk) {
        logPrintf("unable to restore EmRegsUsbD12: missing savestate\n");
        loader.NotifyError();
        return;
    }
    const uint32 version = chunk->Get32();
    if (version > SAVESTATE_VERSION) {
        logPrintf("unable to restore EmRegsUsbD12: unsupported savestate version\n");
        loader.NotifyError();
        return;
    }
    const bool wasListening = Listening();
    if (version < 2) {
        // The stub kept no chip state. Take the state the ROM's D12_Initialize leaves at boot.
        Reset(false);
        mode[0] = 0x10;
        mode[1] = 0x40;
        dma = 0xc0;
    } else {
        LoadChunkHelper helper(*chunk);
        DoSaveLoad(helper);
        for (unsigned i = 0; i < kEndpoints; ++i) {
            Endpoint& e = ep[i];
            e.head %= Buffers(i);
            e.count = std::min<unsigned>(e.count, Buffers(i));
            e.stageSize = std::min<unsigned>(e.stageSize, Capacity(i));
            for (auto& s : e.size) s = std::min<unsigned>(s, Capacity(i));
        }
        if (selected >= kEndpoints) selected = 0;
    }
    ListeningChanged(wasListening);
    // The VZ registers load first; tell them the pin again.
    line = interrupts != 0;
    if (onInterrupt) onInterrupt(line);
}

template <typename T>
void EmRegsUsbD12::DoSave(T& savestate) {
    typename T::chunkT* chunk = savestate.GetChunk(ChunkType::regsUsbClie);
    if (!chunk) return;
    chunk->Put32(SAVESTATE_VERSION);
    SaveChunkHelper helper(*chunk);
    DoSaveLoad(helper);
}

template <typename T>
void EmRegsUsbD12::DoSaveLoad(T& helper) {
    for (auto& e : ep) {
        helper.DoBuffer(e.data, sizeof(e.data))
            .DoBuffer(e.size, sizeof(e.size))
            .Do8(e.head)
            .Do8(e.count)
            .DoBuffer(e.stage, sizeof(e.stage))
            .Do8(e.stageSize)
            .Do8(e.status)
            .DoBool(e.statusUnread)
            .DoBool(e.stalled)
            .DoBool(e.toggle);
    }
    helper.DoBuffer(mode, sizeof(mode))
        .Do8(dma)
        .Do8(address)
        .Do8(pendingAddress)
        .DoBool(addressEnabled)
        .DoBool(addressPending)
        .DoBool(endpointsEnabled)
        .DoBool(setupLocked[0])
        .DoBool(setupLocked[1])
        .DoBool(setupPacket)
        .DoBool(controlActive)
        .Do16(interrupts)
        .Do16(frame)
        .Do8(command)
        .Do8(dataIndex)
        .Do8(selected);
}
