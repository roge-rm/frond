#include "mmiodev_MemoryStickController.h"
#include "CPU.h"
#include "icache.h"
// clang-format off
#include "soc_generic_impl.h" // IWYU pragma: keep
// clang-format on

#include "soc_PXA.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "CPU.h"
#include "MMU.h"
#include "RAM.h"
#include "ROM.h"
#include "cputil.h"
#include "device.h"
#include "device_configuration.h"
#include "keys.h"
#include "pace_patch.h"
#include "patch68k.h"
#include "patch_dispatch.h"
#include "patches.h"
#include "peephole.h"
#include "pxa255_DSP.h"
#include "pxa255_UDC.h"
#include "pxa270_IMC.h"
#include "pxa270_KPC.h"
#include "pxa270_UDC.h"
#include "pxa270_WMMX.h"
#include "pxa_AC97.h"
#include "pxa_DMA.h"
#include "pxa_GPIO.h"
#include "pxa_I2C.h"
#include "pxa_I2S.h"
#include "pxa_IC.h"
#include "pxa_LCD.h"
#include "pxa_MMC.h"
#include "pxa_MemCtrl.h"
#include "pxa_PWM.h"
#include "pxa_PwrClk.h"
#include "pxa_RTC.h"
#include "pxa_SSP.h"
#include "pxa_TIMR.h"
#include "pxa_UART.h"
#include "reschedule.h"
#include "rom_info5.h"
#include "syscall_dispatch.h"
#include "deviceSonyTG50.h"
#include "md5.h"
#include "audio_queue.h"

#define SAVESTATE_VERSION 2

#define CPUID_PXA250 0x69052902ul
#define CPUID_PXA255 0x69052D06ul  // spepping A0
#define CPUID_PXA260 0x69052D06ul  // spepping B1
#define CPUID_PXA270 0x69054114ul  // stepping C0

#define SRAM_BASE 0x5c000000ul
#define SRAM_SIZE 0x00040000ul

#define ROM_BASE 0x00000000UL
#define RAM_BASE 0xA0000000UL

#define PXA_I2C_BASE 0x40301680UL
#define PXA_PWR_I2C_BASE 0x40F00180UL

#define PCM_HZ_ENABLED 44300
#define PCM_HZ_DISABLED (44100 / 3)

