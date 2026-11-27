#include "Dcf660w.h"
#include "machine_state_archive.h"
#include "PalmPrismFrame.h"
#include "wifi_access_point.h"

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <type_traits>

namespace {
constexpr uint32_t optionAddress = 0x3e0;
constexpr uint8_t resetBit = 0x80, optionIndex = 1;
constexpr uint16_t eventReceive = 0x0001, eventTransmit = 0x0002, eventAllocate = 0x0008;
constexpr uint16_t eventCommand = 0x0010, eventInfo = 0x0080;
constexpr uint16_t infoFid = 0x0200, receiveFid = 0x0201;
constexpr uint16_t firmwareDisabled = 1, firmwareListening = 2, firmwareAssociated = 4;
constexpr uint8_t stationPort = 1, openAuthentication = 1;
constexpr size_t maxInfoQueue = 8, maxReceiveQueue = 32;
constexpr unsigned maxReceiveBytes = 1568, maxInfoBytes = 1992;
constexpr uint16_t minBufferSize = 4, maxBufferSize = 2400;
constexpr uint16_t queuedEvents = eventReceive | eventTransmit | eventAllocate | eventInfo;
constexpr uint16_t bapError = 0x4000;
constexpr uint16_t firstFid = 0x0100, bufferCount = 16;
constexpr uint16_t firstConfigRid = 0xfc00, pastConfigRid = 0xfd00;
constexpr uint16_t maxRecordSize = 1024;

// Stock wlan-ng.conf binds this MANFID to prism2_cs. The card advertises a
// 64-port I/O window and level IRQ in its single 3.3 V configuration.
constexpr uint8_t cis[] = {
    0x01, 3, 0xdb, 0x01, 0xff,
    0x15, 20, 4, 1, 'D', '-', 'L', 'i', 'n', 'k', 0,
                    'D', 'C', 'F', '-', '6', '6', '0', 'W', 0, 0, 0xff,
    0x20, 4, 0x01, 0xd6, 0x05, 0x00,
    0x21, 2, 6, 0,
    0x1a, 5, 1, optionIndex, uint8_t(optionAddress), uint8_t(optionAddress >> 8), 1,
    0x1b, 8, 0xc1, 0x41, 0x19, 1, 0xb5, 0x1e, 0x46, 0x20,
    0x14, 0,
    0xff,
};

uint16_t Word(const std::vector<uint8_t>& bytes, size_t offset) {
    return uint16_t(bytes[offset] | (uint16_t(bytes[offset + 1]) << 8));
}

void AppendWord(std::vector<uint8_t>& bytes, uint16_t word) {
    bytes.push_back(uint8_t(word));
    bytes.push_back(uint8_t(word >> 8));
}

std::vector<uint8_t> Words(std::initializer_list<uint16_t> words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(2 * words.size());
    for (const auto word : words) AppendWord(bytes, word);
    return bytes;
}

std::string_view Ssid(const std::vector<uint8_t>& record, size_t offset) {
    if (record.size() < offset + 2) return {};
    const auto length = Word(record, offset);
    if (length > 32 || offset + 2 + length > record.size()) return {};
    return {reinterpret_cast<const char*>(record.data() + offset + 2), length};
}

std::vector<uint8_t> FrondSsid() {
    std::vector<uint8_t> record(34);
    const std::string_view name = WifiAccessPoint::ssid;
    record[0] = uint8_t(name.size());
    std::copy(name.begin(), name.end(), record.begin() + 2);
    return record;
}
}  // namespace

Dcf660w::Dcf660w() { Reset(); }

void Dcf660w::Reset() {
    option = 0;
    InitializeFirmware();
}

