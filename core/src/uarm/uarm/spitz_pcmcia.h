#ifndef SPITZ_PCMCIA_H
#define SPITZ_PCMCIA_H

#include <cstdint>

#include "CardBacking.h"

struct ArmMem;
struct PxaGpio;
struct SpitzPcmcia;

SpitzPcmcia* spitzPcmciaInit(ArmMem* mem, PxaGpio* gpio);
void spitzPcmciaDeinit(SpitzPcmcia* pcmcia);
void spitzPcmciaReset(SpitzPcmcia* pcmcia);
uint16_t spitzPcmciaGpioOutputs(const SpitzPcmcia* pcmcia, unsigned slot);
bool spitzPcmciaGpioDrivenLow(const SpitzPcmcia* pcmcia, unsigned slot, uint16_t mask);
void spitzPcmciaObserveGpioOutputs(SpitzPcmcia* pcmcia, void (*callback)(void*, unsigned), void* context);
bool spitzPcmciaSdPowered(const SpitzPcmcia* pcmcia);
void spitzPcmciaObserveSdPower(SpitzPcmcia* pcmcia, void (*callback)(void*), void* context);

// Attach an uncompressed sector image while the guest is stopped.
bool spitzPcmciaMount(SpitzPcmcia* pcmcia, const char* path, bool writable, const char* mediaId);
bool spitzPcmciaFlushMicrodrive(const SpitzPcmcia* pcmcia);
bool spitzPcmciaForEachMicrodriveChange(const SpitzPcmcia* pcmcia, const CardBacking::OverlayFn& fn);
bool spitzPcmciaWriteMicrodriveBase(SpitzPcmcia* pcmcia, uint64_t offset, const void* data, size_t count);
bool spitzPcmciaResetMicrodriveOverlay(SpitzPcmcia* pcmcia);

bool spitzPcmciaInsertCompactFlash(SpitzPcmcia* pcmcia, bool writeProtected);
void spitzPcmciaEjectCompactFlash(SpitzPcmcia* pcmcia);
bool spitzPcmciaCompactFlashInserted(const SpitzPcmcia* pcmcia);
void spitzPcmciaSetCompactFlashWriteProtect(SpitzPcmcia* pcmcia, bool locked);
void spitzPcmciaSetCompactFlashSwapOnLoad(SpitzPcmcia* pcmcia, bool allow);

bool spitzPcmciaInsertEthernet(SpitzPcmcia* pcmcia);
void spitzPcmciaEjectEthernet(SpitzPcmcia* pcmcia);
bool spitzPcmciaEthernetInserted(const SpitzPcmcia* pcmcia);
bool spitzPcmciaEthernetReceive(SpitzPcmcia* pcmcia, const uint8_t* frame, size_t size);
void spitzPcmciaEthernetTransmit(SpitzPcmcia* pcmcia,
                              void (*transmit)(void*, const uint8_t*, size_t), void* context);

bool spitzPcmciaInsertWifi(SpitzPcmcia* pcmcia);
void spitzPcmciaEjectWifi(SpitzPcmcia* pcmcia);
bool spitzPcmciaWifiInserted(const SpitzPcmcia* pcmcia);
bool spitzPcmciaWifiAssociated(const SpitzPcmcia* pcmcia);
void spitzPcmciaWifiNetworkAvailable(SpitzPcmcia* pcmcia, bool available);
bool spitzPcmciaWifiReceive(SpitzPcmcia* pcmcia, const uint8_t* frame, size_t size);
void spitzPcmciaWifiTransmit(SpitzPcmcia* pcmcia,
                          void (*transmit)(void*, const uint8_t*, size_t), void* context);

template <typename T>
void spitzPcmciaSave(SpitzPcmcia* pcmcia, T& savestate);
template <typename T>
void spitzPcmciaLoad(SpitzPcmcia* pcmcia, T& loader, bool resetting);

#endif
