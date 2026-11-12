#include "CsrBcspRadio.h"
#include "mmiodev_TG50uc.h"
#include "Tg50Trace.h"
#include <sstream>
#include <memory>

CsrBcspRadio::CsrBcspRadio(GetEmuTime t,TG50uc* pins,PxaUart* port):time(t),uc(pins),uart(port) {
    clock=Now();powered=Powered();
    pxaUartSetPacing(uart,true);
    pxaUartSetFuncs(uart,
        [](void* p)->uint_fast16_t{return static_cast<CsrBcspRadio*>(p)->Read();},
        [](uint_fast16_t c,void* p){static_cast<CsrBcspRadio*>(p)->Write(c);},
        [](void* p){auto* r=static_cast<CsrBcspRadio*>(p);return !r->checkpoints && (r->controllerReset.Pending() || (r->powered && !r->needsRestart));},
        [](void* p){static_cast<CsrBcspRadio*>(p)->Tick();},this);
    pxaUartSetModeObserver(uart,[](void* p){static_cast<CsrBcspRadio*>(p)->PinsChanged();},this);
    pxaUartSetModemInputs(uart,0);
    PinsChanged();
}
CsrBcspRadio::~CsrBcspRadio(){
    *alive=false;
    pxaUartSetModeObserver(uart,nullptr,nullptr);
    pxaUartSetFuncs(uart,nullptr,nullptr,nullptr,nullptr,nullptr);
    pxaUartSetModemInputs(uart,0);
}
uint64_t CsrBcspRadio::Now() const{return time.getTimeF(time.userdata);}
bool CsrBcspRadio::Powered() const{return tg50ucGpioIsOutput(uc,0,6) && tg50ucGpioState(uc,0,6);}
bool CsrBcspRadio::Ready() const{
    const auto mode=pxaUartIrState(uart);
    return pxaUartReceiverEnabled(uart) && pxaUartPlainEnabled(uart) &&
        mode.baud==921600 && (mode.lcr&0x3f)==0x1b;
}
void CsrBcspRadio::Fence(bool restart,bool retainController){
    TG50_TRACE("CSR fence t=%llu A6=%u connected=%u retain=%u restart=%u\n",(unsigned long long)Now(),Powered(),backend.Connected(),retainController,restart);
    if(retainController && backend.Connected() && !controllerFailed){
        controllerReset.Begin(Now(),bridge.CommandsPending(),bridge.CommandCredit());controllerReset.Pump(backend,Now());
        if(!controllerReset.Error().empty()){error=controllerReset.Error();controllerFailed=true;}
    }
    else{backend.Close();controllerReset.Clear();}
    bridge.ClearLocal();transport.Stop();started=false;
    if(retainController && backend.Connected() && !controllerFailed && !controllerReset.Pending())bridge.ControllerResetObserved();
    needsRestart=restart || controllerFailed;lastPoll=0;
    pxaUartSetModemInputs(uart,0);
}
void CsrBcspRadio::PinsChanged(){
    const bool on=Powered();
    if(checkpoints){if(on!=powered)deferredReset=true;return;}
    const bool changed=on!=powered || deferredReset;
    if(changed){
        if(deferredExternal){controllerFailed=false;error.clear();}
        Fence(on,!deferredExternal);powered=on;
        if(!on && !controllerFailed){needsRestart=false;error.clear();}
        else if(!deferredReset && !controllerFailed){needsRestart=false;error.clear();}
    }
    deferredReset=deferredExternal=false;
    const auto mode=pxaUartIrState(uart);
    (void)mode;
    TG50_TRACE("CSR pins t=%llu A6=%u A0out=%u A0=%u A14=%u baud=%u lcr=%02x rts=%u\n",
        (unsigned long long)Now(),on,tg50ucGpioIsOutput(uc,0,0),tg50ucGpioState(uc,0,0),
        tg50ucGpioState(uc,0,14),mode.baud,mode.lcr,pxaUartGetModemOutputs(uart));
    if(changed){
        pxaUartSetModemInputs(uart,on && !needsRestart && !controllerReset.Pending() ? PXA_UART_CTS:0);
        pxaUartKick(uart);
    }
}
void CsrBcspRadio::ServiceReset(){
    if(checkpoints || controllerFailed || !controllerReset.Pending())return;
    controllerReset.Pump(backend,Now());
    if(!controllerReset.Error().empty()){error=controllerReset.Error();controllerFailed=true;Fence(true);return;}
    if(controllerReset.Pending())return;
    bridge.ControllerResetObserved();
    pxaUartSetModemInputs(uart,powered && !needsRestart ? PXA_UART_CTS:0);pxaUartKick(uart);
    TG50_TRACE("CSR reset-complete t=%llu A6=%u connected=%u\n",(unsigned long long)Now(),Powered(),backend.Connected());
}
void CsrBcspRadio::Tick(){
    if(checkpoints || controllerFailed)return;
    clock=Now();ServiceReset();
    if(controllerReset.Pending() || controllerFailed)return;
    if(!powered || needsRestart || !Ready())return;
    if(!started){
        transport.Start(clock);started=true;
        if(!path.empty() && !backend.Connected() && !backend.Open(path,error)){
            Fence(true);return;
        }
        TG50_TRACE("CSR start t=%llu A6=%u connected=%u\n",(unsigned long long)clock,Powered(),backend.Connected());
    }
    transport.Tick(clock);
    if(transport.Failed()){error="BCSP retry exhausted; turn Bluetooth off and on in Palm OS.";Fence(true,true);return;}
    if(clock-lastPoll<1000000)return;
    lastPoll=clock;
    if(!backend.Connected()){
        if(const auto* p=transport.Peek()){
            TG50_TRACE("CSR unsupported channel=%u t=%llu bytes=",p->channel,(unsigned long long)clock);
            for(auto c:p->payload){(void)c;TG50_TRACE("%02x",c);}TG50_TRACE("\n");
            error=p->channel==2?"Unsupported CSR channel 2 request.":"No virtual Bluetooth controller selected.";
            transport.Pop();
        }
        return;
    }
    bridge.Pump(clock);
    if(!bridge.Error().empty()){error=bridge.Error()+"; select the Bluetooth controller again.";controllerFailed=true;Fence(true);}
}
void CsrBcspRadio::Write(uint_fast16_t c){
    if(checkpoints || !powered || needsRestart || !Ready())return;
    Tick();if(!started)return;
    if(traceBytes++<8192)TG50_TRACE("CSR byte t=%llu guest=%03x\n",(unsigned long long)clock,unsigned(c));
    if(c&~0xffu){error="Errored BTUART frame.";Fence(true,true);return;}
    const auto generation=transport.Generation();const auto old=transport.State();
    transport.Write(c,clock);
    if(old==csr::BcspTransport::Link::active && generation!=transport.Generation()){
        error="BCSP peer reset; turn Bluetooth off and on in Palm OS.";Fence(true,true);
    }
}
uint_fast16_t CsrBcspRadio::Read(){
    Tick();if(checkpoints || !started || needsRestart || !Ready())return UART_CHAR_NONE;
    const auto c=transport.Read(clock,pxaUartGetModemOutputs(uart)&PXA_UART_RTS);
    if(c!=UART_CHAR_NONE && traceBytes++<8192)TG50_TRACE("CSR byte t=%llu controller=%03x\n",(unsigned long long)clock,unsigned(c));
    return c;
}
bool CsrBcspRadio::SetBackend(const std::string& candidate,std::string& problem){
    if(checkpoints){problem="Bluetooth is busy with a state operation.";return false;}
    if(candidate.empty()){path.clear();error.clear();controllerFailed=false;Fence(true);problem.clear();return true;}
    if(!backend.Open(candidate,problem))return false;
    path=candidate;error.clear();controllerFailed=false;controllerReset.Clear();transport.Stop();bridge.ClearLocal();started=false;
    needsRestart=Powered();powered=Powered();lastPoll=0;problem.clear();PinsChanged();return true;
}
std::string CsrBcspRadio::State() const{
    if(!error.empty())return "error: "+error;
    if(path.empty())return "off";
    if(bridge.UnsupportedControl())return "error: Unsupported CSR channel 2 request.";
    std::ostringstream out;out<<"radio="<<(powered?"on":"off")<<"; BCSP="<<unsigned(transport.State());
    if(needsRestart)out<<"; radio restart required";
    if(controllerReset.Pending())out<<"; controller reset pending";
    if(backend.Connected())out<<"; provider connected";
    out<<"; ACL observed="<<bridge.ObservedAclLinks();
    if(!bridge.AclHistoryKnown())out<<"; ACL history=unknown";
    return out.str();
}
void CsrBcspRadio::BeginCheckpoint(){++checkpoints;}
void CsrBcspRadio::EndCheckpoint(bool sleeping){
    if(!checkpoints)return;
    if(!--checkpoints){PinsChanged();if(sleeping)Suspend();}
}
std::function<void()> CsrBcspRadio::CaptureInput(){
    struct Scope {
        CsrBcspRadio& radio;
        std::shared_ptr<bool> alive;
        explicit Scope(CsrBcspRadio& r):radio(r),alive(r.alive){radio.BeginCheckpoint();}
        ~Scope(){if(*alive)radio.EndCheckpoint(false);}
    };
    auto scope=std::make_shared<Scope>(*this);
    const auto saved=transport;const auto savedClock=clock,savedPoll=lastPoll;
    const auto savedPowered=powered,savedStarted=started,savedRestart=needsRestart,savedDeferred=deferredReset,savedExternal=deferredExternal;
    return [this,scope,saved,savedClock,savedPoll,savedPowered,savedStarted,savedRestart,savedDeferred,savedExternal]{
        if(!*scope->alive)return;
        transport=saved;clock=savedClock;lastPoll=savedPoll;powered=savedPowered;started=savedStarted;
        needsRestart=savedRestart;deferredReset=savedDeferred;deferredExternal=savedExternal;
    };
}
// The radio's supply (A6 on the Sony microcontroller) stays on through PXA sleep, so a powered
// radio keeps its BCSP link and controller, and the original driver carries on after wake.
void CsrBcspRadio::Suspend(){
    if(powered && Powered() && !controllerFailed)return;
    if(checkpoints){deferredReset=true;return;}Fence(true,true);
}
void CsrBcspRadio::Reset(){
    if(checkpoints){deferredReset=deferredExternal=true;return;}
    controllerFailed=false;Fence(true);powered=Powered();needsRestart=powered;error.clear();
}
csr::BcspTransport::Bytes CsrBcspRadio::Save() const{
    auto data=transport.Save(clock);
    data.insert(data.begin(),{3,uint8_t(powered),uint8_t(started),uint8_t(needsRestart),uint8_t(Powered()),uint8_t(deferredReset || Powered()!=powered),uint8_t(deferredExternal)});
    return data;
}
bool CsrBcspRadio::Load(const csr::BcspTransport::Bytes& data){
    if(data.empty()){
        transport.Stop();started=false;clock=Now();powered=Powered();needsRestart=powered;
        deferredExternal=checkpoints && deferredExternal;deferredReset=deferredExternal;return true;
    }
    if(data.size()<4 || (data[0]!=1 && data[0]!=2 && data[0]!=3) || data[1]>1 || data[2]>1 || data[3]>1)return false;
    const size_t header=data[0]==1?4:data[0]==2?6:7;
    if(data.size()<header || (data[0]>=2 && (data[4]>1 || data[5]>1)) || (data[0]==3 && (data[6]>1 || (data[6] && !data[5]))))return false;
    const bool actual=data[0]==1?bool(data[1]):bool(data[4]);
    const bool deferred=data[0]>=2 && data[5];
    const bool external=data[0]==3 && data[6];
    if(actual!=Powered() || (bool(data[1])!=actual && !deferred))return false;
    auto candidate=transport;uint64_t candidateClock=0;
    if(!candidate.Load({data.begin()+header,data.end()},candidateClock) ||
       (data[2] && (!data[1] || data[3])) ||
       (bool(data[2])!=(candidate.State()!=csr::BcspTransport::Link::off && candidate.State()!=csr::BcspTransport::Link::failed)))return false;
    transport=std::move(candidate);clock=candidateClock;powered=data[1];started=data[2];needsRestart=data[3];// A core reset loads power-on state after requesting its external fence.
    // Checkpoint restoration first restores this host-side ownership marker.
    deferredExternal=external || (checkpoints && deferredExternal);deferredReset=deferred || deferredExternal;
    return true;
}