void Dcf660w::InitializeFirmware() {
    status = events = interruptMask = recordId = 0;
    nextFid = firstFid;
    params = softwareSupport = {};
    baps = {};
    enabled = associated = false;
    buffers.clear();
    allocations.clear();
    transmissions.clear();
    receiveFrames.clear();
    infoFrames.clear();
    recordWindow.clear();
    // Synthetic firmware identities and compatibility records for the Sharp 1.01 driver.
    records = {
        {0xfc00, Words({1})},                                // Infrastructure port
        {0xfc01, {0x02, 0, 0, 0, 0, 3}},                     // Locally administered MAC
        {0xfc02, std::vector<uint8_t>(34)},                   // Desired SSID
        {0xfc03, Words({6})},                                // Channel
        {0xfc04, std::vector<uint8_t>(34)},                   // Own SSID
        {0xfc07, Words({2304})},
        {0xfc2a, Words({openAuthentication})},              // Authentication type
        {0xfc2d, Words({1})},                               // Roaming mode
        {0xfc81, Words({0})},                               // Create IBSS
        {0xfc84, Words({15})},                              // Transmit rates
        {0xfcba, Words({0})},                               // Passive scan dwell time
        {0xfd02, Words({0x15, 0, 1, 1})},                    // Primary identity
        {0xfd03, Words({0, 3, 1, 1, 4})},                    // Primary supplier range
        {0xfd04, Words({1, 2, 1, 1, 4})},                    // Primary CFI actor range
        {0xfd0a, {'F', 'R', 'O', 'N', 'D', 'D', 'C', 'F', '6', '6', '0', 'W'}},
        {0xfd0b, Words({0x8013, 0, 1, 0})},                  // NIC identity
        {0xfd0c, Words({0, 1, 1, 1, 1})},                    // MFI supplier range
        {0xfd0d, Words({0, 2, 1, 1, 4})},                    // CFI supplier range
        {0xfd20, Words({0x1f, 4, 1, 8})},                    // Station identity
        {0xfd21, Words({0, 4, 1, 1, 9})},                    // Station supplier range
        {0xfd22, Words({1, 1, 1, 1, 1})},                    // Station MFI actor range
        {0xfd23, Words({1, 2, 1, 1, 4})},                    // Station CFI actor range
        {0xfd4f, Words({0})},                                // Privacy capability
    };
}

bool Dcf660w::IoConfigured() const { return (option & (resetBit | 0x3f)) == optionIndex; }

uint8_t Dcf660w::ReadAttribute(uint32_t address) const {
    if (address == optionAddress) return option;
    if (InReset()) return 0xff;
    const uint32_t index = address / 2;
    return !(address & 1) && index < sizeof(cis) ? cis[index] : 0xff;
}

void Dcf660w::WriteAttribute(uint32_t address, uint8_t value) {
    if (address != optionAddress) return;
    if ((value & resetBit) && !(option & resetBit)) InitializeFirmware();
    option = value;
}

bool Dcf660w::IrqAsserted() const { return IoConfigured() && (events & interruptMask); }

uint16_t Dcf660w::ReadIo(uint32_t address) {
    if (!IoConfigured()) return 0xffff;
    switch (address & 0x3f) {
        case 0x00: return status & 0x003f;
        case 0x02: case 0x04: case 0x06: return params[((address & 7) - 2) / 2];
        case 0x08: return status;
        case 0x0a: case 0x0c: case 0x0e: return 0;
        case 0x18: case 0x1a: return baps[(address >> 1) & 1].select;
        case 0x1c: case 0x1e: return baps[(address >> 1) & 1].offset;
        case 0x10: return infoFrames.empty() ? 0 : infoFid;
        case 0x20: return receiveFrames.empty() ? 0 : receiveFid;
        case 0x22: return allocations.empty() ? 0 : allocations.front();
        case 0x24: return transmissions.empty() ? 0 : transmissions.front();
        case 0x28: return softwareSupport[0];
        case 0x2a: return softwareSupport[1];
        case 0x2c: return softwareSupport[2];
        case 0x30: return events;
        case 0x32: return interruptMask;
        case 0x36: return ReadBap(baps[0]);
        case 0x38: return ReadBap(baps[1]);
        default: return 0xffff;
    }
}

