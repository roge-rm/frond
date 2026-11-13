#ifndef FROND_DEVICE_PROFILE5_H
#define FROND_DEVICE_PROFILE5_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "device_type5.h"
#include "LegacyCardGeometry.h"

// These values are stored in existing sessions.
enum class DisplayMode {
    mode_160x160 = 1, mode_160x220 = 2, mode_240x240 = 3, mode_240x320 = 4,
    mode_320x320 = 5, mode_320x480 = 6, mode_480x480 = 7, mode_480x720 = 8, mode_560x160 = 9,
    mode_480x320 = 10, mode_480x640 = 11
};

enum class DeviceCohort5 { txNand, e2Nand, patchedE2, paravirtual, zire72Nor, tg50Nor, reserved, ez68k, vz68k, zodiacNor };
enum class DeviceSupport5 { txDesktop, e2Desktop, zire72Desktop, inheritedUnverified, staged, reserved, experimentalDesktop };
// How a device is told from its firmware: the card header's company and HAL IDs, the NOR size, or a
// big-endian 68k card header (deviceProfileInspect68kRom5).
enum class DeviceDetect5 { cardHeader, norSize, none, romHeader68k };
// Which board model runs it. ez328 and vz328 run in frond-68k, the others in frond. imx1Zodiac is the
// guarded i.MX1 machine behind SocIMX1.
enum class DeviceBoard5 { pxa255E2, pxa270Tx, pxa270Zire72, paravirtual, none, pxa250SonyTG50, ez328, vz328, imx1Zodiac };
// Which button bar and silkscreen art a device gets (native/DeviceControls.h draws them).
enum class DeviceArt { none, tx, e2, zire72, m515, iiix, tg50, zodiac, hpIpaq210, hpIpaqRx1950, hpIpaqH4350,
                       m125, iiie, iiixe, v, vx, m100, m105, m500, c500, iiic, m505, m130, m520, t415 };
// Settings and peripherals a device supports; the desktop and core gate on these.
// backgroundPanel: the device menu keeps the machine running without drawing and fences its audio.
// modem: a snap-on Hayes modem for the serial port (the core's own, through PPP to slirp).
// irPhone: a mobile phone with a data modem on the infrared port (the core's IrDA peer, same PPP).
struct DeviceCapabilities5 {
    bool network, keyboard, serial, usb, fastAudio, infrared, bluetooth; bool sound{false}; bool microphone{false};
    bool backgroundPanel{false};
    bool modem{false};
    bool irPhone{false};
    // usbPrinter: a USB printer leaf on the host port that collects raw print jobs (Dana).
    bool usbPrinter{false};
    // usbWifi: a USB Wi-Fi adapter for the same host port, one device at a time (Dana).
    bool usbWifi{false};
    // builtInWifi: the Dana Wireless's internal 802.11b module (a Prism 2.5 on the ISP1161's second
    // root port, powered by PK0).
    bool builtInWifi{false};
};

// A device's fixed name is what sessions, settings, the control socket and command line use.
// DeviceType5 is internal only and never saved.
enum class DeviceNorLayout5 { any, headerOffset, smallAndBigCards };
enum class DeviceStorageSlot5 { none, sd, memoryStick };
struct DeviceFrontend5 {
    bool available{false};
    const char* setup{""}; // Public setup recipe in tools/palm_boot.py.
    int width{0}, height{0}, silkscreenHeight{0}, stripHeight{0};
    const char* appNames{""}; // Four hard-key labels, separated by |.
    const char* buttons{""}; // Controller button IDs, separated by |.
    int homeX{-1}, homeY{-1}, menuX{-1}, menuY{-1};
    bool rotatingStatusBar{false};
    bool sdSlot{false}, camera{false};
    const char* fastAudioNorMd5{""};
    const char* launcher{""}; // Frame predicate in tools/palm_boot.py.
    DeviceStorageSlot5 storageSlot{DeviceStorageSlot5::none};
    int homeKey{0}; // SDL keycode for a physical Home button.
    bool holdPresent{false}, holdSupported{false};
    int sideTabWidth{0};
    bool memoryStick{false};
};

struct DeviceProfile5 {
    DeviceType5 deviceType;  // deviceTypeInvalid for a name reserved for a device not built yet
    const char* name;
    const char* displayName;
    DeviceCohort5 cohort;
    DeviceSupport5 support;
    DeviceDetect5 detect;
    uint32_t companyId, halId; // Card header IDs for cardHeader; companyId 0 matches any.
    DeviceBoard5 board;
    DeviceArt art;
    DeviceCapabilities5 capabilities;
    size_t norSize;  // Zero means no fixed size established by this profile.
    size_t nandSize; // Raw NAND includes spare bytes; zero means no NAND.
    uint32_t minRam, maxRam; // Powers of two, in bytes.
    DisplayMode defaultDisplay;
    uint16_t displayModes; // Bit n represents persisted DisplayMode n.
    bool requiresExplicitNand;
    DeviceNorLayout5 norLayout{DeviceNorLayout5::any};
    size_t headerOffset{0}, bigCardOffset{0};
    bool legacySessionNor{false};
    uint64_t sdMin{512}, sdMax{UINT64_MAX};
    DeviceFrontend5 frontend{};
    // Classic Memory Stick sizes in MB, ORed together (each is a power of two); 0 for no slot.
    uint32_t msSizes{0};
    // Largest Memory Stick Pro in MiB (256, 512, 1024 or 1886 for "2 GB"); 0 for none.
    uint32_t msProMaxMib{0};
    // Real CPU clock in Hz for the Original speed setting; 0 where the board works it out itself (an ARM
    // device then runs at 100 MIPS). ARM devices run at deviceProfileOriginalMips5 of it.
    uint32_t cpuHz{0};
};

inline constexpr uint32_t deviceCompanyPalm5 = 0x50616c6d;  // 'Palm'
inline constexpr size_t deviceNameMax5 = 31;