SocPXA::SocPXA(enum DeviceType5 deviceType, uint32_t ramSize, void *romData, const uint32_t romSize,
               uint8_t *nandContent, size_t nandSize, int gdbPort, uint_fast8_t socRev) {
    RomInfo5 romInfo(romData, romSize);
    const auto* variant = romInfo.GetFirmwareVariant();
    disableSleep = variant && variant->deviceType == deviceType && variant->disableSleep;

    struct Reschedule rescheduleCb = {.rescheduleCb = RescheduleCB, .ctx = this};
    struct GetEmuTime getEmuTime = {.getTimeF = GetTimeCB, .userdata = this};

    this->socRev = socRev;

    if (ramSize == 0) ramSize = 16 << 20;
    if (!deviceConfigurationSupportsRamSize(deviceType, ramSize))
        ERR("unsupported RAM size %u\n", ramSize);

    this->ramSize = ramSize;
    // TG50 has 16 MiB fitted; uARM uses 17 MiB of backing for its RAM probe.
    ramBackingSize = deviceType == deviceTypeTG50 ? tg50RamBackingSize : ramSize;
    const auto* profile = deviceProfile5(deviceType);
    checkpointFence = profile && profile->capabilities.microphone;
    backgroundPanel = profile && profile->capabilities.backgroundPanel;
    this->ramBase = RAM_BASE;

    AllocateBuffers();
    SetupScheduler();

    cpu = cpuInit(ROM_BASE, mem, ARM_MEMORY_SYSTEM_MMU, true /* xscale */, false /* omap */,
                  gdbPort, deviceType == deviceTypeTG50 ? CPUID_PXA250
                      : socRev ? ((socRev == 1) ? CPUID_PXA260 : CPUID_PXA270) : CPUID_PXA255,
                  socRev == 2 ? 0x0B1AA1AAUL : 0x0B16A16AUL, patchDispatch, pacePatch, systemState);

#ifndef __EMSCRIPTEN__
    cpuSetDebugSoc(cpu,this);
    cpuSetM68kSyscallHandler(cpu, NetworkSimulation::Dispatch, &networkSimulation);
#endif
    patchDispatchSetCpu(patchDispatch, cpu);

    syscallDispatch = initSyscallDispatch(this);
    // TX boots through IPL/NAND. E2 ROM patches are not validated for this layout.
    // Retail E2 keeps SysSleep and auto-off. Firmware with Power disabled uses
    // the inherited no-sleep patch, as does the patched FrankenE2 ROM.
    // The Zire 72's 5.2.8 ROM runs unpatched, like the TX.
    if (deviceType != deviceTypeTG50 && deviceType != DeviceType5::deviceTypeTX && deviceType != DeviceType5::deviceTypeZire72)
        patchContext = registerPatches(patchDispatch, syscallDispatch, cpu, systemState,
            deviceType == DeviceType5::deviceTypeE2 && !disableSleep ? PATCH_FEATURE_ALL & ~PATCH_FEATURE_NO_SLEEP
                                                     : PATCH_FEATURE_ALL);

    ram = ramInit(mem, this, RAM_BASE, ramBackingSize, &bufferMemory, true);
    rom = romInit(mem, ROM_BASE, romData, romSize);

    void *peepholeBuffer = romGetPeepholeBuffer(rom);

    if (deviceType != deviceTypeTG50 && deviceType != DeviceType5::deviceTypeTX && deviceType != DeviceType5::deviceTypeZire72) {
        pacePatchInit(pacePatch, ROM_BASE, peepholeBuffer, romSize);
        peepholeOptimize((uint32_t *)peepholeBuffer, romSize);
    }
    // The Zire's 5.2.8 PACE has the E2's layout. Patched from boot, 68k code runs natively and
    // host tools (install, launch, export) can call into Palm OS from the first app event.
    if (deviceType == DeviceType5::deviceTypeZire72) pacePatchInit(pacePatch, ROM_BASE, peepholeBuffer, romSize);
    if (deviceType == deviceTypeTG50)
        pacePatchInitTg50(pacePatch, ROM_BASE, romData, peepholeBuffer, romSize);
    patch68kInit(deviceType != deviceTypeTG50 && deviceType != DeviceType5::deviceTypeTX && romInfo.NeedsNandPatch()
                     ? PATCH_68K_NVFS : 0);

    vSD = vsdInit(sdCardRead, sdCardWrite, 0);

    switch (deviceGetRamTerminationStyle()) {
        case RamTerminationMirror:

            // ram mirror for ram probe code
            ramMirror = ramInit(mem, this, RAM_BASE + ramBackingSize, ramBackingSize, &bufferMemory, false);
            break;

        case RamTerminationWriteIgnore:
            ERR("termination not supported");
            break;

        case RamTerminationNone:
            break;

        default:
            __builtin_unreachable();
            break;
    }

    ic = pxaIcInit(cpu, mem, this, socRev);
    dma = pxaDmaInit(mem, rescheduleCb, ic);

    if (socRev == 0 || socRev == 1) {
        dsp = pxa255dspInit(cpu);
    } else if (socRev == 2) {
        wmmx = pxa270wmmxInit(cpu);
        imc = pxaImcInit(mem);
        kpc = pxaKpcInit(mem, ic);
        sram = ramInit(mem, this, SRAM_BASE, SRAM_SIZE, &bufferSram, false);
        sramFb = sram;
        sramBase = SRAM_BASE;
        sramSize = SRAM_SIZE;
    }

    gpio = pxaGpioInit(mem, ic, socRev);
    tmr = pxaTimrInit(mem, ic);
#ifndef __EMSCRIPTEN__
    // The OS timer runs in batches up to its next match, synchronised on register access,
    // so the CPU runs longer stretches between scheduler updates. Every PXA device does this.
    pxaTimrEnableBatching(tmr, TimerElapsedCB, TimerRescheduleCB, this);
#endif
    rtc = pxaRtcInit(mem, ic);
    ffUart = pxaUartInit(mem, rescheduleCb, ic, PXA_FFUART_BASE, PXA_I_FFUART, socRev == 2);
    stUart = pxaUartInit(mem, rescheduleCb, ic, PXA_STUART_BASE, PXA_I_STUART, socRev == 2);
    btUart = pxaUartInit(mem, rescheduleCb, ic, PXA_BTUART_BASE, PXA_I_BTUART, socRev == 2);

    if (socRev != 2) {
        hwUart = pxaUartInit(mem, rescheduleCb, ic, PXA_HWUART_BASE, PXA_I_HWUART, true);
    }

    if (socRev == 2) {
        // DRCMR request lines: RX, then TX at the next number (PXA27x table 5-13).
        const std::pair<PxaUart*, uint8_t> lines[] = {
            {ffUart, DMA_CMR_FFUART_RX}, {btUart, DMA_CMR_BTUART_RX}, {stUart, DMA_CMR_STUART_RX}};
        for (size_t i = 0; i < 3; i++) {
            uartDma[i] = {dma, lines[i].second};
            pxaUartSetDma(lines[i].first, [](void* context, bool receive, bool requested) {
                auto* line = static_cast<UartDmaLine*>(context);
                pxaDmaExternalReq(line->dma, line->rxRequest + (receive ? 0 : 1), requested);
            }, &uartDma[i]);
            const PxaDmaPeripheral handshake{
                [](void* uart) -> uint_fast8_t { return pxaUartDmaValidBytes(static_cast<PxaUart*>(uart)); },
                [](void* uart) { return pxaUartDmaEndOfReceive(static_cast<PxaUart*>(uart)); },
                [](void* uart) { pxaUartDmaEndOfChain(static_cast<PxaUart*>(uart)); }, lines[i].first};
            pxaDmaSetPeripheral(dma, lines[i].second, &handshake);
            pxaDmaSetPeripheral(dma, lines[i].second + 1, &handshake);
        }
    }

    pwrClk = pxaPwrClkInit(cpu, mem, this, socRev == 2);

    if (socRev == 2) {
        pwrI2c = pxaI2cInit(mem, ic, dma, PXA_PWR_I2C_BASE, PXA_I_PWR_I2C);
    }

    i2c = pxaI2cInit(mem, ic, dma, PXA_I2C_BASE, PXA_I_I2C);
    memCtrl = pxaMemCtrlrInit(mem, socRev);
    ac97 = pxaAC97Init(mem, ic, dma);

    // SSP/SSP1
    ssp[0] = pxaSspInit(mem, rescheduleCb, ic, dma, PXA_SSP1_BASE, PXA_I_SSP, DMA_CMR_SSP_RX);

    if (socRev == 0 || socRev == 1) {
        // NSSP
        ssp[1] = pxaSspInit(mem, rescheduleCb, ic, dma, PXA_NSSP_BASE, PXA_I_NSSP, DMA_CMR_NSSP_RX);
        udc1 = pxa255UdcInit(mem, ic, dma);
        pxaPwrClkConnectUdc(pwrClk, udc1);
    }

    if (socRev == 1) {
        // ASSP
        ssp[2] = pxaSspInit(mem, rescheduleCb, ic, dma, PXA_ASSP_BASE, PXA_I_ASSP, DMA_CMR_ASSP_RX);
    }

    if (socRev == 2) {
        // SSP2
        ssp[1] = pxaSspInit(mem, rescheduleCb, ic, dma, PXA_SSP2_BASE, PXA_I_SSP2, DMA_CMR_SSP2_RX);

        // SSP3
        ssp[2] = pxaSspInit(mem, rescheduleCb, ic, dma, PXA_SSP3_BASE, PXA_I_SSP3, DMA_CMR_SSP3_RX);

        udc2 = pxa270UdcInit(mem, ic, dma);
    }

    i2s = pxaI2sInit(mem, ic, dma);
    pxaPwrClkConnectI2s(pwrClk, i2s);

    pwm[0] = pxaPwmInit(mem, PXA_PWM0_BASE);
    pwm[1] = pxaPwmInit(mem, PXA_PWM1_BASE);

    if (socRev == 2) {
        pwm[2] = pxaPwmInit(mem, PXA_PWM2_BASE);
        pwm[3] = pxaPwmInit(mem, PXA_PWM3_BASE);
    }

    mmc = pxaMmcInit(mem, ic, dma, socRev == 2);

    displayConfiguration =
        deviceConfigurationDisplayConfigForDevice(deviceType, deviceConfigurationDefaultDisplayMode(deviceType));

    lcd = pxaLcdInit(mem, this, ic, &bufferLcd, displayConfiguration.width,
                     displayConfiguration.height, socRev == 2);

    kp = keypadInit(gpio, true);

    struct SocPeriphs sp = {};
    sp.mem = mem;
    sp.soc = this;
    sp.gpio = gpio;
    sp.ic = ic;
    sp.dma = dma;
    sp.i2c = i2c;
    sp.i2s = i2s;
    sp.pwm0 = pwm[0];
    sp.pwrClk = pwrClk;
    sp.ac97 = ac97;
    sp.ssp = ssp[0];
    sp.ssp2 = ssp[1];
    sp.ssp3 = ssp[2];
    if (socRev == 2) sp.kpc = kpc;
    irUart = deviceType == deviceTypeTG50 ? ffUart : stUart;
    // The E2's FFUART is its serial port (the T|X's runs paced as a PXA27x); a host endpoint needs the paced model.
    if (deviceProfileIsE2Board5(deviceType)) pxaUartSetPacing(ffUart, true);
    if (deviceType==DeviceType5::deviceTypeTX || deviceProfileIsE2Board5(deviceType) ||
        deviceType==DeviceType5::deviceTypeZire72 || deviceType==deviceTypeTG50) {
        pxaUartSetPacing(irUart,true);
        pxaUartSetModeObserver(irUart,RefreshIrConfiguration,this);
        pxaGpioSetConfigurationObserver(gpio,RefreshIrConfiguration,this);
        pxaUartSetFuncs(irUart,
            [](void* ctx) -> uint_fast16_t {
                auto* soc=static_cast<SocPXA*>(ctx);
                if (soc->serialHostHooks.read && soc->SerialEnabled()) return soc->serialHostHooks.read(soc->serialHostHooks.context);
                if (!pxaUartIrReceiveEnabled(soc->irUart) ||
                    (soc->GetDeviceType()==deviceTypeTG50 && !soc->IrPowerEnabled())) return UART_CHAR_NONE;
                if (soc->irHostHooks.read) return soc->IrPowerEnabled() ? soc->irHostHooks.read(soc->irHostHooks.context) : UART_CHAR_NONE;
                return soc->irKeyboard.Read(soc->GetTime());
            }, [](uint_fast16_t value,void* ctx) {
                auto* soc=static_cast<SocPXA*>(ctx);
                if (soc->serialHostHooks.write && soc->SerialEnabled())
                    soc->serialHostHooks.write(value,soc->serialHostHooks.context);
                if (soc->irHostHooks.write && soc->IrPowerEnabled() && pxaUartIrTransmitEnabled(soc->irUart))
                    soc->irHostHooks.write(value,soc->irHostHooks.context);
            },
            [](void* ctx) {
                auto* soc=static_cast<SocPXA*>(ctx);
                if (soc->serialHostHooks.pending && soc->SerialEnabled()) return soc->serialHostHooks.pending(soc->serialHostHooks.context);
                if (!pxaUartIrReceiveEnabled(soc->irUart) ||
                    (soc->GetDeviceType()==deviceTypeTG50 && !soc->IrPowerEnabled())) return false;
                return soc->irHostHooks.pending ? soc->IrPowerEnabled() && soc->irHostHooks.pending(soc->irHostHooks.context) : soc->irKeyboard.Pending();
            },
            nullptr, this);
    }
    sp.uarts[0] = ffUart;
    sp.uarts[1] = hwUart;
    sp.uarts[2] = stUart;
    sp.uarts[3] = btUart;

    dev = deviceSetup(deviceType, &sp, rescheduleCb, getEmuTime, kp, vSD, nandContent, nandSize,
                      &bufferNand);
    deviceSetTransportObserver(dev, RefreshTransportPins, this);
    if (pxaI2sHasCodec(i2s)) scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 20_usec, 1);

    nand = sp.nand;

    pxaPwrClkConnectWakeSources(pwrClk, gpio, rtc, socRev == 2 ? kpc : nullptr);

    if (deviceType == DeviceType5::deviceTypeTX)
        scheduler->ScheduleTask(SCHEDULER_TASK_AUX_3, 1_msec, 1);

    if (!powerOnState->Save(*this)) ERR("failed to capture power-on state");
    SdEject();
}

uint32_t *SocPXA::GetPendingFrame(uint32_t &firstDirtyLine, uint32_t &lastDirtyLine) {
    firstDirtyLine = 0;
    lastDirtyLine = displayConfiguration.height - 1;

    return pxaLcdGetPendingFrame(lcd);
}

void SocPXA::ResetPendingFrame() { return pxaLcdResetPendingFrame(lcd); }

DeviceType5 SocPXA::GetDeviceType() { return deviceGetType(dev); }

DisplayMode SocPXA::GetDisplayMode() {
    return deviceConfigurationDefaultDisplayMode(deviceGetType(dev));
}

void SocPXA::SuspendTimerInterrupts(bool suspendInterrupts) {
    pxaTimrSuspendInterrupts(tmr, suspendInterrupts);
}

