#ifndef _CHUNK_TYPE_H_
#define _CHUNK_TYPE_H_

#include <cstdint>

enum class ChunkType : uint32_t {
    socGeneric = 0x0010,
    pxaUart = 0x0030,
    pxaSsp = 0x0040,
    pxaGpio = 0x0050,
    pxaTimr = 0x0060,
    pxaIc = 0x0070,
    pxaLcd = 0x0080,
    pxaDma = 0x0090,
    pxaAc97 = 0x00a0,
    pxaI2s = 0x00b0,
    pxaI2c = 0x00c0,
    pxaMemCtrlr = 0x00d0,
    pxaPwrClk = 0x00e0,
    pxaPwm = 0x00f0,
    pxaMmc = 0x0100,
    pxaRtc = 0x0110,
    pxa255dsp = 0x0120,
    pxa255udc = 0x0130,
    wm9712L = 0x0140,
    bcm2035 = 0x0150,
    pxa270imc = 0x0160,
    pxa270kpc = 0x0170,
    pxa270udc = 0x0180,
    pxa270wmmx = 0x0190,
    palmTx = 0x01a0,
    sonyTg50Uc = 0x01b0,
    sonyTg50Touch = 0x01c0,
    sonyTg50Codec = 0x01d0,
    sonyTg50Power = 0x01e0,
    sonyTg50Ms = 0x01f0,
    sonyTg50Board = 0x0200,
    sonyTg50Audio = 0x0210,
    sonyTg50MsCard = 0x0220,
    sonyTg50Configuration = 0x0230,
    sonyTg50Bluetooth = 0x0240,
    bluetoothAddress = 0x0250,
    ov9640 = 0x0260,
    pxa27xQci = 0x0270,

    pvIc = 0x1000,
    pvTimer = 0x1010,
    pvDisplay = 0x1020,
    pvKeys = 0x1030,
    pvAudio = 0x1040,
    pvTouch = 0x1050,
    pvStorage = 0x1060,

    scheduler = 0x10000,
    cpu = 0x10010,
    pace = 0x10020,
    mmu = 0x10030,
    cp15mmu = 0x10040,
    patchDispatch = 0x10050,
    nand = 0x10060,
    vsd = 0x10070,
    systemState = 0x10080,
    mpu = 0x10090,
    cp15mpu = 0x100a0,
    networkSimulation = 0x100b0,
    irKeyboard = 0x100c0,
    libertasCF = 0x100d0,
    xscaleDcache = 0x100e0,
};

inline ChunkType operator+(ChunkType chunkType, uint32_t index) {
    return static_cast<ChunkType>(static_cast<uint32_t>(chunkType) + index);
}

#endif  // _CHUNK_TYPE_H_