// Inventory of native models and reserved names, not a list of verified Palm devices.
inline constexpr DeviceProfile5 deviceProfiles5[] = {
    {deviceTypeTX, "palm-tx", "Palm TX", DeviceCohort5::txNand, DeviceSupport5::txDesktop,
     DeviceDetect5::norSize, 0, 0, DeviceBoard5::pxa270Tx, DeviceArt::tx,
     {true, true, true, true, true, true, true, true, false, false, false, true},
     16u << 20, 2112u * 64 * 1024, 32u << 20, 32u << 20, DisplayMode::mode_320x480, 1u << 6, true,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048u << 20,
     {true, "tx", 320, 480, 0, 84, "Home|Calendar|Contacts|Web",
      "up|down|left|right|centre|app1|app2|app3|app4|menu|power", -1, -1, 54, 466, true, true, false,
      "85b048a692aa317763a7077ea72da6cb", "wallpaper"},
     0, 0, 312000000},
    {deviceTypeE2, "palm-e2", "Tungsten E2", DeviceCohort5::e2Nand, DeviceSupport5::e2Desktop,
     DeviceDetect5::cardHeader, deviceCompanyPalm5, 0x68737072 /* hspr */, DeviceBoard5::pxa255E2,
     DeviceArt::e2, {false, true, true, true, true, true, true, true, false, false, false, true},
     8u << 20, 34603008, 16u << 20, 32u << 20, DisplayMode::mode_320x320, 1u << 5, false,
     DeviceNorLayout5::headerOffset, 0x20000, 0, true, 512, 1024u << 20,
     {true, "e2", 320, 320, 120, 96, "Calendar|Contacts|Tasks|Note Pad",
      "up|down|left|right|centre|app1|app2|app3|app4|home|menu|power", 27, 351, 27, 405, false, true, false,
      "2a9faf69b43ba590cfb9ba78e4eef11c", "wallpaper-upper"},
     0, 0, 200000000},
    // The E2 board with a patched E3-style ROM. The ROM has no Bluetooth stack, and its Fake Net
    // Library links to BtLib, so NetLib can't open: no Bluetooth and no mobile phone.
    {deviceTypeFrankenE2, "palm-e2-franken", "Tungsten E3", DeviceCohort5::patchedE2,
     DeviceSupport5::experimentalDesktop, DeviceDetect5::cardHeader, deviceCompanyPalm5,
     0x446d4752 /* DmGR */, DeviceBoard5::pxa255E2, DeviceArt::e2,
     {false, true, true, true, true, true, false, true, false, false, false, false},
     8u << 20, 34603008, 16u << 20, 32u << 20, DisplayMode::mode_320x480, 1u << 6, false,
     DeviceNorLayout5::headerOffset, 0x20000, 0, true, 512, 1024u << 20,
     {true, "e3", 320, 480, 0, 96, "Calendar|Contacts|Tasks|Note Pad",
      "up|down|left|right|centre|app1|app2|app3|app4|home|menu", 20, 466, 88, 466, false, true, false, "a79f98bddd50c61e7eb89eee46eb81bd", "e3-stock"},
     0, 0, 200000000},
    {deviceTypePV, "repalm", "rePalm paravirtualized", DeviceCohort5::paravirtual,
     DeviceSupport5::inheritedUnverified, DeviceDetect5::cardHeader, 0, 0x50765076 /* PvPv */,
     DeviceBoard5::paravirtual, DeviceArt::none, {},
     0, 0, 8u << 20, 1u << 31, DisplayMode::mode_320x480, 0x1fe, false},
    // XIP NOR with a small and a big card like the E2, on a PXA270; no NAND, Wi-Fi or serial. The camera stays
    // off until it's emulated.
    {deviceTypeZire72, "palm-zire-72", "Zire 72", DeviceCohort5::zire72Nor,
     DeviceSupport5::zire72Desktop, DeviceDetect5::cardHeader, deviceCompanyPalm5,
     0x615a3732 /* aZ72 */, DeviceBoard5::pxa270Zire72, DeviceArt::zire72,
     {false, true, false, true, true, true, true, true, true, false, false, true},
     14942208, 0, 32u << 20, 32u << 20, DisplayMode::mode_320x320, 1u << 5, false,
     DeviceNorLayout5::smallAndBigCards, 0, 0x20000, false, 32u << 20, 2048u << 20,
     {true, "zire", 320, 320, 120, 104, "Calendar|Contacts|Music|Camera",
      "up|down|left|right|centre|app1|app2|app3|app4|home|menu|voice|power", 23, 352, 23, 412, false, true, true,
      "0f3e0bf04ceaa5ad73a1768b7d9ede98", "wallpaper"},
     0, 0, 312000000},
    // PXA250 with a built-in keyboard; unsupported transports stay disabled.
    {deviceTypeTG50, "sony-clie-tg50", "Sony CLIÉ TG50", DeviceCohort5::tg50Nor, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::cardHeader, 0x736f6e79 /* sony */, 0x76726e61 /* vrna */,
     DeviceBoard5::pxa250SonyTG50, DeviceArt::tg50,
     {false, true, true, true, false, true, true, true, true, true, false, true},
     0xc70000, 0, 16u << 20, 16u << 20, DisplayMode::mode_320x320, 1u << 5, false,
     DeviceNorLayout5::smallAndBigCards, 0, 0x20000, false, 0, 0,
     {true, "tg50", 320, 320, 0, 251, "Date Book|Address|To Do|Memo Pad",
      "up|down|app1|app2|app3|app4|home|power", -1, -1, -1, -1, false, false, false, "", "tg50-clie",
      DeviceStorageSlot5::memoryStick, 1073741887, true, true, 38, true},
     4 | 8 | 16 | 32 | 64 | 128, 1886, 200000000},
    // DragonBall EZ with 4 MiB RAM and a 2 MiB flash part, run by frond-68k. Takes the IIIx 3.1 ROMs, the
    // generic OS 3.5 EZ ROMs and the IIIx/V 4.1 ROM. Piezo, serial, IrDA and the snap-on Palm III modem; nothing else
    // is on the device.
    // The desktop and Android run it in their 68k runners (frond-68k, libmain68k.so).
    {deviceTypeIIIx, "palm-iiix", "Palm IIIx", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x73756d6f /* sumo */, DeviceBoard5::ez328, DeviceArt::iiix,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 4u << 20, 4u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4|power", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 16580608},
    // Palm IIIe: the IIIx board with 2 MiB of RAM and no flash. It has no ROM of its own; it runs the IIIx 3.1 ROM and
    // is picked by choosing the device, as the ROM can only say IIIx.
    {deviceTypeIIIe, "palm-iiie", "Palm IIIe", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0, 0, DeviceBoard5::ez328, DeviceArt::iiie,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 2u << 20, 2u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4|power", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 16580608},
    // Palm IIIxe: the IIIx board with 8 MiB of RAM. Its ROM header has no HAL ID, so it is picked by choosing the device.
    {deviceTypeIIIxe, "palm-iiixe", "Palm IIIxe", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0, 0, DeviceBoard5::ez328, DeviceArt::iiixe,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4|power", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 16580608},
    // Palm V: 2 MiB of RAM, the IIIx peripherals (the snap-on modem is the Palm V one) and no power key in the bar.
    {deviceTypeV, "palm-v", "Palm V", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0, 0, DeviceBoard5::ez328, DeviceArt::v,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 2u << 20, 2u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 48, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 16580608},
    // Palm Vx: the V with 8 MiB of RAM.
    {deviceTypeVx, "palm-vx", "Palm Vx", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0, 0, DeviceBoard5::ez328, DeviceArt::vx,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 48, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 20250624},  // the Vx ROMs set the PLL to 20.25 MHz
    // Palm m100: 2 MiB of RAM, serial cradle and infrared. The ROM is told by its clock database (no HAL ID in the header).
    {deviceTypeM100, "palm-m100", "Palm m100", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0, 0, DeviceBoard5::ez328, DeviceArt::m100,
     {false, false, true, false, false, true, false, true, false, false, false, true},
     0, 0, 2u << 20, 2u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 16580608},
    // Palm m105: the m100 with 8 MiB of RAM. Its ROM looks like the m100's, so it is picked by choosing the device.
    {deviceTypeM105, "palm-m105", "Palm m105", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0, 0, DeviceBoard5::ez328, DeviceArt::m105,
     {false, false, true, false, false, true, false, true, false, false, false, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 16580608},
    // Palm m125: DragonBall VZ with 8 MiB of RAM, one SD/MMC slot, serial and USB HotSync on the Universal
    // Connector (USB isn't emulated yet) and infrared. Power is on the top edge, outside the bar.
    {deviceTypeM125, "palm-m125", "Palm m125", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x76737475 /* vstu */, DeviceBoard5::vz328, DeviceArt::m125,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix"},
     0, 0, 33161216},
    // Palm m500: the m125's board with the m515's keys, 8 MiB of RAM and the same slot and connector.
    {deviceTypeM500, "palm-m500", "Palm m500", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x7674726e /* vtrn */, DeviceBoard5::vz328, DeviceArt::m500,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix"},
     0, 0, 33161216},
    // IBM WorkPad c500: an m500 sold by IBM. Its ROM has the m500's HAL ID, so it is picked by choosing the device.
    {deviceTypeC500, "ibm-workpad-c500", "IBM WorkPad c500", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::none, 0x70616c6d /* palm */, 0x7674726e /* vtrn */, DeviceBoard5::vz328, DeviceArt::c500,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix"},
     0, 0, 33161216},
    // Palm IIIc: the EZ board with 8 MiB of RAM and a colour STN panel on Epson's SED1375 controller, picked by
    // its HAL ID. Serial cradle, IrDA, piezo and the snap-on modem like the IIIx.
    {deviceTypeIIIc, "palm-iiic", "Palm IIIc", DeviceCohort5::ez68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x6173746e /* astn */, DeviceBoard5::ez328, DeviceArt::iiic,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Memo Pad",
      "up|down|app1|app2|app3|app4|power", -1, -1, -1, -1, false, false, false, "", "iiix"},
     0, 0, 20250624},  // the IIIc ROM sets the PLL to 20.25 MHz
    // Palm m505: the m500 with a colour reflective TFT on Epson's SED1376 controller, 8 MiB of RAM.
    {deviceTypeM505, "palm-m505", "Palm m505", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x65637479 /* ecty */, DeviceBoard5::vz328, DeviceArt::m505,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix"},
     0, 0, 33161216},
    // Palm m515: the m505 with a brighter colour TFT and 16 MiB of RAM.
    {deviceTypeM515, "palm-m515", "Palm m515", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x766c6974 /* vlit */, DeviceBoard5::vz328, DeviceArt::m515,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 16u << 20, 16u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix"},
     0, 0, 33161216},
    // Palm m130: the m125's keys with a colour STN panel on a MediaQ MQ1100, 8 MiB of RAM and an SD/MMC slot.
    {deviceTypeM130, "palm-m130", "Palm m130", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x76686273 /* vhbs */, DeviceBoard5::vz328, DeviceArt::m130,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_160x160, 1u << 1, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix", 160, 160, 60, 44, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix"},
     0, 0, 33161216},
    // Palm m520: an unreleased m515 with a 320x320 colour panel on a MediaQ MQ1100 and 16 MiB of RAM.
    {deviceTypeM520, "palm-m520", "Palm m520", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x70616c6d /* palm */, 0x7366676f /* sfgo */, DeviceBoard5::vz328, DeviceArt::m520,
     {false, false, true, false, false, true, false, true, false, false, true, true},
     0, 0, 16u << 20, 16u << 20, DisplayMode::mode_320x320, 1u << 5, false,
     DeviceNorLayout5::any, 0, 0, false, 32u << 20, 2048ull << 20,
     {true, "iiix-hires", 320, 320, 120, 88, "Date Book|Address|To Do List|Note Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, true, false, "", "iiix-hires"},
     0, 0, 33161216},
    // Dana uses its built-in keyboard and two independent SD/MMC slots.
    {deviceTypeDana, "alphasmart-dana", "AlphaSmart Dana", DeviceCohort5::vz68k, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::romHeader68k, 0x61736d74 /* asmt */, 0x44616e61 /* Dana */,
     DeviceBoard5::vz328, DeviceArt::none, {false, true, false, false, false, true, false, true, false, false, false, false, true, true},
     6u << 20, 0, 8u << 20, 8u << 20, DisplayMode::mode_560x160, 1u << 9, false,
     DeviceNorLayout5::any, 0, 0x8000, false, 32u << 20, 2048ull << 20,
     {true, "dana", 560, 160, 0, 0, "", "", -1, -1, -1, -1, false, true, false, "", "dana"}},
    // The Dana board with 16 MiB and built-in Wi-Fi, on the same ROM: the core makes the ROM's serial
    // number token read as a Wireless one. The USB host port takes the printer only, as the module
    // already gives it Wi-Fi.
    {deviceTypeDanaWireless, "alphasmart-dana-wireless", "AlphaSmart Dana Wireless", DeviceCohort5::vz68k,
     DeviceSupport5::experimentalDesktop, DeviceDetect5::none, 0x61736d74 /* asmt */, 0x44616e61 /* Dana */,
     DeviceBoard5::vz328, DeviceArt::none, {false, true, false, false, false, true, false, true, false, false, false, false, true, false, true},
     6u << 20, 0, 16u << 20, 16u << 20, DisplayMode::mode_560x160, 1u << 9, false,
     DeviceNorLayout5::any, 0, 0x8000, false, 32u << 20, 2048ull << 20,
     {true, "dana", 560, 160, 0, 0, "", "", -1, -1, -1, -1, false, true, false, "", "dana"}},
    // The guarded i.MX1 machine behind SocIMX1: screen, pen, buttons, stick, an SD card in slot 1, up
    // to 2 GB as the OS has no SDHC, and PCM sound. The other peripherals (slot 2, IrDA, Bluetooth, USB,
    // MIDI, vibration) aren't finished, so their capabilities stay off.
    {deviceTypeZodiac2, "tapwave-zodiac-2", "Tapwave Zodiac 2", DeviceCohort5::zodiacNor, DeviceSupport5::experimentalDesktop,
     DeviceDetect5::cardHeader, 0x54707776 /* Tpwv */, 0x4d583161 /* MX1a */,
     DeviceBoard5::imx1Zodiac, DeviceArt::zodiac, {false, false, false, false, false, false, false, true},
     0x7d0000, 0, 128u << 20, 128u << 20, DisplayMode::mode_480x320, 1u << 10, false,
     DeviceNorLayout5::smallAndBigCards, 0, 0x20000, false, 32u << 20, 2048ull << 20,
     {true, "", 480, 320, 0, 200, "", "app1|app2|app3|app4|home|power", -1, -1, -1, -1, false, true}},

    // Sony CLIE T415: DragonBall VZ with 8 MiB of RAM, a 320x320 16-grey panel and the silkscreen below it,
    // IrDA and a USB cradle (USB isn't emulated yet). Staged: not in the frontends yet.
    {deviceTypeT415, "sony-clie-t415", "Sony CLIÉ T415", DeviceCohort5::vz68k, DeviceSupport5::staged,
     DeviceDetect5::romHeader68k, 0x736f6e79 /* sony */, 0x766e6365 /* vnce */, DeviceBoard5::vz328, DeviceArt::t415,
     {false, false, false, false, false, true, false, true, false, false, false, true},
     0, 0, 8u << 20, 8u << 20, DisplayMode::mode_320x320, 1u << 5, false,
     DeviceNorLayout5::any, 0, 0, false, 0, 0,
     {false, "", 320, 320, 128, 104, "Date Book|Address|To Do|Memo Pad",
      "up|down|app1|app2|app3|app4", -1, -1, -1, -1, false, false, false, "", "", DeviceStorageSlot5::none, 0,
      false, false, 38},
     0, 0, 33161216},
    // Windows Mobile iPAQs: button bar art only, no Windows CE core yet.
    {deviceTypeInvalid, "hp-ipaq-210", "HP iPAQ 210", DeviceCohort5::reserved, DeviceSupport5::reserved,
     DeviceDetect5::none, 0, 0, DeviceBoard5::none, DeviceArt::hpIpaq210, {},
     0, 0, 0, 0, DisplayMode::mode_480x640, 0, false},
    {deviceTypeInvalid, "hp-ipaq-rx1950", "HP iPAQ rx1950", DeviceCohort5::reserved, DeviceSupport5::reserved,
     DeviceDetect5::none, 0, 0, DeviceBoard5::none, DeviceArt::hpIpaqRx1950, {},
     0, 0, 0, 0, DisplayMode::mode_240x320, 0, false},
    {deviceTypeInvalid, "hp-ipaq-h4350", "HP iPAQ h4350", DeviceCohort5::reserved, DeviceSupport5::reserved,
     DeviceDetect5::none, 0, 0, DeviceBoard5::none, DeviceArt::hpIpaqH4350, {},
     0, 0, 0, 0, DisplayMode::mode_240x320, 0, false},
};

// Firmware variants keep their board's device and session identity.
struct DeviceFirmwareVariant5 {
    DeviceType5 deviceType;
    const char* id;
    size_t norSize, headerOffset;
    const char* cardName;
    const char* manufacturer;
    uint32_t companyId, halId;
    bool cardEndsAtEof;
    const char* fastAudioNorMd5;
    uint32_t fastAudioArmStartEntry{0}, fastAudioArmStartCall{0}, fastAudioArmStartBranch{0};
    bool disableSleep{false};
};
inline constexpr DeviceFirmwareVariant5 deviceFirmwareVariants5[] = {
    {deviceTypeE2, "e2-denvfs", 6u << 20, 0x20000, "rePalm", "Dmitry.GR",
     deviceCompanyPalm5, 0x68737072 /* hspr */, true, "c2d90f775b1737de5bcd11634bf51d7d",
     0x205bc180, 0x205bc200, 0xeb00543b, true},
};

// The caller supplies the variant parsed from this NOR's identity and geometry.
inline bool deviceProfileFastAudioQualified5(const DeviceProfile5& profile, const char* digest,
    const DeviceFirmwareVariant5* variant = nullptr) {
    if (!profile.capabilities.fastAudio || !digest || !*digest) return false;
    auto matches = [&](const char* expected) {
        if (!expected || !*expected) return false;
        const char* actual = digest;
        while (*expected && *expected == *actual) ++expected, ++actual;
        return !*expected && !*actual;
    };
    return matches(profile.frontend.fastAudioNorMd5) ||
        (variant && variant->deviceType == profile.deviceType && matches(variant->fastAudioNorMd5));
}

// 68k ROMs: a big-endian card header at 0 with the reset vector inside the ROM at its base address.
inline constexpr uint32_t rom68kEzBase5 = 0x10c00000;
inline constexpr uint64_t rom68kMaxSize5 = 2u << 20; // The IIIx's flash part.
// VZ ROMs run from 0x10000000; the m5xx series has the largest, 4 MiB.
inline constexpr uint32_t rom68kVzBase5 = 0x10000000;
inline constexpr uint64_t rom68kVzMaxSize5 = 4u << 20;

// 68k devices with a colour panel, whose controllers give Cloudpilot's 24-bit frames.
inline bool deviceProfileColourLcd5(int32_t deviceType) {
    switch (deviceType) {
        case deviceTypeIIIc: case deviceTypeM505: case deviceTypeM515: case deviceTypeM130: case deviceTypeM520: return true;
        default: return false;
    }
}

// The Dana board: the Dana and the Dana Wireless.
inline bool deviceTypeIsDana5(int32_t deviceType) {
    return deviceType == deviceTypeDana || deviceType == deviceTypeDanaWireless;
}

// Built devices only; reserved names have no internal type.
inline const DeviceProfile5* deviceProfile5(int32_t deviceType) {
    if (deviceType < 0) return nullptr;
    for (const auto& profile : deviceProfiles5)
        if (int32_t(profile.deviceType) == deviceType) return &profile;
    return nullptr;
}

// Shared board hardware, including variants with their own ROM patches and display geometry.
inline bool deviceProfileIsE2Board5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && profile->board == DeviceBoard5::pxa255E2;
}

// The cradle or connector has a separate HotSync button on these boards.
inline bool deviceProfileHasHotSyncButton5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && (profile->board == DeviceBoard5::pxa255E2 ||
                       profile->board == DeviceBoard5::pxa270Tx || profile->board == DeviceBoard5::ez328 ||
                       (profile->board == DeviceBoard5::vz328 && !deviceTypeIsDana5(deviceType)));
}

// A connector the device has that Frond doesn't run yet, named as the frontends grey it out; empty for none.
// The Dana's USB computer port and the VZ Palms' USB HotSync wait on their USB device controllers.
inline const char* deviceProfilePendingUsb5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    if (!profile || profile->board != DeviceBoard5::vz328) return "";
    return deviceTypeIsDana5(deviceType) ? "USB computer" : "USB HotSync";
}