bool SocPXA::LcdEnabled() { return pxaLcdIsEnabled(lcd) && devicePanelEnabled(dev); }
bool SocPXA::HoldSwitchSupported() const {
    return deviceProfile5(deviceGetType(dev))->frontend.holdSupported;
}
bool SocPXA::PressHotSync() {
    if (!deviceProfileHasHotSyncButton5(GetDeviceType())) return false;
    if (jammedKey != keyInvalid) return false;
    // Long enough for the E2 ROM to see it (150 ms is missed, 300 ms works) and short of the 5 s the E2 FIQ watchdog allows.
    JamKey(keyIdHotSync, 500);
    return true;
}
bool SocPXA::HoldSwitchOn() const { return deviceHoldSwitchOn(dev); }
bool SocPXA::SetHoldSwitch(bool on) {
    if (!HoldSwitchSupported()) return false;
    // Host commands run on the owning SoC thread; this is a persistent external switch.
    deviceKey(dev, keyIdHold, on);
    return true;
}

uint32_t SocPXA::DispatchTicks(uint32_t clientType, uint32_t batchedTicks) {
    // Sleep stops the peripheral clocks. RTC and external input delivery remain live.
    if (PowerSleeping() && clientType != SCHEDULER_TASK_RTC && clientType != SCHEDULER_TASK_AUX_2)
        return 0;
    switch (clientType) {
        case SCHEDULER_TASK_TIMER:
            pxaTimrTick(tmr, batchedTicks);

#ifndef __EMSCRIPTEN__
            return pxaTimrNextBatch(tmr, sleeping ? UINT32_MAX : 32);
#else
            return sleeping ? pxaTimrTicksToNextInterrupt(tmr) : 1;
#endif

        case SCHEDULER_TASK_RTC:
            pxaRtcTick(rtc);
            if (pxaPwrClkWakePending(pwrClk)) ResumePowerSleep();
            return 1;

        case SCHEDULER_TASK_LCD:
            pxaLcdTick(lcd);
            return 1;

        case SCHEDULER_TASK_I2S:
            if (pxaI2sHasCodec(i2s)) {
                for (uint32_t i = 0; i < batchedTicks; ++i) {
                    pxaI2sAdvance(i2s, 20000);
                    deviceAudioAdvance(dev, 20000);
                }
            }
            else pxaI2sPeriodic(i2s);
            return 1;

        case SCHEDULER_TASK_PCM:
            if (!syscallDispatchInProgress(syscallDispatch)) devicePcmPeriodic(dev);
            return (pcmSuspended && pcmEnabled && !fastAudioRouted) ? 0 : 1;

        case SCHEDULER_TASK_AUX_1:
            CycleBatch0();
            return Batch0Ticks();

        case SCHEDULER_TASK_AUX_2:
            // Complete controller fences even while the guest UART clock is off.
            deviceServiceBluetoothReset(dev);
            PumpEventQueues();
            if (PowerSleeping()) {
                if (pxaPwrClkWakePending(pwrClk)) ResumePowerSleep();
                return 1;
            }
            deviceNetworkPeriodic(dev);
            if (fastAudio) { fastAudio->Tick(GetTime()); UpdateFastAudioRoute(); }
            return 1;

        case SCHEDULER_TASK_AUX_3:
            // Install the bridge before the first guest PACE invocation. Once
            // active, a slower check catches OS reloads after guest resets.
            return PrepareM68kSupport() ? 1000 : 1;

        default:
            ERR("invalid client type\n");
    }
}

void SocPXA::OnSetFramebufferDirty() { pxaLcdSetFramebufferDirty(lcd); }

bool SocPXA::OnSleep() {
    if (PowerSleeping()) {
        deviceSuspendBluetooth(dev);
        if (!checkpointFenceDepth && !irReplacementDepth) deviceDisarmInput(dev);
        else microphoneInvalidatePending = true;
        if (irHostHooks.replaced && !usbHostPaused) irHostHooks.replaced(irHostHooks.context);
        if (serialHostHooks.replaced && !usbHostPaused) serialHostHooks.replaced(serialHostHooks.context);
        if (audioQueue) audioQueueClear(audioQueue);
        if (pxaI2sHasCodec(i2s)) pxaI2sOutputBoundary(i2s);
        if (fastAudio) { fastAudio->Sleep(); RestoreFastAudioRoute(); }
        pxaTimrSynchronize(tmr);
        cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        return true;
    }
    if (cpuHasPendingInterrupt(cpu)) return false;

    scheduler->RescheduleTaskAtLeast(SCHEDULER_TASK_TIMER, pxaTimrTicksToNextInterrupt(tmr));
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);

    return true;
}

void SocPXA::OnWakeup() {
    if(fastAudio) {fastAudio->Wake(GetTime());RestoreFastAudioRoute();}
    scheduler->RescheduleTaskAtLeast(SCHEDULER_TASK_TIMER, 1);
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
}

bool SocPXA::PowerSleeping() const { return pxaPwrClkSleeping(pwrClk); }

void SocPXA::Wakeup(uint8_t wakeupSource) {
    // Interrupt-controller wake implements idle, not PWRMODE=3 sleep (3.6.9).
    if (PowerSleeping() || restoringPowerState) return;
    SoC::Wakeup(wakeupSource);
}

void SocPXA::LoadSleepReset(SavestateLoader<ChunkType>& loader) {
    // Use the same reset register values as cold boot, but only for reset domains
    // in PXA27x Table 3-2. Do not load RAM, flash, RTC, clocks, scheduler, VSD,
    // board peripherals, card identity or host configuration from this snapshot.
    cpuLoad(cpu, loader);
    pxaUartLoad(ffUart, loader, 0);
    pxaUartLoad(stUart, loader, 2);
    pxaUartLoad(btUart, loader, 3);
    for (unsigned i = 0; i < 3; ++i) pxaSspLoad(ssp[i], loader, i);
    pxaAC97Load(ac97, loader);
    pxaDmaLoad(dma, loader);
    pxaI2sLoad(i2s, loader);
    pxaI2cLoad(i2c, loader, 0);
    if (!pxaPwrClkRetainPowerI2c(pwrClk)) pxaI2cLoad(pwrI2c, loader, 1);
    pxaMemCtrlrLoad(memCtrl, loader);
    for (unsigned i = 0; i < 4; ++i) pxaPwmLoad(pwm[i], loader, i);
    if (!(pxaPwrClkSleepRetention(pwrClk) & 0x0c)) pxaTimrLoad(tmr, loader);
    pxaMmcLoad(mmc, loader);
    pxaLcdLoad(lcd, loader);
    pxaImcLoad(imc, loader);
    pxa270UdcLoad(udc2, loader);
    pxa270wmmxLoad(wmmx, loader);
    pxaKpcSleepReset(kpc);
    deviceCameraSleepReset(dev);
    pxaGpioSleepReset(gpio);
    pxaIcLoad(ic, loader);
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartResync(uart);  // after deassertions from reset peripherals
    pxaRtcRefreshInterrupts(rtc);  // retained alarm lines survive the IC reset
}

void SocPXA::ResumePowerSleep() {
    ++powerWakes;
    if (socRev != 2) {
        ResumePowerSleep25x();
        return;
    }
    const uint32_t retention = pxaPwrClkSleepRetention(pwrClk);
    struct ResetDomains {
        SocPXA& soc;
        void Load(SavestateLoader<ChunkType>& loader) { soc.LoadSleepReset(loader); }
    } domains{*this};
    SavestateLoader<ChunkType> loader;
    if (!loader.Load(powerOnState->GetBuffer(), powerOnState->GetSize(), domains))
        ERR("failed to restore sleep-reset domains");

    // Unretained SRAM has undefined contents on hardware; choose zero deterministically.
    for (unsigned bank = 0; bank < 4; ++bank) {
        if (retention & (0x100u << bank)) continue;
        memset(bufferSram.buffer + bank * 0x10000, 0, 0x10000);
        memoryBufferMarkRangeDirty(&bufferSram, bank * 0x10000, 0x10000);
    }
    pxaPwrClkFinishSleep(pwrClk);
    // The TX's PACE patch is in RAM and found again later; the Zire's is in NOR and stays.
    if (GetDeviceType() == deviceTypeTG50) {
        pacePatchInitTg50(pacePatch, ROM_BASE, romGetData(rom), romGetPeepholeBuffer(rom), romGetSize(rom));
    } else if (GetDeviceType() != DeviceType5::deviceTypeZire72) *pacePatch = {};
    if (audioQueue) audioQueueClear(audioQueue);
    if (fastAudio) { fastAudio->Wake(GetTime()); RestoreFastAudioRoute(); }
    pxaLcdResetPaletteBuffer(lcd);
    pxaTimrRestartClock(tmr);
    scheduler->ScheduleTask(SCHEDULER_TASK_TIMER, 1_sec / 3250000ULL, 1);
    scheduler->ScheduleTask(SCHEDULER_TASK_LCD, 1_sec / (64 * 3 * 60), 1);
    scheduler->ScheduleTask(SCHEDULER_TASK_AUX_1, 36_sec / 3686400ULL, 1);
    scheduler->ScheduleTask(SCHEDULER_TASK_AUX_3, 1_msec, 1);
    if (pxaI2sHasCodec(i2s)) {
        pxaI2sSetClockEnabled(i2s, pxaPwrClkI2sClock(pwrClk), false);
        pxaI2sRefresh(i2s);
        scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 20_usec, 1);
    } else if (deviceI2sConnected()) scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 1_sec / 44100, 1);
    SchedulePcmTask();
    SoC::Wakeup(0);
}