void Dcf660w::WriteIo(uint32_t address, uint16_t value) {
    if (!IoConfigured()) return;
    switch (address & 0x3f) {
        case 0x00: CompleteCommand(value); break;
        case 0x02: case 0x04: case 0x06: params[((address & 7) - 2) / 2] = value; break;
        case 0x18: case 0x1a: baps[(address >> 1) & 1].select = value; break;
        case 0x1c: case 0x1e: {
            auto& bap = baps[(address >> 1) & 1];
            bap.offset = value & 0x0ffe;
            if (value & ~0x0ffe) bap.offset |= bapError;
            if (!Window(bap.select) && (bap.select < firstConfigRid || bap.select >= pastConfigRid))
                bap.offset |= bapError;
            break;
        }
        case 0x28: softwareSupport[0] = value; break;
        case 0x2a: softwareSupport[1] = value; break;
        case 0x2c: softwareSupport[2] = value; break;
        case 0x32: interruptMask = value; break;
        case 0x34: Acknowledge(value); break;
        case 0x36: WriteBap(baps[0], value); break;
        case 0x38: WriteBap(baps[1], value); break;
    }
}

void Dcf660w::CompleteCommand(uint16_t command) {
    bool ok = true;
    const uint16_t code = command & 0x003f;
    switch (code) {
        case 0x00: InitializeFirmware(); break;
        case 0x01: case 0x02:
            ok = (command & 0x0700) == 0;  // Only MAC port zero.
            if (ok) {
                enabled = code == 1;
                EvaluateLink();
            }
            break;
        case 0x0a: ok = Allocate(params[0]); break;
        case 0x0b: ok = Transmit(params[0], command & 0x0100); break;
        case 0x11:
            if (params[0] == 0xf100) QueueInfo(0xf100, std::vector<uint8_t>(84));
            else if (params[0] == 0xf200) QueueInfo(0xf200, Words({uint16_t(associated ? 1 : 2)}));
            else ok = false;
            break;
        case 0x21: ok = AccessRecord(params[0], command & 0x0100); break;
        default: ok = false; break;
    }
    status = code | (ok ? 0 : 0x7f00);
    events |= eventCommand;
}

bool Dcf660w::Allocate(uint16_t size) {
    if (size < minBufferSize || size > maxBufferSize || nextFid == firstFid + bufferCount) return false;
    const auto id = nextFid++;
    allocations.push_back(id);
    buffers.emplace(id, std::vector<uint8_t>((size + 1) & ~1u));
    events |= eventAllocate;
    return true;
}

bool Dcf660w::AccessRecord(uint16_t id, bool write) {
    if (write) {
        if (id < firstConfigRid || id >= pastConfigRid || id != recordId || recordWindow.size() < 4 ||
            Word(recordWindow, 2) != id || 2u * (1u + Word(recordWindow, 0)) != recordWindow.size())
            return false;
        std::vector<uint8_t> value(recordWindow.begin() + 4, recordWindow.end());
        const auto current = records.find(id);
        if (current != records.end() && value.size() != current->second.size()) return false;
        if (id == 0xfce5 && value.size() != 38) return false;
        records[id] = std::move(value);
        if (id == 0xfce5) Scan(records[id]);
        return true;
    }
    const auto bytes = Record(id);
    if (bytes.empty()) return false;
    recordWindow = Words({uint16_t(1 + bytes.size() / 2), id});
    recordWindow.insert(recordWindow.end(), bytes.begin(), bytes.end());
    recordId = id;
    return true;
}

std::vector<uint8_t>* Dcf660w::Window(uint16_t id) {
    if (id == recordId && !recordWindow.empty()) return &recordWindow;
    if (id == receiveFid && !receiveFrames.empty()) return &receiveFrames.front();
    if (id == infoFid && !infoFrames.empty()) return &infoFrames.front();
    const auto it = buffers.find(id);
    return it == buffers.end() ? nullptr : &it->second;
}

uint16_t Dcf660w::ReadBap(Bap& bap) {
    const auto* bytes = Window(bap.select);
    if (!bytes || size_t(bap.offset) + 2 > bytes->size()) {
        bap.offset |= bapError;
        return 0xffff;
    }
    const auto value = Word(*bytes, bap.offset);
    bap.offset += 2;
    return value;
}