// Any table entry, reserved names included; nullptr for an unknown name.
inline const DeviceProfile5* deviceProfileNamed5(const char* name) {
    if (!name) return nullptr;
    for (const auto& profile : deviceProfiles5) {
        const char *a = profile.name, *b = name;
        while (*a && *a == *b) ++a, ++b;
        if (!*a && !*b) return &profile;
    }
    return nullptr;
}

// A reader returns false for a short read. Header identity is kept when its layout is invalid.
struct DeviceNorInfo5 {
    const DeviceProfile5* profile{nullptr};
    bool hasHeader{false}, validLayout{true};
    const DeviceFirmwareVariant5* variant{nullptr};
};
template <typename Reader>
inline DeviceNorInfo5 deviceProfileInspectNor5(uint64_t size, Reader read) {
    auto word = [&](uint64_t at, uint32_t& value) {
        uint8_t b[4];
        if (at > size || size - at < 4 || !read(at, b, 4)) return false;
        value = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        return true;
    };
    auto text = [&](uint64_t at, const char* expected) {
        // Card names and manufacturers occupy separate 32-byte, nul-terminated fields.
        uint8_t b[32];
        if (at > size || size - at < sizeof(b) || !read(at, b, sizeof(b))) return false;
        for (unsigned i = 0; i < sizeof(b); ++i) {
            if (b[i] != uint8_t(expected[i])) return false;
            if (!expected[i]) return true;
        }
        return false;
    };
    auto cardName = [&](uint64_t at) {
        uint8_t b[9];
        const char name[] = "PalmCard";
        if (at > size || size - at < 0x19 || !read(at + 0x10, b, 9)) return false;
        for (unsigned i = 0; i < 9; ++i) if (b[i] != uint8_t(name[i])) return false;
        return true;
    };
    for (uint64_t at = 0; at + 12 <= size; at += 32768) {
        uint32_t signature, company, hal;
        if (!word(at + 8, signature) || signature != 0xfeedbeef) continue;
        if (!word(at + 0x80, company) || !word(at + 0x84, hal)) return {};
        for (const auto& p : deviceProfiles5) {
            if (p.detect != DeviceDetect5::cardHeader || p.halId != hal || (p.companyId && p.companyId != company)) continue;
            bool valid = p.norLayout == DeviceNorLayout5::any || at == p.headerOffset;
            if (p.norLayout == DeviceNorLayout5::smallAndBigCards) {
                const uint64_t big = p.bigCardOffset;
                uint32_t bigSignature, bigCompany, bigHal, length;
                valid = valid && cardName(at) && cardName(big) && word(big + 8, bigSignature) && bigSignature == signature &&
                    word(big + 0x80, bigCompany) && bigCompany == company && word(big + 0x84, bigHal) && bigHal == hal &&
                    word(big + 0x6c, length) && big + length == size;
            }
            const DeviceFirmwareVariant5* variant = nullptr;
            for (const auto& v : deviceFirmwareVariants5) {
                uint32_t length;
                if (v.deviceType == p.deviceType && v.norSize == size && v.headerOffset == at &&
                    v.companyId == company && v.halId == hal && text(at + 0x10, v.cardName) &&
                    text(at + 0x30, v.manufacturer) &&
                    (!v.cardEndsAtEof || (word(at + 0x6c, length) && at + length == size))) {
                    variant = &v;
                    break;
                }
            }
            return {&p, true, valid, variant};
        }
        return {nullptr, true, true};
    }
    return {};
}

