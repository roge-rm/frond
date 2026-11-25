#include "soc_S3C2442.h"
#include "sync/ActiveSync.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "CPCrc.h"
#include "HostUartPort.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"
#include "audio_queue.h"
#include "device_configuration.h"
#include "device_profile5.h"
#include "nand.h"
#include "rx1950_board.h"
#include "rx1950_nand_image.h"
#include "rx1950_power.h"
#include "sdcard.h"
#include "vSD.h"

namespace {
    // CEBL.nbf: a 0xa5a byte header, then eboot, which the loader copies to SDRAM and starts.
    constexpr uint32_t nbfHeader = 0xa5a, ebootBytes = 256 * 1024, ebootPhysical = 0x30080000;
    // Guest time one machine run covers at most, so input and frames are served often.
    constexpr uint64_t sliceNs = 1000000;
    // Host time one Run call may take, so input and frames are served while the guest is busy.
    constexpr auto runHostLimit = std::chrono::milliseconds(12);

    // Host keys to the rx1950's keys (rx1950_board.h): the four app keys, the five-way, power and record.
    const Rx1950Key* keyFor(KeyId key) {
        switch (key) {
            case keyIdHard1: return rx1950Key("calendar");
            case keyIdHard2: return rx1950Key("contacts");
            case keyIdHard3: return rx1950Key("inbox");
            case keyIdHard4: return rx1950Key("wireless");
            case keyIdUp: return rx1950Key("up");
            case keyIdDown: return rx1950Key("down");
            case keyIdLeft: return rx1950Key("left");
            case keyIdRight: return rx1950Key("right");
            case keyIdSelect: return rx1950Key("action");
            case keyIdPower: return rx1950Key("power");
            case keyIdVoice: return rx1950Key("record");
            default: return nullptr;
        }
    }
}

bool rx1950IsCebl(const uint8_t* data, size_t size) {
    return data && size == rx1950Cebl5.bytes && crc::CRC32(data, size) == rx1950Cebl5.crc32;
}

bool rx1950IsCeos(const uint8_t* data, size_t size) {
    return data && size == rx1950Ceos5.bytes && crc::CRC32(data, size) == rx1950Ceos5.crc32;
}

// The machine's NAND pages kept in the uARM NAND, whose file sessions keep crash safe (journal, then a
// rename over the old one). Writes mark the chip dirty and its dirty page bits (4224-byte units) as its own
// program and erase do, so IsNandDirty and GetNandDirtyPages see them.
struct SocS3C2442::NandPages : S3cNand::Store {
    NAND* chip;
    explicit NandPages(NAND* chip) : chip(chip) {}
    bool readPage(uint32_t page, uint8_t* out) override { return nandReadRawPage(chip, page, out); }
    bool writePage(uint32_t page, const uint8_t* in) override {
        if (!nandWriteRawPage(chip, page, in)) return false;
        markDirty(page, 1);
        return true;
    }
    bool eraseBlock(uint32_t firstPage, uint32_t count) override {
        uint8_t ones[S3cNand::pageSize];
        std::memset(ones, 0xff, sizeof ones);
        for (uint32_t p = 0; p < count; ++p)
            if (!nandWriteRawPage(chip, firstPage + p, ones)) return false;
        markDirty(firstPage, count);
        return true;
    }
    void markDirty(uint32_t firstPage, uint32_t count) {
        const Buffer bits = nandGetDirtyPages(chip);
        auto* words = static_cast<uint32_t*>(bits.data);
        const size_t first = size_t(firstPage) * S3cNand::pageSize / 4224;
        const size_t last = (size_t(firstPage + count) * S3cNand::pageSize - 1) / 4224;
        for (size_t unit = first; unit <= last && unit / 32 < bits.size / 4; ++unit) words[unit / 32] |= 1u << (unit % 32);
        nandSetDirty(chip, true);
    }
};

// FROND_RX1950_UART_TRACE: every byte UART0 and UART2 send and take from a host, to stderr.
namespace {
    const bool uartTrace = std::getenv("FROND_RX1950_UART_TRACE");
}

// UART0 on the sync connector for HostSerial.
struct SocS3C2442::SerialPort final : HostUartPort {
    explicit SerialPort(SocS3C2442& soc) : soc(soc) {}
    SocS3C2442& soc;
    Read read{};
    Write write{};
    Active active{};
    Detached detached{};
    void* context{};
    uint32_t disables{0};
    bool wasOn{false};

    S3cUart& Uart() const { return soc.machine->uarts[rx1950SerialUart]; }
    bool Attach(Read r, Write w, Active a, Detached d, void* c) override {
        if (soc.halted) return false;
        read = r; write = w; active = a; detached = d; context = c;
        wasOn = Uart().receiverOn();
        soc.ApplyUartLines();
        return true;
    }
    void Detach() override {
        read = nullptr; write = nullptr; active = nullptr; detached = nullptr; context = nullptr;
        Uart().setCts(true);
        if (!soc.halted) soc.ApplyUartLines();
    }
    // A load or reset made a new board: the host attaches again.
    void Replaced() {
        if (!context) return;
        const auto notify = detached;
        void* const host = context;
        Detach();
        if (notify) notify(host);
    }
    bool SetInputs(uint8_t asserted) override {
        if (soc.halted) return false;
        Uart().setCts(asserted & hostUartCts);
        return true;
    }
    // RTS out: UMCON bit 0, or with auto flow control (bit 4) while the FIFO has room. WM5 leaves GPH1 as an
    // input rather than nRTS0, so the connector has no RTS from the UART: the far end may always send.
    bool WantsInput() const override {
        const auto& u = Uart();
        const bool rtsPin = ((soc.machine->gpio.con[S3cGpio::h] >> 2) & 3) == 2;
        return u.receiverOn() && u.rxRoom() && (!rtsPin || (u.umcon & 0x10) || (u.umcon & 1));
    }
    bool ReceiverEnabled() const override { return Uart().receiverOn(); }
    uint32_t DisableCount() const override {
        auto& self = const_cast<SerialPort&>(*this);
        const bool on = Uart().receiverOn();
        if (self.wasOn && !on) ++self.disables;
        self.wasOn = on;
        return disables;
    }
    void Kick() override {}
    HostUartMode Mode() const override {
        const auto& u = Uart();
        return {soc.machine->uartBaud(rx1950SerialUart), u.ulcon & 0x3f, u.receiverOn(), u.transmitterOn()};
    }
};