void SocPXA::LoadSleepReset25x(SavestateLoader<ChunkType>& loader) {
    // PXA25x sleep exit (3.4.4) is a reset with RCSR[SMR] set. RTC, power manager,
    // clocks, SDRAM contents and board devices are kept; the units below take their
    // cold boot register values and the OS reprograms them on resume.
    cpuLoad(cpu, loader);
    pxaUartLoad(ffUart, loader, 0);
    pxaUartLoad(hwUart, loader, 1);
    pxaUartLoad(stUart, loader, 2);
    pxaUartLoad(btUart, loader, 3);
    pxaSspLoad(ssp[0], loader, 0);
    pxaSspLoad(ssp[1], loader, 1);
    if (socRev > 0) pxaSspLoad(ssp[2], loader, 2);
    pxaAC97Load(ac97, loader);
    pxaDmaLoad(dma, loader);
    pxaI2sLoad(i2s, loader);
    pxaI2cLoad(i2c, loader, 0);
    pxaMemCtrlrLoad(memCtrl, loader);
    pxaPwmLoad(pwm[0], loader, 0);
    pxaPwmLoad(pwm[1], loader, 1);
    deviceAudioSleepReset(dev);
    pxaTimrLoad(tmr, loader);
    pxaMmcLoad(mmc, loader);
    pxaLcdLoad(lcd, loader);
    pxa255dspLoad(dsp, loader);
    pxa255UdcLoad(udc1, loader);
    pxaGpioSleepReset(gpio);
    pxaIcLoad(ic, loader);
    for (auto* uart : {ffUart, hwUart, stUart, btUart})
        if (uart) pxaUartResync(uart);
    pxaRtcRefreshInterrupts(rtc);
}

void SocPXA::ResumePowerSleep25x() {
    struct ResetDomains {
        SocPXA& soc;
        void Load(SavestateLoader<ChunkType>& loader) { soc.LoadSleepReset25x(loader); }
    } domains{*this};
    SavestateLoader<ChunkType> loader;
    if (!loader.Load(powerOnState->GetBuffer(), powerOnState->GetSize(), domains))
        ERR("failed to restore sleep-reset domains");

    pxaPwrClkFinishSleep(pwrClk);
    if (GetDeviceType() == DeviceType5::deviceTypeTG50)
        pacePatchInitTg50(pacePatch, ROM_BASE, romGetData(rom),
                         romGetPeepholeBuffer(rom), romGetSize(rom));
    // The boot code clears RAM before it enables SDRAM; on hardware those writes are
    // lost while SDRAM is still in self-refresh.
    pxaMemCtrlrHoldSdram(memCtrl);
    if(fastAudio) {fastAudio->Wake(GetTime());RestoreFastAudioRoute();}
    if (audioQueue) audioQueueClear(audioQueue);
    pxaLcdResetPaletteBuffer(lcd);
    pxaTimrRestartClock(tmr);
    scheduler->ScheduleTask(SCHEDULER_TASK_TIMER, 1_sec / 3686400ULL, 1);
    scheduler->ScheduleTask(SCHEDULER_TASK_LCD, 1_sec / (64 * 3 * 60), 1);
    scheduler->ScheduleTask(SCHEDULER_TASK_AUX_1, 36_sec / 3686400ULL, 1);
    if (pxaI2sHasCodec(i2s)) {
        pxaI2sSetClockEnabled(i2s, pxaPwrClkI2sClock(pwrClk), false);
        pxaI2sRefresh(i2s);
        scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 20_usec, 1);
    } else if (deviceI2sConnected()) scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 1_sec / 44100, 1);
    SchedulePcmTask();
    SoC::Wakeup(0);
}

void SocPXA::OnSetAudioQueue(struct AudioQueue *audioQueue) {
    if (fastAudio) fastAudio->SetQueue(audioQueue);
    deviceSetAudioQueue(dev, ((GetDeviceType() == deviceTypeTG50 && !pcmEnabled) || audioOutputFenced || checkpointFenceDepth || fastAudioRouted || (fastAudio && fastAudio->OutputFenced())) ? nullptr : audioQueue);
}

void SocPXA::OnSetPcmOutputEnabled() {
    if (GetDeviceType() == deviceTypeTG50) RestoreFastAudioRoute();
    if (fastAudio) fastAudio->RestartOutput();
    SchedulePcmTask();
}

void SocPXA::OnSetPcmSuspended() {
    if (!pcmEnabled || fastAudioRouted) return;

    scheduler->RescheduleTask(SCHEDULER_TASK_PCM, pcmSuspended ? 0 : 1);
    if (!pcmSuspended) cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
}

void SocPXA::OnSdInsert() {
    vsdReset(vSD, sdCardSectorCount());
    pxaMmcInsert(mmc, vSD);
    deviceSetSdCardInserted(dev, true);
}

void SocPXA::OnSdRemount() { pxaMmcInsert(mmc, vSD); }

void SocPXA::OnSdEject() {
    vsdReset(vSD, 0);
    pxaMmcInsert(mmc, nullptr);
    deviceSetSdCardInserted(dev, false);
}

// USB cable detect: Linux palmtx.h TX GPIO13 and palmte2.h E2 GPIO35 are active low; the Zire's
// GPIO15 is active high (palmz72.c palm27x_udc_init(15, 95, 0)).
static int UsbDetectPin(DeviceType5 type) {
    return type==deviceTypeTX ? 13 : deviceProfileIsE2Board5(type) ? 35 : type==deviceTypeZire72 ? 15 : -1;
}

bool SocPXA::SetUsbTestCable(bool connected) {
    if (GetDeviceType() == deviceTypeTG50) { deviceTg50SetUsbCable(dev, connected); return true; }
    const int pin=UsbDetectPin(GetDeviceType());
    if(pin<0)return false;
    pxaGpioSetState(gpio, pin, GetDeviceType()==deviceTypeZire72 ? connected : !connected);
    return true;
}

bool SocPXA::UsbTestCableAttached() {
    if (GetDeviceType() == deviceTypeTG50) return deviceTg50UsbCableAttached(dev);
    const int pin=UsbDetectPin(GetDeviceType());
    return pin>=0 && pxaGpioInputLevel(gpio,pin)==(GetDeviceType()==deviceTypeZire72);
}

void SocPXA::KickUsbHost() {
    scheduler->RescheduleTask(SCHEDULER_TASK_AUX_1, 1);
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
}

void SocPXA::OnTouch(int x, int y) { deviceTouch(dev, x, y); }

bool SocPXA::HoldKeyEvent(const KeyEvent& event) {
    const bool zire = GetDeviceType() == DeviceType5::deviceTypeZire72;
    if ((!zire && GetDeviceType() != DeviceType5::deviceTypeTX) || event.key != keyIdPower) return false;
    const uint64_t now = GetTime();
    // The Zire's and TX's DAL stay awake only if power is still held after the wake (about
    // 130 ms on the Zire, 80 ms isn't enough on the TX), so a short press is held for 250 ms.
    if (!event.keyDown) return powerKeyDownAt && now < powerKeyDownAt + 250_msec;
    if (!zire) return false;
    // A press while the DAL goes to sleep (LCD already off) puts it in a sleep that only a held
    // key wakes, so the press waits until the Zire is asleep, for 3 s at most.
    if (PowerSleeping() || LcdEnabled()) { powerPressHeldSince = 0; return false; }
    if (!powerPressHeldSince) powerPressHeldSince = now;
    if (now < powerPressHeldSince + 3_sec) return true;
    powerPressHeldSince = 0;
    return false;
}

void SocPXA::OnEngageKey(KeyId key, bool down) {
    if (key == keyIdPower && down) powerKeyDownAt = GetTime();
    deviceKey(dev, key, down);
    keypadKeyEvt(kp, key, down);
}

AudioInputDemand SocPXA::MicrophoneDemand() { return deviceInputDemand(dev); }

void SocPXA::SetMicrophoneSource(const AudioInputSource *source) {
    deviceSetInputSource(dev, source);
    if (PowerSleeping()) deviceDisarmInput(dev);
}

void SocPXA::SetCameraSource(const CameraSource *source) {
    deviceSetCameraSource(dev, source);
    if (PowerSleeping()) deviceDisarmInput(dev);
}

void SocPXA::OnBeforeReset() { if (!checkpointFenceDepth && !irReplacementDepth) deviceDisarmInput(dev); else microphoneInvalidatePending = true; resetRtc = pxaRtcRetained(rtc); resetHold = deviceHoldSwitchOn(dev); deviceBeginReset(dev); }