// Reads only the card header of a 68k ROM, so a ROM it accepts can still be one the board can't run;
// frond-68k checks that properly. Header versions 1 and 2 (68328 ROMs) and 68328-only ROMs are refused.
// Version 5 headers carry company and HAL IDs, which must match the profile's; older ones have none.
struct Device68kRomInfo5 {
    const DeviceProfile5* profile{nullptr};
    bool hasHeader{false};
    uint16_t headerVersion{0};
    uint32_t companyId{0}, halId{0};
};
// Whether a text (without its nul) is in the file, read in 64 KiB pieces.
template <typename Reader>
inline bool rom68kHasText5(uint64_t size, Reader read, const char* text) {
    const size_t length = std::strlen(text);
    std::vector<uint8_t> buffer(0x10000 + length);
    for (uint64_t at = 0; at < size; at += 0x10000) {
        const size_t count = size_t(std::min<uint64_t>(buffer.size(), size - at));
        if (!read(at, buffer.data(), count)) return false;
        for (size_t i = 0; i + length <= count; ++i)
            if (std::memcmp(&buffer[i], text, length) == 0) return true;
    }
    return false;
}

template <typename Reader>
inline Device68kRomInfo5 deviceProfileInspect68kRom5(uint64_t size, Reader read) {
    uint8_t h[0x100];
    if (size < sizeof(h) || !read(0, h, sizeof(h))) return {};
    auto be16 = [&](unsigned at) { return uint16_t((h[at] << 8) | h[at + 1]); };
    auto be32 = [&](unsigned at) {
        return uint32_t(h[at]) << 24 | uint32_t(h[at + 1]) << 16 | uint32_t(h[at + 2]) << 8 | h[at + 3];
    };
    const char name[] = "PalmCard";
    for (unsigned i = 0; i < sizeof(name); ++i) if (h[0x10 + i] != uint8_t(name[i])) return {};
    if (be32(8) != 0xfeedbeef) return {};
    Device68kRomInfo5 info;
    info.hasHeader = true;
    info.headerVersion = be16(0x0c);
    const uint16_t flags = be16(0x0e);
    const uint32_t reset = be32(4), cardBytes = be32(0x6c);
    if (info.headerVersion >= 5) info.companyId = be32(0x7e), info.halId = be32(0x82);
    const auto* dana = deviceProfileNamed5("alphasmart-dana");
    if (info.headerVersion == 5 && info.companyId == dana->companyId && info.halId == dana->halId) {
        // Dana has a VZ small card and a second card at 32 KiB, with padding after it.
        if (!(flags & 0x0040) || (flags & 0x0001) || size != dana->norSize ||
            cardBytes < sizeof(h) || cardBytes > 0x8000 ||
            reset < 0x10000000 || reset - 0x10000000 >= cardBytes)
            return info;
        uint8_t big[0x100];
        if (!read(0x8000, big, sizeof(big))) return info;
        auto word = [&](unsigned at) {
            return uint32_t(big[at]) << 24 | uint32_t(big[at + 1]) << 16 |
                   uint32_t(big[at + 2]) << 8 | big[at + 3];
        };
        for (unsigned i = 0; i < sizeof(name); ++i) if (big[0x10 + i] != uint8_t(name[i])) return info;
        const auto bigReset = word(4), length = word(0x6c);
        if (word(8) != 0xfeedbeef || big[0x0c] || big[0x0d] != 5 ||
            !(big[0x0f] & 0x40) || (big[0x0f] & 1) || word(0x7e) != info.companyId ||
            word(0x82) != info.halId || length < sizeof(big) || length > size - 0x8000 ||
            bigReset < 0x10008000 || bigReset - 0x10008000 >= length) return info;
        info.profile = dana;
        return info;
    }
    // Card flags: 0x0001 RAM only, 0x0020 runs on the 68EZ328, 0x0040 on the 68VZ328. A VZ ROM has a version 5
    // header whose company and HAL IDs name the device.
    if (info.headerVersion == 5 && (flags & 0x0040) && !(flags & 0x0021)) {
        if (size > rom68kVzMaxSize5 || size % 0x1000 || cardBytes > size) return info;
        if (reset < rom68kVzBase5 || reset - rom68kVzBase5 >= size) return info;
        // A ROM split into a small card and a big one (m515, m130, m520) names its device in the big card's
        // header, as Cloudpilot reads it; the m520's small card carries the m505's IDs.
        const uint32_t big = be32(0x68);
        uint8_t b[0x100];
        if (big >= rom68kVzBase5 && big - rom68kVzBase5 >= cardBytes && big - rom68kVzBase5 <= size - sizeof(b) &&
            read(big - rom68kVzBase5, b, sizeof(b))) {
            auto bigBe32 = [&](unsigned at) {
                return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
            };
            bool named = bigBe32(8) == 0xfeedbeef && ((b[0x0c] << 8) | b[0x0d]) == 5;
            for (unsigned i = 0; named && i < sizeof(name); ++i) named = b[0x10 + i] == uint8_t(name[i]);
            if (named) info.companyId = bigBe32(0x7e), info.halId = bigBe32(0x82);
        }
        for (const auto& p : deviceProfiles5) {
            if (p.detect != DeviceDetect5::romHeader68k || p.board != DeviceBoard5::vz328 ||
                deviceTypeIsDana5(p.deviceType) || info.companyId != p.companyId || info.halId != p.halId) continue;
            info.profile = &p;
            return info;
        }
        return info;
    }
    if (info.headerVersion < 3 || info.headerVersion > 5 || (flags & 0x0001) || !(flags & 0x0020)) return info;
    if (size > rom68kMaxSize5 || size % 0x1000 || cardBytes > size) return info;
    if (reset < rom68kEzBase5 || reset - rom68kEzBase5 >= size) return info;
    // The m100 and m105 ROMs have a version 4 header with no HAL ID. Cloudpilot tells their family by the
    // clock database only the m100 series carries, and the two only by the device chosen (2 or 8 MiB).
    // The family is the m100 until the user chooses the m105.
    if (info.headerVersion < 5 && rom68kHasText5(size, read, "cclkPalmClock")) {
        info.profile = deviceProfileNamed5("palm-m100");
        return info;
    }
    for (const auto& p : deviceProfiles5) {
        if (p.detect != DeviceDetect5::romHeader68k || p.board != DeviceBoard5::ez328) continue;
        if (info.headerVersion >= 5 && (info.companyId != p.companyId || info.halId != p.halId)) continue;
        info.profile = &p;
        return info;
    }
    return info;
}

