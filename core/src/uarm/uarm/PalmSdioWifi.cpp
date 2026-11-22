#include "PalmSdioWifi.h"
#include "PalmPrismFrame.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>

#include "NetworkSimulation.h"
#include "net/WifiRouter.h"

namespace {
constexpr uint32_t commonCisAddress = 0x1000;
constexpr uint32_t functionCisAddress = 0x1100;

// Minimal SDIO 1.10 CIS. MANFID comes from the shipped driver's comparison.
// The function advertises the driver's 512-byte maximum, with a 100 ms enable timeout.
constexpr uint8_t commonCis[] = {
    0x20, 4, 0x96, 0x02, 0x01, 0x01,
    0x22, 4, 0, 0, 2, 0x32,
    0xff,
};
constexpr uint8_t functionCis[] = {
    0x20, 4, 0x96, 0x02, 0x01, 0x01,
    0x22, 42,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0xff,
};

}  // namespace

class PalmSdioWifi::Functions final : public SdioFunctions {
   public:
    Functions(NetworkSimulation& network, bool receiveBeforeReclaim)
        : network(network), receiveBeforeReclaim(receiveBeforeReclaim) {}

    void Poll(uint64_t now) {
        now_ = now;
        if (network.Frozen()) return;
        if (!associated || !Wireless()) {
            router.reset();
            PublishReclaim();
            return;
        }
        if (now < nextPoll) return;
        if (!router) router = std::make_unique<frondNet::WifiRouter>();
        nextPoll = now + 1'000'000;
        router->Poll(now);
        if (events & eventReceive) return;
        std::vector<uint8_t> frame;
        if (!router->Receive(frame)) {
            PublishReclaim();
            return;
        }
        auto descriptor = palmPrismFrame::Receive(frame, bssid);
        if (descriptor.empty()) return;
        bapWrites[rxFid] = std::move(descriptor);
        events |= eventReceive;
        TraceFrame("RX", frame);
        UpdateIrq();
    }

    void Attach(SdioCard& card) { card_ = &card; }

    bool Read(unsigned fn, uint32_t address, uint8_t& value) override {
        fifoLengthAddress = 0;
        if (fn == 0) {
            if (address == 0x100)
                value = 7;  // WLAN standard interface
            else if (address >= 0x109 && address <= 0x10b)
                value = functionCisAddress >> (8 * (address - 0x109));
            else if (address >= commonCisAddress && address - commonCisAddress < sizeof(commonCis))
                value = commonCis[address - commonCisAddress];
            else if (address >= functionCisAddress && address - functionCisAddress < sizeof(functionCis))
                value = functionCis[address - functionCisAddress];
            else
                return false;
            return true;
        }
        if (address <= 1) {
            // Empty host-to-card FIFO. Bit 10 is the empty flag.
            value = 0x0400 >> (8 * address);
        } else if (primary && address == 4) {
            value = functionIrq;
        } else if (primary && (address == 2 || address == 3)) {
            const unsigned available = fifoRemaining == 1024 ? 0x0800 :
                                       fifoRemaining ? fifoRemaining : 0x0400;
            value = available >> (8 * (address - 2));
        } else if (primary && address == 0x0c && fifoRemaining && fifoRegister == 0x3e) {
            // Synthetic board record with a locally administered MAC.
            constexpr std::array<uint8_t, 22> pda{
                2, 0, 1, 0, 0, 0,
                4, 0, 1, 1, 2, 0, 0, 0, 0, 1,
                2, 0, 0, 0, 0, 0,
            };
            value = pdaOffset < pda.size() ? pda[pdaOffset] : 0xff;
            if (pdaOffset < 1024) ++pdaOffset;
            --fifoRemaining;
            return true;
        } else if (primary && address == 0x0c && fifoRemaining) {
            if (!ReadBapByte(value)) return false;
            --fifoRemaining;
        } else if (primary && ReadPrimaryRegister(address, value)) {
            return true;
        } else if (address == 0x18814 || address == 0x18815) {
            value = auxControl >> (8 * (address - 0x18814));
        } else if ((address == 0x1883e || address == 0x1883f) && genesis &&
                   auxPage == 0xfc00 && auxOffset >= 0x38 && auxOffset < 0x3c &&
                   genesisWritten[auxOffset - 0x38]) {
            value = genesisRam[auxOffset++ - 0x38];
        } else {
            return false;
        }
        return true;
    }

