#ifndef __EMSCRIPTEN__
#include "UsbHostBridge.h"
#include "soc_PXA.h"
#include "../../common/UsbHostBridge.cpp"
bool UsbHostBridge::Start(SocPXA* soc, const Config& config, std::string& error) {
    if(!soc) return Start(Backend{},config,error);
    Backend backend{
        [soc] { return soc->GetUsbPort(); },
        [soc](bool attached) { soc->SetUsbTestCable(attached); },
        [soc] { return soc->UsbTestCableAttached(); },
        [soc] { return soc->UsbDefaultCableAttached(); },
        [soc] { return soc->UsbHostIoPaused(); },
        [soc] { return soc->GetTime(); },
        [soc] { soc->KickUsbHost(); },
        [soc](const UsbHostHooks* hooks) { soc->SetUsbHostHooks(hooks); }
    };
    return Start(std::move(backend),config,error);
}
#endif
