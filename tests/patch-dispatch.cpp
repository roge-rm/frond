#include <cassert>
#include <cstdio>
#include <vector>
#include "patch_dispatch.h"
#include "savestate/savestateAll.h"
struct ArmCpu { uint32_t r[16]{}, slow=0; };
uint32_t cpuGetRegExternal(ArmCpu* c,uint_fast8_t r){return c->r[r];}
void cpuSetReg(ArmCpu* c,uint_fast8_t r,uint32_t v){c->r[r]=v;}
void cpuSetSlowPath(ArmCpu* c,uint32_t v){c->slow|=v;}
void cpuClearSlowPath(ArmCpu* c,uint32_t v){c->slow&=~v;}
struct Target {
    PatchDispatch* pd;
    template<class T>void Save(T& s){patchDispatchSave(pd,s);}
    void Load(SavestateLoader<ChunkType>& s){patchDispatchLoad(pd,s);}
};
int main(){
    ArmCpu cpu;auto* pd=initPatchDispatch();patchDispatchSetCpu(pd,&cpu);
    std::vector<unsigned> returned;
    patchDispatchAddPatch(pd,0x4324,nullptr,[](void* p,uint32_t,const uint32_t* before,uint32_t*){
        static_cast<std::vector<unsigned>*>(p)->push_back(before[0]);
    },&returned);
    auto call=[&](unsigned id){
        cpu.r[0]=id;cpu.r[12]=0x3000;cpu.r[13]=0x8000+id*64;cpu.r[14]=0x5001+id*4;
        cpu.r[15]=0x1000;patchDispatchOnLoadR12FromR9(pd,-4);
        cpu.r[15]=0x1004;patchDispatchOnLoadPcFromR12(pd,0x324,cpu.r);
    };
    auto ret=[&](unsigned id){cpu.r[15]=0x5000+id*4;cpu.r[13]=0x8000+id*64;patchOnBeforeExecute(pd,cpu.r);};
    call(1);call(2);ret(1);ret(2);
    assert((returned==std::vector<unsigned>{1,2}));
    assert(!(cpu.slow&SLOW_PATH_REASON_PATCH_PENDING));
    returned.clear();
    for(unsigned i=0;i<4;++i){assert(patchDispatchCanTrackTail(pd));call(i);}
    assert(!patchDispatchCanTrackTail(pd));call(4);
    for(unsigned i:{1u,0u,3u,2u})ret(i);
    assert((returned==std::vector<unsigned>{1,0,3,2}));
    assert(patchDispatchCanTrackTail(pd));
    call(7);Target t{pd};Savestate<ChunkType> saved;assert(saved.Save(t));
    std::vector<uint8_t> bytes(static_cast<uint8_t*>(saved.GetBuffer()),static_cast<uint8_t*>(saved.GetBuffer())+saved.GetSize());
    SavestateLoader<ChunkType> loader;patchDispatchReset(pd);assert(loader.Load(bytes.data(),bytes.size(),t));ret(7);assert(returned.back()==7);
    // Last three words contain packed table/count, loaded R12 and its PC.
    bytes[bytes.size()-12]=255;
    assert(!loader.Load(bytes.data(),bytes.size(),t));
    assert(patchDispatchCanTrackTail(pd));
    assert(!(cpu.slow&SLOW_PATH_REASON_PATCH_PENDING));
    destroyPatchDispatch(pd);
    puts("PASS: non-last tail retirement, full capacity, restore and corrupt-count refusal");
}