// Windows Mobile sends CLIENT on UART0 before starting PPP.
struct SocS3C2442::SerialSync {
    std::shared_ptr<frondSync::ActiveSyncSession> session = std::make_shared<frondSync::ActiveSyncSession>(true);
    frondNet::SlirpIp network{session};
    frondModem::PppServer ppp{network, {false, false}};
    static constexpr char request[] = "CLIENT";
    static constexpr char response[] = "CLIENTSERVER";

    explicit SerialSync(SocS3C2442& soc) : soc(soc) {}

    static uint_fast16_t Read(void* context) {
        auto& self = *static_cast<SerialSync*>(context);
        if (self.matched != sizeof(request) - 1) return hostUartNone;
        if (self.replied < sizeof(response) - 1) return response[self.replied++];
        uint8_t byte;
        return self.ppp.ToModem(&byte, 1, self.soc.GetTime()) ? byte : hostUartNone;
    }

    static void Write(uint_fast16_t value, void* context) {
        auto& self = *static_cast<SerialSync*>(context);
        const auto now = self.soc.GetTime();
        const uint8_t byte = uint8_t(value);
        if (self.matched == sizeof(request) - 1) {
            self.ppp.FromModem(&byte, 1, now);
        } else if (byte == request[self.matched]) {
            if (++self.matched == sizeof(request) - 1) self.ppp.Answer(now);
        } else {
            self.matched = byte == request[0] ? 1 : 0;
        }
    }

    void Poll(uint64_t now) {
        if (matched == sizeof(request) - 1 && now >= pollAt) {
            pollAt = now + 1000000;
            ppp.Service(now);
        }
    }

    SocS3C2442& soc;
    size_t matched{}, replied{};
    uint64_t attachAt{}, pollAt{};
};

// UART2 in IrDA mode for HostInfrared.
struct SocS3C2442::InfraredPort final : HostInfraredPort {
    explicit InfraredPort(SocS3C2442& soc) : soc(soc) {}
    SocS3C2442& soc;
    SerialHostHooks hooks{};
    HostUartMode last{};
    bool lastPower{false};

    S3cUart& Uart() const { return soc.machine->uarts[rx1950InfraredUart]; }
    uint64_t Time() override { return soc.GetTime(); }
    bool SetHooks(const SerialHostHooks* next) override {
        if (!next) { hooks = {}; return true; }
        if (soc.halted) return false;
        hooks = *next;
        return true;
    }
    void Replaced() {
        if (hooks.context && hooks.replaced) hooks.replaced(hooks.context);
    }
    bool PowerEnabled() override { return Uart().infrared(); }
    bool ReceiveEnabled() override { return Uart().infrared() && Uart().receiverOn(); }
    HostUartMode Mode() override {
        const auto& u = Uart();
        return {soc.machine->uartBaud(rx1950InfraredUart), u.ulcon & 0x7f, u.infrared() && u.receiverOn(),
                u.infrared() && u.transmitterOn()};
    }
    void Kick() override {}
    bool HostIoPaused() override { return false; }
    bool Sleeping() override { return soc.PowerSleeping(); }
    bool KeyboardAttached() override { return false; }
    // Tells the host when the mode it receives in changes (its receive window).
    void CheckMode() {
        if (!hooks.context || !hooks.modeChanged) return;
        const auto mode = Mode();
        const bool power = PowerEnabled();
        if (mode.baud != last.baud || mode.format != last.format || mode.receive != last.receive ||
            mode.transmit != last.transmit || power != lastPower) {
            last = mode;
            lastPower = power;
            hooks.modeChanged(hooks.context);
        }
    }
};

HostUartPort* SocS3C2442::SerialHostPort() {
    if (!serialPort) serialPort = std::make_unique<SerialPort>(*this);
    return serialPort.get();
}

HostInfraredPort* SocS3C2442::InfraredHostPort() {
    if (!infraredPort) infraredPort = std::make_unique<InfraredPort>(*this);
    return infraredPort.get();
}

// The cable sense follows the serial host; the host's CTS is let go with no host.
void SocS3C2442::ApplyUartLines() {
    if (halted) return;
    const bool cable = serialPort && serialPort->context;
    machine->setPin(rx1950CablePort, rx1950CableBit, cable);
    if (!cable) machine->uarts[rx1950SerialUart].setCts(true);
}

void SocS3C2442::UartSent(unsigned channel, uint8_t byte) {
    if (uartTrace) {
        const auto& u = machine->uarts[channel];
        std::fprintf(stderr, "uart%u %.6f tx %02x (ULCON %02x UCON %04x UMCON %02x GPHCON %08x baud %u)\n", channel,
                     machine->nanoseconds() / 1e9, byte, u.ulcon, u.ucon, u.umcon, machine->gpio.con[S3cGpio::h],
                     machine->uartBaud(channel));
    }
    if (serialCheckpoints) return;
    if (channel == rx1950SerialUart && serialPort && serialPort->write) serialPort->write(byte, serialPort->context);
    if (channel == rx1950InfraredUart && infraredPort && infraredPort->hooks.write && machine->uarts[channel].infrared())
        infraredPort->hooks.write(byte, infraredPort->hooks.context);
}

// Host work on guest time, at points that don't depend on the run slices. Each UART with a host takes one host byte
// a character time (10 bits at its rate) while its receiver is on and its FIFO has room. With nothing to take it looks
// again at the next hostPollNs step of guest time, which is where bytes that came from the host meanwhile enter.
// The USB host ticks at its own wake times, and the Wi-Fi router is polled at each poll step (hostNetNextNs). All of
// it goes on while the machine sleeps.
namespace {
    constexpr uint64_t hostPollNs = 1000000;
    uint64_t nextPoll(uint64_t t) { return (t / hostPollNs + 1) * hostPollNs; }
}

bool SocS3C2442::UartHasHost(unsigned channel) const {
    if (channel == rx1950SerialUart) return serialPort && serialPort->read;
    if (channel == rx1950InfraredUart) return infraredPort && infraredPort->hooks.read;
    return false;
}

