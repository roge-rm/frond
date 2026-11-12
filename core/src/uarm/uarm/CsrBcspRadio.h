#ifndef FROND_CSR_BCSP_RADIO_H
#define FROND_CSR_BCSP_RADIO_H
#include "BcspH4Bridge.h"
#include "BluetoothBackend.h"
#include "CsrControllerReset.h"
#include "get_emu_time.h"
#include "pxa_UART.h"
#include <functional>
#include <memory>
struct TG50uc;

class CsrBcspRadio {
 public:
    CsrBcspRadio(GetEmuTime,TG50uc*,PxaUart*);
    ~CsrBcspRadio();
    bool SetBackend(const std::string&,std::string&);
    // The machine's controller address, programmed on every controller connection.
    void SetAddress(const std::array<uint8_t,6>& address){backend.SetAddress(address);}
    bool Address(std::array<uint8_t,6>& address) const {return backend.Address(address);}
    std::string State() const;
    void PinsChanged();
    void ServiceReset();
    void BeginCheckpoint();
    void EndCheckpoint(bool sleeping);
    std::function<void()> CaptureInput();
    void Suspend();
    void Reset();
    csr::BcspTransport::Bytes Save() const;
    bool Load(const csr::BcspTransport::Bytes&);
 private:
    // Emulator-thread token for checkpoint closures released after teardown.
    std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
    GetEmuTime time;
    TG50uc* uc;
    PxaUart* uart;
    BluetoothBackend backend;
    csr::ControllerReset<BluetoothBackend> controllerReset;
    csr::BcspTransport transport{{250000000,250000000,12}};
    csr::BcspH4Bridge<BluetoothBackend> bridge{transport,backend};
    uint64_t clock{},lastPoll{};
    bool powered{},started{},needsRestart{},deferredReset{},deferredExternal{},controllerFailed{};
    unsigned checkpoints{},traceBytes{};
    std::string path,error;
    bool Powered() const;
    bool Ready() const;
    uint64_t Now() const;
    void Tick();
    void Write(uint_fast16_t);
    uint_fast16_t Read();
    void Fence(bool restart,bool retainController=false);
};
#endif
