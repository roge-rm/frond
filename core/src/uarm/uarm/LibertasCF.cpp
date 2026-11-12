#include "LibertasCF.h"
#ifndef __EMSCRIPTEN__

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cputil.h"
namespace {
uint16_t le16(const std::vector<uint8_t>& b, size_t at) {
    return at + 1 < b.size() ? uint16_t(b[at]) | uint16_t(b[at + 1]) << 8 : 0;
}
void put16(std::vector<uint8_t>& b, size_t at, uint16_t value) {
    if (at + 1 >= b.size()) b.resize(at + 2);
    b[at] = value;
    b[at + 1] = value >> 8;
}
const uint8_t mac[] = {0x02, 0x46, 0x72, 0x6f, 0x6e, 0x64};
}  // namespace
// Palm's bus library maps CompactFlash I/O/attribute space here and uses
// GPIO94/108 for power, GPIO106 for reset, and active-low GPIO116 for IRQ.
LibertasCF::LibertasCF(ArmMem* memory, PxaGpio* pins, Reschedule scheduler, GetEmuTime emuClock,
                       NetworkSimulation* mode)
    : network(mode),
      gpio(pins),
      schedule(scheduler),
      clock(emuClock),
      trace(getenv("FROND_WIFI_TRACE") != nullptr) {
    if (!memRegionAdd(memory, 0x20000000, 0x10000, Memory, this) ||
        !memRegionAdd(memory, 0x28000000, 0x10000, Memory, this))
        ERR("cannot map TX WLAN");
    for (auto pin : {94, 108, 106}) pxaGpioSetNotif(gpio, pin, Power, this);
    Reset();
    irqDirty = false;
}
void LibertasCF::Reset() {
    mask = 0x1f;
    cause = commandLength = txLength = 0;
    configured = false;
    associated = false;
    if (!network->Frozen()) router.reset();
    rx.clear();
    rxPosition = 0;
    helper = running = bootAck = false;
    bootHeader = true;
    firmwareRequest = 16;
    command.clear();
    response.clear();
    tx.clear();
    responsePosition = 0;
    attribute.fill(0);
    Dirty();
}
// GPIO notifications run before the GPIO bank finishes updating its levels.
// Defer pin writes to the existing device scheduler to avoid re-entering it.
void LibertasCF::Dirty() {
    irqDirty = true;
    schedule.rescheduleCb(schedule.ctx, RESCHEDULE_TASK_DEVICE_TIER0);
}
void LibertasCF::Tick() {
    Sync();
    if (irqDirty) {
        irqDirty = false;
        UpdateIrq();
    }
}
void LibertasCF::UpdateIrq() {
    pxaGpioSetState(gpio, 116, !(enabled && power1 && power2 && !reset && (cause & ~mask & 0x1f)));
}
void LibertasCF::Power(void* context, uint32_t pin, bool, bool value) {
    auto& self = *static_cast<LibertasCF*>(context);
    if (self.trace) fprintf(stderr, "WIFI GPIO %u=%u\n", pin, unsigned(value));
    if (pin == 94)
        self.power1 = value;
    else if (pin == 108)
        self.power2 = value;
    else
        self.reset = value;
    if (!self.power1 || !self.power2 || self.reset) self.Reset();
    self.Dirty();
}
void LibertasCF::Command() {
    // Model the CF bootloader transport, rather than executing the radio CPU.
    // The helper requests 16-byte headers followed by the specified data length.
    if (!running) {
        bootAck = true;
        if (!helper) {
            if (!commandLength) {
                helper = true;
                if (trace) fprintf(stderr, "WIFI helper loaded\n");
            }
        } else if (bootHeader && command.size() >= 16) {
            const uint32_t op = le16(command, 0) | uint32_t(le16(command, 2)) << 16;
            const uint32_t count = le16(command, 8) | uint32_t(le16(command, 10)) << 16;
            if (op == 4 && count == 0) {
                running = true;
                traced = 0;
                if (trace) fprintf(stderr, "WIFI firmware running\n");
            } else if (op == 1 && count && count <= 4096) {
                firmwareRequest = count;
                bootHeader = false;
            } else if (trace)
                fprintf(stderr, "WIFI invalid boot header %u %u\n", op, count);
        } else {
            firmwareRequest = 16;
            bootHeader = true;
        }
        command.clear();
        return;
    }
    if (command.size() < 8) return;
    const auto declared = le16(command, 2);
    if (declared < 8 || declared > 3072 || declared > command.size()) {
        command.clear();
        return;
    }
    command.resize(declared);
    const auto id = le16(command, 0);
    if (trace) {
        fprintf(stderr, "WIFI CMD %04x length %zu seq %u\n", id, command.size(), le16(command, 4));
        for (size_t i = 0; i < std::min<size_t>(command.size(), 96); ++i)
            fprintf(stderr, "%02x%s", command[i], i % 16 == 15 ? "\n" : " ");
        fprintf(stderr, "\n");
    }
    response = command;
    put16(response, 0, id | 0x8000);
    put16(response, 6, 0);
    if (id == 3) {
        response.resize(46);
        std::fill(response.begin() + 8, response.end(), 0);
        put16(response, 8, 1);
        put16(response, 10, 4);
        put16(response, 12, 16);
        put16(response, 14, 32);
        std::copy(mac, mac + 6, response.begin() + 16);
        put16(response, 22, 0x1000);
        put16(response, 24, 1);
        response[26] = 102;
        response[27] = 0;
        response[28] = 4;
        response[29] = 0;
        put16(response, 42, 0x303);
    } else if (id == 6) {
        bool matches = command.size() >= 15 && command[8] != 2 &&
                       network->GetMode() == NetworkSimulation::wireless;
        if (command.size() >= 15) {
            bool anyZero = true, anyBroadcast = true;
            for (unsigned i = 0; i < 6; ++i) {
                anyZero &= command[9 + i] == 0;
                anyBroadcast &= command[9 + i] == 255;
            }
            if (!anyZero && !anyBroadcast) {
                for (unsigned i = 0; i < 6; ++i)
                    matches &= command[9 + i] == (i == 5 ? 0x65 : mac[i]);
            }
            for (size_t at = 15; at + 4 <= command.size();) {
                const auto type = le16(command, at), length = le16(command, at + 2);
                at += 4;
                if (length > command.size() - at) {
                    matches = false;
                    break;
                }
                if (type == 0 && length)
                    matches &= length == 5 && !memcmp(command.data() + at, "Frond", 5);
                if (type == 0x101 && length) {
                    bool channel = false;
                    for (size_t i = 0; i + 7 <= length; i += 7) channel |= command[at + i + 1] == 6;
                    matches &= channel && length % 7 == 0;
                }
                at += length;
            }
        }
        std::vector<uint8_t> desc(21, 0);
        std::copy(mac, mac + 6, desc.begin() + 2);
        desc[7] = 0x65;
        desc[8] = 35;
        put16(desc, 17, 100);
        put16(desc, 19, 1);
        const uint8_t ies[] = {0, 5,    'F',  'r',  'o',  'n', 'd', 1,
                               4, 0x82, 0x84, 0x8b, 0x96, 3,   1,   6};
        desc.insert(desc.end(), ies, ies + sizeof(ies));
        put16(desc, 0, desc.size() - 2);
        response.resize(11);
        put16(response, 8, matches ? desc.size() : 0);
        response[10] = matches ? 1 : 0;
        if (matches) response.insert(response.end(), desc.begin(), desc.end());
    } else if (id == 0x4d) {
        response.resize(16);
        std::copy(mac, mac + 6, response.begin() + 10);
    } else if (id == 0x12 || id == 0x50) {
        response.resize(14);
        std::fill(response.begin() + 8, response.end(), 0);
        put16(response, 0, 0x8012);
        if (network->GetMode() == NetworkSimulation::wireless) {
            put16(response, 12, 1);
            associated = true;
            if (!router) router = std::make_unique<frondNet::WifiRouter>();
        } else {
            // Out of range the join times out: status 2 is the pre-v9 firmware's
            // "timer expired waiting for the AP".
            put16(response, 10, 2);
            associated = false;
        }
    }
    if (id == 0x24 || id == 0x26) {
        associated = false;
        if (!network->Frozen()) router.reset();
        rx.clear();
        rxPosition = 0;
    }
    put16(response, 2, response.size());
    // The CF response length includes eight trailing transport bytes, which
    // Palm's driver (like Libertas) subtracts before parsing the command.
    response.resize(response.size() + 8, 0);
    responsePosition = 0;
    cause |= 8;
    if (trace)
        fprintf(stderr, "WIFI RESPONSE %04x mask=%x cause=%x power=%u%u reset=%u\n", id, mask,
                cause, power1, power2, reset);
    command.clear();
    UpdateIrq();
}
void LibertasCF::Send() {
    if (trace) {
        fprintf(stderr, "WIFI TX %zu bytes\n", tx.size());
        for (size_t i = 0; i < std::min<size_t>(tx.size(), 64); ++i)
            fprintf(stderr, "%02x%s", tx[i], i % 16 == 15 ? "\n" : " ");
        fprintf(stderr, "\n");
    }
    if (!network->Frozen() && associated && tx.size() >= 24 &&
        network->GetMode() == NetworkSimulation::wireless) {
        const uint32_t offset = le16(tx, 8) | (uint32_t(le16(tx, 10)) << 16);
        const uint16_t length = le16(tx, 12);
        if (offset >= 24 && length >= 14 && length <= 1518 && offset <= tx.size() &&
            length <= tx.size() - offset) {
            // Normal restore discards host sockets but retains association and
            // the transmit FIFO. A guest can finish that FIFO before Poll runs.
            if (!router) router = std::make_unique<frondNet::WifiRouter>();
            router->Input(tx.data() + offset, length, clock.getTimeF(clock.userdata));
        }
    }
    tx.clear();
    cause |= 1;
    UpdateIrq();
}
void LibertasCF::Poll() {
    if (!Sync() || network->Frozen() || !associated ||
        network->GetMode() != NetworkSimulation::wireless)
        return;
    if (!router) router = std::make_unique<frondNet::WifiRouter>();
    router->Poll(clock.getTimeF(clock.userdata));
    std::vector<uint8_t> bytes;
    while (rx.size() < 64 && router->Receive(bytes)) {
        if (trace)
            fprintf(stderr, "WIFI RX %zu bytes ethertype=%02x%02x\n", bytes.size(), bytes[12],
                    bytes[13]);
        std::vector<uint8_t> packet(20, 0);
        put16(packet, 0, 1);
        packet[2] = 50;
        packet[6] = 4;
        put16(packet, 4, bytes.size() + 8);
        put16(packet, 8, 20);
        packet.insert(packet.end(), bytes.begin(), bytes.begin() + 12);
        const auto payloadLength = bytes.size() - 6;
        packet.push_back(payloadLength >> 8);
        packet.push_back(payloadLength);
        // This firmware's receive path strips LLC/SNAP unconditionally.
        const uint8_t llc[] = {0xaa, 0xaa, 3, 0, 0, 0};
        packet.insert(packet.end(), llc, llc + 6);
        packet.insert(packet.end(), bytes.begin() + 12, bytes.end());
        rx.push_back(std::move(packet));
    }
    if (!rx.empty()) {
        cause |= 2;
        UpdateIrq();
    }
}
bool LibertasCF::Access(uint32_t pa, uint_fast8_t size, bool write, void* data) {
    if (size != 1 && size != 2 && size != 4) return false;
    if (!Sync() || !power1 || !power2 || reset) {
        if (!write) memset(data, 0xff, size);
        return true;
    }
    const auto offset = pa & 0xffff;
    uint32_t value = 0;
    if (write) memcpy(&value, data, size);
    if (pa >= 0x28000000) {
        if (offset + size > attribute.size()) {
            if (!write) memset(data, 0, size);
            return true;
        }
        if (write) {
            memcpy(attribute.data() + offset, data, size);
            if (offset == 0x200) configured = value & 1;
        } else
            memcpy(data, attribute.data() + offset, size);
    } else if (write) {
        switch (offset) {
            case 0x04:
                mask = value;
                break;
            case 0x18:
                commandLength = value;
                command.clear();
                break;
            case 0x1a:
                for (unsigned i = 0; i < size && command.size() < 4096; ++i)
                    command.push_back(value >> (8 * i));
                break;
            case 0x14:
                txLength = value;
                tx.clear();
                break;
            case 0x16:
                for (unsigned i = 0; i < size && tx.size() < 4096; ++i)
                    tx.push_back(value >> (8 * i));
                break;
            case 0x02:
                if (value & 4) Command();
                if (value & 1) Send();
                if (value & 2) {
                    if (trace)
                        fprintf(stderr, "WIFI RX ACK position=%zu size=%zu\n", rxPosition,
                                rx.empty() ? 0 : rx.front().size());
                    if (!rx.empty()) rx.pop_front();
                    rxPosition = 0;
                    cause &= ~2;
                }
                break;
            case 0x22:
                // IRQ W1C acknowledges notification, not the unread response.
                cause &= ~value;
                break;
        }
        UpdateIrq();
    } else {
        switch (offset) {
            case 0x04:
                value = mask;
                break;
            case 0x20:
                value = (!running || bootAck) ? 4
                                              : (5 | (responsePosition < response.size() ? 8 : 0) |
                                                 (rx.empty() ? 0 : 2) | (cause & 0x10));
                bootAck = false;
                break;
            case 0x22:
                value = cause;
                break;
            case 0x1c:
                value = 4;
                break;
            case 0x28:
                value = firmwareRequest;
                break;
            case 0x3e:
                value = helper ? 0x5a00 : 0;
                break;
            case 0x3f:
                value = helper ? 0x5a : 0;
                break;
            case 0x24:
                value = rx.empty() ? 0 : rx.front().size();
                break;
            case 0x10:
                for (unsigned i = 0; i < size; ++i)
                    if (!rx.empty() && rxPosition < rx.front().size())
                        value |= uint32_t(rx.front()[rxPosition++]) << (8 * i);
                break;
            case 0x30:
                value = response.size();
                break;
            case 0x12:
                for (unsigned i = 0; i < size; ++i)
                    if (responsePosition < response.size())
                        value |= uint32_t(response[responsePosition++]) << (8 * i);
                break;
        }
        memcpy(data, &value, size);
    }
    if (trace && traced++ < 350 && offset != 0x1a && offset != 0x12)
        fprintf(stderr, "WIFI %c %08x/%u=%08x\n", write ? 'W' : 'R', pa, unsigned(size), value);
    return true;
}