void SocPXA::OnReset() {
    deviceEndReset(dev);
    if (resetHold) deviceKey(dev, keyIdHold, true);
    pxaLcdResetPaletteBuffer(lcd);
    if (nand) nandResetPageBuffer(nand);
    // RAM-backed stores retain their reset-pin cause and running RTC. A power-on
    // cause lets the small ROM invalidate the store on boards that request it.
    if (GetDeviceType() == deviceTypeZire72 || GetDeviceType() == deviceTypeTG50) {
        pxaPwrClkSetResetCause(pwrClk, powerOnReset ? 1 : 8);
        pxaRtcRestoreRetained(rtc, resetRtc);
    }
    if (GetDeviceType() == deviceTypeTG50) {
        pacePatchInitTg50(pacePatch, ROM_BASE, romGetData(rom), romGetPeepholeBuffer(rom), romGetSize(rom));
        cpuInvalidateCodeCache(cpu);
    }
    powerOnReset = false;
    powerKeyDownAt = powerPressHeldSince = 0;
}

void SocPXA::OnLoad(SavestateLoader<ChunkType> &loader) {
    // Load callbacks rebuild host registrations and IRQ wiring. They must not
    // discard the restored XScale fetch buffers or add a spurious scheduler kick
    // when returning to the same checkpointed guest timeline.
    struct CheckpointCpuGuard {
        ArmCpu* cpu;
        bool checkpoint;
        uint32_t reasons;
        uint32_t fetch[ICACHE_FETCH_STATE_WORDS];
        CheckpointCpuGuard(ArmCpu* cpu,bool checkpoint):cpu(cpu),checkpoint(checkpoint),reasons(cpuGetSlowPathReason(cpu)) {
            if(checkpoint)cpuGetInstructionFetchState(cpu,fetch);
        }
        ~CheckpointCpuGuard() {
            if(!checkpoint)return;
            if(!cpuRestoreInstructionFetchState(cpu,fetch))std::abort();
            if(reasons&SLOW_PATH_REASON_RESCHEDULE)cpuSetSlowPath(cpu,SLOW_PATH_REASON_RESCHEDULE);
            else cpuClearSlowPath(cpu,SLOW_PATH_REASON_RESCHEDULE);
        }
    } checkpointCpuGuard{cpu,RestoringCheckpoint()};
    // Emulated time starts again from the loaded state; a checkpoint restores these with the input.
    if (!RestoringCheckpoint()) powerKeyDownAt = powerPressHeldSince = 0;
    restoringPowerState = true;
    pxaPwrClkRestoreInProgress(pwrClk, true);
    struct RestoreGuard {
        SocPXA& soc;
        ~RestoreGuard() {
            soc.restoringPowerState = false;
            pxaPwrClkRestoreInProgress(soc.pwrClk, false);
        }
    } restoreGuard{*this};
    vsdLoad(vSD, loader);

    pxaUartLoad(ffUart, loader, 0);
    if (socRev != 2) pxaUartLoad(hwUart, loader, 1);
    pxaUartLoad(stUart, loader, 2);
    irKeyboard.Load(loader);
    pxaUartLoad(btUart, loader, 3);

    pxaSspLoad(ssp[0], loader, 0);
    pxaSspLoad(ssp[1], loader, 1);
    if (socRev > 0) pxaSspLoad(ssp[2], loader, 2);

    pxaGpioLoad(gpio, loader);
    pxaAC97Load(ac97, loader);
    pxaDmaLoad(dma, loader);
    pxaI2sLoad(i2s, loader);
    pxaI2cLoad(i2c, loader, 0);
    pxaIcLoad(ic, loader);
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartResync(uart);
    pxaMemCtrlrLoad(memCtrl, loader);
    pxaPwrClkLoad(pwrClk, loader);
    if (socRev == 2) pxaI2cLoad(pwrI2c, loader, 1);

    pxaPwmLoad(pwm[0], loader, 0);
    pxaPwmLoad(pwm[1], loader, 1);
    if (socRev == 2) {
        pxaPwmLoad(pwm[2], loader, 2);
        pxaPwmLoad(pwm[3], loader, 3);
    }

    pxaTimrLoad(tmr, loader);
    pxaMmcLoad(mmc, loader);
    pxaRtcLoad(rtc, loader);
    pxaLcdLoad(lcd, loader);

    if (socRev < 2) {
        pxa255dspLoad(dsp, loader);
        pxa255UdcLoad(udc1, loader);
    } else {
        pxaImcLoad(imc, loader);
        pxaKpcLoad(kpc, loader);
        pxa270UdcLoad(udc2, loader);
        pxa270wmmxLoad(wmmx, loader);
    }

    if (pxaI2sHasCodec(i2s))
        pxaI2sSetClockEnabled(i2s, pxaPwrClkI2sClock(pwrClk), false);
    deviceLoad(dev, loader, RestoringCheckpoint());
    if (pxaI2sHasCodec(i2s)) {
        if (!pxaI2sClockStateValid(i2s)) loader.NotifyError();
        pxaI2sRefresh(i2s);
        if (pxaI2sLegacyState(i2s))
            scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 20_usec, PowerSleeping() ? 0 : 1);
        else if (!scheduler->TaskPeriodIs(SCHEDULER_TASK_I2S, 20_usec)) loader.NotifyError();
    }
    if (PowerSleeping() && !sleeping) { loader.NotifyError(); return; }
    if (GetDeviceType() == deviceTypeTG50 &&
        !pacePatchInitTg50(pacePatch, ROM_BASE, romGetData(rom), romGetPeepholeBuffer(rom), romGetSize(rom)) &&
        cpuIsModePace(cpu)) {
        loader.NotifyError();
        return;
    }
    if (GetDeviceType() == DeviceType5::deviceTypeTX) {
        if (!RestoringCheckpoint())
            scheduler->ScheduleTask(SCHEDULER_TASK_TIMER, 1_sec / 3250000ULL, PowerSleeping() ? 0 : 1);
        *pacePatch = {};
        if (!PrepareM68kSupport() && cpuIsModePace(cpu)) {
            loader.NotifyError();
            return;
        }
    }
    if (!RestoringCheckpoint()) {
        // Full session replacement discards host inputs from the previous guest.
        // Checkpoint rollback instead restores these from its host-side snapshot.
        penEventQueue->Clear();
        keyEventQueue->Clear();
        penDown = false;
        if (GetDeviceType() == DeviceType5::deviceTypeTX || GetDeviceType() == DeviceType5::deviceTypeZire72) {
            for (unsigned key = keyIdHard1; key <= keyIdPower; ++key)
                deviceKey(dev, key, false);
            deviceKey(dev, keyIdVoice, false);
        }
    }
    if (audioQueue) audioQueueClear(audioQueue);
    if (fastAudio) {
        fastAudio->GuestLoaded(RestoringCheckpoint());
        if(!RestoringCheckpoint() && PowerSleeping())fastAudio->Sleep();
        if(RestoringCheckpoint())RestoreFastAudioRoute();else UpdateFastAudioRoute();
    }

    if (!RestoringCheckpoint()) {
        SchedulePcmTask();
        keypadReset(kp);
        UpdateSchedulePcm();
    }
    SetFramebufferDirty();
}

template <typename T>
void SocPXA::OnSave(T &savestate) {
    vsdSave(vSD, savestate);

    pxaUartSave(ffUart, savestate, 0);
    if (socRev != 2) pxaUartSave(hwUart, savestate, 1);
    pxaUartSave(stUart, savestate, 2);
    irKeyboard.Save(savestate);
    pxaUartSave(btUart, savestate, 3);

    pxaSspSave(ssp[0], savestate, 0);
    pxaSspSave(ssp[1], savestate, 1);
    if (socRev > 0) pxaSspSave(ssp[2], savestate, 2);

    pxaGpioSave(gpio, savestate);
    pxaAC97Save(ac97, savestate);
    pxaDmaSave(dma, savestate);
    pxaI2sSave(i2s, savestate);
    pxaI2cSave(i2c, savestate);
    pxaIcSave(ic, savestate);
    pxaMemCtrlrSave(memCtrl, savestate);
    pxaPwrClkSave(pwrClk, savestate);
    if (socRev == 2) pxaI2cSave(pwrI2c, savestate, 1);

    pxaPwmSave(pwm[0], savestate, 0);
    pxaPwmSave(pwm[1], savestate, 1);
    if (socRev == 2) {
        pxaPwmSave(pwm[2], savestate, 2);
        pxaPwmSave(pwm[3], savestate, 3);
    }

    pxaTimrSave(tmr, savestate);
    pxaMmcSave(mmc, savestate);
    pxaRtcSave(rtc, savestate);
    pxaLcdSave(lcd, savestate, socRev == 2);

    if (socRev < 2) {
        pxa255dspSave(dsp, savestate);
        pxa255UdcSave(udc1, savestate);
    } else {
        pxaImcSave(imc, savestate);
        pxaKpcSave(kpc, savestate);
        pxa270UdcSave(udc2, savestate);
        pxa270wmmxSave(wmmx, savestate);
    }

    deviceSave(dev, savestate);
}

