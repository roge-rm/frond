#ifndef FROND_SYNC_LINK_H
#define FROND_SYNC_LINK_H

// The HotSync line below DLP, desktop side.
// - Serial and USB on Palm OS 1 to 4: SLP packets (BE EF ED, 10-byte header, CRC-16), PADP
//   inside them (fragments of up to 1024 bytes, each acknowledged), and a CMP wakeup and
//   init exchange before DLP.
// - Network HotSync, and USB on Palm OS 5: NetSync framing, a 6-byte header {1, transaction
//   ID, length32} per message, after a short fixed handshake.
// AcceptHandheld waits for either from the first bytes the handheld sends.

#include <atomic>
#include <memory>
#include <string>

#include "Dlp.h"
#include "SyncManager.h"
#include "SyncStream.h"

namespace frondSync {

class HandheldLink : public PacketLink {
   public:
    // After EndOfSync: lets the last acknowledgements out before the next wait.
    virtual void Finish() {}
};

// Waits for a handheld to start a HotSync on stream and answers its wakeup. Returns null when
// stop is set or the stream closes (error "closed"), or when the start failed (error set).
std::unique_ptr<HandheldLink> AcceptHandheld(SyncStream& stream, Transport transport, const std::atomic<bool>& stop,
                                             std::string& error);

// SLP's CRC-16 (CCITT, polynomial 0x1021, initial 0) and header checksum (8-bit sum).
uint16_t SlpCrc(const uint8_t* data, size_t size, uint16_t crc = 0);
uint8_t SlpHeaderSum(const uint8_t* header);
// One SLP packet with its header and CRC.
Bytes SlpPacket(uint8_t dest, uint8_t src, uint8_t type, uint8_t xid, const Bytes& body);

}  // namespace frondSync

#endif
