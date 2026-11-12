#include "uartdev_bcm2035.h"

#include <cstdio>

#include "savestate/savestateAll.h"
#include "BluetoothBackend.h"
#include "BluetoothBootstrap.h"
#include "BluetoothUartMessaging.h"
#include "pxa_GPIO.h"
#include <string>
#include <vector>
#include "scheduler.h"

#define SAVESTATE_VERSION 2

namespace {
    constexpr uint8_t CMD_ENTER_UPLOAD[] = {0x01, 0x2e, 0xfc, 0x00};
    constexpr uint8_t CMD_ENTER_UPLOAD_RESPONSE[] = {0x04, 0x0e, 0x04, 0x01, 0x2e, 0xfc, 0x00};
    constexpr uint8_t INVALID_ACK_UPLOAD_MODE_RESPONSE[] = {0xff, 0xff};

    constexpr uint64_t CMD_ENTER_UPLOAD_RESPONSE_DELAY = 10_msec;
    constexpr uint64_t ACK_UPLOAD_MODE_DELAY = 150_msec;
    constexpr uint64_t WAIT_FOR_RECEIVE_ACK = 150_msec;

    enum class State : uint8_t {
        receiveUploadCmd,
        waitForSendCmdEnterUploadResponse,
        sendCmdUploadResponse,
        waitForSendAckUploadMode,
        sendAckUploadMode,
        waitForReceiveAckDelay
    };

    enum class Mode : uint8_t { send, receive, wait };
}  // namespace

struct Bcm2035 {
    explicit Bcm2035(GetEmuTime clock,PxaGpio* gpio,bool tx):getTime(clock),gpio(gpio),txBoard(tx),bootstrap(!tx){}
    ~Bcm2035(){backend.Close();}
    GetEmuTime getTime;

    State state{};
    Mode mode{};

    const uint8_t* buffer{};
    uint32_t bufferSize{};
    uint32_t bufferIndex{};

    uint64_t delay{};
    uint64_t transitionTimestamp{};
    PxaGpio* gpio{};
    PxaUart* uart{};
    bool txBoard{},enabled{},blocked{},lastPowered{},powerKnown{};
    unsigned checkpoints{};
    bool deferredPowerReset{};
    std::vector<unsigned> restoringPower;
    uint64_t lastPoll{};
    std::string path,lastError;
    BluetoothBackend backend;
    BluetoothBootstrap bootstrap;
    BluetoothUartMessaging uartMessaging;
    uint64_t Now(){return getTime.getTimeF(getTime.userdata);}
    bool Powered() const {
        if(!gpio)return true;
        return pxaGpioGetState(gpio,txBoard?17:32)==PxaGpioStateHigh &&
            pxaGpioGetState(gpio,txBoard?83:80)==PxaGpioStateHigh;
    }
    void PowerChanged(){
        if(!enabled)return;
        const bool on=Powered();
        if(checkpoints){
            if(restoringPower.empty() && powerKnown && on!=lastPowered)deferredPowerReset=true;
            return;
        }
        if(deferredPowerReset || (powerKnown && on!=lastPowered)){
            backend.Close();bootstrap.Reset();uartMessaging.Reset();blocked=!on;lastPoll=0;if(on)lastError.clear();
        }
        deferredPowerReset=false;lastPowered=on;powerKnown=true;
    }
    void Suspend(){backend.Close();bootstrap.Reset();uartMessaging.Reset();blocked=true;powerKnown=true;lastPowered=Powered();lastPoll=0;deferredPowerReset=false;}
    bool EnsureBackend(){
        if(blocked || !Powered())return false;
        if(backend.Connected())return true;
        if(!backend.Open(path,lastError)){blocked=true;return false;}
        return true;
    }
    bool SendHci(const uint8_t* bytes,size_t size){
        if(!EnsureBackend())return false;
        if(txBoard){
            const auto result=uartMessaging.Command(bytes,size,[this](const uint8_t* b,size_t n){return bootstrap.Respond(b,n);});
            if(result!=BluetoothUartMessaging::Result::unhandled)return result==BluetoothUartMessaging::Result::handled;
        }
        // The virtual controller does not implement other board-vendor extensions.
        // Return a correctly framed Unknown HCI Command completion; never claim
        // that a factory address or low-power register was changed.
        if(size>=4 && bytes[0]==1 && (bytes[2]&0xfc)==0xfc &&
           !(txBoard && bytes[1]==1 && bytes[2]==0xfc)){
            const uint8_t response[]={4,14,4,1,bytes[1],bytes[2],1};
            return bootstrap.Respond(response,sizeof(response));
        }
        return backend.Write(bytes,size);
    }
    void TickHost(){
        if(checkpoints || !enabled)return;
        PowerChanged();if(blocked || !Powered())return;
        const auto now=Now();bootstrap.Tick(now);
        if(now-lastPoll>=1000000){lastPoll=now;const bool was=backend.Connected();backend.Poll();if(was && !backend.Connected()){lastError="Bluetooth controller disconnected; turn Bluetooth off and on in Palm OS.";Suspend();}}
    }
    uint_fast16_t ReadHost(){
        TickHost();if(checkpoints || blocked || !Powered())return UART_CHAR_NONE;
        // The controller owns queued bytes while the guest closes RTS, including
        // hardware automatic RTS at the UART FIFO threshold.
        if(uart && (txBoard?!(pxaUartGetModemOutputs(uart)&PXA_UART_RTS):!pxaUartHwReceiveReady(uart)))return UART_CHAR_NONE;
        // The UART clocks individual bytes. A local completion must wait for
        // the current controller packet, even across callbacks/checkpoints.
        if(backend.FrameInProgress())return backend.Read();
        if(txBoard && !uartMessaging.PendingControllerInput(backend.Pending()!=0,[this](const uint8_t* b,size_t n){return bootstrap.Respond(b,n);})){lastError="Bluetooth UART wake queue overflow; turn Bluetooth off and on in Palm OS.";Suspend();return UART_CHAR_NONE;}
        const auto c=bootstrap.Read(Now());
        return c!=UART_CHAR_NONE?c:txBoard && !uartMessaging.CanReadController()?UART_CHAR_NONE:backend.Read();
    }
    void WriteHost(uint8_t c){
        if(checkpoints)return;
        PowerChanged();if(blocked || !Powered())return;
        if(!bootstrap.Write(c,Now(),[this](const uint8_t*b,size_t n){return SendHci(b,n);})){lastError="Unsupported or malformed Bluetooth UART stream; turn Bluetooth off and on in Palm OS.";Suspend();}
    }
    bool ActiveHost() const {return !checkpoints && !blocked && Powered() && (backend.Connected() || bootstrap.Pending());}