void Dcf660w::WriteBap(Bap& bap, uint16_t value) {
    if (bap.offset & bapError) return;
    auto* bytes = Window(bap.select);
    if (bap.select >= firstConfigRid && bap.select < pastConfigRid) {
        if (bap.offset == 0) {
            recordId = bap.select;
            recordWindow.clear();
            bytes = &recordWindow;
        }
        if (bytes && size_t(bap.offset) + 2 <= maxRecordSize && bap.offset == bytes->size())
            bytes->resize(bap.offset + 2);
    } else if (bap.select == recordId || bap.select == infoFid || bap.select == receiveFid) {
        bap.offset |= bapError;
        return;
    }
    if (!bytes || size_t(bap.offset) + 2 > bytes->size()) {
        bap.offset |= bapError;
        return;
    }
    (*bytes)[bap.offset] = uint8_t(value);
    (*bytes)[bap.offset + 1] = uint8_t(value >> 8);
    bap.offset += 2;
}

void Dcf660w::Acknowledge(uint16_t mask) {
    const uint16_t pending = mask & events;
    events &= ~mask;
    if ((pending & eventReceive) && !receiveFrames.empty()) receiveFrames.pop_front();
    if ((pending & eventTransmit) && !transmissions.empty()) transmissions.pop_front();
    if ((pending & eventAllocate) && !allocations.empty()) allocations.pop_front();
    if ((pending & eventInfo) && !infoFrames.empty()) infoFrames.pop_front();
    if (!receiveFrames.empty()) events |= eventReceive;
    if (!transmissions.empty()) events |= eventTransmit;
    if (!allocations.empty()) events |= eventAllocate;
    if (!infoFrames.empty()) events |= eventInfo;
}

void Dcf660w::SetNetworkAvailable(bool available) {
    if (networkAvailable == available) return;
    networkAvailable = available;
    EvaluateLink();
}

void Dcf660w::EvaluateLink() {
    const auto& desired = records.at(0xfc02);
    const auto name = Ssid(desired, 0);
    const bool nameMatches = Word(desired, 0) == 0 || name == WifiAccessPoint::ssid;
    const bool linked = enabled && networkAvailable && nameMatches &&
                        Word(records.at(0xfc00), 0) == stationPort &&
                        Word(records.at(0xfc2a), 0) == openAuthentication;
    if (associated == linked) return;
    associated = linked;
    if (!linked) {
        receiveFrames.clear();
        events &= ~eventReceive;
    }
    QueueInfo(0xf200, Words({uint16_t(linked ? 1 : 2)}));
}

void Dcf660w::QueueInfo(uint16_t type, const std::vector<uint8_t>& body) {
    if (infoFrames.size() >= maxInfoQueue) return;
    auto frame = Words({uint16_t(1 + body.size() / 2), type});
    frame.insert(frame.end(), body.begin(), body.end());
    infoFrames.push_back(std::move(frame));
    events |= eventInfo;
}

void Dcf660w::Scan(const std::vector<uint8_t>& request) {
    const bool hit = enabled && networkAvailable && (Word(request, 0) & (1u << (WifiAccessPoint::apChannel - 1))) &&
                     (Word(request, 4) == 0 || Ssid(request, 4) == WifiAccessPoint::ssid);
    auto body = Words({uint16_t(hit), 0});
    if (hit) {
        AppendWord(body, WifiAccessPoint::apChannel);
        AppendWord(body, 27);
        AppendWord(body, 80);
        body.insert(body.end(), std::begin(WifiAccessPoint::bssid), std::end(WifiAccessPoint::bssid));
        AppendWord(body, 100);
        AppendWord(body, 1);  // Infrastructure BSS, open authentication.
        const auto name = FrondSsid();
        body.insert(body.end(), name.begin(), name.end());
        const uint8_t rates[10] = {0x82, 0x84, 0x0b, 0x16};
        body.insert(body.end(), std::begin(rates), std::end(rates));
        AppendWord(body, 2);  // Probe response at 1 Mbps, measured in 500 kbit/s units.
        AppendWord(body, 0);  // No ATIM window.
    }
    QueueInfo(0xf103, body);
}

