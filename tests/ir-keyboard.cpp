#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
#include "IrKeyboard.h"
int main() {
    IrKeyboard k;
    assert(!k.Key(0x1a,true));k.Attach(true);
    assert(k.Key(0x1a,true));assert(!k.Key(0x1a,true));
    uint64_t now=0;
    assert(k.Read(now)==0xff);assert(k.Read(now)==0x100);
    Savestate<ChunkType> state;assert(state.Save(k));
    const uint8_t rest[]={0xc0,0x1a,0xe5,0x7d,0xc1};
    for(auto expected:rest) {now+=1145834;assert(k.Read(now)==expected);}
    assert(!k.Pending());k.ReleaseAll();
    const uint8_t up[]={0xff,0xc0,0x9a,0x65,0xfd,0xc1};
    for(auto expected:up) {now+=1145834;assert(k.Read(now)==expected);}
    SavestateLoader<ChunkType> loader;assert(loader.Load(state.GetBuffer(),state.GetSize(),k));now=0;
    for(auto expected:rest) {now+=1145834;assert(k.Read(now)==expected);}
    // Preserve attach state, but release held keys when the host loses focus.
    k.ReleaseAll();for(auto expected:up){now+=1145834;assert(k.Read(now)==expected);}
    for(unsigned i=0;i<1000;i++) { k.Key(0x1a,true);k.Key(0x1a,false); }
    k.ReleaseAll();unsigned packets=0;
    while(k.Pending()) { now+=1145834;k.Read(now);++packets;assert(packets<=IrKeyboard::capacity*6); }
    assert(packets%12==0); // accepted press/release pairs, including at overflow
    assert(k.Key(0x1a,true));k.Attach(false);assert(!k.Key(0x1a,true));
    while(k.Pending()){now+=1145834;k.Read(now);}
    k.Attach(true);assert(k.Key(0x1a,true));k.Reset();assert(!k.Pending()&&k.Attached());
    assert(state.Save(k));
    uint32_t legacy=0;assert(loader.Load(&legacy,sizeof(legacy),k));assert(!k.Attached());
    puts("PASS: infrared keyboard framing, guest-time baud spacing, press/release, overflow, detach, reset and partial packet restore");
}
