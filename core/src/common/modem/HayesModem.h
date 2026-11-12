#ifndef FROND_HAYES_MODEM_H
#define FROND_HAYES_MODEM_H

// An external Hayes-compatible data modem on a guest serial port. The core feeds it what the
// guest sends (Write), takes what the modem sends back (Read) and calls Service between slices.
// A dialled call connects to a ModemLine, the far end of the phone line. Single-threaded; every
// time is guest nanoseconds, so guard times and pacing follow the guest clock.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace frondModem {

// The far end of a call.
class ModemLine {
   public:
    virtual ~ModemLine() = default;
    // The call is up and the modems have trained.
    virtual void Answer(uint64_t now) = 0;
    // The modem hung up.
    virtual void HangUp() = 0;
    // Data from the guest, already paced at the line speed.
    virtual void FromModem(const uint8_t* data, size_t size, uint64_t now) = 0;
    // Up to max bytes for the guest; the modem asks no faster than the line speed.
    virtual size_t ToModem(uint8_t* data, size_t max, uint64_t now) = 0;
    // False once the far end has hung up.
    virtual bool Carrier(uint64_t now) const = 0;
    virtual void Service(uint64_t now) {}
    // Called when a number is dialled, with its digits (0-9, *, #, A-D). False gives NO ANSWER.
    virtual bool Route(const std::string& digits) { return true; }
};

class HayesModem {
   public:
    enum class Result { Ok = 0, Connect = 1, Ring = 2, NoCarrier = 3, Error = 4, NoDialtone = 6, Busy = 7, NoAnswer = 8 };
    enum class State { Off, Command, Dialling, Online, OnlineCommand };

    struct Options {
        // Line speed in bits a second, for CONNECT and for pacing data both ways.
        unsigned lineSpeed = 33600;
        // Ringing and training after the last digit, before CONNECT.
        uint64_t handshakeNs = 6000000000ull;
        // Dialled digits that don't connect, with what they give instead.
        std::map<std::string, Result> dialPlan;
    };

    explicit HayesModem(ModemLine& line);
    HayesModem(ModemLine& line, Options options);

    // The serial line driver: off is an unpowered modem, which hangs up and forgets its settings.
    void SetPower(bool on, uint64_t now);
    void SetDTR(bool on, uint64_t now);
    // Bytes from the guest's TXD.
    void Write(const uint8_t* data, size_t size, uint64_t now);
    // Bytes for the guest's RXD.
    size_t Read(uint8_t* data, size_t max);
    size_t Available() const { return toGuest.size(); }
    void Service(uint64_t now);
    // After a session load: an idle modem, with NO CARRIER if a call was up.
    void Drop(uint64_t now);
    // For a session save: the guest's settings as text while off hook (a call up, dialling or
    // holding the line), or empty.
    std::string SavedCall() const;
    // After loading a session saved off hook: the line has dropped. The modem is powered with
    // the saved settings, in command mode, and the guest gets NO CARRIER, as from a real modem
    // whose call went while the Palm wasn't looking. False, and nothing changed, for text
    // SavedCall didn't write.
    bool LineDropped(const std::string& saved, uint64_t now);

    State GetState() const { return state; }
    bool Dtr() const { return dtr; }
    unsigned LineSpeed() const { return options.lineSpeed; }
    // Off hook: dialling, in a call or holding the line after a dial ending in ';'.
    bool OffHook() const { return state != State::Off && (state != State::Command || holding); }

   private:
    void Reset();
    void Respond(Result result);
    void Text(const std::string& text);
    void Info(const std::string& text);
    void Execute(const std::string& line, uint64_t now);
    void Dial(const std::string& number, uint64_t now);
    void Connect(uint64_t now);
    void EndCall(bool report);
    void CommandByte(uint8_t byte, uint64_t now);
    void OnlineByte(uint8_t byte, uint64_t now);
    void Pace(uint64_t now);
    uint64_t Guard() const;

    ModemLine& line;
    Options options;
    State state{State::Off};
    bool dtr{true};

    // Settings that ATZ and &F restore.
    std::array<uint8_t, 256> s{};
    bool echo{true}, quiet{false}, verbose{true};
    unsigned x{4}, dtrMode{2};
    bool pulse{false};
    unsigned selected{0};

    std::string command, lastCommand;
    std::string lastNumber;
    bool holding{false};
    uint8_t previous{0};

    // Dialling.
    uint64_t dialEnd{0}, dialLimit{0};
    Result dialResult{Result::Connect};
    bool dialStay{false};

    // Escape: the time of the last guest byte while online and how many escape characters
    // have come in a row, each within the guard time.
    uint64_t lastByte{0};
    unsigned escapes{0};
    uint64_t escapeAt{0};

    // Bytes waiting in each direction; line pacing moves them.
    std::deque<uint8_t> toGuest, toLine;
    uint64_t paceMark{0};
    double toGuestCredit{0}, toLineCredit{0};
};

}  // namespace frondModem

#endif