void SocS3C2442::PumpUart(unsigned channel, uint64_t t) {
    if (!UartHasHost(channel)) { machine->hostUartNextNs[channel] = UINT64_MAX; return; }
    if (t < machine->hostUartNextNs[channel]) return;
    machine->hostUartNextNs[channel] = nextPoll(t);
    // ASSUMPTION "UART asleep": in sleep the UART has no power and RXD isn't a wake source, so what the host sent
    // meanwhile is lost. Taken each poll step, so none of it comes in at the wake.
    if (machine->asleep()) {
        for (unsigned n = 0; n < 4096; ++n) {
            uint_fast16_t value = hostUartNone;
            if (channel == rx1950SerialUart) {
                if (serialPort->active && !serialPort->active(serialPort->context)) break;
                value = serialPort->read(serialPort->context);
            } else {
                if (infraredPort->hooks.pending && !infraredPort->hooks.pending(infraredPort->hooks.context)) break;
                value = infraredPort->hooks.read(infraredPort->hooks.context);
            }
            if (value & ~0xffu) break;
        }
        return;
    }
    auto& uart = machine->uarts[channel];
    const uint64_t baud = machine->uartBaud(channel);
    if (!baud || !uart.receiverOn()) return;
    const uint64_t charNs = std::max<uint64_t>(1, 10000000000ull / baud);
    if (!uart.rxRoom()) { machine->hostUartNextNs[channel] = t + charNs; return; }
    uint_fast16_t value = hostUartNone;
    if (channel == rx1950SerialUart) {
        if (serialPort->active && !serialPort->active(serialPort->context)) return;
        value = serialPort->read(serialPort->context);
    } else {
        if (!uart.infrared()) return;
        if (infraredPort->hooks.pending && !infraredPort->hooks.pending(infraredPort->hooks.context)) return;
        value = infraredPort->hooks.read(infraredPort->hooks.context);
    }
    if (value & ~0xffu) return;
    if (uartTrace)
        std::fprintf(stderr, "uart%u %.6f rx %02x (ULCON %02x UCON %04x UFCON %02x UMCON %02x baud %u)\n", channel,
                     t / 1e9, unsigned(value), uart.ulcon, uart.ucon, uart.ufcon, uart.umcon, unsigned(baud));
    machine->uartReceive(channel, uint8_t(value));
    machine->hostUartNextNs[channel] = t + charNs;
}

// Called by the machine's run loop once machine->hostNextNs is reached.
void SocS3C2442::HostService() {
    const uint64_t t = machine->nanoseconds();
    if (infraredPort) infraredPort->CheckMode();
    if (!serialCheckpoints && !halted) {
        if (serialSync && !serialSync->session->Running()) StopSerialActiveSync();
        if (serialSync && !machine->asleep()) {
            if (serialSync->attachAt && t >= serialSync->attachAt) {
                auto* port = SerialHostPort();
                if (!port->Attach(SerialSync::Read, SerialSync::Write, nullptr, nullptr, serialSync.get())) {
                    StopSerialActiveSync();
                } else {
                    port->SetInputs(hostUartCts | hostUartDsr | hostUartDcd);
                    serialSync->attachAt = 0;
                }
            }
            if (serialSync) serialSync->Poll(t);
        }
        PumpUart(rx1950SerialUart, t);
        PumpUart(rx1950InfraredUart, t);
        TickUsbHost(t);
    } else {
        // Paused: look again at the next poll step.
        for (auto& at : machine->hostUartNextNs)
            if (at != UINT64_MAX) at = std::max(at, nextPoll(t));
    }
    // Wi-Fi frames have their own pause (the network frozen for a checkpoint), polled at the poll steps only.
    if (!halted && router && !networkSimulation.Frozen() && t >= machine->hostNetNextNs) {
        machine->hostNetNextNs = nextPoll(t);
        PollNetwork();
    }
    // Work still due waits a nanosecond, so the guest moves on between two calls.
    ScheduleHost(t + 1);
}

// Brings machine->hostNextNs forward to the earliest host work. Anything due by now goes to late: just after this in
// the service, now for a guest access to the USB controller, the next poll step from outside the run loop. While any
// host is attached a service runs at least every poll step, so host-side news (bytes, a kick) enters there.
void SocS3C2442::ScheduleHost(uint64_t late) {
    if (!machine) return;
    const uint64_t now = machine->nanoseconds();
    auto due = [&](uint64_t at) { return at <= now ? late : at; };
    uint64_t next = machine->hostNextNs;
    bool any = false;
    for (const unsigned channel : {rx1950SerialUart, rx1950InfraredUart}) {
        if (!UartHasHost(channel)) { machine->hostUartNextNs[channel] = UINT64_MAX; continue; }
        any = true;
        if (machine->hostUartNextNs[channel] == UINT64_MAX) machine->hostUartNextNs[channel] = 0;
        next = std::min(next, due(machine->hostUartNextNs[channel]));
    }
    if (serialSync) any = true;
    if (usbHostHooks.tick) {
        any = true;
        if (usbHostHooks.wake && !UsbHostIoPaused()) next = std::min(next, due(usbHostHooks.wake(usbHostHooks.context)));
    }
    // Frames from the Wi-Fi router enter at the poll steps. A new router is first polled at the next one.
    if (!router) {
        machine->hostNetNextNs = UINT64_MAX;
    } else if (!networkSimulation.Frozen()) {
        any = true;
        if (machine->hostNetNextNs == UINT64_MAX) machine->hostNetNextNs = nextPoll(now);
        next = std::min(next, due(machine->hostNetNextNs));
    }
    if (any) next = std::min(next, nextPoll(now));
    machine->hostNextNs = next;
}

namespace {
    const bool usbTrace = std::getenv("FROND_RX1950_USB_TRACE");
    UsbPacketResult usbTraced(S3c2442Machine& m, const char* what, unsigned ep, UsbPacketResult r, unsigned size) {
        if (usbTrace)
            std::fprintf(stderr, "usbh %.6f %s ep%u size %u -> %d (csr0 %02x int %02x/%02x INTMSK %08x SRCPND %08x)\n",
                         m.nanoseconds() / 1e9, what, ep, size, int(r), m.usbDevice.ep0Csr, m.usbDevice.regs[2],
                         m.usbDevice.regs[(0x15c - 0x140) / 4], m.intc.intmsk, m.intc.srcpnd);
        return r;
    }
}