// Whether a chosen 68k device can take a ROM whose header named info.profile. The IIIx, IIIxe, V, Vx, IIIe,
// m100 and m105 share the EZ board and ROM format, and the header can't tell all of them apart (the IIIe has
// no ROM of its own), so any of them may be chosen for their ROM and frond-68k's full check decides; the IIIc
// takes only its own ROM. The Dana
// Wireless takes the Dana's ROM, and a VZ Palm takes a ROM with its company and HAL IDs (the WorkPad c500 the
// m500's).
inline bool deviceProfileTakes68kRom5(const DeviceProfile5& chosen, const Device68kRomInfo5& info) {
    if (!info.profile) return false;
    if (info.profile == &chosen) return true;
    if (chosen.deviceType == deviceTypeDanaWireless) return info.profile->deviceType == deviceTypeDana;
    if (chosen.board == DeviceBoard5::vz328)
        return !deviceTypeIsDana5(chosen.deviceType) && info.profile->board == DeviceBoard5::vz328 &&
               chosen.companyId == info.profile->companyId && chosen.halId == info.profile->halId;
    // The IIIc's ROM (its own HAL ID) goes to the IIIc only and the IIIc takes no other; the rest of the EZ
    // devices share the IIIx family's ROMs.
    if (chosen.deviceType == deviceTypeIIIc || info.profile->deviceType == deviceTypeIIIc) return false;
    return chosen.board == DeviceBoard5::ez328 && info.profile->board == DeviceBoard5::ez328;
}