    template <typename T>
    void DoSaveLoad(T& chunkHelper) {
        uint8_t state8 = static_cast<uint8_t>(state);
        uint8_t mode8 = static_cast<uint8_t>(mode);

        chunkHelper.Do(typename T::Pack8() << state8 << mode8)
            .Do32(bufferSize)
            .Do32(bufferIndex)
            .Do64(delay)
            .Do64(transitionTimestamp);

        state = static_cast<State>(state8);
        mode = static_cast<Mode>(mode8);
    }
};

static void bcm2035SetModeSend(Bcm2035* bcm2035, const uint8_t* buffer, size_t bufferSize) {
    bcm2035->buffer = buffer;
    bcm2035->bufferSize = bufferSize;
    bcm2035->bufferIndex = 0;

    bcm2035->mode = Mode::send;
}

static void bcm2035SetModeReceive(Bcm2035* bcm2035, const uint8_t* buffer, size_t bufferSize) {
    bcm2035->buffer = buffer;
    bcm2035->bufferSize = bufferSize;
    bcm2035->bufferIndex = 0;

    bcm2035->mode = Mode::receive;
}

static void bcm2035SetModeWait(Bcm2035* bcm2035, uint64_t delay) {
    bcm2035->transitionTimestamp = bcm2035->getTime.getTimeF(bcm2035->getTime.userdata);
    bcm2035->delay = delay;

    bcm2035->mode = Mode::wait;
}

static void bcm2035SetState(Bcm2035* bcm2035, State state) {
    switch (state) {
        case State::receiveUploadCmd:
            bcm2035SetModeReceive(bcm2035, CMD_ENTER_UPLOAD, sizeof(CMD_ENTER_UPLOAD));
            break;

        case State::waitForSendCmdEnterUploadResponse:
            bcm2035SetModeWait(bcm2035, CMD_ENTER_UPLOAD_RESPONSE_DELAY);
            break;

        case State::sendCmdUploadResponse:
            bcm2035SetModeSend(bcm2035, CMD_ENTER_UPLOAD_RESPONSE,
                               sizeof(CMD_ENTER_UPLOAD_RESPONSE));
            break;

        case State::waitForSendAckUploadMode:
            bcm2035SetModeWait(bcm2035, ACK_UPLOAD_MODE_DELAY);
            break;

        case State::sendAckUploadMode:
            bcm2035SetModeSend(bcm2035, INVALID_ACK_UPLOAD_MODE_RESPONSE,
                               sizeof(INVALID_ACK_UPLOAD_MODE_RESPONSE));
            break;

        case State::waitForReceiveAckDelay:
            bcm2035SetModeWait(bcm2035, WAIT_FOR_RECEIVE_ACK);
    }

    bcm2035->state = state;
}