UsbDevicePort SocS3C2442::GetUsbPort() {
    using R = UsbPacketResult;
    auto m = [](void* p) -> S3c2442Machine& { return *static_cast<SocS3C2442*>(p)->machine; };
    static const UsbDevicePort::Operations ops{
        [](void* p) {
            auto& machine = *static_cast<SocS3C2442*>(p)->machine;
            if (!machine.usbListening()) return false;
            machine.usbDevice.busReset();
            machine.usbHostChanged();
            usbTraced(machine, "reset", 0, UsbPacketResult::Ack, 0);
            return true;
        },
        [](void*) { return false; },  // no start-of-frame model
        [](void* p) { return static_cast<SocS3C2442*>(p)->machine->usbListening(); },
        [](void* p) { return static_cast<SocS3C2442*>(p)->machine->usbListening(); },
        [](void*) { return false; },
        [](void* p) { return static_cast<SocS3C2442*>(p)->machine->usbEnableCount(); },
        [](void* p) { return static_cast<SocS3C2442*>(p)->machine->usbDevice.address(); },
        [](void* p, void (*notify)(void*), void* context) {
            auto& soc = *static_cast<SocS3C2442*>(p);
            soc.usbNotify = notify;
            soc.usbNotifyContext = context;
        },
        [](void* p, const uint8_t* data) {
            auto& machine = *static_cast<SocS3C2442*>(p)->machine;
            const auto r = machine.usbDevice.setup(data);
            machine.usbHostChanged();
            return usbTraced(machine, "setup", 0, r, 8);
        },
        [](void* p, const uint8_t* data, unsigned size) {
            auto& machine = *static_cast<SocS3C2442*>(p)->machine;
            const auto r = machine.usbDevice.out(0, data, size);
            machine.usbHostChanged();
            return usbTraced(machine, "out", 0, r, size);
        },
        [](void* p, uint8_t* data, unsigned* size) {
            auto& machine = *static_cast<SocS3C2442*>(p)->machine;
            const auto r = machine.usbDevice.in(0, data, size, true);
            machine.usbHostChanged();
            return usbTraced(machine, "in", 0, r, size ? *size : 0);
        },
        [](void* p, uint8_t ep, const uint8_t* data, unsigned size) {
            ep &= 0x7f;
            if (!ep) return R::Unsupported;
            auto& machine = *static_cast<SocS3C2442*>(p)->machine;
            const auto r = machine.usbDevice.out(ep, data, size);
            machine.usbHostChanged();
            return r;
        },
        [](void* p, uint8_t ep, uint8_t* data, unsigned* size) {
            ep &= 0x7f;
            if (!ep) return R::Unsupported;
            auto& machine = *static_cast<SocS3C2442*>(p)->machine;
            const auto r = machine.usbDevice.in(ep, data, size, true);
            machine.usbHostChanged();
            return r;
        },
        [](void* p, uint8_t ep, unsigned* size) {
            ep &= 0x7f;
            return ep ? static_cast<SocS3C2442*>(p)->machine->usbDevice.in(ep, nullptr, size, false) : R::Unsupported;
        },
        [](void* p, uint8_t ep, unsigned) {
            ep &= 0x7f;
            return !ep || static_cast<SocS3C2442*>(p)->machine->usbDevice.outReady(ep);
        }};
    (void)m;
    return {this, ops};
}

void SocS3C2442::StartActiveSync() {
    StopSerialActiveSync();
    usbDesktop.reset();
    activeSync = std::make_shared<frondSync::ActiveSyncSession>();
    activeSync->Start();
}

bool SocS3C2442::StartSerialActiveSync() {
    StopSerialActiveSync();
    auto next = std::make_unique<SerialSync>(*this);
    if (!next->network.Valid() || halted) return false;
    // WM5 must see the serial cable out before starting another PPP link.
    next->attachAt = machine->nanoseconds() + 2000000000;
    next->session->Start();
    serialSync = std::move(next);
    ScheduleHost(nextPoll(machine->nanoseconds()));
    return true;
}

bool SocS3C2442::StopSerialActiveSync() {
    if (!serialSync) return false;
    if (serialPort && serialPort->context == serialSync.get()) serialPort->Detach();
    serialSync->session->Stop();
    serialSync.reset();
    return true;
}

bool SocS3C2442::SetUsbCable(bool attached) {
    if (halted) return false;
    if (!attached) {
        usbDesktop.reset();
        activeSync.reset();
    }
    machine->setUsbVbus(attached);
    // USB power is AC to the battery driver.
    rx1950ApplyBattery(*machine, battery);
    return true;
}

void SocS3C2442::TickUsbHost(uint64_t t) {
    if (!usbHostHooks.tick || UsbHostIoPaused()) return;
    if (!usbHostHooks.wake || usbHostHooks.wake(usbHostHooks.context) <= t) usbHostHooks.tick(usbHostHooks.context, t);
}