std::vector<uint8_t> Dcf660w::Record(uint16_t id) const {
    switch (id) {
        case 0xfd40: return Words({!enabled ? firmwareDisabled : associated ? firmwareAssociated : firmwareListening});
        case 0xfd41: return associated ? FrondSsid() : std::vector<uint8_t>(34);
        case 0xfd42:
            if (associated) return {std::begin(WifiAccessPoint::bssid), std::end(WifiAccessPoint::bssid)};
            return std::vector<uint8_t>(6);
        case 0xfd43: return associated ? Words({53, 80, 27}) : std::vector<uint8_t>(6);
        case 0xfd44: return Words({uint16_t(associated ? 8 : 0)});
        case 0xfdc1: return Words({uint16_t(associated ? WifiAccessPoint::apChannel : 0)});
        default: {
            const auto it = records.find(id);
            return it == records.end() ? std::vector<uint8_t>{} : it->second;
        }
    }
}

bool Dcf660w::Transmit(uint16_t id, bool reclaim) {
    const auto it = buffers.find(id);
    if (!associated || it == buffers.end() || transmissions.size() >= bufferCount ||
        (reclaim && allocations.size() >= bufferCount)) return false;
    const auto& configuredMac = records.at(0xfc01);
    std::array<uint8_t, 6> mac{};
    std::copy_n(configuredMac.begin(), mac.size(), mac.begin());
    auto frame = palmPrismFrame::Transmit(it->second, mac);
    if (frame.empty()) return false;
    transmissions.push_back(id);
    events |= eventTransmit;
    if (reclaim) {
        allocations.push_back(id);
        events |= eventAllocate;
    }
    if (transmitFrame) transmitFrame(frame);
    return true;
}

bool Dcf660w::ReceiveFrame(const std::vector<uint8_t>& frame) {
    if (!Associated() || frame.size() < 14 || receiveFrames.size() >= maxReceiveQueue) return false;
    if (!(frame[0] & 1) && !std::equal(frame.begin(), frame.begin() + 6, records.at(0xfc01).begin()))
        return false;
    std::array<uint8_t, 6> bssid{};
    std::copy_n(WifiAccessPoint::bssid, bssid.size(), bssid.begin());
    auto descriptor = palmPrismFrame::Receive(frame, bssid);
    if (descriptor.empty()) return false;
    receiveFrames.push_back(std::move(descriptor));
    events |= eventReceive;
    return true;
}

namespace {
constexpr uint16_t snapshotVersion = 1;

template <class Archive>
void TransferRecords(Archive& state, std::map<uint16_t, std::vector<uint8_t>>& entries,
                     unsigned limit, unsigned maxBytes) {
    auto count = uint16_t(entries.size());
    state.Do16(count).Require(count <= limit, "Prism record count");
    if constexpr (Archive::loading) {
        entries.clear();
        for (unsigned index = 0; state.ok() && index < count; ++index) {
            uint16_t id = 0;
            std::vector<uint8_t> bytes;
            state.Do16(id).DoVector(bytes, maxBytes);
            if (state.ok()) state.Require(entries.emplace(id, std::move(bytes)).second, "Duplicate Prism record");
        }
    } else {
        for (auto& [key, bytes] : entries) {
            uint16_t id = key;
            state.Do16(id).DoVector(bytes, maxBytes);
        }
    }
}

template <class Archive, class Item>
void TransferQueue(Archive& state, std::deque<Item>& queue, unsigned limit, unsigned maxBytes = 0) {
    auto count = uint16_t(queue.size());
    state.Do16(count).Require(count <= limit, "Prism queue length");
    if (!state.ok()) return;
    if constexpr (Archive::loading) queue.resize(count);
    for (auto& item : queue) {
        if constexpr (std::is_same_v<Item, uint16_t>) state.Do16(item);
        else state.DoVector(item, maxBytes);
    }
}
}  // namespace

