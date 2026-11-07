// Messages carried in records. Plaintext of a record: type u8, channel u16, payload.
// Channel 0 is the link itself; channels the initiator opens are odd, the responder's even.
#pragma once

#include "bytes.h"

#include <string>

namespace frond::link {

constexpr size_t kMaxPayload = 16384;
constexpr size_t kMessageHeader = 3;

enum class MsgType : uint8_t {
    Hello = 1,    // u16 min version, u16 max version, name
    Open = 2,     // u8 kind, machine, device
    Accept = 3,   // machine, device of the side that joined
    Waiting = 4,  // no machine is set to this link yet; the opener is held
    Busy = 5,     // machine, device already using that kind
    Data = 6,     // bytes
    Credit = 7,   // u32 bytes the sender may add
    Close = 8,    // u8 reason
    State = 9,    // u8 PeerState
    Ping = 10,    // u64 sender's clock in ms
    Pong = 11,    // the ping's u64 echoed
};

enum class ChannelKind : uint8_t { Infrared = 1, Serial = 2, Bluetooth = 3 };

enum class CloseReason : uint8_t {
    Normal = 0,
    Version = 1,
    Protocol = 2,
    Timeout = 3,
    Gone = 4,  // the machine stopped or changed its setting
};

enum class PeerState : uint8_t { Running = 0, Asleep = 1, Paused = 2, Reset = 3 };

struct Message {
    MsgType type = MsgType::Data;
    uint16_t channel = 0;
    Bytes payload;
};

Bytes encodeMessage(const Message& m);
bool decodeMessage(const uint8_t* p, size_t n, Message& out);

struct HelloBody {
    uint16_t minVersion = 1;
    uint16_t maxVersion = 1;
    std::string name;
};
Bytes encodeHello(const HelloBody& h);
bool decodeHello(const Bytes& p, HelloBody& out);

struct OpenBody {
    ChannelKind kind = ChannelKind::Infrared;
    std::string machine;
    std::string device;
};
Bytes encodeOpen(const OpenBody& o);
bool decodeOpen(const Bytes& p, OpenBody& out);

// Accept and Busy name a machine.
struct MachineBody {
    std::string machine;
    std::string device;
};
Bytes encodeMachine(const MachineBody& m);
bool decodeMachine(const Bytes& p, MachineBody& out);

}  // namespace frond::link
