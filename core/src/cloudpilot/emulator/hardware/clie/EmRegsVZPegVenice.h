#ifndef _EM_REGS_VZ_PEG_VENICE_H_
#define _EM_REGS_VZ_PEG_VENICE_H_

#include <memory>

#include "EmRegsVZ.h"
#include "EmSonyXZWithSlot.h"

class EmRegsVzPegVenice : public EmSonyXzWithSlot<EmRegsVZ> {
   public:
    EmRegsVzPegVenice(EmRegsMB86189& mb86189);
    virtual ~EmRegsVzPegVenice(void);

   public:
    virtual Bool GetLCDScreenOn(void);
    virtual Bool GetLCDBacklightOn(void);
    virtual uint16 GetLEDState(void);

    virtual Bool GetSerialPortOn(int uartNum);
    virtual Bool GetVibrateOn(void);

    virtual uint8 GetPortInputValue(int);
    virtual uint8 GetPortInternalValue(int);
    virtual void GetKeyInfo(int* numRows, int* numCols, uint16* keyMap, Bool* rows);

    virtual bool CopyLCDFrame(Frame& frame, bool fullRefresh);

    // Jog Dial turns: one IRQ2 edge (PD5) per detent, PG1 high for up.
    void DispatchButtonEvent(ButtonEvent evt) override;

   protected:
    virtual EmSPISlave* GetSPI2Slave(void);
    // Jog Dial push and Back as matrix keys.
    uint16 ButtonToBits(ButtonEvent::Button btn) override;

   private:
    EmSPISlave* fSPISlaveADC;
    unique_ptr<uint32[]> framebufferTmp;
    // PG1 as JogSys reads it in its IRQ2 handler, right after the edge; not saved.
    bool jogUp{false};
};

#endif  // _EM_REGS_VZ_PEG_VENICE_H_