void SocPXA::AllocateBuffers() {
    size_t memoryBufferSize = ramBackingSize + 2 * MEMORY_BUFFER_GRANULARITY;
    if (socRev == 2) memoryBufferSize += SRAM_SIZE;

    bool success = memoryBufferAllocate(&bufferMemory, memoryBufferSize);

    size_t offset = ramBackingSize;

    success = success && memoryBufferGetSubBuffer(&bufferMemory, &bufferNand, offset,
                                                  MEMORY_BUFFER_GRANULARITY);
    offset += MEMORY_BUFFER_GRANULARITY;

    success = success && memoryBufferGetSubBuffer(&bufferMemory, &bufferLcd, offset,
                                                  MEMORY_BUFFER_GRANULARITY);
    offset += MEMORY_BUFFER_GRANULARITY;

    if (socRev == 2)
        success =
            success && memoryBufferGetSubBuffer(&bufferMemory, &bufferSram, offset, SRAM_SIZE);

    if (!success) ERR("failed to allocate memory buffers");
}

void SocPXA::SetupScheduler() {
    // PXA27x OSCR0 runs at 3.25 MHz; PXA25x uses 3.6864 MHz.
    scheduler->ScheduleTask(SCHEDULER_TASK_TIMER,
                            1_sec / (socRev == 2 ? 3250000ULL : 3686400ULL), 1);

    // RTC: 1 Hz
    scheduler->ScheduleTask(SCHEDULER_TASK_RTC, 1_sec, 1);

    // LCD: one frame every 64 ticks, 3 ticks per frame, 60 FPS -> 11.52 kHz
    scheduler->ScheduleTask(SCHEDULER_TASK_LCD, 1_sec / (64 * 3 * 60), 1);

    // Periodic tasks 0: every 36 timer ticks -> 102.4 kHz
    scheduler->ScheduleTask(SCHEDULER_TASK_AUX_1, 36_sec / 3686400ULL, 1);

    // PCM -> run at 44.3 kHz (higher than 44.1 kHz to create backpressure and
    // avoid underruns)
    scheduler->ScheduleTask(SCHEDULER_TASK_PCM, 1_sec / PCM_HZ_DISABLED, 1);

    if (pxaI2sHasCodec(i2s)) scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 20_usec, 1);
    else if (deviceI2sConnected()) {
        // I2S -> run at 44.1 kHz
        scheduler->ScheduleTask(SCHEDULER_TASK_I2S, 1_sec / 44100, 1);
    }

    // Pump event queues: 30 Hz
    scheduler->ScheduleTask(SCHEDULER_TASK_AUX_2, 1_sec / 30, 1);
}

void SocPXA::SchedulePcmTask() { UpdateSchedulePcm(); }

std::function<void()> SocPXA::CapturePeripheralInput() {
    auto keypad = keypadCaptureInput(kp);
    auto device = deviceCaptureInput(dev);
    // InputCheckpoint exists before checkpointed guest work and before a load.
    // Sharing the scope through the restoration closure fences output until the
    // checkpoint lifetime ends, including refused-load recovery.
    struct AudioScope {
        SocPXA& soc;
        FastAudio& audio;
        uint64_t token;
        AudioScope(SocPXA& soc,FastAudio& audio):soc(soc),audio(audio),token(audio.BeginAudioCheckpoint()) {
            soc.RestoreFastAudioRoute();
        }
        ~AudioScope() {audio.EndAudioCheckpoint(token);soc.RestoreFastAudioRoute();}
        void Restore() {audio.RestoreAudioCheckpoint(token);soc.RestoreFastAudioRoute();}
    };
    std::shared_ptr<AudioScope> audio;
    if(fastAudio)audio=std::make_shared<AudioScope>(*this,*fastAudio);
    // Fences the microphone and audio output while checkpointed guest work runs.
    struct FenceScope {
        SocPXA& soc;
        bool previousInvalidation;
        std::weak_ptr<int> lifetime;
        explicit FenceScope(SocPXA& soc) : soc(soc), previousInvalidation(soc.microphoneInvalidatePending), lifetime(soc.checkpointLifetime) {
            ++soc.checkpointFenceDepth;
            deviceFenceInput(soc.dev, true);
            soc.RestoreFastAudioRoute();
        }
        ~FenceScope() {
            if (lifetime.expired()) return;
            --soc.checkpointFenceDepth;
            if (!soc.checkpointFenceDepth && !soc.irReplacementDepth &&
                (soc.PowerSleeping() || soc.microphoneInvalidatePending)) {
                deviceDisarmInput(soc.dev);
                soc.microphoneInvalidatePending = false;
            }
            deviceFenceInput(soc.dev, false);
            soc.RestoreFastAudioRoute();
        }
    };
    std::shared_ptr<FenceScope> fence;
    if (checkpointFence) fence = std::make_shared<FenceScope>(*this);
    const uint64_t keyDownAt=powerKeyDownAt, heldSince=powerPressHeldSince;
    std::weak_ptr<int> lifetime = checkpointLifetime;
    return [this, keypad, device, audio, fence, keyDownAt, heldSince, lifetime] {
        if (lifetime.expired()) return;
        if(audio)audio->Restore();
        if(fence) microphoneInvalidatePending = fence->previousInvalidation;
        keypad();device();
        powerKeyDownAt=keyDownAt;powerPressHeldSince=heldSince;
    };
}

// External UART clients are suspended while a load is tried. A refused load keeps
// them, since the guest carries on as it was.
void SocPXA::DiscardMemoryStickSaveImage() {
    msCtrlrDiscardSaveImage(GetMemoryStick());
}

void SocPXA::BeforeStateReplacement() {
    deviceFenceInput(dev, true);
    ++irReplacementDepth;
    deviceBeginBluetoothCheckpoint(dev);
    if(fastAudio) {fastAudio->BeforeGuestReplacement();RestoreFastAudioRoute();}
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartSuspendExternal(uart);
}

void SocPXA::StateReplacementRefused(const std::function<void()>& restore) {
    // The UART checkpoint keeps the resumed clients through the reload. It ends without
    // a UART kick, so scheduler phases stay as saved.
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) {
            pxaUartResumeExternal(uart, true);
            pxaUartBeginCheckpoint(uart);
        }
    restore();
    if(fastAudio) {fastAudio->GuestReplacementFinished();RestoreFastAudioRoute();}
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartEndCheckpoint(uart, false);
    if (irReplacementDepth) --irReplacementDepth;
    deviceEndBluetoothCheckpoint(dev,PowerSleeping());
    deviceFenceInput(dev, false);
    RefreshIrMode(this);
}

void SocPXA::StateReplaced() {
    if (!RestoringCheckpoint()) {
        if (checkpointFenceDepth) microphoneInvalidatePending = true;
        else { deviceDisarmInput(dev); microphoneInvalidatePending = false; }
    }
    deviceFenceInput(dev, false);
    if (irReplacementDepth) --irReplacementDepth;
    deviceEndBluetoothCheckpoint(dev,PowerSleeping());
    if (!RestoringCheckpoint()) deviceBluetoothStateReplaced(dev);
    if(fastAudio) {fastAudio->GuestReplacementFinished();RestoreFastAudioRoute();}
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartResumeExternal(uart, false);
    // A checkpoint restore for a read (database list, export) puts back the state from
    // just before; the USB host was paused meanwhile and carries on.
    if (usbHostHooks.replaced && !RestoringCheckpoint()) usbHostHooks.replaced(usbHostHooks.context);
    if (irHostHooks.replaced && !RestoringCheckpoint()) irHostHooks.replaced(irHostHooks.context);
    if (serialHostHooks.replaced && !RestoringCheckpoint()) serialHostHooks.replaced(serialHostHooks.context);
}

void SocPXA::BeginSerialCheckpoint() {
    deviceBeginBluetoothCheckpoint(dev);
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartBeginCheckpoint(uart);
    ++usbHostPaused;
}
void SocPXA::EndSerialCheckpoint() {
    for (auto* uart : {ffUart, stUart, btUart, hwUart})
        if (uart) pxaUartEndCheckpoint(uart, false);
    if (usbHostPaused) --usbHostPaused;
    deviceEndBluetoothCheckpoint(dev,PowerSleeping());
    serialResumePending = true;
    if (!usbHostPaused && PowerSleeping() && irHostHooks.replaced)
        irHostHooks.replaced(irHostHooks.context);
    if (!usbHostPaused && PowerSleeping() && serialHostHooks.replaced)
        serialHostHooks.replaced(serialHostHooks.context);
    RefreshIrMode(this);
}

uint64_t SocPXA::Run(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    // Releasing a host guard does not alter the restored guest's scheduler/CPU.
    // Deliver the host notification when execution resumes, including new peer data.
    if (serialResumePending && !usbHostPaused && !PowerSleeping()) {
        serialResumePending = false;
        for (auto* uart : {ffUart, stUart, btUart, hwUart})
            if (uart) pxaUartKick(uart);
    }
    return SocGeneric<SocPXA>::Run(maxCycles, cyclesPerSecond);
}