SocS3C2442* SocS3C2442::Create(const uint8_t* cebl, size_t size, uint8_t* flash, size_t flashSize, std::string& error,
                               const BatteryPower& battery, bool anyFirmware) {
    // Any CEBL has the NBF header and the 256 KiB eboot the loader copies to SDRAM.
    if (anyFirmware ? !cebl || size != nbfHeader + ebootBytes : !rx1950IsCebl(cebl, size)) {
        error = anyFirmware ? "the CEBL.nbf isn't a 0xa5a byte header and 256 KiB of eboot"
                            : "not the rx1950's CEBL.nbf that Frond runs (HP's WM5 ROM 1.01.00 ENG)";
        return nullptr;
    }
    std::unique_ptr<SocS3C2442> soc(new SocS3C2442());
    soc->anyFirmware = anyFirmware;
    soc->rom.assign(cebl, cebl + size);
    soc->battery = battery;
    S3c2442Options options;
    options.rtcStart = rx1950RtcStart;
    // No serial cable.
    options.pinLevels[rx1950CablePort] &= ~(1u << rx1950CableBit);
    rx1950BatteryPins(soc->battery, options);
    soc->machine = S3c2442Machine::create(options, error);
    if (!soc->machine) return nullptr;

    if (!memoryBufferAllocate(&soc->nandPageBuffer, MEMORY_BUFFER_GRANULARITY)) {
        error = "the NAND page buffer could not be allocated";
        return nullptr;
    }
    static const NandSpecs specs = {
        .bytesPerPage = S3cNand::pageSize,
        .blocksPerDevice = S3cNand::blocks,
        .pagesPerBlockLg2 = 5,
        .flags = NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS,
        .devIdLen = 2,
        .devId = {0xec, 0x76},
    };
    const Reschedule noReschedule{.rescheduleCb = [](void*, uint32_t) {}, .ctx = nullptr};
    if (flashSize != size_t(S3cNand::pages) * S3cNand::pageSize ||
        !(soc->nand = nandInit(flash, &soc->nandPageBuffer, noReschedule, flashSize, &specs, nullptr, nullptr))) {
        error = "the NAND isn't the rx1950's 64 MB";
        return nullptr;
    }
    soc->pages = std::make_unique<NandPages>(soc->nand);
    soc->machine->nand.setStore(soc->pages.get());

    soc->machine->resetLoader = [s = soc.get()] { s->BootLoader(); };
    soc->machine->pcmSink = [s = soc.get()](int16_t left, int16_t right, uint32_t gain) {
        uint64_t numerator = 0, denominator = 1;
        s->machine->audioRate(numerator, denominator);
        s->sound.setRate(numerator, denominator);
        s->sound.setGain(gain);
        if (s->capture) {
            s->captureRawPeak = std::max<unsigned>(s->captureRawPeak, unsigned(std::max(std::abs(int(left)), std::abs(int(right)))));
            s->captureGainPeak = std::max(s->captureGainPeak, gain);
        }
        s->sound.word(uint16_t(left), 0, PushFrame, s);
        s->sound.word(uint16_t(right), 1, PushFrame, s);
    };
    soc->machine->wifiEthernetOut = [s = soc.get()](const uint8_t* frame, size_t size) {
        if (s->networkSimulation.Frozen()) return;
        if (!s->router) s->router = std::make_unique<frondNet::WifiRouter>();
        s->router->Input(frame, size, s->machine->nanoseconds());
        // A new router is polled from the next poll step.
        s->ScheduleHost(nextPoll(s->machine->nanoseconds()));
    };
    soc->machine->uartSink = [s = soc.get()](unsigned channel, uint8_t byte) { s->UartSent(channel, byte); };
    // A guest access to the USB controller can bring the host's next tick forward: due now, after this instruction.
    soc->machine->usbNotify = [s = soc.get()] {
        if (s->usbNotify) s->usbNotify(s->usbNotifyContext);
        s->ScheduleHost(s->machine->nanoseconds());
    };
    soc->machine->hostService = [s = soc.get()] { s->HostService(); };
    // A watchdog reset: the board's inputs, host keys and pen, sound and the host side start again at the
    // instruction it happened.
    soc->machine->watchdogResetHook = [s = soc.get()] {
        s->ApplyHostInputs();
        s->StateChanged();
    };
    // FROND_RX1950_USB_TRACE: every guest access to the USB device controller and every host packet, to stderr.
    if (std::getenv("FROND_RX1950_USB_TRACE")) {
        soc->machine->mmioTrace = [s = soc.get()](uint32_t pc, uint32_t address, unsigned size, bool write, uint32_t value) {
            const bool gpio = (address >= 0x560000d0 && address <= 0x560000d8) || address == 0x56000080 ||
                              (address >= 0x56000060 && address <= 0x56000068) || address == 0x5600008c;
            if ((address & 0xff000000) != 0x52000000 && !(gpio && write)) return;
            std::fprintf(stderr, "usbd %.6f pc %08x %s %03x %0*x\n", s->machine->nanoseconds() / 1e9, pc, write ? "W" : "R",
                         address & 0xfff, int(size * 2), value);
            if (gpio) std::fprintf(stderr, "  GPJCON %08x GPJDAT %08x CLKCON %08x MISCCR %08x vbus %d\n", s->machine->gpio.con[S3cGpio::j],
                                   s->machine->gpio.dat[S3cGpio::j], s->machine->clock.clkcon, s->machine->gpio.misccr, s->machine->usbVbus());
        };
    }
    soc->BootLoader();
    soc->ApplyHostInputs();
    soc->displayConfiguration = deviceConfigurationDisplayConfigForDevice(deviceTypeRx1950, DisplayMode::mode_240x320);
    soc->ramSize = options.ramBytes;
    soc->StateChanged();
    // The owner keeps it for the life of the process, like the machine inside.
    return soc.release();
}

void SocS3C2442::BootLoader() {
    machine->loadRam(ebootPhysical, rom.data() + nbfHeader, ebootBytes);
    // ASSUMPTION "rx1950 IPL clocks": the block 0 loader sets the PLL before eboot, which never does.
    // 296.35 MHz from the 16.9344 MHz crystal (MDIV 97, PDIV 1, SDIV 2), FCLK:HCLK:PCLK 1:3:6.
    machine->clock.write(0x04, 0x61012);
    machine->clock.write(0x14, 0x7);
    rx1950LoaderPins(machine->gpio);
    machine->start(ebootPhysical);
}

void SocS3C2442::PushFrame(void* context, uint32_t frame) {
    auto& s = *static_cast<SocS3C2442*>(context);
    if (s.audioQueue && s.pcmEnabled && !s.pcmSuspended) audioQueuePush(s.audioQueue, frame);
    if (s.capture) {
        std::fwrite(&frame, 4, 1, s.capture);
        ++s.captureFrames;
        const int l = int16_t(frame), r = int16_t(frame >> 16);
        const unsigned peak = unsigned(std::max(std::abs(l), std::abs(r)));
        if (peak) ++s.captureLoud;
        s.capturePeak = std::max(s.capturePeak, peak);
    }
}

bool SocS3C2442::SetPcmCapture(const std::string& path) {
    if (capture) std::fclose(capture);
    capture = path.empty() ? nullptr : std::fopen(path.c_str(), "wb");
    captureFrames = captureLoud = 0;
    capturePeak = captureRawPeak = captureGainPeak = 0;
    return path.empty() || capture;
}

std::string SocS3C2442::PcmCaptureSummary() const {
    return std::to_string(captureFrames) + " frames, " + std::to_string(captureLoud) + " not silent, peak " +
           std::to_string(capturePeak) + " (IIS peak " + std::to_string(captureRawPeak) + ", gain peak " +
           std::to_string(captureGainPeak) + "/65536)";
}

