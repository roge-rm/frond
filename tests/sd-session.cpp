#include <cassert>
#include <cstring>
#include <vector>
#include "session/session_file5.h"
#include "SdImage.h"

int main() {
    uint8_t nor[] = {1,2,3}, ram[] = {4,5,6,7}, state[] = {8,9};
    std::vector<uint8_t> card(512);
    for (unsigned i=0;i<card.size();i++) card[i]=i*17;
    SessionFile5 source;
    source.SetDeviceName("palm-tx").SetRamSize(32u<<20).SetNor(sizeof(nor),nor)
        .SetMemory(sizeof(ram),ram).SetSavestate(sizeof(state),state).SetSd(card.size(),card.data());
    assert(source.Serialize());
    std::vector<uint8_t> saved(static_cast<const uint8_t*>(source.GetSerializedSession()),
                               static_cast<const uint8_t*>(source.GetSerializedSession())+source.GetSerializedSessionSize());
    assert(saved[4]==6);
    SessionFile5 decoded;
    assert(decoded.Deserialize(saved.size(),saved.data()));
    assert(decoded.GetSdSize()==card.size() && !memcmp(decoded.GetSd(),card.data(),card.size()));
    assert(decoded.GetMemorySize()==sizeof(ram) && !memcmp(decoded.GetMemory(),ram,sizeof(ram)));
    auto broken=saved;broken.back()^=1;
    assert(!decoded.Deserialize(broken.size(),broken.data()));
    assert(!decoded.Deserialize(saved.size()-1,saved.data()));
    // An oversized SD TOC must be rejected before allocating its claimed payload.
    broken=saved;for(int i=68;i<72;i++) broken[i]=255;
    assert(!decoded.Deserialize(broken.size(),broken.data()));
    source.SetSd(0,nullptr);assert(source.Serialize());
    assert(static_cast<const uint8_t*>(source.GetSerializedSession())[4]==6);
    assert(decoded.Deserialize(source.GetSerializedSessionSize(),source.GetSerializedSession()));
    assert(decoded.GetSdSize()==0 && decoded.GetSd()==nullptr);
    source.SetSd(513,card.data());assert(!source.Serialize());
    for(unsigned mib : {32,64,128}) {
        auto image=frondSd::create(mib);
        assert(image.size()==size_t(mib)*1024*1024 && image[510]==0x55 && image[511]==0xaa);
        auto* boot=image.data()+63*512;
        assert(boot[510]==0x55 && boot[511]==0xaa && boot[16]==2);
        auto get16=[](const uint8_t* p){return unsigned(p[0])|(unsigned(p[1])<<8);};
        const auto fat=get16(boot+22); const auto cluster=boot[13];
        const auto clusters=(image.size()/512-63-1-32-2*fat)/cluster;
        assert(clusters>=4085 && clusters<65525 && (clusters+2)*2<=fat*512);
        assert(!memcmp(image.data()+(63+1)*512,image.data()+(63+1+fat)*512,fat*512));
    }
    puts("PASS: self-contained SD session integrity, legacy loading, corrupt/truncated rejection and FAT16 geometry");
}
