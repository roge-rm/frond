#ifndef FROND_HOST_UART_PORT_H
#define FROND_HOST_UART_PORT_H
#include <cstdint>

#include "SerialHostRoute.h"

// What HostSerial and HostInfrared need from a board's UART, so the same host endpoints serve the
// PXA's UARTs and the i.MX1's. Every call runs on the emulation thread between slices.

// Modem inputs as asserted levels, the same bits as PXA_UART_CTS and the rest.
enum : uint8_t { hostUartCts = 0x10, hostUartDsr = 0x20, hostUartRi = 0x40, hostUartDcd = 0x80 };
// What a read callback gives when nothing is waiting (UART_CHAR_NONE).
constexpr uint_fast16_t hostUartNone = 0x100;

struct HostUartMode {
    uint32_t baud{0};    // bits a second
    uint32_t format{0};  // the board's frame format bits; only compared
    bool receive{false}, transmit{false};
};

// The serial port of a board.
class HostUartPort {
   public:
    using Read = uint_fast16_t (*)(void*);
    using Write = void (*)(uint_fast16_t, void*);
    using Active = bool (*)(void*);
    using Detached = void (*)(void*);
    virtual ~HostUartPort() = default;
    // The host as the UART's peer. False while the UART can't take one just now (busy, frozen or in
    // loopback); try again later. detached runs when a reset or load takes the peer off.
    virtual bool Attach(Read read, Write write, Active active, Detached detached, void* context) = 0;
    virtual void Detach() = 0;
    // The host's modem inputs (hostUart* bits). False when the UART can't take them now.
    virtual bool SetInputs(uint8_t asserted) = 0;
    // The guest takes more characters: RTS out on the PXA, CTS out on the i.MX1.
    virtual bool WantsInput() const = 0;
    virtual bool ReceiverEnabled() const = 0;
    // Counts the guest turning the UART off, so the host drops what was waiting across one. Not saved.
    virtual uint32_t DisableCount() const = 0;
    // Bytes are waiting: wake the receiver.
    virtual void Kick() = 0;
    virtual HostUartMode Mode() const = 0;
};

// The infrared port of a board: the transceiver behind its UART, fed through SerialHostHooks.
class HostInfraredPort {
   public:
    virtual ~HostInfraredPort() = default;
    // Guest time in ns.
    virtual uint64_t Time() = 0;
    // One peer at a time; null takes it off. Reset and load call hooks->replaced.
    virtual bool SetHooks(const SerialHostHooks* hooks) = 0;
    // The transceiver is powered.
    virtual bool PowerEnabled() = 0;
    virtual bool ReceiveEnabled() = 0;
    virtual HostUartMode Mode() = 0;
    virtual void Kick() = 0;
    // Host I/O held off (a USB host replacing the board, for one).
    virtual bool HostIoPaused() = 0;
    virtual bool Sleeping() = 0;
    // An infrared keyboard holds the port.
    virtual bool KeyboardAttached() = 0;
};
#endif