bool LibertasCF::Sync() {
    // The radio is soldered in on a real TX, so the chip is always there. Outside
    // Virtual Wi-Fi the access point is out of range: scans find nothing, joins time
    // out, frames are dropped and host connections close. Palm resets if its driver
    // powers the radio up and finds no chip.
    if (!enabled) {
        // Older sessions saved an absent chip outside Wi-Fi mode.
        enabled = true;
        Reset();
    } else if (network->GetMode() != NetworkSimulation::wireless && router &&
               !network->Frozen()) {
        router.reset();
    }
    return true;
}
void LibertasCF::Load(SavestateLoader<ChunkType>& loader) {
    if (!network->Frozen()) router.reset();
    if (!loader.HasChunk(ChunkType::libertasCF)) {
        enabled = true;
        Reset();
        power1 = pxaGpioGetState(gpio, 94) == PxaGpioStateHigh;
        power2 = pxaGpioGetState(gpio, 108) == PxaGpioStateHigh;
        reset = pxaGpioGetState(gpio, 106) == PxaGpioStateHigh;
        irqDirty = false;
        return;
    }
    auto* chunk = loader.GetChunkOrFail(ChunkType::libertasCF, 0, "TX Wi-Fi");
    if (!chunk) return;
    valid = true;
    LoadChunkHelper state(*chunk);
    Transfer(state);
    valid &= !(mask & ~0x1f) && !(cause & ~0x1f) && commandLength <= 4097 && txLength <= 4096 &&
             firmwareRequest > 0 && firmwareRequest <= 4096 && !(firmwareRequest & 1) &&
             responsePosition <= response.size() &&
             rxPosition <= (rx.empty() ? 0 : rx.front().size()) &&
             (!associated || (enabled && running && helper && power1 && power2 && !reset)) &&
             (!running || helper);
    for (const auto& packet : rx) valid &= packet.size() >= 42 && packet.size() <= 1560;
    if (!valid) {
        Reset();
        loader.NotifyError();
    }
}
#endif