    bool Write(unsigned fn, uint32_t address, uint8_t value) override {
        if (address != fifoLengthAddress) fifoLengthAddress = 0;
        if (fn == 1 && address == 0x1483e && (genesis || volatileDownload)) {
            const uint32_t target = (uint32_t(auxPage) << 7) | (auxOffset & 0x7f);
            if (target < firmwareBase || target >= firmwareBase + firmwareRam.size()) return false;
            // The second primary boot adds a 16-byte runtime patch to the same image.
            const bool primaryPatch = genesis && bulkBytes >= primaryBytes;
            if (primaryPatch && (bulkBytes >= primaryBytes + primaryPatchBytes ||
                                 target != primaryPatchAddress + bulkBytes - primaryBytes))
                return false;
            if (volatileDownload && bulkBytes >= stationBytes) return false;
            firmwareRam[target - firmwareBase] = value;
            firmwareWritten[(target - firmwareBase) / 8] |= 1u << ((target - firmwareBase) % 8);
            if ((++auxOffset & 0x7f) == 0) {
                ++auxPage;
                auxOffset &= ~0x80;
            }
            ++bulkBytes;
            if (!primaryPatch) bulkDigest = (bulkDigest ^ value) * 1099511628211ull;
            return true;
        }
        if (fn != 1) return false;
        if (station && address == 0x14836)
            return WriteBapByte(value);
        if (primary && address == 4) {
            functionIrq = value & 0x80;
            UpdateIrq();
            return true;
        }
        if (primary && (address == 0x10838 || address == 0x1083e)) {
            if (fifoLengthAddress != address) {
                fifoLengthAddress = address;
                fifoLengthLow = value;
                return true;
            }
            fifoLengthAddress = 0;
            const unsigned length = fifoLengthLow | (unsigned(value) << 8);
            fifoRegister = address & 0x3f;
            if (length > 1024 || (fifoRegister == 0x3e && (auxPage != 0xfe00 || auxOffset != 0))) {
                fifoRemaining = 0;
                return false;
            }
            fifoRemaining = length;
            return true;
        }
        if (address >= 0x1c3e0 && address <= 0x1c3e3) {
            if (address >= 0x1c3e2) SetByte(resetSelector, address - 0x1c3e2, value);
            if (address == 0x1c3e0 && value == 0x45) {
                if (resetSelector == 0x000f) {
                    Reset();
                    resetSelector = 0x000f;
                    genesis = true;
                } else if (resetSelector == 0x0007) {
                    // Only the v1.2 firmware streams have a known RID descriptor layout.
                    primary = genesis && bulkDigest == 0x4b9aa1293e5f1a5dull &&
                              (bulkBytes == primaryBytes || bulkBytes == primaryBytes + primaryPatchBytes);
                    if (trace)
                        std::fprintf(stderr, "Palm SDIO primary firmware %s (%u bytes)\n",
                                     primary ? "ready" : "unrecognised", bulkBytes);
                    genesis = station = volatileDownload = associated = false;
                    command = events = status = 0;
                    pendingReclaims.clear();
                    UpdateIrq();
                }
            }
            return true;
        }
        if ((address & ~0x3fu) != 0x1c800 &&
            (!primary || (address & ~0x3fu) != 0x1c000)) return false;
        const unsigned reg = address & 0x3f;
        if (reg == 0 || reg == 1) {
            SetByte(command, reg, value);
            if (reg == 0) return true;
            if (!primary) return false;
            switch (command) {
                case 0x000b:
                case 0x010b:
                    if (!station || !Transmit(params[0])) return false;
                    break;
                case 0:
                    break;
                case 1:
                    if (!station) return false;
                    EvaluateLink();
                    break;
                case 2:
                    if (!station) return false;
                    associated = false;
                    break;
                case 0x0a:
                    if (params[0] < 4 || params[0] > 2400) return false;
                    if (receiveBeforeReclaim && (events & eventAllocate || !pendingReclaims.empty()))
                        return false;
                    allocFid = nextFid;
                    nextFid = nextFid == lastBufferFid ? firstBufferFid : nextFid + 1;
                    bapWrites.erase(allocFid);
                    events |= eventAllocate;
                    break;
                case 0x0121: {
                    auto it = bapWrites.find(params[0]);
                    if (!station || it == bapWrites.end() || it->second.size() < 4) return false;
                    auto& record = it->second;
                    const unsigned length = 2 * (1 + record[0] + 256 * record[1]);
                    if (length > record.size() || (record[2] | record[3] << 8) != params[0])
                        return false;
                    record.resize(length);
                    if (params[0] == 0xfce5) QueueScanResult();
                    break;
                }
                case 0x21:
                    if (!ReadFirmwareRid(params[0])) return false;
                    break;
                case 0x0122:
                    volatileDownload = true;
                    bulkBytes = 0;
                    bulkDigest = digestBasis;
                    break;
                case 0x0022:
                    if (!volatileDownload) return false;
                    volatileDownload = false;
                    station = bulkBytes == stationBytes && bulkDigest == 0xffd59a0f5dfcd440ull;
                    if (trace)
                        std::fprintf(stderr, "Palm SDIO station firmware %s (%u bytes)\n",
                                     station ? "ready" : "unrecognised", bulkBytes);
                    if (!station) {
                        associated = false;
                        return false;
                    }
                    break;
                default:
                    return false;
            }
            status = command & 0x3f;
            events |= eventCommand;
            UpdateIrq();
        } else if (station && (reg == 0x18 || reg == 0x19)) {
            SetByte(bapWriteSelect, reg & 1, value);
        } else if (station && (reg == 0x1c || reg == 0x1d)) {
            SetByte(bapWriteOffset, reg & 1, value);
            bapWriteOffset &= ~1u;
        } else if (station && (reg == 0x36 || reg == 0x37)) {
            return WriteBapByte(value);
        } else if (primary && (reg == 0x1a || reg == 0x1b)) {
            SetByte(bapSelect, reg & 1, value);
        } else if (primary && (reg == 0x1e || reg == 0x1f)) {
            SetByte(bapOffset, reg & 1, value);
            bapOffset &= ~1u;
        } else if (primary && (reg == 0x32 || reg == 0x33)) {
            SetByte(interruptMask, reg & 1, value);
            UpdateIrq();
        } else if (primary && (reg == 0x34 || reg == 0x35)) {
            events &= ~(uint16_t(value) << (8 * (reg & 1)));
            UpdateIrq();
        } else if (reg >= 2 && reg <= 7) {
            SetByte(params[(reg - 2) / 2], reg & 1, value);
        } else if (reg == 0x15 && value == 0x80 &&
                   params == std::array<uint16_t, 3>{0xfe01, 0xdc23, 0xba45}) {
            auxControl = 0xc000;
        } else if (reg == 0x15 && value == 0x40) {
            auxControl = 0;
        } else if (reg == 0x3a || reg == 0x3b) {
            SetByte(auxPage, reg & 1, value);
        } else if (reg == 0x3c || reg == 0x3d) {
            SetByte(auxOffset, reg & 1, value);
            pdaOffset = 0;
        } else if (reg == 0x3e || reg == 0x3f) {
            if (!genesis || auxPage != 0xfc00 || auxOffset < 0x38 || auxOffset >= 0x3c) return false;
            genesisRam[auxOffset - 0x38] = value;
            genesisWritten[auxOffset++ - 0x38] = true;
        } else if (reg != 0x14) {
            return false;
        }
        return true;
    }