void SocPXA::CycleBatch0() {
    pxaDmaPeriodic(dma);
    pxaUartProcess(ffUart);
    if (hwUart) pxaUartProcess(hwUart);
    pxaUartProcess(stUart);
    pxaUartProcess(btUart);
    for (int i = 0; i < 3; i++) {
        if (ssp[i]) pxaSspPeriodic(ssp[i]);
    }
    devicePeriodic(dev, DEVICE_PERIODIC_TIER0);
    if (usbHostHooks.tick && !usbHostPaused &&
        (!usbHostHooks.wake || usbHostHooks.wake(usbHostHooks.context) <= GetTime()))
        usbHostHooks.tick(usbHostHooks.context, GetTime());
}

bool SocPXA::Batch0Required() {
    if (!dev) return true;

    if (pxaDmaTaskRequired(dma)) return true;

    for (int i = 0; i < 3; i++) {
        if (ssp[i] && pxaSspTaskRequired(ssp[i])) return true;
    }

    if (deviceTaskRequired(dev, DEVICE_PERIODIC_TIER0)) return true;

    if (pxaUartTaskRequired(ffUart) || pxaUartTaskRequired(stUart) || pxaUartTaskRequired(btUart))
        return true;

    if (hwUart && pxaUartTaskRequired(hwUart)) return true;

    return false;
}

// Batches until AUX_1 runs again: 1 while a peripheral needs it, else as many as the USB
// host can sleep, else 0.
uint32_t SocPXA::Batch0Ticks() {
    if (Batch0Required()) return 1;
    if (!usbHostHooks.wake || usbHostPaused) return 0;
    const uint64_t wake = usbHostHooks.wake(usbHostHooks.context);
    if (wake == UINT64_MAX) return 0;
    const uint64_t now = GetTime(), period = 36_sec / 3686400ULL;
    if (wake <= now + period) return 1;
    return uint32_t(std::min<uint64_t>((wake - now + period - 1) / period, UINT32_MAX));
}

void SocPXA::UpdateSchedulePcm() {
    // Preview backpressure pauses the whole guest; keep its codec and DMA clocks together.
    scheduler->ScheduleTask(SCHEDULER_TASK_PCM,
                            1_sec / (pcmEnabled || fastAudioRouted ? PCM_HZ_ENABLED : PCM_HZ_DISABLED),
                            (pcmSuspended && !fastAudioRouted) ? 0 : 1);
}

void SocPXA::RescheduleCB(void *ctx, uint32_t task) {
    auto *self = reinterpret_cast<SocPXA *>(ctx);

    switch (task) {
        case RESCHEDULE_TASK_DEVICE_TIER0:
        case RESCHEDULE_TASK_SSP:
        case RESCHEDULE_TASK_UART:
        case RESCHEDULE_TASK_DMA:
            self->scheduler->RescheduleTask(SCHEDULER_TASK_AUX_1, 1);
            cpuSetSlowPath(self->cpu, SLOW_PATH_REASON_RESCHEDULE);
            break;
    }
}

#ifndef __EMSCRIPTEN__
uint32_t SocPXA::TimerElapsedCB(void* ctx) {
    auto* self = static_cast<SocPXA*>(ctx);
    if (self->PowerSleeping() && self->sleeping) return 0;
    const uint64_t extra = self->runningCyclesPerSecond
        ? uint64_t(cpuCyclesInProgress(self->cpu)) * 1_sec / self->runningCyclesPerSecond : 0;
    return self->scheduler->ElapsedTicks(SCHEDULER_TASK_TIMER, extra);
}
void SocPXA::TimerRescheduleCB(void* ctx) {
    auto* self = static_cast<SocPXA*>(ctx);
    self->scheduler->RescheduleTask(SCHEDULER_TASK_TIMER, pxaTimrNextBatch(self->tmr));
    cpuSetSlowPath(self->cpu, SLOW_PATH_REASON_RESCHEDULE);
}
#endif

uint64_t SocPXA::GetTimeCB(void *ctx) {
    return reinterpret_cast<SocPXA *>(ctx)->scheduler->GetTime();
}

template void SocGeneric<SocPXA>::Save<Savestate<ChunkType>>(Savestate<ChunkType> &savestate);
template void SocGeneric<SocPXA>::Save<SavestateProbe<ChunkType>>(
    SavestateProbe<ChunkType> &savestate);
// Out of line for tests that pump input directly; the core may inline its only call.
template void SocGeneric<SocPXA>::PumpEventQueues();

// TX's IPL copies the OS from NAND into this RAM window. Enable the existing
// signature-based PACE bridge before the guest enters its 68K application loop.
// Never patch the IPL, NAND, or an image whose active mappings do not match.
bool SocPXA::PrepareM68kSupport() {
    if (GetDeviceType() == deviceTypeTG50)
        return pacePatchInitTg50(pacePatch, ROM_BASE, romGetData(rom), romGetPeepholeBuffer(rom), romGetSize(rom));
    if (GetDeviceType() != DeviceType5::deviceTypeTX) return IsPacePatched();

    constexpr uint32_t virtualBase = 0x20000000;
    constexpr uint32_t physicalOffset = 0x00200000;
    constexpr uint32_t imageSize = 0x01000000;
    ArmMmu* mmu = cpuGetMMU(cpu);
    if (!mmuIsOn(mmu) || ramSize < physicalOffset + imageSize) return false;

    // A guest-initiated reset can reload the original interpreter over our patch.
    // Do not advertise a stale bridge after that reload.
    if (IsPacePatched()) {
        const uint32_t offset = pacePatch->enterPace - virtualBase;
        const auto mapping = mmuTranslate(mmu, pacePatch->enterPace, true, false);
        if (!MMU_TRANSLATE_RESULT_OK(mapping) ||
            MMU_TRANSLATE_RESULT_PA(mapping) != RAM_BASE + physicalOffset + offset)
            return false;
        if (offset <= imageSize - 12) {
            const auto* entry = reinterpret_cast<const uint32_t*>(
                bufferMemory.buffer + physicalOffset + offset);
            if (entry[0] == INSTR_PACE_ENTER && entry[1] == INSTR_PACE_RESUME &&
                entry[2] == INSTR_PACE_RETURN_FROM_CALLOUT)
                return true;
        }
        *pacePatch = {};
    }

    for (uint32_t offset = 0; offset < imageSize; offset += 1024) {
        const auto mapping = mmuTranslate(mmu, virtualBase + offset, true, false);
        if (!MMU_TRANSLATE_RESULT_OK(mapping) ||
            MMU_TRANSLATE_RESULT_PA(mapping) != RAM_BASE + physicalOffset + offset)
            return false;
    }

    pacePatchInit(pacePatch, virtualBase, bufferMemory.buffer + physicalOffset, imageSize);
    if (!IsPacePatched()) return false;

    memoryBufferMarkRangeDirty(&bufferMemory, physicalOffset, imageSize);
    cpuInvalidateCodeCache(cpu);
    return true;
}


bool SocPXA::EnableFastAudio() {
    const auto type=GetDeviceType();
    if ((type != DeviceType5::deviceTypeTX && type != DeviceType5::deviceTypeE2 &&
         type != DeviceType5::deviceTypeZire72 && type != DeviceType5::deviceTypeFrankenE2) || fastAudio) return false;
    auto start=type==deviceTypeFrankenE2?FastAudio::E3Start:
        type==deviceTypeZire72?FastAudio::Zire72Start:FastAudio::ArmStartSignature{};
    const auto rom=GetRomData();
    const auto* variant=RomInfo5(rom.data,rom.size).GetFirmwareVariant();
    if(variant && variant->deviceType==type && variant->fastAudioArmStartEntry)
        start={variant->fastAudioArmStartEntry,variant->fastAudioArmStartCall,variant->fastAudioArmStartBranch};
    fastAudio = std::make_unique<FastAudio>([this](uint32_t addr, void* data, unsigned size) {
        return cpuInspectRead(cpu,addr,data,size);
    }, deviceProfileIsE2Board5(type) ? FastAudio::Driver::e2 : type==deviceTypeZire72 ? FastAudio::Driver::zire72 : FastAudio::Driver::tx,start);
    fastAudio->SetQueue(audioQueue);
    // ARM Sound Managers preview at the HAL write only.
    const bool early=!start.entry;
    fastAudio->SetEarlyPlayback(early);
    if(PowerSleeping())fastAudio->Sleep();
    auto observer=[](ArmCpu* cpu,void* context,uint32_t pc,uint32_t from) {
        auto* self=static_cast<SocPXA*>(context);
        if(self->GetCpu()==cpu && self->fastAudio)
            {self->fastAudio->EarlyTransition(cpuGetRegisters(cpu),pc,from,self->GetTime(),self->pcmEnabled);
            self->fastAudio->ValidateEarly(self->GetTime());self->UpdateFastAudioRoute();}
    };
    const bool e2=type==deviceTypeE2;
    if(early)cpuSetEarlyAudioHook(cpu,observer,this,e2?0x20072c7c:0x20057cd4,e2?0x200665c0:0x2004b618);
    auto hook = [](void* ctx, uint32_t trap, uint32_t* regs) {
        auto soc = static_cast<SocPXA*>(ctx);
        if(trap==0x4324)soc->fastAudio->ControlHead(regs,soc->GetTime(),patchDispatchCanTrackTail(soc->patchDispatch));
        const bool suppress=soc->fastAudio->ObserveHal(trap,regs,soc->GetTime());
        soc->fastAudio->ValidateEarly(soc->GetTime());
        soc->fastAudio->Handle(trap,regs,soc->GetTime(),soc->pcmEnabled,suppress);
        soc->fastAudio->TryEarly(soc->GetTime(),soc->pcmEnabled);
        soc->UpdateFastAudioRoute();
        return true;
    };
    auto tail=[](void* ctx,uint32_t,const uint32_t* before,uint32_t* regs) {
        auto* soc=static_cast<SocPXA*>(ctx);
        soc->fastAudio->ControlTail(before,regs,soc->GetTime());
        soc->fastAudio->TryEarly(soc->GetTime(),soc->pcmEnabled);
        soc->UpdateFastAudioRoute();
    };
    for (auto trap : {0x4314u,0x4318u,0x4320u})
        patchDispatchAddPatch(patchDispatch,trap,hook,nullptr,this);
    patchDispatchAddPatch(patchDispatch,0x4324,hook,tail,this);
    return true;
}
void SocPXA::SetFastAudioPeriod(uint32_t frames) {
    if (fastAudio) fastAudio->SetPeriod(frames);
}

