#include "device_profile5.h"
#include <cassert>
#include <iostream>
int main() {
    constexpr uint64_t MiB=1ull<<20;
    const auto& p=*deviceProfile5(deviceTypeDana);
    assert(p.frontend.available && p.support==DeviceSupport5::experimentalDesktop);
    assert(!deviceProfileRunnerEligible5(p.deviceType) && deviceProfileRuns68k5(p.deviceType));
    assert(!p.capabilities.usb && !p.capabilities.network && !p.capabilities.serial);
    assert(int(DeviceCardSlot5::sd0)==0 && int(DeviceCardSlot5::memoryStick)==1 && int(DeviceCardSlot5::sd2)==2);
    assert(deviceProfileCardSlotCount5(p)==2);
    for (auto slot : {DeviceCardSlot5::sd0,DeviceCardSlot5::sd2}) {
        DeviceCardRule5 rule;
        assert(deviceProfileCardRule5(p,slot,DeviceCardFamily5::sdsc,rule));
        assert(rule.legacyCsd && rule.guestSizeCount==1 && rule.guestSizes[0]==32*MiB);
        for (uint64_t mb=0;mb<=2049;++mb) {
            uint32_t mult,size;
            const bool sd=mb>=32 && mb<=2048 && legacyCardLayout(mb*MiB/512,mult,size);
            assert(deviceProfileCardSizeValid5(p.deviceType,slot,DeviceCardFamily5::sdsc,mb*MiB)==sd);
            const bool mmc=mb==32 || mb==64 || mb==128 || mb==256;
            assert(deviceProfileCardSizeValid5(p.deviceType,slot,DeviceCardFamily5::legacyMmc,mb*MiB)==mmc);
        }
        for (uint64_t bytes : std::initializer_list<uint64_t>{0,512,32*MiB-512,32*MiB+1,32*MiB+512,2048*MiB+512,4096*MiB,UINT64_MAX})
            assert(!deviceProfileCardSizeValid5(p.deviceType,slot,DeviceCardFamily5::sdsc,bytes));
        assert(!deviceProfileCardSizeValid5(p.deviceType,slot,DeviceCardFamily5(99),32*MiB));
    }
    for (auto slot : {DeviceCardSlot5::memoryStick,DeviceCardSlot5(-1),DeviceCardSlot5(3)})
        assert(!deviceProfileCardSizeValid5(p.deviceType,slot,DeviceCardFamily5::sdsc,32*MiB));
    for (const auto& other : deviceProfiles5) if(other.deviceType!=p.deviceType) {
        assert(deviceProfileCardSlotCount5(other)<=1);
        assert(!deviceProfileCardSizeValid5(other.deviceType,DeviceCardSlot5::sd2,DeviceCardFamily5::sdsc,32*MiB));
        assert(!deviceProfileCardSizeValid5(other.deviceType,DeviceCardSlot5::sd0,DeviceCardFamily5::legacyMmc,32*MiB));
    }
    assert(deviceProfileCardSizeValid5(p.deviceType,DeviceCardSlot5::sd0,DeviceCardFamily5::sdsc,32*MiB+16384));
    assert(deviceProfileSdSizeValid5(p.deviceType,1023*MiB));
    assert(!deviceProfileSdSizeValid5(p.deviceType,1024*MiB));
    assert(!deviceProfileSdSizeValid5(p.deviceType,2048*MiB));
    assert(deviceProfileSdSizeValid5(p.deviceType,32*MiB));
    assert(!deviceProfileSdSizeValid5(p.deviceType,32*MiB+512));
    std::cout << "PASS: Dana physical slots, runtime boundaries, discrete MMC, exact CSD and guest metadata\n";
}