void SocS3C2442::ApplyHostInputs() {
    ApplyUartLines();
    // VBUS isn't saved: the bridge's cable decides GPG5 after a load or reset.
    machine->setUsbVbus(machine->usbVbus());
    rx1950ApplyBattery(*machine, battery);
    machine->setHeadphones(headphones);
    ApplyCardPins();
}

void SocS3C2442::ApplyCardPins() {
    const bool in = cardInserted && machine->sdCard();
    machine->setPin(rx1950CardDetectPort, rx1950CardDetectBit, !in);
    machine->setPin(rx1950WriteProtectPort, rx1950WriteProtectBit, in && sdProtect);
}

void SocS3C2442::SetSdWriteProtect(bool on) {
    sdProtect = on;
    ApplyCardPins();
}

void SocS3C2442::OnSdInsert() {
    const size_t sectors = sdCardSectorCount();
    if (sectors > rx1950MaxCardSectors) {
        std::cerr << "rx1950: the card is over 2 GB, which Windows Mobile 5 can't use; left out" << std::endl;
        cardInserted = false;
        return;
    }
    if (!vsd) vsd = vsdInit(sdCardRead, sdCardWrite, 0);
    vsdReset(vsd, uint32_t(sectors));
    std::strncpy(cardId, sdCardGetId(), sizeof(cardId) - 1);
    cardId[sizeof(cardId) - 1] = '\0';
    machine->sdCardId = cardId;
    machine->setSdCard(vsd);
    ApplyCardPins();
}

void SocS3C2442::OnSdEject() {
    machine->setSdCard(nullptr);
    machine->sdCardId.clear();
    if (vsd) vsdReset(vsd, 0);
    ApplyCardPins();
}

void SocS3C2442::LoadCard() {
    const auto& saved = machine->loadedCard;
    cardSwapped = false;
    const size_t sectors = sdCardInitialized() ? sdCardSectorCount() : 0;
    if (saved.in && sectors && sectors <= rx1950MaxCardSectors && (saved.id.empty() || saved.id == sdCardGetId())) {
        if (!vsd) vsd = vsdInit(sdCardRead, sdCardWrite, 0);
        vsdReset(vsd, uint32_t(sectors));
        // A card mid-command or mid-transfer carries on; one whose state doesn't fit starts idle.
        if (!saved.card.empty()) vsdMachineLoad(vsd, saved.card.data(), saved.card.size());
        machine->setSdCard(vsd);
        std::strncpy(cardId, sdCardGetId(), sizeof(cardId) - 1);
        cardId[sizeof(cardId) - 1] = '\0';
        machine->sdCardId = cardId;
        cardInserted = true;
        return;
    }
    // The saved card isn't in the store: the slot is empty (the guest sees it taken out) and the store's card,
    // if any, goes in later as a new one.
    cardSwapped = saved.in && sdCardInitialized();
    machine->setSdCard(nullptr);
    machine->sdCardId.clear();
    if (vsd) vsdReset(vsd, 0);
    cardInserted = false;
}

bool SocS3C2442::SetHeadphones(bool in) {
    headphones = in;
    machine->setHeadphones(in);
    return true;
}

void SocS3C2442::StateChanged() {
    const auto stop = machine->stopInfo().stop;
    halted = stop != S3c2442Stop::running && stop != S3c2442Stop::budget;
    lcdView.pending = false;
    redraw = true;
    jams.clear();
    keys.clear();
    // The host's keys and pen are up now.
    for (const auto& k : rx1950Keys) machine->setPin(k.port, k.bit, true);
    pen.clear(*machine);
    // The sound starts again from the new state's next frame.
    sound.restart();
    RestartHost();
    if (audioQueue) audioQueueClear(audioQueue);
}

// Host peers start again with the new machine, their work from the next poll step. A checkpoint restore keeps the
// peers and the schedule the state was saved with (machine->hostNextNs and hostUartNextNs), so a replay services
// the host at the same points; a peer attached since then starts at the next poll step.
void SocS3C2442::RestartHost() {
    if (!RestoringCheckpoint()) {
        StopSerialActiveSync();
        if (serialPort) serialPort->Replaced();
        if (infraredPort) infraredPort->Replaced();
        if (usbHostHooks.replaced) usbHostHooks.replaced(usbHostHooks.context);
        std::fill(std::begin(machine->hostUartNextNs), std::end(machine->hostUartNextNs), 0);
        machine->hostNextNs = UINT64_MAX;
        machine->hostNetNextNs = UINT64_MAX;
    }
    ScheduleHost(nextPoll(machine->nanoseconds()));
}

void SocS3C2442::SetClockMultiplier(uint32_t num, uint32_t den) {
    SoC::SetClockMultiplier(num, den);
    machine->setClockMultiplier(ClockMultiplierNum(), ClockMultiplierDen());
}

uint64_t SocS3C2442::CoreHz() const {
    return machine->clock.cpuClock() * clockMultiplierNum / clockMultiplierDen;
}

// The reset button. ASSUMPTION "rx1950 reset button": it pulls the S3C2442's nRESET, which keeps the RTC,
// and the loader and eboot start the OS from flash as after a power on.
void SocS3C2442::PollNetwork() {
    if (!router || networkSimulation.Frozen()) return;
    const uint64_t now = machine->nanoseconds();
    router->Poll(now);
    std::vector<uint8_t> frame;
    for (unsigned n = 0; n < 64 && router->Receive(frame); ++n) machine->wifiEthernetIn(frame.data(), frame.size());
}