static State transitionNext(State currentState) {
    switch (currentState) {
        case State::receiveUploadCmd:
            fprintf(stderr, "BCM2035: upload command received\n");
            return State::waitForSendCmdEnterUploadResponse;

        case State::waitForSendCmdEnterUploadResponse:
            fprintf(stderr, "BCM2035: sending EnterUpload response\n");
            return State::sendCmdUploadResponse;

        case State::sendCmdUploadResponse:
            fprintf(stderr, "BCM2035: response sent off\n");
            return State::waitForSendAckUploadMode;

        case State::waitForSendAckUploadMode:
            fprintf(stderr, "BCM2035: sending ACK\n");
            return State::sendAckUploadMode;

        case State::sendAckUploadMode:
            fprintf(stderr, "BCM2035: ACK sent off\n");
            return State::waitForReceiveAckDelay;

        case State::waitForReceiveAckDelay:
            fprintf(stderr, "BCM2035: resetting state machine\n");
            return State::receiveUploadCmd;

        default:
            ERR("unreachable");
    }
}

static uint_fast16_t bcm2035ReadF(void* userData) {
    Bcm2035* bcm2035 = reinterpret_cast<Bcm2035*>(userData);

    if(bcm2035->enabled)return bcm2035->ReadHost();
    if(bcm2035->txBoard)return UART_CHAR_NONE;
    if (bcm2035->mode != Mode::send) return UART_CHAR_NONE;

    uint8_t c = bcm2035->buffer[bcm2035->bufferIndex++];

    if (bcm2035->bufferIndex >= bcm2035->bufferSize) {
        bcm2035SetState(bcm2035, transitionNext(bcm2035->state));
    }

    return c;
}

static void bcm2035WriteF(uint_fast16_t chr, void* userData) {
    Bcm2035* bcm2035 = reinterpret_cast<Bcm2035*>(userData);

    if(bcm2035->enabled){bcm2035->WriteHost(uint8_t(chr));return;}
    if(bcm2035->txBoard)return;
    if (bcm2035->mode != Mode::receive) return;

    if (static_cast<uint_fast16_t>(bcm2035->buffer[bcm2035->bufferIndex++]) != chr) {
        bcm2035SetState(bcm2035, State::receiveUploadCmd);
    }

    if (bcm2035->bufferIndex >= bcm2035->bufferSize) {
        bcm2035SetState(bcm2035, transitionNext(bcm2035->state));
    }
}

static bool bcm2035ClientIsActive(void* userData) {
    Bcm2035* bcm2035 = reinterpret_cast<Bcm2035*>(userData);

    return bcm2035->enabled?bcm2035->ActiveHost():!bcm2035->txBoard && bcm2035->state != State::receiveUploadCmd;
}

static void bcm2035ClientTick(void* userData) {
    Bcm2035* bcm2035 = reinterpret_cast<Bcm2035*>(userData);

    if(bcm2035->enabled){bcm2035->TickHost();return;}
    if(bcm2035->txBoard)return;
    if (bcm2035->mode != Mode::wait) return;

    const uint64_t timestamp = bcm2035->getTime.getTimeF(bcm2035->getTime.userdata);

    if (timestamp - bcm2035->transitionTimestamp >= bcm2035->delay) {
        bcm2035SetState(bcm2035, transitionNext(bcm2035->state));
    }
}

struct Bcm2035* bcm2035Init(GetEmuTime getTime,PxaGpio* gpio,bool tx) {
    auto* bcm2035 = new Bcm2035(getTime,gpio,tx);
    bcm2035SetState(bcm2035, State::receiveUploadCmd);

    return bcm2035;
}

void bcm2035RegisterWithUart(struct Bcm2035* bcm2035, PxaUart* uart) {
    bcm2035->uart=uart;
    pxaUartSetFuncs(uart, bcm2035ReadF, bcm2035WriteF, bcm2035ClientIsActive, bcm2035ClientTick,
                    bcm2035);
}

template <typename T>
void bcm2035Save(struct Bcm2035* bcm2035, T& savestate) {
    auto chunk = savestate.GetChunk(ChunkType::bcm2035, SAVESTATE_VERSION);
    if (!chunk) abort();

    SaveChunkHelper helper(*chunk);
    bcm2035->DoSaveLoad(helper);
    bcm2035->bootstrap.Transfer(helper);
    bcm2035->uartMessaging.Transfer(helper);
}