    template <bool loading, typename H>
    bool TransferState(H& state, uint32_t version) {
        state.Do64(now_).Do64(nextPoll).Do64(bulkDigest).Do32(bulkBytes)
            .Do16(resetSelector).Do16(auxPage).Do16(auxOffset).Do16(auxControl)
            .Do16(command).Do16(events).Do16(status)
            .Do16(interruptMask).Do16(allocFid).Do16(nextFid).Do8(functionIrq)
            .DoBool(genesis).DoBool(primary).DoBool(station)
            .DoBool(volatileDownload).DoBool(associated)
            .Do16(bapSelect).Do16(bapOffset).Do16(bapWriteSelect).Do16(bapWriteOffset)
            .Do16(activeRid).Do16(fifoRemaining).Do32(fifoLengthAddress)
            .Do32(fifoLengthLow).Do32(fifoRegister).Do32(pdaOffset);
        for (auto& value : params) state.Do16(value);
        for (auto& written : genesisWritten) state.DoBool(written);
        state.DoBuffer(genesisRam.data(), genesisRam.size())
            .DoBuffer(firmwareRam.data(), firmwareRam.size())
            .DoBuffer(firmwareWritten.data(), firmwareWritten.size());

        if (!TransferRecord<loading>(state, compatibilityRecord, 1028)) return false;
        uint32_t count = bapWrites.size();
        state.Do32(count);
        if constexpr (loading) {
            if (count > 514) return false;
            bapWrites.clear();
            for (uint32_t i = 0; i < count; ++i) {
                uint16_t id = 0;
                state.Do16(id);
                if ((!IsWritableBap(id) && id != infoFid && id != rxFid) ||
                    bapWrites.count(id) || !TransferRecord<true>(state, bapWrites[id], 4096))
                    return false;
            }
        } else {
            for (auto& [id, record] : bapWrites) {
                state.Do16(id);
                TransferRecord<false>(state, record, 4096);
            }
        }
        if (version >= 1) {
            uint32_t count = pendingReclaims.size();
            state.Do32(count);
            if constexpr (loading) {
                if (count > reclaimLimit || (count && !receiveBeforeReclaim)) return false;
                pendingReclaims.resize(count);
            }
            for (auto& fid : pendingReclaims) {
                state.Do16(fid);
                if (fid < firstBufferFid || fid > lastBufferFid) return false;
            }
        } else {
            pendingReclaims.clear();
        }
        return true;
    }