void SocPXA::NotifyFastAudioUnderrun() {
    if (!fastAudio) return;
    fastAudio->NotifyUnderrun(GetTime());
    UpdateFastAudioRoute();
}

void SocPXA::UpdateFastAudioRoute() {
    const bool bypass = fastAudio && fastAudio->Bypass();
    if (bypass == fastAudioRouted) return;
    fastAudioRouted = bypass;
    deviceSetAudioQueue(dev,((GetDeviceType() == deviceTypeTG50 && !pcmEnabled) || audioOutputFenced || checkpointFenceDepth || bypass || (fastAudio && fastAudio->OutputFenced())) ? nullptr : audioQueue);
    UpdateSchedulePcm();
    cpuSetSlowPath(cpu,SLOW_PATH_REASON_RESCHEDULE);
}

void SocPXA::RefreshIrMode(void* context) {
    auto& soc=*static_cast<SocPXA*>(context);
    if (soc.dev) deviceBluetoothPinsChanged(soc.dev);
    if (!soc.usbHostPaused && !soc.irReplacementDepth && soc.irHostHooks.modeChanged)
        soc.irHostHooks.modeChanged(soc.irHostHooks.context);
    if (!soc.usbHostPaused && !soc.irReplacementDepth && soc.serialHostHooks.modeChanged)
        soc.serialHostHooks.modeChanged(soc.serialHostHooks.context);
}

void SocPXA::RefreshIrConfiguration(void* context) {
    RefreshIrMode(context);
    auto& soc=*static_cast<SocPXA*>(context);
    if (!soc.restoringPowerState && !soc.irReplacementDepth && soc.GetDeviceType()==deviceTypeTG50 &&
        soc.irKeyboard.Pending() && soc.IrPowerEnabled())
        RescheduleCB(&soc,RESCHEDULE_TASK_UART);
}

bool SocPXA::IrPowerEnabled() {
    // Active-high transceiver shutdown: Linux palmte2.h, mach/palmtx.h, palmz72.h.
    // An input/alternate-function pin has not explicitly powered this link on.
    const auto type=GetDeviceType();
    if (type == deviceTypeTG50) return deviceTg50IrPowerEnabled(dev);
    return pxaGpioGetState(gpio,type==deviceTypeTX?40:type==deviceTypeZire72?49:48)==PxaGpioStateLow;
}

bool SocPXA::SerialEnabled() {
    // Original Sony wired driver selects ISR0 and drives the GPIO27 mux high.
    return GetDeviceType()==deviceTypeTG50 && !HostIoPaused() && !irReplacementDepth &&
        !PowerSleeping() && !irKeyboard.Attached() && !irKeyboard.Pending() && pxaUartPlainEnabled(ffUart) &&
        pxaGpioGetState(gpio,27)==PxaGpioStateHigh;
}
SerialHostRoute SocPXA::GetSerialRoute() {
    if (GetDeviceType()!=deviceTypeTG50) return {};
    return {this,[](void* p,const SerialHostHooks* h){return static_cast<SocPXA*>(p)->SetSerialHostHooks(h);},
        [](void* p){return static_cast<SocPXA*>(p)->SerialEnabled();},
        [](void* p){auto* s=static_cast<SocPXA*>(p);return s->HostIoPaused() || s->irReplacementDepth!=0 || s->PowerSleeping();},
        [](void* p){auto* s=static_cast<SocPXA*>(p);return !s->KeyboardAttached() && !s->irKeyboard.Pending() && !s->IrHostAttached();}};
}
bool SocPXA::SetSerialHostHooks(const SerialHostHooks* hooks) {
    if (hooks && (GetDeviceType()!=deviceTypeTG50 || !hooks->context || !hooks->read || !hooks->write ||
        !hooks->pending || !hooks->replaced || !hooks->modeChanged || serialHostHooks.context ||
        irHostHooks.context || irKeyboard.Attached() || irKeyboard.Pending() || !pxaUartIrState(ffUart).idle)) return false;
    serialHostHooks=hooks?*hooks:SerialHostHooks{};
    return true;
}

// Not while a byte is still going out. A received byte the guest hasn't read yet stays the
// guest's, whoever owns the line next.
bool SocPXA::SetIrHostHooks(const IrHostHooks* hooks) {
    if (hooks) {
        if (!hooks->context || !hooks->read || !hooks->write || !hooks->pending || !hooks->replaced ||
            (GetDeviceType()!=deviceTypeTX && !deviceProfileIsE2Board5(GetDeviceType()) &&
             GetDeviceType()!=deviceTypeZire72 && GetDeviceType()!=deviceTypeTG50) ||
            irHostHooks.context || serialHostHooks.context || irKeyboard.Attached() || irKeyboard.Pending() ||
            pxaUartIrState(irUart).sending) return false;
    }
    irHostHooks = hooks ? *hooks : IrHostHooks{};
    return true;
}

void SocPXA::AttachKeyboard(bool value) {
    if (value && (IrHostAttached() || serialHostHooks.context)) return;
    if (GetDeviceType()!=DeviceType5::deviceTypeTX && !deviceProfileIsE2Board5(GetDeviceType()) &&
        GetDeviceType()!=DeviceType5::deviceTypeZire72 && GetDeviceType()!=deviceTypeTG50) return;
    irKeyboard.Attach(value);
    RescheduleCB(this,RESCHEDULE_TASK_UART);
}
bool SocPXA::KeyboardKey(uint8_t code,bool down) {
    const bool accepted=(GetDeviceType()==DeviceType5::deviceTypeTX || deviceProfileIsE2Board5(GetDeviceType()) ||
                         GetDeviceType()==DeviceType5::deviceTypeZire72 || GetDeviceType()==deviceTypeTG50) &&
        (!down || (pxaUartIrReceiveEnabled(irUart) &&
                   (GetDeviceType()!=deviceTypeTG50 || IrPowerEnabled()))) && irKeyboard.Key(code,down);
    if (accepted) RescheduleCB(this,RESCHEDULE_TASK_UART);
    return accepted;
}
void SocPXA::ReleaseKeyboard() {
    irKeyboard.ReleaseAll();
    if (irKeyboard.Pending()) RescheduleCB(this,RESCHEDULE_TASK_UART);
}

bool SocPXA::SetBluetooth(const std::string& path,std::string& error) {return deviceSetBluetooth(dev,path,error);}
std::string SocPXA::BluetoothState() {return deviceBluetoothState(dev);}
SocPXA::~SocPXA() {
    checkpointLifetime.reset();
    deviceStopBluetooth(dev);
    cpuSetEarlyAudioHook(cpu,nullptr,nullptr,0,0);
    deviceDestroy(dev);
}

void SocPXA::RestoreFastAudioRoute() {
    fastAudioRouted=fastAudio && fastAudio->Bypass();
    deviceSetAudioQueue(dev,((GetDeviceType() == deviceTypeTG50 && !pcmEnabled) || audioOutputFenced || checkpointFenceDepth || fastAudioRouted || (fastAudio && fastAudio->OutputFenced())) ? nullptr : audioQueue);
}

bool SocPXA::UsbBoardListening() { return GetDeviceType() != deviceTypeTG50 || deviceTg50UsbListening(dev); }

void SocPXA::RefreshTransportPins(void* context) {
    auto& soc = *static_cast<SocPXA*>(context);
    RefreshIrConfiguration(context);
    if (!soc.usbHostPaused && !soc.irReplacementDepth && soc.usbHostHooks.pinsChanged)
        soc.usbHostHooks.pinsChanged(soc.usbHostHooks.context);
}

MemoryStickController* SocPXA::GetMemoryStick(){return deviceMemoryStick(dev);}

void SocPXA::SetAudioOutputFenced(bool fenced) {
    if (!backgroundPanel || audioOutputFenced == fenced) return;
    audioOutputFenced = fenced;
    RestoreFastAudioRoute();
}
