// Mock SDL capture plus real SoC state replacement. No host device is opened.
#include "SdlMicrophone.h"
#include "soc_PXA.h"
#include "mem.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <iostream>
#include <vector>
static SDL_AudioSpec callbackSpec{};
static unsigned opens{},closes{};
static bool failOpen{};
static std::thread::id deviceThread;
extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int capture, const SDL_AudioSpec* requested, SDL_AudioSpec* actual, int) {
    assert(std::this_thread::get_id()==deviceThread && capture==1 && requested->freq==48000 && requested->channels==1 && requested->format==AUDIO_S16SYS);
    ++opens;if(failOpen)return 0;callbackSpec=*requested;*actual=*requested;return 77;
}
extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID id) {assert(std::this_thread::get_id()==deviceThread && id==77);++closes;callbackSpec={};}
extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID id,int) {assert(std::this_thread::get_id()==deviceThread && id==77);}
extern "C" const char* SDL_GetError() {return "Mock capture unavailable";}
static void publish(size_t frames=9600) {
    assert(callbackSpec.callback);
    std::vector<int16_t> pcm(frames);
    for(size_t i=0;i<frames;++i) pcm[i]=int16_t(1000*std::sin(2*3.141592653589793*1000*i/48000));
    callbackSpec.callback(callbackSpec.userdata,reinterpret_cast<uint8_t*>(pcm.data()),pcm.size()*2);
}
struct Machine {
    std::vector<uint8_t> rom=std::vector<uint8_t>(4096);
    SocPXA soc{deviceTypeTG50,16*1024*1024,rom.data(),uint32_t(rom.size()),nullptr,0,-1,0};
    void wr(uint32_t a,uint32_t v){assert(memAccess(cpuGetMem(soc.GetCpu()),a,4,true,&v));}
    void run(unsigned ms=10){soc.Run(ms*100000,100000000);}
    void reg(unsigned a,unsigned v){wr(0x40301688,0x20);wr(0x40301690,0x49);wr(0x40301688,a);wr(0x40301690,0x48);wr(0x40301688,v);wr(0x40301690,0x4a);}
    void configure(){wr(0xa0003000,0xeafffffe);cpuInvalidateCodeCache(soc.GetCpu());cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(soc.GetCpu(),15,0xa0003000);wr(0x41300004,0x100);reg(0,0x83);reg(1,0x60);reg(4,0x6a);reg(7,0x0c);wr(0x40400000,0x7701);}
    std::vector<uint8_t> save(){assert(soc.Save());auto d=soc.GetSavestate();return {(uint8_t*)d.data,(uint8_t*)d.data+d.size};}
};
int main() {
    deviceThread=std::this_thread::get_id();
    Machine m;m.configure();
    SdlMicrophone mic;auto hooks=mic.Source();
    mic.prepare=[&]{m.soc.SetMicrophoneSource(&hooks);return !m.soc.PowerSleeping();};
    failOpen=true;assert(!mic.Arm());assert(opens==1 && mic.State()=="off Mock capture unavailable");
    for(unsigned i=0;i<100;++i) mic.Service(m.soc.MicrophoneDemand());
    assert(opens==1); // no hidden retry
    failOpen=false;assert(mic.Arm());mic.Service(m.soc.MicrophoneDemand());publish();
    const auto pending=mic.Pending();assert(pending==9600);
    auto saved=m.save();
    auto bad=saved;auto* words=reinterpret_cast<uint32_t*>(bad.data());unsigned offset=1+2*words[0];bool changed=false;
    for(unsigned i=0;i<words[0];++i){if(words[1+2*i]==0x1d0){words[offset+37]=72;changed=true;}offset+=words[2+2*i];}
    assert(changed);
    auto mem=m.soc.GetMemoryData();std::vector<uint8_t> ram((uint8_t*)mem.data,(uint8_t*)mem.data+mem.size);
    assert(!m.soc.Load(bad.size(),bad.data()));
    assert(mic.State()=="on" && mic.Pending()==pending && closes==0 && opens==2 && m.save()==saved);
    assert(!std::memcmp(ram.data(),mem.data,mem.size));
    {
        SoC::InputCheckpoint cp(m.soc);
        publish(512);m.run(100);mic.Service(m.soc.MicrophoneDemand());
        assert(mic.Pending()==pending && mic.State()=="on" && closes==0);
        assert(cp.Restore(saved.size(),saved.data()));
    }
    assert(mic.Pending()==pending && m.save()==saved);
    m.run(100);assert(mic.Pending()<pending);
    saved=m.save();assert(m.soc.Load(saved.size(),saved.data()));
    assert(mic.State()=="off" && mic.Pending()==0 && closes==1);
    mic.Service(m.soc.MicrophoneDemand());assert(opens==2);
    assert(mic.Arm());mic.Service(m.soc.MicrophoneDemand());publish();assert(mic.Pending()==9600);mic.Removed(77);assert(mic.State()=="off Capture device disconnected" && closes==2);
    for(unsigned i=0;i<100;++i)mic.Service(m.soc.MicrophoneDemand());assert(opens==3);
    auto epoch=mic.Epoch();mic.Disarm();assert(!mic.ArmOnForeground(epoch));
    assert(mic.Arm());m.soc.Reset();assert(mic.State()=="off" && closes==3);
    m.soc.SetMicrophoneSource(nullptr);
    {
        SdlMicrophone stopped;
        assert(stopped.Arm());
        stopped.SetDeviceThread([](const std::function<void()>&) {});
        stopped.Disarm();
        assert(stopped.State()=="on"); // cancelled dispatch retains ownership for final SDL teardown
    }
    assert(closes==4);
    std::cout<<"Mock SDL failures/loss/epoch/reset and real SoC refused-load/checkpoint preserve live handle and queue\n";
}