// The internal type for a name Frond can run, or deviceTypeInvalid.
inline DeviceType5 deviceTypeNamed5(const char* name) {
    const auto* profile = deviceProfileNamed5(name);
    return profile ? profile->deviceType : deviceTypeInvalid;
}

// The fixed name of a built device, or nullptr.
inline const char* deviceName5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile ? profile->name : nullptr;
}

inline DeviceArt deviceArtFor5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile ? profile->art : DeviceArt::none;
}

// Whether frond (the ARM runner) can start it. Native qualification can precede desktop/Android
// acceptance. Art reservations and inventory-only profiles have no board and cannot enter a runner.
inline bool deviceProfileRunnerEligible5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && profile->board != DeviceBoard5::none && profile->board != DeviceBoard5::ez328 && profile->board != DeviceBoard5::vz328;
}

// Devices frond-68k runs instead of frond.
inline bool deviceProfileRuns68k5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && (profile->board == DeviceBoard5::ez328 || profile->board == DeviceBoard5::vz328);
}

// Guest MIPS an ARM device runs at in Original speed: 150 on the 312 MHz PXA270 (set against ZQuake on a
// T|X), scaled by CPU clock for the others. The 200 MHz PXA25x figure (96) is an estimate, not measured.
inline uint32_t deviceProfileOriginalMips5(int32_t deviceType) {
    if (deviceProfileRuns68k5(deviceType)) return 100;
    const auto* profile = deviceProfile5(deviceType);
    if (!profile || !profile->cpuHz) return 100;
    return uint32_t((uint64_t(150) * profile->cpuHz + 156000000) / 312000000);
}