    bool StateValid() const {
        return bulkBytes <= stationBytes && fifoRemaining <= 1024 && pdaOffset <= 1024 &&
               (fifoLengthAddress == 0 || fifoLengthAddress == 0x10838 || fifoLengthAddress == 0x1083e) &&
               (fifoRegister == 0 || fifoRegister == 0x38 || fifoRegister == 0x3e) &&
               fifoLengthLow <= 255 && (functionIrq == 0 || functionIrq == 0x80) &&
               nextFid >= firstBufferFid && nextFid <= lastBufferFid &&
               (!station || primary) && (!associated || station);
    }

    void TakeHost(Functions& previous) { router = std::move(previous.router); }
    void DropHost() { router.reset(); }

    void Reset() override {
        resetSelector = auxPage = auxOffset = auxControl = command = events = status = 0;
        params.fill(0);
        genesisWritten.fill(false);
        firmwareWritten.fill(0);
        firmwareRam.fill(0);
        bapWrites.clear();
        pendingReclaims.clear();
        if (!network.Frozen()) router.reset();
        nextPoll = 0;
        bapWriteSelect = bapWriteOffset = bapSelect = bapOffset = 0;
        activeRid = fifoRemaining = fifoLengthLow = fifoRegister = pdaOffset = 0;
        fifoLengthAddress = 0;
        compatibilityRecord.clear();
        bulkBytes = 0;
        bulkDigest = digestBasis;
        genesis = primary = station = volatileDownload = associated = false;
        functionIrq = 0;
        interruptMask = allocFid = 0;
        nextFid = firstBufferFid;
        UpdateIrq();
    }

   private:
    template <bool loading, typename H>
    static bool TransferRecord(H& state, std::vector<uint8_t>& record, uint32_t limit) {
        uint32_t size = record.size();
        state.Do32(size);
        if constexpr (loading) {
            if (size > limit) return false;
            record.resize(size);
        }
        if (size) state.DoBuffer(record.data(), size);
        return true;
    }

    static bool IsWritableBap(uint16_t id) {
        return (id >= firstBufferFid && id <= lastBufferFid) || (id & 0xff00) == 0xfc00;
    }

