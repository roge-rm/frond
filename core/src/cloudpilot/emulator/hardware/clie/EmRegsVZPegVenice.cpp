#include "EmRegsVZPegVenice.h"

#include "EmBankRegs.h"  // EmBankRegs::InvalidAccess
#include "EmCommon.h"
#include "EmMemory.h"
#include "EmRegsVZPrv.h"
#include "EmSPISlaveADS784x.h"  // EmSPISlaveADS784x
#include "Frame.h"
#include "PalmPack.h"

using VZ = EmSonyXzWithSlot<EmRegsVZ>;

#define NON_PORTABLE
#include "EZAustin/IncsPrv/HardwareAustin.h"  // hwrEZPortCKbdRow0, hwrEZPortBRS232Enable, etc.
#undef NON_PORTABLE
#include "PalmPackPop.h"

#define hwrVZVenicePortEAlermLED 0x08  // PE3  AlermLED

// Port B Bit settings for Venice

// Port D Bit settings for Venice
#define hwrVZVenicePortDKbdCol0 0x01     // PD0 /
#define hwrVZVenicePortDKbdCol1 0x02     // PD1 /
#define hwrVZVenicePortDKbdCol2 0x04     // PD2 /
#define hwrVZVenicePortDMS_IF_Intl 0x08  // PD3

#define hwrVZVenicePortDDockButton 0x10  // PD4 / HotSync
#define hwrVZVenicePortDPowerFail \
    0x80  // PD7 / (L) Power Fail Interrupt	(aka IRQ6) (level, low)

// Port F Bit settings for Venice
#define hwrVZVenicePortFLCDPowered 0x40  // PF6 / LCD�d������M��

// Port G Bit settings for Venice
#define hwrVZVenicePortGJogDirection 0x02  // PG1 / Jog Dial direction, high turning up
#define hwrVZVenicePortGADC_CS 0x08       // PG3 / ADC chip select
#define hwrVZVenicePortGRS232Enable 0x10  // PG4 / Enable the RS232 Transceiver

// Port K Bit settings for VENICE
#define hwrVZVenicePortKVibrate 0x04      // PK2  Vibrate
#define hwrVZVenicePortKBacklightOn 0x10  // PK4 / (H) Backlight ON Enable
#define hwrVZVenicePortKKbdRow0 0x20      // PK5 /
#define hwrVZVenicePortKKbdRow1 0x40      // PK6
#define hwrVZVenicePortKKbdRow2 0x80      // PK7

// Port J Bit settings for Venice
#define hwrVZVenicePortJDC_IN 0x04        // PJ2 : DC_IN / DC
#define hwrVZVenicePortJLCDEnableOn 0x08  // PJ3 :(H) LCD Enable

#define keyBitJogBack (0x0800)  // JogDial: Back key
// JogDial push: row 1 (PK6), column 2 (PD2), from the key tables of the ROM's PrvKeyScan users.
#define keyBitJogPush (0x0400)

const int kNumButtonRows = 3;
const int kNumButtonCols = 3;

const uint16 kButtonMap[kNumButtonRows][kNumButtonCols] = {
    {keyBitHard1, keyBitHard2, keyBitHard3},
    {keyBitPageUp, keyBitPageDown, keyBitJogPush},
    {keyBitPower, keyBitHard4, keyBitJogBack}};

// ---------------------------------------------------------------------------
//		� EmRegsEZPalmIIIc::EmRegsEZPalmIIIc
// ---------------------------------------------------------------------------

EmRegsVzPegVenice::EmRegsVzPegVenice(EmRegsMB86189& mb86189)
    : VZ(mb86189), mb86189(mb86189), fSPISlaveADC(new EmSPISlaveADS784x(kChannelSet2)) {
    mb86189.SetGpioReadHandler(
        []() { return gExternalStorage.IsMounted(EmHAL::Slot::memorystick) ? 0x01 : 0x00; });
    mb86189.irqChange.AddHandler([=](bool newState) { UpdateIRQ3(newState ? 0x40 : 0x00); });
    framebufferTmp = make_unique<uint32[]>(320 * 160 / 2 / 4);
}

// ---------------------------------------------------------------------------
//		� EmRegsEZPalmIIIc::~EmRegsEZPalmIIIc
// ---------------------------------------------------------------------------

EmRegsVzPegVenice::~EmRegsVzPegVenice(void) { delete fSPISlaveADC; }

void EmRegsVzPegVenice::Mount(EmHAL::Slot slot, CardImage& cardImage) {
    VZ::Mount(slot, cardImage);
    if (slot == EmHAL::Slot::memorystick) mb86189.NotifyGpioChanged();
}