namespace {
// FROND_RX1950_RNDIS_TRACE: a line for every Ethernet frame on the RNDIS link, to stderr.
const bool rndisTrace = std::getenv("FROND_RX1950_RNDIS_TRACE");
void traceFrame(const char* way, const uint8_t* f, size_t n, uint64_t now) {
    if (!rndisTrace) return;
    std::fprintf(stderr, "rndis %.6f %s %zu %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x", now / 1e9,
                 way, n, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
    if (n >= 34 && f[12] == 8 && f[13] == 0) {
        const uint8_t* ip = f + 14;
        const unsigned ihl = (ip[0] & 15) * 4;
        std::fprintf(stderr, " %u.%u.%u.%u -> %u.%u.%u.%u p%u", ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19], ip[9]);
        if ((ip[9] == 6 || ip[9] == 17) && n >= 14 + ihl + 4) {
            const uint8_t* t = ip + ihl;
            std::fprintf(stderr, " %u -> %u", t[0] << 8 | t[1], t[2] << 8 | t[3]);
            if (ip[9] == 6 && n >= 14 + ihl + 14) std::fprintf(stderr, " flags %02x", t[13]);
            if (ip[9] == 17 && n >= 14 + ihl + 8 + 243 && (t[2] << 8 | t[3]) >= 67 && (t[2] << 8 | t[3]) <= 68) {
                // DHCP: op, yiaddr and the message type option.
                const uint8_t* d = t + 8;
                std::fprintf(stderr, " dhcp op %u yiaddr %u.%u.%u.%u", d[0], d[16], d[17], d[18], d[19]);
                for (size_t o = 240; o + 2 < n - 14 - ihl - 8 && d[o] != 255;)
                    if (d[o] == 0) ++o;
                    else {
                        if (d[o] == 53) std::fprintf(stderr, " type %u", d[o + 2]);
                        o += 2 + d[o + 1];
                    }
            }
        }
    } else if (n >= 42 && f[12] == 8 && f[13] == 6) {
        std::fprintf(stderr, " arp op %u %u.%u.%u.%u -> %u.%u.%u.%u", f[21], f[28], f[29], f[30], f[31], f[38], f[39], f[40], f[41]);
    }
    std::fprintf(stderr, "\n");
}
}  // namespace

void SocS3C2442::UsbEthernetOut(const uint8_t* frame, size_t size, uint64_t now) {
    if (size < 14 || networkSimulation.Frozen()) return;
    traceFrame("out", frame, size, now);
    if (usbDesktop) usbDesktop->Input(frame, size, now);
}

void SocS3C2442::UsbEthernetPoll(uint64_t now, const uint8_t* mac, uint32_t link) {
    if (networkSimulation.Frozen()) return;
    // A new RNDIS link (a restart or a replug, maybe with another MAC) gets a new desktop end and DHCP.
    if (!usbDesktop || link != usbDesktopLink) {
        usbDesktop = std::make_unique<frondNet::DesktopLink>(mac, frondNet::DesktopLink::connectPort, activeSync);
        usbDesktopLink = link;
    }
    usbDesktop->Poll(now);
}

bool SocS3C2442::UsbEthernetIn(std::vector<uint8_t>& frame) {
    if (!usbDesktop || networkSimulation.Frozen() || !usbDesktop->Receive(frame)) return false;
    traceFrame("in", frame.data(), frame.size(), machine->nanoseconds());
    return true;
}

void SocS3C2442::Reset() {
    SoC::Reset();
    if (!networkSimulation.Frozen()) {
        router.reset();
        usbDesktop.reset();
        activeSync.reset();
    }
    DisarmMicrophone();
    machine->reset();
    ApplyHostInputs();
    StateChanged();
}

void SocS3C2442::HardReset() {
    SoC::Reset();
    if (!networkSimulation.Frozen()) {
        router.reset();
        usbDesktop.reset();
        activeSync.reset();
    }
    DisarmMicrophone();
    // The partition table's own start, so another ROM's store isn't cut into.
    uint8_t mbr[S3cNand::pageSize];
    const uint32_t storeBlock = pages && pages->readPage(21 * S3cNand::pagesPerBlock, mbr)
        ? rx1950UserStoreBlockFromMbr(mbr) : rx1950UserStoreBlock;
    if (!machine->nand.eraseBlocks(storeBlock, S3cNand::blocks - storeBlock))
        std::cerr << "rx1950: hard reset couldn't erase the whole user store" << std::endl;
    machine->powerOnReset();
    ApplyHostInputs();
    StateChanged();
}

bool SocS3C2442::Save() {
    std::string why;
    if (halted || !machine->saveState(saved, why)) {
        std::cerr << "rx1950: not saved, " << (halted ? std::string("the machine has stopped") : why) << std::endl;
        return false;
    }
    return true;
}

bool SocS3C2442::Load(size_t savestateSize, void* savestateData) {
    // A machine started from the firmware has no state to load.
    if (!savestateSize || !savestateData) return true;
    std::string why;
    if (!machine->loadState(static_cast<const uint8_t*>(savestateData), savestateSize, why)) {
        std::cerr << "rx1950: saved state refused: " << why << std::endl;
        return false;
    }
    machine->nand.dropCache();
    if (!networkSimulation.Frozen()) {
        router.reset();
        usbDesktop.reset();
        activeSync.reset();
    }
    LoadCard();
    if (!RestoringCheckpoint()) DisarmMicrophone();
    // A session saved before sound worked has GPA1 as an address line, as the loader pins left it then; the
    // speaker amplifier needs it as an output (rx1950LoaderPins).
    if (!machine->stateHadSound) machine->gpio.con[S3cGpio::a] &= ~2u;
    // One saved before Wi-Fi has GPC8 and GPC9 as inputs; the loader leaves them outputs (rx1950LoaderPins).
    if (!machine->stateHadWifi) {
        machine->gpio.con[S3cGpio::c] = (machine->gpio.con[S3cGpio::c] & ~0xf0000u) | 0x50000;
        machine->wifiPinsChanged();
    }
    ApplyHostInputs();
    StateChanged();
    return true;
}

void SocS3C2442::DisarmMicrophone() {
    if (machine->microphoneFences) microphoneInvalidatePending = true;
    else { machine->disarmMicrophone(); microphoneInvalidatePending = false; }
}

void SocS3C2442::SetMicrophoneSource(const AudioInputSource* source) {
    machine->disarmMicrophone();
    machine->microphone = source;
    if (machine->microphoneFences && source && source->fence) source->fence(source->context, true);
    if (machine->asleep()) DisarmMicrophone();
}

// Fences the microphone while checkpointed guest work runs; a disarm asked for meanwhile happens at the end.
std::function<void()> SocS3C2442::CapturePeripheralInput() {
    struct Fence {
        SocS3C2442& soc;
        std::weak_ptr<int> lifetime;
        bool previous;
        explicit Fence(SocS3C2442& s) : soc(s), lifetime(s.checkpointLifetime), previous(s.microphoneInvalidatePending) {
            ++soc.checkpointFenceDepth;
            soc.machine->fenceMicrophone(true);
        }
        ~Fence() {
            if (lifetime.expired()) return;
            --soc.checkpointFenceDepth;
            soc.machine->fenceMicrophone(false);
            if (!soc.machine->microphoneFences && (soc.microphoneInvalidatePending || soc.machine->asleep()))
                soc.DisarmMicrophone();
        }
    };
    auto fence = std::make_shared<Fence>(*this);
    std::weak_ptr<int> lifetime = checkpointLifetime;
    return [this, fence, lifetime] {
        if (lifetime.expired()) return;
        microphoneInvalidatePending = fence->previous;
    };
}