    bool Transmit(uint16_t fid) {
        const auto it = bapWrites.find(fid);
        if (!associated || it == bapWrites.end() || fid < firstBufferFid || fid > lastBufferFid ||
            pendingReclaims.size() >= reclaimLimit) return false;
        auto frame = palmPrismFrame::Transmit(it->second, mac);
        if (frame.empty()) return false;
        if (Wireless()) {
            if (!router) router = std::make_unique<frondNet::WifiRouter>();
            router->Input(frame.data(), frame.size(), now_);
            TraceFrame("TX", frame);
        }
        if (receiveBeforeReclaim) {
            pendingReclaims.push_back(fid);
        } else {
            events |= eventAllocate;
            allocFid = fid;
        }
        return true;
    }

    // The E3 slot driver needs a separate interrupt for the receive FID.
    void PublishReclaim() {
        if (pendingReclaims.empty() || (events & (eventReceive | eventAllocate))) return;
        allocFid = pendingReclaims.front();
        pendingReclaims.pop_front();
        events |= eventAllocate;
        UpdateIrq();
    }

    static constexpr std::array<uint8_t, 6> mac{2, 0, 0, 0, 0, 1};
    static constexpr std::array<uint8_t, 6> bssid{2, 0, 0, 0, 0, 2};
    static constexpr uint16_t eventReceive = 0x0001, eventAllocate = 0x0008;
    static constexpr uint16_t eventCommand = 0x0010, eventInfo = 0x0080;
    static constexpr uint16_t rxFid = 0x201;
    std::unique_ptr<frondNet::WifiRouter> router;
    uint64_t now_ = 0, nextPoll = 0;

    void EvaluateLink() {
        const auto desired = bapWrites.find(0xfc02);
        constexpr std::array<uint8_t, 5> ssid{'F', 'r', 'o', 'n', 'd'};
        if (!Wireless() || desired == bapWrites.end() || desired->second.size() < 11 ||
            desired->second[4] != ssid.size() || desired->second[5] != 0 ||
            !std::equal(ssid.begin(), ssid.end(), desired->second.begin() + 6)) return;
        associated = true;
        if (trace) std::fprintf(stderr, "Palm SDIO associated with Frond\n");
        bapWrites[infoFid] = {2, 0, 0, 0xf2, 1, 0};
        events |= eventInfo;
    }

    bool ReadLinkRid(uint16_t rid) {
        switch (rid) {
            case 0xfc01:
                compatibilityRecord = {4, 0, 1, 0xfc};
                compatibilityRecord.insert(compatibilityRecord.end(), mac.begin(), mac.end());
                break;
            case 0xfd41:
                compatibilityRecord = {18, 0, 0x41, 0xfd, 5, 0, 'F', 'r', 'o', 'n', 'd'};
                compatibilityRecord.resize(38);
                break;
            case 0xfd42:
                compatibilityRecord = {4, 0, 0x42, 0xfd, 2, 0, 0, 0, 0, 2};
                break;
            case 0xfd43:
                compatibilityRecord = {4, 0, 0x43, 0xfd, 92, 0, 154, 0, 27, 0};
                break;
            case 0xfd44:
                compatibilityRecord = {2, 0, 0x44, 0xfd, 8, 0};
                break;
            case 0xfdc1:
                compatibilityRecord = {2, 0, 0xc1, 0xfd, 1, 0};
                break;
            default:
                return false;
        }
        activeRid = rid;
        return true;
    }

    void QueueScanResult() {
        if (!Wireless()) {
            bapWrites[infoFid] = {3, 0, 0x03, 0xf1, 64, 0, 0, 0};
            events |= eventInfo;
            return;
        }

        // One open virtual AP and a 64-byte host-scan result.
        bapWrites[infoFid] = {
            35, 0, 0x03, 0xf1, 64, 0, 0, 0,
            1, 0, 0xa1, 0xff, 0xd3, 0xff,
            2, 0, 0, 0, 0, 2,
            100, 0, 1, 0, 5, 0,
            'F', 'r', 'o', 'n', 'd',
            0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x82, 0x84, 0x0b, 0x16, 0, 0, 0, 0, 0, 0,
            2, 0, 0, 0,
        };
        events |= eventInfo;
    }

