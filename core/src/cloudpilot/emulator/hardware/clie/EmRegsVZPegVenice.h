#ifndef _EM_REGS_VZ_PEG_VENICE_H_
#define _EM_REGS_VZ_PEG_VENICE_H_

#include <memory>

#include "EmRegsVZ.h"
#include "EmSonyXZWithSlot.h"

class EmRegsVzPegVenice : public EmSonyXzWithSlot<EmRegsVZ> {
   public:
    EmRegsVzPegVenice(EmRegsMB86189& mb86189);
    virtual ~EmRegsVzPegVenice(void);

    // The slot's card-detect line on the MB86189's port pins changes with the stick.
    void Mount(EmHAL::Slot slot, CardImage& cardImage) override;
    void Unmount(EmHAL::Slot slot) override;

   public:
    virtual Bool GetLCDScreenOn(void);
    virtual Bool GetLCDBacklightOn(void);
    virtual uint16 GetLEDState(void);

    virtual Bool GetSerialPortOn(int uartNum);
    // IrDA is UART1. The cradle is USB, so there is no serial line.
    virtual Bool GetLineDriverState(EmUARTDeviceType type);
    virtual EmUARTDeviceType GetUARTDevice(int uartNum);
    virtual Bool GetVibrateOn(void);

    virtual uint8 GetPortInputValue(int);
    virtual uint8 GetPortInternalValue(int);
    virtual void GetKeyInfo(int* numRows, int* numCols, uint16* keyMap, Bool* rows);

    virtual bool CopyLCDFrame(Frame& frame, bool fullRefresh);

    // Jog Dial turns: one IRQ2 edge (PD5) per detent, PG1 high for up.
    void DispatchButtonEvent(ButtonEvent evt) override;

    // In the USB cradle: HwrDockStatus reads the cradle's ID resistor on ADC input 6.
    void SetUsbCradle(bool docked);

   protected:
    virtual EmSPISlave* GetSPI2Slave(void);
    // Jog Dial push and Back as matrix keys.
    uint16 ButtonToBits(ButtonEvent::Button btn) override;

   private:
    EmRegsMB86189& mb86189;
    EmSPISlave* fSPISlaveADC;
    unique_ptr<uint32[]> framebufferTmp;
    // PG1 as JogSys reads it in its IRQ2 handler, right after the edge; not saved.
    bool jogUp{false};
};

#endif  // _EM_REGS_VZ_PEG_VENICE_H_
