#ifndef FROND_BLUETOOTH_BACKEND_H
#define FROND_BLUETOOTH_BACKEND_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// User-selected H4 controller endpoint. It owns live host I/O only; no descriptor,
// partial host packet or peer generation is portable guest state. Emulator thread.
class BluetoothBackend {
 public:
    static constexpr size_t maxFrame=65540, capacity=1024*1024;
    BluetoothBackend()=default;
    ~BluetoothBackend(){Close();}
    BluetoothBackend(const BluetoothBackend&)=delete;
    BluetoothBackend& operator=(const BluetoothBackend&)=delete;
    bool Open(const std::string& path,std::string& error);
    // The machine's own controller address, least significant byte first. Each Open
    // programs it with Write_BD_ADDR (0xfc01) before any guest command, and drops
    // that command's completion, so peers see the same address on every connection.
    // A Write_BD_ADDR from the guest (the T|X's factory address, the Zire's own) becomes it.
    void SetAddress(const std::array<uint8_t,6>& value){localAddress=value;hasAddress=true;}
    bool Address(std::array<uint8_t,6>& value) const {value=localAddress;return hasAddress;}
    void Close();
    bool Connected() const {return fd>=0;}
    bool Write(const uint8_t* data,size_t size);
    void Poll(bool receive = true);
    uint16_t Read(); // 0x100 means no byte, same as UART_CHAR_NONE.
    bool FrameInProgress() const {return frameRemaining!=0;}
    size_t Pending() const {return received.size()-readAt;}
    size_t Queued() const {return outgoing.size()-sentAt;}
 private:
    int fd{-1};
    std::vector<uint8_t> incoming,outgoing,received;
    size_t expected{},sentAt{},readAt{},frameRemaining{};
    std::array<uint8_t,6> localAddress{};
    bool hasAddress{},addressReplyDue{};
    bool AddressReply() const;
    bool Consume(const uint8_t* bytes,size_t size);
};
#endif