void EmRegsVzPegVenice::Unmount(EmHAL::Slot slot) {
    VZ::Unmount(slot);
    if (slot == EmHAL::Slot::memorystick) mb86189.NotifyGpioChanged();
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetLCDScreenOn
// ---------------------------------------------------------------------------

Bool EmRegsVzPegVenice::GetLCDScreenOn(void) {
    UInt8 portJData = READ_REGISTER(portJData);
    return (portJData & hwrVZVenicePortJLCDEnableOn) != 0;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetBacklightOn
// ---------------------------------------------------------------------------

Bool EmRegsVzPegVenice::GetLCDBacklightOn(void) {
    return (READ_REGISTER(portKData) & hwrVZVenicePortKBacklightOn) != 0;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetSerialPortOn
// ---------------------------------------------------------------------------

Bool EmRegsVzPegVenice::GetSerialPortOn(int /*uartNum*/) {
    return (READ_REGISTER(portGData) & hwrVZVenicePortGRS232Enable) != 0;
}

// The ROM gates nothing on a port bit for IR, so the transceiver counts as always powered.
Bool EmRegsVzPegVenice::GetLineDriverState(EmUARTDeviceType type) { return type == kUARTIR; }

EmUARTDeviceType EmRegsVzPegVenice::GetUARTDevice(int uartNum) {
    return uartNum == 0 ? kUARTIR : kUARTNone;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetPortInputValue
// ---------------------------------------------------------------------------

uint8 EmRegsVzPegVenice::GetPortInputValue(int port) {
    uint8 result = VZ::GetPortInputValue(port);

    if (port == 'M') {
        result |= 0x00;
    }

    if (port == 'J') {
        result |= hwrVZVenicePortJDC_IN;
    }

    if (port == 'F') {
        // Make sure this is always set, or HwrDisplayWake will hang
        result |= hwrVZVenicePortFLCDPowered;
    }

    if (port == 'G') {
        result = jogUp ? result | hwrVZVenicePortGJogDirection : result & ~hwrVZVenicePortGJogDirection;
    }
    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetPortInternalValue
// ---------------------------------------------------------------------------

uint8 EmRegsVzPegVenice::GetPortInternalValue(int port) {
    uint8 result = VZ::GetPortInternalValue(port);

    if (port == 'C') {
        result |= 0x10;
    }

    if (port == 'D') {
        // Ensure that bit hwrEZPortDDockButton is set.  If it's clear, HotSync
        // will sync via the modem instead of the serial port.
        //
        // Also make sure that hwrEZPortDPowerFail is set.  If it's clear,
        // the battery code will make the device go to sleep immediately.

        result |= hwrVZVenicePortDDockButton | hwrVZVenicePortDPowerFail;
    }

    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetKeyInfo
// ---------------------------------------------------------------------------

void EmRegsVzPegVenice::GetKeyInfo(int* numRows, int* numCols, uint16* keyMap, Bool* rows) {
    *numRows = kNumButtonRows;
    *numCols = kNumButtonCols;

    memcpy(keyMap, kButtonMap, sizeof(kButtonMap));

    // Determine what row is being asked for.
    UInt8 portKDir = READ_REGISTER(portKDir);
    UInt8 portKData = READ_REGISTER(portKData);

    rows[0] =
        (portKDir & hwrVZVenicePortKKbdRow0) != 0 && (portKData & hwrVZVenicePortKKbdRow0) == 0;
    rows[1] =
        (portKDir & hwrVZVenicePortKKbdRow1) != 0 && (portKData & hwrVZVenicePortKKbdRow1) == 0;
    rows[2] =
        (portKDir & hwrVZVenicePortKKbdRow2) != 0 && (portKData & hwrVZVenicePortKKbdRow2) == 0;
}

// The Jog Dial's encoder isn't in the key matrix: JogSys takes an IRQ2 edge per detent, reads
// the direction from PG1 and queues vchrJogUp or vchrJogDown. The front rocker is PageUp/PageDown.
void EmRegsVzPegVenice::DispatchButtonEvent(ButtonEvent event) {
    const auto button = event.GetButton();
    if (button != ButtonEvent::Button::wheelUp && button != ButtonEvent::Button::wheelDown)
        return VZ::DispatchButtonEvent(event);
    if (event.GetType() != ButtonEvent::Type::press) return;

    jogUp = button == ButtonEvent::Button::wheelUp;
    WRITE_REGISTER(intPendingHi, READ_REGISTER(intPendingHi) | hwrVZ328IntHiIRQ2);
    UpdateInterrupts();
}

uint16 EmRegsVzPegVenice::ButtonToBits(ButtonEvent::Button btn) {
    switch (btn) {
        case ButtonEvent::Button::wheelPush:
            return keyBitJogPush;
        case ButtonEvent::Button::back:
            return keyBitJogBack;
        default:
            return VZ::ButtonToBits(btn);
    }
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetSPISlave
// ---------------------------------------------------------------------------

EmSPISlave* EmRegsVzPegVenice::GetSPI2Slave(void) {
    if ((READ_REGISTER(portGData) & hwrVZVenicePortGADC_CS) == 0) {
        return fSPISlaveADC;
    }

    return NULL;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetLEDState
// ---------------------------------------------------------------------------

uint16 EmRegsVzPegVenice::GetLEDState(void) {
    uint16 result = kLEDOff;
    UInt8 portEData = READ_REGISTER(portEData);

    if (portEData & hwrVZVenicePortEAlermLED) result |= (kLEDRed | kLEDGreen);
    return result;
}

// ---------------------------------------------------------------------------
//		� EmRegsVzPegVenice::GetVibrateOn
// ---------------------------------------------------------------------------

Bool EmRegsVzPegVenice::GetVibrateOn(void) {
    UInt8 portKData = READ_REGISTER(portKData);
    return (portKData & hwrVZVenicePortKVibrate);
}

bool EmRegsVzPegVenice::CopyLCDFrame(Frame& frame, bool fullRefresh) {
    if (!VZ::CopyLCDFrame(frame, fullRefresh)) {
        return false;
    }

    if (frame.lineWidth == 160 && frame.lines >= 160) {
        frame.lines = 160;
        frame.scaleX = frame.scaleY = 2;
    }

    return true;
}
