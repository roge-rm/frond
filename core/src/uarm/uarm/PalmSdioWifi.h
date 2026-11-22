#ifndef FROND_PALM_SDIO_WIFI_H
#define FROND_PALM_SDIO_WIFI_H

#include <memory>

#include "SdioCard.h"
#include "savestate/savestateAll.h"

class NetworkSimulation;

class PalmSdioWifi {
   public:
    explicit PalmSdioWifi(NetworkSimulation& network, bool receiveBeforeReclaim = false);
    ~PalmSdioWifi();

    SdioCard& Card() { return card; }
    void Poll(uint64_t now);

    template <typename S>
    void Save(S& state);
    bool Load(SavestateLoader<ChunkType>& loader);
    void TakeHost(PalmSdioWifi& previous);
    void DropHost();

   private:
    class Functions;
    std::unique_ptr<Functions> functions;
    SdioCard card;
};

#endif
