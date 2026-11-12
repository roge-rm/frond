#ifndef FROND_IMX1_I2C_H
#define FROND_IMX1_I2C_H

#include <cstdint>

enum class Imx1I2cAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };
enum class Imx1I2cPhase { start, repeatedStart, address, transmit, receive, stop };
enum class Imx1I2cResponse { unknown, ack, nack, arbitrationLost };
struct Imx1I2cTransfer {
    Imx1I2cPhase phase = Imx1I2cPhase::start;
    uint8_t address = 0, byte = 0;
    bool read = false, acknowledge = false;
};
struct Imx1I2cReply {
    Imx1I2cResponse response = Imx1I2cResponse::unknown;
    uint8_t byte = 0;
};
// Single master, initially idle, no clock stretching. Missing slaves have unknown responses.
// A bus implementation supplies a measured/modelled response, including an explicit NACK.
class StateWriter;
class StateReader;
class Imx1I2cBus {
   public:
    virtual ~Imx1I2cBus() = default;
    virtual void signal(const Imx1I2cTransfer&) {}
    // Receive ACK means data available; transfer.acknowledge is the master's wire response.
    virtual Imx1I2cReply transfer(const Imx1I2cTransfer&) = 0;
    // For a machine state. The identity names the device and its fixed contents, and a state only
    // loads onto a bus with the same one. 0 means the bus can't be saved.
    virtual uint32_t stateIdentity() const { return 0; }
    virtual void saveState(StateWriter&) const {}
    // Checks the whole state and, with commit, takes it. A refused state leaves the bus as it was.
    virtual bool loadState(StateReader&, bool commit) { (void)commit; return true; }
};
using Imx1I2cObserver = void (*)(void*, const Imx1I2cTransfer&, Imx1I2cResponse);

class Imx1I2c {
   public:
    void setSingleMasterScenario(bool enabled) { singleMaster = enabled; }
    void setBus(Imx1I2cBus* source) { bus = source; }
    void setObserver(Imx1I2cObserver callback, void* context) { observer = callback; observerContext = context; }
    Imx1I2cAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    void advance(uint64_t hclkCycles);
    bool interruptOutput() const { return (control & 0xc0) == 0xc0 && (status & 2); }
    bool active() const { return control & 0x20; }
    Imx1I2cAccess clockStatus() const { return failure; }
    // HCLK cycles until the transfer in flight completes.
    uint64_t idleQuietHclk() const {
        if (!inFlight || failure != Imx1I2cAccess::ok) return UINT64_MAX;
        return remaining ? remaining : 1;
    }
    uint32_t clockDivider() const;
    uint32_t controlValue() const { return control; }
    uint32_t statusValue() const { return status; }
    uint32_t dividerValue() const { return frequency; }
    const Imx1I2cTransfer& pendingTransfer() const { return pending; }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        // The bus and the observer belong to the machine and are bound again after a load.
        a.Do8(ownAddress).Do8(frequency).Do8(control).Do8(status).Do8(data);
        a.FixedBool(singleMaster).DoBool(awaitingAddress).DoBool(addressed).DoBool(readDirection);
        a.DoBool(inFlight).DoBool(receivedNack).Do64(remaining);
        a.DoEnum(pending.phase, 6).Do8(pending.address).Do8(pending.byte).DoBool(pending.read).DoBool(pending.acknowledge);
        a.DoEnum(failure, 6);
    }
    static const char* registerName(uint32_t address);

   private:
    uint8_t ownAddress = 0, frequency = 0, control = 0, status = 0x81, data = 0;
    bool singleMaster = false, awaitingAddress = false, addressed = false, readDirection = false;
    bool inFlight = false, receivedNack = false;
    uint64_t remaining = 0;
    Imx1I2cTransfer pending{};
    Imx1I2cAccess failure = Imx1I2cAccess::ok;
    Imx1I2cBus* bus = nullptr;
    Imx1I2cObserver observer = nullptr;
    void* observerContext = nullptr;
    void notify(const Imx1I2cTransfer&, Imx1I2cResponse) const;
    void launch(Imx1I2cTransfer transfer);
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1I2c) == 64, "update state() and stateVersion");
#endif
const char* imx1I2cPhaseName(Imx1I2cPhase);
const char* imx1I2cResponseName(Imx1I2cResponse);

#endif