template <typename T>
void bcm2035Load(struct Bcm2035* bcm2035, T& loader) {
    // TX sessions predating the radio model have no controller chunk.
    if(!loader.HasChunk(ChunkType::bcm2035))return;
    uint32_t version=0;
    auto chunk = loader.GetChunkOrFail(ChunkType::bcm2035, SAVESTATE_VERSION, "bcm2035",version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    bcm2035->DoSaveLoad(helper);
    if(version)bcm2035->bootstrap.Transfer(helper);
    else bcm2035->bootstrap.Reset();
    if(version>=2)bcm2035->uartMessaging.Transfer(helper);
    else bcm2035->uartMessaging.Reset();
    const auto state=static_cast<unsigned>(bcm2035->state),mode=static_cast<unsigned>(bcm2035->mode);
    const bool bufferState=state==0 || state==2 || state==4;
    const auto expectedMode=state==0?unsigned(Mode::receive):bufferState?unsigned(Mode::send):unsigned(Mode::wait);
    const uint32_t expectedSize=state==0?4:state==2?7:2;
    if(state>5 || mode!=expectedMode || !bcm2035->bootstrap.Valid() || !bcm2035->uartMessaging.Valid() ||
       (bufferState && (bcm2035->bufferSize!=expectedSize || bcm2035->bufferIndex>=expectedSize))){loader.NotifyError();return;}

    switch (bcm2035->state) {
        case State::receiveUploadCmd:
            bcm2035->buffer = CMD_ENTER_UPLOAD;
            break;

        case State::sendCmdUploadResponse:
            bcm2035->buffer = CMD_ENTER_UPLOAD_RESPONSE;
            break;

        case State::sendAckUploadMode:
            bcm2035->buffer = INVALID_ACK_UPLOAD_MODE_RESPONSE;
            break;

        default:
            break;
    }
}

template void bcm2035Save<Savestate<ChunkType>>(Bcm2035* bcm2035, Savestate<ChunkType>& savestate);
template void bcm2035Save<SavestateProbe<ChunkType>>(Bcm2035* bcm2035,
                                                     SavestateProbe<ChunkType>& savestate);
template void bcm2035Load<SavestateLoader<ChunkType>>(Bcm2035* bcm2035,
                                                      SavestateLoader<ChunkType>& loader);

// Host configuration/lifecycle is intentionally outside the guest-state chunk.
bool bcm2035SetBackend(Bcm2035* b,const std::string& path,std::string& error){
    if(b->checkpoints){error="Bluetooth cannot change during temporary guest work.";return false;}
    if(path==b->path && b->enabled==!path.empty()){error.clear();return true;}
    if(!path.empty()){BluetoothBackend candidate;if(!candidate.Open(path,error))return false;}
    b->backend.Close();b->deferredPowerReset=false;b->restoringPower.clear();b->path=path;b->enabled=!path.empty();b->bootstrap.Reset();b->uartMessaging.Reset();
    bcm2035SetState(b,State::receiveUploadCmd);b->blocked=b->Powered();
    b->lastPowered=b->Powered();b->powerKnown=true;b->lastPoll=0;b->lastError.clear();
    if(b->uart)pxaUartKick(b->uart);
    error.clear();return true;
}
void bcm2035SetAddress(Bcm2035* b,const std::array<uint8_t,6>& address){if(b)b->backend.SetAddress(address);}
bool bcm2035Address(Bcm2035* b,std::array<uint8_t,6>& address){return b && b->backend.Address(address);}
std::string bcm2035BackendState(Bcm2035* b){
    if(!b || !b->enabled)return "off";
    if(b->checkpoints && b->backend.Connected())return "connected";
    if(!b->lastError.empty())return "disconnected: "+b->lastError;
    if(!b->Powered())return "configured; radio off";
    return b->backend.Connected()?"connected":b->blocked?"configured; turn Bluetooth off and on in Palm OS":"configured";
}
void bcm2035BeginCheckpoint(Bcm2035* b){if(b)++b->checkpoints;}
void bcm2035EndCheckpoint(Bcm2035* b,bool sleeping){
    if(!b || !b->checkpoints)return;
    --b->checkpoints;
    while(!b->restoringPower.empty() && b->checkpoints<=b->restoringPower.back())b->restoringPower.pop_back();
    if(!b->checkpoints){b->PowerChanged();if(sleeping && b->enabled)b->Suspend();}
}
std::function<void()> bcm2035CaptureInput(Bcm2035* b){
    if(!b)return {};
    const auto pending=b->deferredPowerReset;const auto restoring=b->restoringPower;
    return [b,pending,restoring]{
        b->deferredPowerReset=pending;b->restoringPower=restoring;
        // Input restoration precedes nested GPIO load. Ignore those synthetic
        // edges until that load returns to the enclosing checkpoint depth.
        b->restoringPower.push_back(b->checkpoints);
    };
}
void bcm2035Suspend(Bcm2035* b){if(b && b->enabled && !b->checkpoints)b->Suspend();}
void bcm2035PowerChanged(Bcm2035* b){if(b)b->PowerChanged();}
void bcm2035Stop(Bcm2035* b){
    if(!b)return;
    if(b->uart)pxaUartSetFuncs(b->uart,nullptr,nullptr,nullptr,nullptr,nullptr);
    delete b;
}