bool SocS3C2442::SetBattery(const BatteryPower& value) {
    battery = value;
    if (machine) rx1950ApplyBattery(*machine, battery);
    return true;
}

// The charge and notification LED, stepped by the board's blinker in guest time. No vibrate motor.
SoC::Attention SocS3C2442::GuestAttention() {
    Attention attention;
    attention.ledColour = rx1950LedColour(*machine, machine->nanoseconds());
    return attention;
}

bool SocS3C2442::LcdEnabled() { return !machine->asleep() && machine->lcd.enabled(); }

// The panel as the LCD controller scans it, to the frontend's ABGR8888 (S3cLcdView). Black with the LCD off.
uint32_t* SocS3C2442::GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) {
    S3cLcd::Layout layout;
    std::string why;
    const bool on = LcdEnabled() && machine->lcd.layout(layout, why);
    // The panel takes a new frame about 60 times a guest second; scanning SDRAM much more often than that
    // only repeats the compare, so it's left until 1/120 s of guest time has passed. When guest time
    // stands still between two calls (paused, audio backpressure) it scans once, so the last writes show.
    const uint64_t now = machine->nanoseconds();
    const bool stopped = now == frameCallNs && now != scannedNs;
    frameCallNs = now;
    if (!redraw && !stopped && on == scannedOn && now >= scannedNs && now - scannedNs < 1000000000 / 120)
        return lcdView.take(firstDirtyLine, lastDirtyLine);
    scannedNs = now;
    scannedOn = on;
    lcdView.scan(displayConfiguration.width, displayConfiguration.height, on ? &layout : nullptr,
                 [&](uint32_t pa, size_t n) { return machine->sdramSpan(pa, n); });
    if (redraw) {
        redraw = false;
        ClearFramebufferDirty();
        lcdView.all();
    }
    return lcdView.take(firstDirtyLine, lastDirtyLine);
}

void SocS3C2442::JamKey(enum KeyId key, uint32_t durationMsec) {
    const auto* k = keyFor(key);
    if (!k || halted) return;
    keys.jam(*machine, k->port, k->bit, true);
    jams.push_back({key, machine->nanoseconds() + uint64_t(durationMsec) * 1000000});
}

// Hands queued host input to the machine in the guest's own time: keys pull their pins low through the
// key pacer, the pen goes to the touch panel through the pen pacer, at most one down or up each call.
// Events are stamped as they're taken here, which is within a frame of when the host made them.
void SocS3C2442::PumpInput() {
    const uint64_t now = machine->nanoseconds();
    for (auto it = jams.begin(); it != jams.end();) {
        if (it->untilNs > now) { ++it; continue; }
        if (const auto* k = keyFor(it->key)) keys.jam(*machine, k->port, k->bit, false);
        it = jams.erase(it);
    }
    const uint64_t at = inputGuestTime ? now : uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch()).count());
    while (keyEventQueue->GetSize()) {
        const KeyEvent event = keyEventQueue->Peek();
        keyEventQueue->Pop();
        if (const auto* k = keyFor(event.key)) keys.push(k->port, k->bit, event.keyDown, at);
    }
    while (penEventQueue->GetSize()) {
        const PenEvent event = penEventQueue->Peek();
        penEventQueue->Pop();
        const unsigned x = unsigned(std::clamp(event.x, 0, int(displayConfiguration.width) - 1));
        const unsigned y = unsigned(std::clamp(event.y, 0, int(displayConfiguration.height) - 1));
        pen.push(event.penDown, rx1950TouchRawX(x), rx1950TouchRawY(y), at);
    }
    keys.pump(*machine);
    pen.pump(*machine);
}

// Runs the guest for at least maxCycles / cyclesPerSecond of its own time, or for a short stretch of host
// time when it can't keep up, and returns the guest time that passed in the same cycle units. Each machine
// run covers up to sliceNs of guest time at the CPU clock it has when it starts.
uint64_t SocS3C2442::Run(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (halted || !cyclesPerSecond) {
        keyEventQueue->Clear();
        penEventQueue->Clear();
        return maxCycles;
    }
    const uint64_t before = machine->nanoseconds();
    const uint64_t wanted = std::max<uint64_t>(1, uint64_t(__uint128_t(maxCycles) * 1000000000 / cyclesPerSecond));
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        // Input goes in every slice, so pen and key times are kept in guest time.
        PumpInput();
        machine->setAccessPointOn(networkSimulation.GetMode() == NetworkSimulation::wireless);
        // A host peer attached since the last slice: its work starts at the next poll step.
        if (infraredPort) infraredPort->CheckMode();
        ScheduleHost(nextPoll(machine->nanoseconds()));
        const uint64_t hz = std::max<uint64_t>(CoreHz(), 1000000);
        const uint64_t left = before + wanted - std::min(before + wanted, machine->nanoseconds());
        const uint64_t ns = std::min(sliceNs, std::max<uint64_t>(left, 1));
        const auto stop = machine->run(std::max<uint64_t>(1, uint64_t(__uint128_t(ns) * hz / 1000000000)));
        if (stop != S3c2442Stop::budget) {
            halted = true;
            std::cerr << "rx1950: the machine stopped: " << machine->stopText() << ", after " << machine->retired()
                      << " instructions" << std::endl;
            break;
        }
        if (machine->nanoseconds() >= before + wanted) break;
        if (hostTimeLimit && std::chrono::steady_clock::now() - started >= runHostLimit) break;
        if (HostInputPending()) break;
    }
    runInstructions = machine->retired();
    if (machine->asleep() != wasAsleep) {
        wasAsleep = machine->asleep();
        if (wasAsleep) DisarmMicrophone();
    }
    if (halted) return maxCycles;
    const uint64_t elapsed = machine->nanoseconds() - before;
    const __uint128_t credit = __uint128_t(elapsed) * cyclesPerSecond + creditRemainder;
    creditRemainder = uint64_t(credit % 1000000000);
    return uint64_t(std::min<__uint128_t>(credit / 1000000000, UINT64_MAX));
}
