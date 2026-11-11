#include "SdlMicrophone.h"
#include <cassert>
#include <iostream>
#include <vector>

static double tone(uint32_t rate, double hz) {
    MicrophoneQueue queue;
    queue.Configure({true, false, AudioInputRoute::internalMic, rate, 1});
    std::vector<int16_t> input(9600);
    for (size_t i=0;i<input.size();++i) input[i]=int16_t(10000*std::sin(2*3.141592653589793*hz*i/48000));
    queue.Publish(input.data(), input.size(), queue.Generation());
    double sum=0;
    const size_t frames=rate/6;
    for(size_t i=0;i<frames;++i) {
        const auto sample=queue.Pull(AudioInputRoute::internalMic,rate,1);
        if(i>rate/100) sum+=double(sample)*sample;
    }
    return std::sqrt(sum/(frames-rate/100-1));
}
int main() {
    for(auto rate : {8000u,22050u,44100u}) {
        const auto rms=tone(rate,1000);
        std::cout<<rate<<" Hz, 1kHz RMS "<<rms<<'\n';
        assert(rms>6700 && rms<7300);
    }
    const auto alias=tone(8000,12000);
    std::cout<<"8kHz out, 12kHz input RMS "<<alias<<'\n';
    assert(alias<30);
    MicrophoneQueue queue;
    auto demand=AudioInputDemand{true,false,AudioInputRoute::internalMic,22050,1};
    queue.Configure(demand);
    const auto gen=queue.Generation();
    std::vector<int16_t> input(15000,1000);
    queue.Publish(input.data(), input.size(), gen);
    assert(queue.Pending()==12000 && queue.Dropped()==3000);
    queue.Fence(true);
    queue.Publish(input.data(),1,gen);
    assert(queue.Pull(demand.route,22050,1)==0 && queue.Pending()==12000);
    demand.active=false;queue.Configure(demand);
    assert(queue.Pending()==12000);
    queue.Fence(false);
    demand.active=true;
    assert(queue.Pull(demand.route,22050,1)==0); // filter startup
    assert(queue.Pending()==12000);
    queue.Reset();assert(queue.Pending()==0 && queue.Generation()!=gen);
    queue.Configure(demand);queue.Publish(input.data(),100,gen);assert(queue.Pending()==0);
    assert(queue.Pull(demand.route,22050,1)==0 && queue.Underruns()==1);
    queue.Publish(input.data(),100,queue.Generation());
    const auto oldRateGeneration=queue.Generation();
    demand.rateNumerator=8000;queue.Configure(demand);assert(queue.Pending()==0 && queue.Generation()!=oldRateGeneration);
    queue.Publish(input.data(),100,oldRateGeneration);assert(queue.Pending()==0);
    demand.active=false;queue.Configure(demand);queue.Publish(input.data(),100,queue.Generation());assert(queue.Pending()==0);

    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    assert(SDL_Init(SDL_INIT_AUDIO|SDL_INIT_EVENTS)==0);
    {
        SdlMicrophone mic;
        int dispatch=0;
        mic.SetDeviceThread([&](const std::function<void()>& f){++dispatch;f();});
        assert(mic.State()=="off");
        assert(mic.Arm() && mic.State()=="on");
        auto source=mic.Source();
        source.fence(source.context,true);
        mic.Service({true,false,AudioInputRoute::internalMic,22050,1});
        assert(source.pull(source.context,AudioInputRoute::internalMic,22050,1)==0);
        source.fence(source.context,false);
        assert(mic.State()=="on");
        mic.AppEventOnDeviceThread(SDL_APP_WILLENTERBACKGROUND);
        assert(mic.State()=="off");
        assert(!mic.Arm());
        mic.AppEventOnDeviceThread(SDL_APP_DIDENTERFOREGROUND);
        mic.Service({});assert(mic.State().substr(0,3)=="off");
        assert(mic.ArmOnForeground(mic.Epoch()));mic.Service({});assert(mic.State()=="on");
        source.disarm(source.context);assert(mic.State()=="off");
        mic.Service({});assert(mic.State()=="off");
        assert(mic.ArmOnForeground(mic.Epoch()));mic.AppEventOnDeviceThread(SDL_APP_WILLENTERBACKGROUND);
        mic.AppEventOnDeviceThread(SDL_APP_DIDENTERFOREGROUND);mic.Service({});assert(mic.State()=="off");
        const auto oldEpoch = mic.Epoch();
        mic.AppEventOnDeviceThread(SDL_APP_WILLENTERBACKGROUND);
        assert(!mic.ArmOnForeground(oldEpoch));
        mic.AppEventOnDeviceThread(SDL_APP_DIDENTERFOREGROUND);
        mic.Service({});assert(mic.State()=="off");
        assert(dispatch>0);
    }
    {
        // The user's choice outlasts a guest sleep: capture the guest dropped comes back once it's awake,
        // and only while the user has it on.
        SdlMicrophone mic;
        bool awake=true;
        mic.awake=[&]{return awake;};
        auto source=mic.Source();
        mic.Service({});assert(mic.State()=="off");
        assert(mic.Arm() && mic.State()=="on");
        awake=false;source.disarm(source.context);assert(mic.State()=="off");
        mic.Service({});assert(mic.State()=="off");
        awake=true;mic.Service({});assert(mic.State()=="on");
        mic.Disarm();mic.Service({});assert(mic.State()=="off");
        assert(mic.Arm());mic.AppEventOnDeviceThread(SDL_APP_WILLENTERBACKGROUND);
        mic.AppEventOnDeviceThread(SDL_APP_DIDENTERFOREGROUND);mic.Service({});assert(mic.State()=="off");
    }
    SDL_Quit();
    std::cout<<"Bounded capture, rates, alias rejection, fence, generations, re-arm after sleep and SDL dummy lifecycle pass\n";
}