    static constexpr uint16_t infoFid = 0x200;

    bool ReadBapByte(uint8_t& value) {
        const auto written = bapWrites.find(bapSelect);
        if (written != bapWrites.end() && bapOffset < written->second.size()) {
            value = written->second[bapOffset++];
            return true;
        }
        if (bapSelect != activeRid || bapOffset >= compatibilityRecord.size()) return false;
        value = compatibilityRecord[bapOffset++];
        return true;
    }

    bool WriteBapByte(uint8_t value) {
        if (!IsWritableBap(bapWriteSelect) || bapWriteOffset >= 4096) return false;
        auto& bytes = bapWrites[bapWriteSelect];
        if (bapWriteOffset >= bytes.size()) bytes.resize(bapWriteOffset + 1);
        bytes[bapWriteOffset++] = value;
        return true;
    }

    void UpdateIrq() {
        const bool next = (functionIrq != 0) && (events & interruptMask);
        if (card_) card_->SetPending(next ? 2 : 0);
    }

    bool ReadFirmwareRid(uint16_t rid) {
        if ((associated || rid == 0xfc01) && ReadLinkRid(rid)) return true;
        if (station && ReadFirmwareRid(rid, 0x7f0d7c, 0x7f10e2)) return true;
        return ReadFirmwareRid(rid, 0x7e111e, 0x7e118c);
    }

    bool ReadFirmwareRid(uint16_t rid, uint32_t first, uint32_t last) {
        // The uploaded primary image contains 10-byte descriptors with
        // a record ID, two access words, SRAM offset and byte length.
        for (uint32_t entry = first; entry <= last; entry += 10) {
            if (DownloadWord(entry) != rid) continue;
            const uint32_t address = 0x7e0000 + DownloadWord(entry + 6);
            const unsigned length = DownloadWord(entry + 8);
            if (!length || length > 1024 || length % 2) return false;
            if (address - firmwareBase + length > firmwareRam.size()) return false;
            for (unsigned index = 0; index < length; ++index)
                if (!FirmwareByteWritten(address - firmwareBase + index)) return false;
            compatibilityRecord.resize(length + 4);
            compatibilityRecord[0] = (length + 2) / 2;
            compatibilityRecord[1] = ((length + 2) / 2) >> 8;
            compatibilityRecord[2] = rid;
            compatibilityRecord[3] = rid >> 8;
            std::copy_n(firmwareRam.begin() + (address - firmwareBase), length,
                        compatibilityRecord.begin() + 4);
            activeRid = rid;
            return true;
        }
        return false;
    }

    bool FirmwareByteWritten(uint32_t offset) const {
        return firmwareWritten[offset / 8] & (1u << (offset % 8));
    }

    unsigned DownloadWord(uint32_t address) const {
        const auto offset = address - firmwareBase;
        if (offset >= firmwareRam.size() - 1 ||
            !FirmwareByteWritten(offset) || !FirmwareByteWritten(offset + 1)) return 0;
        return firmwareRam[offset] | (unsigned(firmwareRam[offset + 1]) << 8);
    }

    void TraceFrame(const char* direction, const std::vector<uint8_t>& frame) const {
        if (trace) std::fprintf(stderr, "Palm SDIO %s bytes=%zu ethertype=%02x%02x\n",
                                direction, frame.size(), frame[12], frame[13]);
    }

    bool Wireless() const {
        return !network.Frozen() && network.GetMode() == NetworkSimulation::wireless;
    }