inline DeviceStorageSlot5 deviceProfileStorageSlot5(const DeviceProfile5& profile) {
    return profile.frontend.sdSlot ? DeviceStorageSlot5::sd : profile.frontend.storageSlot;
}

// Physical IDs match EmHAL::Slot and ExternalStorage; labels are one-based.
enum class DeviceCardSlot5 : int { sd0 = 0, memoryStick = 1, sd2 = 2 };
enum class DeviceCardFamily5 { sdsc, legacyMmc, memoryStick, memoryStickPro };
struct DeviceCardSlotInfo5 {
    DeviceCardSlot5 id;
    unsigned label;
    DeviceStorageSlot5 kind;
};
inline unsigned deviceProfileCardSlotCount5(const DeviceProfile5& p) {
    return deviceTypeIsDana5(p.deviceType) ? 2 :
        deviceProfileStorageSlot5(p) == DeviceStorageSlot5::none ? 0 : 1;
}
inline bool deviceProfileCardSlot5(const DeviceProfile5& p, unsigned index, DeviceCardSlotInfo5& out) {
    if (index >= deviceProfileCardSlotCount5(p)) return false;
    const auto kind = deviceProfileStorageSlot5(p);
    out = {kind == DeviceStorageSlot5::memoryStick ? DeviceCardSlot5::memoryStick :
           index ? DeviceCardSlot5::sd2 : DeviceCardSlot5::sd0, index + 1, kind};
    return true;
}
struct DeviceCardRule5 {
    uint64_t minBytes{}, maxBytes{};
    const uint64_t* sizes{};
    unsigned sizeCount{};
    bool legacyCsd{};
    const uint64_t* guestSizes{};
    unsigned guestSizeCount{};
};
inline constexpr uint64_t danaMmcSizes5[] = {32ull<<20, 64ull<<20, 128ull<<20, 256ull<<20};
inline constexpr uint64_t danaGuestCardSizes5[] = {32ull<<20};
// Sizes the m125, m500 and c500 have been seen to mount.
inline constexpr uint64_t vzPalmGuestCardSizes5[] = {32ull<<20};
// 68k devices whose slots take cards in frond-68k's linked card files: Cloudpilot's SPI SD/MMC card on the VZ.
inline bool deviceProfileLinkedCards5(const DeviceProfile5& p) {
    return p.board == DeviceBoard5::vz328 && deviceProfileStorageSlot5(p) == DeviceStorageSlot5::sd;
}
inline bool deviceProfileCardRule5(const DeviceProfile5& p, DeviceCardSlot5 slot,
                                  DeviceCardFamily5 family, DeviceCardRule5& rule) {
    rule = {};
    bool found = false;
    DeviceCardSlotInfo5 info{};
    for (unsigned n=0; deviceProfileCardSlot5(p,n,info); ++n)
        if (info.id == slot) { found = true; break; }
    if (!found) return false;
    if (info.kind == DeviceStorageSlot5::sd && family == DeviceCardFamily5::sdsc) {
        rule = {p.sdMin, p.sdMax};
    } else if (deviceProfileLinkedCards5(p) && family == DeviceCardFamily5::legacyMmc) {
        rule = {danaMmcSizes5[0], danaMmcSizes5[3], danaMmcSizes5, 4};
    } else return false;
    if (deviceProfileLinkedCards5(p)) {
        rule.legacyCsd = true;
        if (deviceTypeIsDana5(p.deviceType)) rule.guestSizes = danaGuestCardSizes5, rule.guestSizeCount = 1;
        else rule.guestSizes = vzPalmGuestCardSizes5, rule.guestSizeCount = sizeof(vzPalmGuestCardSizes5) / sizeof(vzPalmGuestCardSizes5[0]);
    }
    return true;
}
// Native media validation. Guest-qualified sizes are evidence, not runtime limits.
inline bool deviceProfileCardSizeValid5(int32_t type, DeviceCardSlot5 slot,
                                       DeviceCardFamily5 family, uint64_t bytes) {
    const auto* p = deviceProfile5(type);
    DeviceCardRule5 rule;
    if (!p || !deviceProfileCardRule5(*p,slot,family,rule) || !bytes || bytes%512 ||
        bytes < rule.minBytes || bytes > rule.maxBytes) return false;
    if (rule.sizeCount) {
        bool found = false;
        for (unsigned i=0;i<rule.sizeCount;++i) found |= bytes == rule.sizes[i];
        if (!found) return false;
    }
    uint32_t mult, size;
    return !rule.legacyCsd || legacyCardLayout(bytes/512,mult,size);
}

