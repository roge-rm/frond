#ifndef EmBusTimingSavestate_h
#define EmBusTimingSavestate_h

#include "EmBusTiming.h"
#include "savestate/ChunkHelper.h"
#include "savestate/ChunkType.h"
#include "savestate/SavestateLoader.h"

namespace EmBusTiming {
    template <typename H>
    void DoState(H& helper, State& state) {
        helper.Do32(state.steal.lcdLineClocks).Do32(state.steal.lcdDmaClocks)
            .Do32(state.steal.refreshPeriod).Do32(state.steal.refreshClocks)
            .Do32(state.sdramConfig).Do64(state.nextLcd).Do64(state.nextRefresh)
            .Do64(state.count).Do32(state.lcdFraction).Do32(state.refreshFraction)
            .Do32(state.wait).Do32(state.cycles);
        for (auto& row : state.openRow) helper.Do32(row);
    }

    template <typename T>
    void Save(T& savestate) {
        auto* chunk = savestate.GetChunk(ChunkType::busTiming);
        if (!chunk) return;
        chunk->Put32(1);
        SaveChunkHelper helper(*chunk);
        auto state = Capture();
        DoState(helper, state);
    }

    inline void Load(SavestateLoader<ChunkType>& loader, uint64_t now) {
        // Older sessions have no bus phase or SDRAM page history.
        if (!loader.HasChunk(ChunkType::busTiming)) return Reset(now);
        auto* chunk = loader.GetChunk(ChunkType::busTiming);
        if (chunk->Get32() != 1) return loader.NotifyError();
        State state{};
        LoadChunkHelper helper(*chunk);
        DoState(helper, state);
        if (!chunk->IsAtEnd() || !Restore(state)) loader.NotifyError();
    }
}

#endif