    static constexpr uint16_t firstBufferFid = 0x100, lastBufferFid = 0x1ff;
    static constexpr uint32_t firmwareBase = 0x7e0000;
    static constexpr uint32_t primaryBytes = 4874, primaryPatchBytes = 16;
    static constexpr uint32_t primaryPatchAddress = 0x7e1384, stationBytes = 60754;
    static constexpr uint64_t digestBasis = 14695981039346656037ull;
    NetworkSimulation& network;
    const bool receiveBeforeReclaim;
    static constexpr size_t reclaimLimit = lastBufferFid - firstBufferFid + 1;
    std::deque<uint16_t> pendingReclaims;
    const bool trace = std::getenv("FROND_WIFI_TRACE") != nullptr;
    SdioCard* card_ = nullptr;
    uint8_t functionIrq = 0;
    uint16_t interruptMask = 0, allocFid = 0, nextFid = firstBufferFid;
    bool station = false, volatileDownload = false, associated = false;
    uint16_t activeRid = 0;
    uint16_t bapWriteSelect = 0, bapWriteOffset = 0;
    std::map<uint16_t, std::vector<uint8_t>> bapWrites;
    bool ReadPrimaryRegister(uint32_t address, uint8_t& value) {
        if ((address & ~0x3fu) != 0x18000 && (address & ~0x3fu) != 0x18800)
            return false;
        uint16_t word;
        switch (address & 0x3e) {
            case 0x00: word = command; break;
            case 0x08: word = status; break;
            case 0x0a:
            case 0x0c:
            case 0x0e: word = 0; break;
            case 0x1c: word = bapWriteOffset; break;
            case 0x1e: word = bapOffset; break;
            case 0x10: word = infoFid; break;
            case 0x20: word = rxFid; break;
            case 0x22: word = allocFid; break;
            case 0x30: word = events; break;
            case 0x32: word = interruptMask; break;
            case 0x38:
                return ReadBapByte(value);
            default: return false;
        }
        value = word >> (8 * (address & 1));
        return true;
    }

    static void SetByte(uint16_t& word, unsigned lane, uint8_t value) {
        const unsigned shift = 8 * lane;
        word = (word & ~(0xffu << shift)) | (uint16_t(value) << shift);
    }

    uint16_t resetSelector = 0, auxPage = 0, auxOffset = 0, auxControl = 0;
    std::array<uint16_t, 3> params{};
    std::array<uint8_t, 4> genesisRam{};
    std::array<bool, 4> genesisWritten{};
    std::array<uint8_t, 0x20000> firmwareRam{};
    std::array<uint8_t, 0x20000 / 8> firmwareWritten{};
    unsigned bulkBytes = 0;
    uint64_t bulkDigest = digestBasis;
    uint16_t command = 0, events = 0, status = 0;
    uint32_t fifoLengthAddress = 0;
    unsigned fifoLengthLow = 0, fifoRegister = 0, pdaOffset = 0;
    uint16_t bapSelect = 0, bapOffset = 0, fifoRemaining = 0;
    std::vector<uint8_t> compatibilityRecord;
    bool genesis = false, primary = false;
};

PalmSdioWifi::PalmSdioWifi(NetworkSimulation& network, bool receiveBeforeReclaim)
    : functions(std::make_unique<Functions>(network, receiveBeforeReclaim)),
      card(*functions, 1, commonCisAddress) {
    functions->Attach(card);
}

PalmSdioWifi::~PalmSdioWifi() = default;

void PalmSdioWifi::Poll(uint64_t now) { functions->Poll(now); }

template <typename S>
void PalmSdioWifi::Save(S& state) {
    auto* chunk = state.GetChunk(ChunkType::palmSdioWifi, 1);
    if (!chunk) return;
    SaveChunkHelper helper(*chunk);
    card.TransferState(helper);
    functions->TransferState<false>(helper, 1);
}

bool PalmSdioWifi::Load(SavestateLoader<ChunkType>& loader) {
    uint32_t version;
    auto* chunk = loader.GetChunkOrFail(ChunkType::palmSdioWifi, 1, "Palm SDIO Wi-Fi", version);
    if (!chunk) return false;
    LoadChunkHelper helper(*chunk);
    card.TransferState(helper);
    if (!functions->TransferState<true>(helper, version) || !card.StateValid() || !functions->StateValid()) {
        loader.NotifyError();
        return false;
    }
    return true;
}

void PalmSdioWifi::TakeHost(PalmSdioWifi& previous) {
    functions->TakeHost(*previous.functions);
}
void PalmSdioWifi::DropHost() { functions->DropHost(); }

template void PalmSdioWifi::Save(Savestate<ChunkType>& state);
template void PalmSdioWifi::Save(SavestateProbe<ChunkType>& state);