// Raw SD image sizes the CLI takes. Bounds are the largest standard-capacity card each device is
// believed to read; the E2 stays at 1 GiB, and its minimum stays 512 bytes so old sessions load.
inline bool deviceProfileSdSizeValid5(int32_t deviceType, uint64_t bytes) {
    if (!bytes || bytes % 512) return false;
    const auto* profile = deviceProfile5(deviceType);
    if (profile && deviceProfileLinkedCards5(*profile))
        return deviceProfileCardSizeValid5(deviceType,DeviceCardSlot5::sd0,DeviceCardFamily5::sdsc,bytes);
    return !profile || (bytes >= profile->sdMin && bytes <= profile->sdMax);
}

// Classic Memory Stick sizes the device's driver takes, in MB.
inline bool deviceProfileMsSizeValid5(int32_t deviceType, uint64_t megabytes) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && megabytes && megabytes <= 128 && !(megabytes & (megabytes - 1)) &&
           (profile->msSizes & megabytes);
}

// Memory Stick Pro sizes the device takes, in MiB, up to its largest.
inline bool deviceProfileMsProSizeValid5(int32_t deviceType, uint64_t mib) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && mib >= 256 && mib <= profile->msProMaxMib;
}

inline bool deviceProfileSupportsDesktop5(int32_t deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && profile->frontend.available;
}

inline bool deviceProfileSupportsRam5(int32_t deviceType, uint32_t bytes) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && bytes >= profile->minRam && bytes <= profile->maxRam &&
           bytes != 0 && (bytes & (bytes - 1)) == 0;
}

inline bool deviceProfileSupportsDisplay5(int32_t deviceType, int32_t mode) {
    const auto* profile = deviceProfile5(deviceType);
    return profile && mode >= 1 && mode <= 11 && (profile->displayModes & (1u << mode));
}

enum class DeviceFirmwareError5 { none, unknownDevice, norSize, explicitNand, nandSize, norLayout };

// E2-board NOR: 128 KiB boot block, then the PalmCard header that maps the 8 MiB part.
// Fresh firmware only: sessions from earlier builds may use another layout.
inline constexpr size_t e2NorHeaderOffset5 = 0x20000;

// Zire 72 NOR: a small card at 0x0 and a big card at 0x20000 running to the end of the part.
// smallAndBigCards says the two cards were found and agree (RomInfo5::HasSmallAndBigCards).
inline DeviceFirmwareError5 deviceProfileValidateNorLayout5(int32_t deviceType, size_t headerOffset,
                                                            bool fromSession, bool smallAndBigCards = false) {
    const auto* profile = deviceProfile5(deviceType);
    if (!profile) return DeviceFirmwareError5::unknownDevice;
    if (fromSession && profile->legacySessionNor) return DeviceFirmwareError5::none;
    if (profile->norLayout != DeviceNorLayout5::any && headerOffset != profile->headerOffset)
        return DeviceFirmwareError5::norLayout;
    if (profile->norLayout == DeviceNorLayout5::smallAndBigCards && !smallAndBigCards)
        return DeviceFirmwareError5::norLayout;
    return DeviceFirmwareError5::none;
}

// Bounds for readers that hash a NOR before matching its firmware fingerprint.
inline bool deviceProfileNorSizeSupported5(int32_t deviceType, uint64_t bytes) {
    const auto* profile = deviceProfile5(deviceType);
    if (!profile || !bytes) return false;
    if (!profile->norSize || bytes == profile->norSize) return true;
    for (const auto& v : deviceFirmwareVariants5)
        if (int32_t(v.deviceType) == deviceType && v.norSize == bytes) return true;
    return false;
}

// Geometry checks cannot identify a dump's device or establish boot compatibility.
inline DeviceFirmwareError5 deviceProfileValidateFirmware5(
    int32_t deviceType, size_t norSize, size_t nandSize, bool fromSession, bool explicitNand,
    const DeviceFirmwareVariant5* variant = nullptr) {
    const auto* profile = deviceProfile5(deviceType);
    if (!profile) return DeviceFirmwareError5::unknownDevice;
    // Legacy sessions saved before size checks may hold another NOR size; they still load.
    const bool norSizeChecked = profile->norSize && !(fromSession && profile->legacySessionNor);
    bool variantSize = false;
    for (const auto& v : deviceFirmwareVariants5)
        if (&v == variant && int32_t(v.deviceType) == deviceType && v.norSize == norSize) variantSize = true;
    if (norSize == 0 || (norSizeChecked && norSize != profile->norSize && !variantSize))
        return DeviceFirmwareError5::norSize;
    if (profile->requiresExplicitNand && !fromSession && !explicitNand)
        return DeviceFirmwareError5::explicitNand;
    if (nandSize != profile->nandSize) return DeviceFirmwareError5::nandSize;
    return DeviceFirmwareError5::none;
}

#endif