template <class Archive>
void Dcf660w::Transfer(Archive& state) {
    auto version = snapshotVersion;
    state.Fixed16(version).Do8(option).DoBool(enabled).DoBool(associated).DoBool(networkAvailable)
        .Do16(status).Do16(events).Do16(interruptMask).Do16(nextFid).Do16(recordId)
        .DoBuffer16(params.data(), params.size()).DoBuffer16(softwareSupport.data(), softwareSupport.size());
    for (auto& bap : baps) state.Do16(bap.select).Do16(bap.offset);
    state.DoVector(recordWindow, maxRecordSize);
    TransferRecords(state, buffers, bufferCount, maxBufferSize);
    TransferRecords(state, records, 272, maxRecordSize);
    TransferQueue(state, allocations, bufferCount);
    TransferQueue(state, transmissions, bufferCount);
    TransferQueue(state, receiveFrames, maxReceiveQueue, maxReceiveBytes);
    TransferQueue(state, infoFrames, maxInfoQueue, maxInfoBytes);
}

std::vector<uint8_t> Dcf660w::Save() {
    std::vector<uint8_t> bytes;
    StateWriter writer(bytes);
    Transfer(writer);
    return bytes;
}

bool Dcf660w::Load(const uint8_t* bytes, size_t size) {
    if (!bytes || size > maxSnapshotBytes) return false;
    Dcf660w candidate;
    const auto defaults = candidate.records;
    StateReader reader(bytes, size);
    candidate.Transfer(reader);
    if (!reader.complete()) return false;

    const uint16_t expectedEvents = (candidate.allocations.empty() ? 0 : eventAllocate) |
                                    (candidate.transmissions.empty() ? 0 : eventTransmit) |
                                    (candidate.receiveFrames.empty() ? 0 : eventReceive) |
                                    (candidate.infoFrames.empty() ? 0 : eventInfo);
    if ((candidate.events & queuedEvents) != expectedEvents ||
        (candidate.events & ~(queuedEvents | eventCommand)) || (candidate.status & ~0x7f3f) ||
        (candidate.associated && (!candidate.enabled || !candidate.networkAvailable)) ||
        (!candidate.associated && !candidate.receiveFrames.empty()) ||
        candidate.nextFid < firstFid || candidate.nextFid > firstFid + bufferCount ||
        candidate.buffers.size() != unsigned(candidate.nextFid - firstFid) ||
        (candidate.recordId == 0) != candidate.recordWindow.empty() ||
        (candidate.recordId && (candidate.recordId < firstConfigRid || candidate.recordId > 0xfdff)) ||
        (candidate.recordWindow.size() & 1)) return false;

    for (const auto& bap : candidate.baps)
        if (bap.offset & ~(bapError | 0x0ffe)) return false;
    for (const auto& [id, value] : candidate.buffers)
        if (id < firstFid || id >= candidate.nextFid || value.size() < minBufferSize || (value.size() & 1)) return false;
    for (const auto id : candidate.allocations)
        if (!candidate.buffers.count(id)) return false;
    for (const auto id : candidate.transmissions)
        if (!candidate.buffers.count(id)) return false;
    for (const auto& [id, value] : defaults) {
        const auto it = candidate.records.find(id);
        if (it == candidate.records.end() || it->second.size() != value.size() ||
            (id >= pastConfigRid && it->second != value)) return false;
    }
    for (const auto& [id, value] : candidate.records)
        if (id < firstConfigRid || (id >= pastConfigRid && !defaults.count(id)) || (value.size() & 1)) return false;
    for (const auto& frame : candidate.receiveFrames)
        if (frame.size() < palmPrismFrame::descriptorSize + 8 || (frame.size() & 1)) return false;
    for (const auto& frame : candidate.infoFrames)
        if (frame.size() < 4 || 2u * (1u + Word(frame, 0)) != frame.size()) return false;

    candidate.transmitFrame = std::move(transmitFrame);
    *this = std::move(candidate);
    return true;
}
