#ifndef __EMSCRIPTEN__
#include "UsbHostBridge.h"
#include "soc_PXA.h"
#include "soc_IMX1.h"
#include "../../common/UsbHostBridge.cpp"
bool UsbHostBridge::Start(SocPXA* soc, const Config& config, std::string& error) {
    if(!soc) return Start(Backend{},config,error);
    if(config.kind!=HostSerial::Kind::off && soc->GamepadAttached()){
        error="The game controller is attached; detach it first.";
        return false;
    }
    Backend backend{
        [soc] { return soc->GetUsbPort(); },
        [soc](bool attached) { soc->SetUsbTestCable(attached); },
        [soc] { return soc->UsbTestCableAttached(); },
        [soc] { return soc->UsbDefaultCableAttached(); },
        [soc] { return soc->UsbHostIoPaused(); },
        [soc] { return soc->GetTime(); },
        [soc] { soc->KickUsbHost(); },
        [soc](const UsbHostHooks* hooks) { soc->SetUsbHostHooks(hooks); },
        [soc] { return !soc->GamepadAttached(); }
    };
    return Start(std::move(backend),config,error);
}
bool UsbHostBridge::Start(SoC* soc, const Config& config, std::string& error) {
    if (auto* pxa = dynamic_cast<SocPXA*>(soc)) return Start(pxa, config, error);
    auto* zodiac = dynamic_cast<SocIMX1*>(soc);
    if (!zodiac) return Start(Backend{}, config, error);
    // The Zodiac's USB comes through the cradle: the cable is the cradle, off leaves it out.
    Backend backend{
        [zodiac] { return zodiac->GetUsbPort(); },
        [zodiac](bool attached) { zodiac->SetCradle(attached); },
        [zodiac] { return zodiac->CradleAttached(); },
        [] { return false; },
        [zodiac] { return zodiac->UsbHostIoPaused(); },
        [zodiac] { return zodiac->GetTime(); },
        [] {},  // the host ticks at every run slice end
        [zodiac](const UsbHostHooks* hooks) { zodiac->SetUsbHostHooks(hooks); },
        [] { return true; }
    };
    return Start(std::move(backend), config, error);
}
#endif
