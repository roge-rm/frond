#include "PortablePublication.h"
#include "EmBankRegs.h"
#include "UsbDanaPort.h"
#include "UsbPrinterRuntime.h"
#include "UsbWifiRuntime.h"
#include "dana_wireless5.h"
#include "UsbHostBridge.h"
#include <sys/syscall.h>
// frond-68k: the runner for DragonBall (68k) devices, built on Cloudpilot's core. It checks a ROM,
// runs a ROM or saved session in a window with the LCD, silkscreen and button bar, or headless for
// a set number of guest seconds, and takes console commands on stdin, or the desktop's commands on
// its control socket.

#include <SDL.h>
#include <SDL_image.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "../../uarm/native/DeviceControls.h"
#include "../../uarm/native/ShortcutKeys.h"
#include "../../uarm/native/DanaHostKeys.h"
#include "../../uarm/native/WindowId.h"
#include "../../uarm/native/WindowLayout.h"
#include "../../uarm/native/DanaView.h"
#include "ButtonEvent.h"
#include "PeripheralGuards.h"
#include "Chars.h"
#include "DbBackup.h"
#include "DbExporter.h"
#include "DbInstaller.h"
#include "DesktopChannel.h"
#include "EmBankROM.h"
#include "EmBusTiming.h"
#include "DebugSupport.h"
#include "Debugger.h"
#include "EmCommon.h"
#include "EmCPU68K.h"
#include "EmDevice.h"
#include "EmHAL.h"
#include "EmMemory.h"
#include "EmPalmOS.h"
#include "EmROMReader.h"
#include "EmSession.h"
#include "ExternalStorage.h"
#include "RuntimeCards.h"
#include "EmSPISlaveSD.h"
#include "savestate/SavestateLoader.h"
#include "EmSystemState.h"
#include "Feature.h"
#include "GdbStub.h"
#include "HostLink.h"
#include "HostTransport.h"
#include "IrPhoneTransport.h"
#include "ModemTransport.h"
#include "Lcd.h"
#include "Miscellaneous.h"
#include "zip.h"
#include "PenEvent.h"
#include "Sound.h"
#include "RawFingers.h"
#include "Touch.h"
#include "device_profile5.h"
#include "session/session_file5.h"

using namespace std;

namespace {

#if defined(__linux__) && !defined(__ANDROID__)
// Xlib's XErrorEvent; X11/Xlib.h can't be included beside the Palm headers.
struct XError {
    int type;
    void* display;
    unsigned long resource;
    unsigned long serial;
    unsigned char code, request, minor;
};
using XErrorHandler = int (*)(void*, XError*);
XErrorHandler previousXErrorHandler = nullptr;

// A window that's already gone, like one the desktop held while it embedded the Palm, gives
// BadWindow (3) or BadDrawable (9). Xlib's default handler ends the process for them, so they're
// logged and ignored.
int IgnoreGoneWindow(void* display, XError* error) {
    if (error->code == 3 || error->code == 9) {
        // The first few only, since drawing to a gone window repeats every frame.
        static int logged = 0;
        if (logged++ < 8)
            cerr << "x11: ignored " << (error->code == 3 ? "BadWindow" : "BadDrawable") << " for 0x" << hex
                 << error->resource << dec << " (request " << int(error->request) << ")" << endl;
        return 0;
    }
    return previousXErrorHandler ? previousXErrorHandler(display, error) : 0;
}

// Call after SDL's video starts, so this sits in front of SDL's own handler.
void IgnoreGoneXWindows() {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "x11") != 0) return;
    // SDL loaded libX11 itself; this only finds it.
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (!x11) return;
    if (auto set = reinterpret_cast<XErrorHandler (*)(XErrorHandler)>(dlsym(x11, "XSetErrorHandler")))
        previousXErrorHandler = set(IgnoreGoneWindow);
    dlclose(x11);
}
#else
void IgnoreGoneXWindows() {}
#endif

// Frond's session metadata for 68k machines. The savestate blob follows Cloudpilot's pin, so a
// session from another pin is refused rather than half loaded.
constexpr const char* kSessionFormat = "frond-68k 1";
constexpr const char* kCloudpilotPin = "cloudpilot 064d5d5";
// Uncapped runs the CPU this many times faster than the real clock, with timers on real time.
constexpr double kUncappedFactor = 8;

enum class Speed { original, uncapped };

struct Options {
    string image;
    optional<string> danaView;
    string refused;  // an ARM-only option given, refused with its own message
    string device;
    bool checkOnly{false};
    double runFor{0};
    optional<string> framebufferOutput;
    optional<string> inputReplay;
    Speed speed{Speed::original};
    optional<int> contrast;
    float windowScale{2};
    bool sound{true};
    optional<string> audioOutput;
    bool toneLog{false};
    string serial{"off"};
    string irda{"off"};
    // The modem's HotSync link: where a call to the HotSync number goes.
    string modemSync{"off"};
    string sdAttach[2];
    uint16_t gdbPort{0};
    bool smooth{false};
    optional<string> desktopSocket;
    // Where the session is saved when the desktop's socket goes without a quit.
    optional<string> saveOnDisconnect;
    // A fitted window (Android, or --window-fit) places the device by WindowLayout in the background colour.
    WindowFit fit{WindowFit::none};
    uint32_t windowBackground{0};
    int audioPeriod{512};
};

void usage() {
    cerr << "usage: frond-68k [--rom] FILE [--device NAME] [--check-rom] [--run-for SECONDS]\n"
            "                 [--framebuffer-output PNG] [--input-replay FILE] [--speed original|uncapped]\n"
            "                 [--contrast 0-100] [--window-scale 1-4] [--no-sound] [--audio-output WAV]\n"
            "                 [--tone-log] [--serial off|pty|tcp:PORT|unix:PATH|modem]\n"
            "                 [--irda off|pty|tcp:PORT|unix:PATH|phone|printer] [--modem-sync off|pty|tcp:PORT|unix:PATH]\n"
            "                 [--gdb PORT] [--smooth-lcd] [--desktop-socket PATH] [--save-on-disconnect FILE]\n"
            "                 [--dana-view \"keyboard=auto zoom=2 pointer=touch\"]\n"
            "                 [--window-fit fit|whole|fill] [--window-background RRGGBB] [--audio-period FRAMES]\n"
            "FILE is a ROM or a saved session. --run-for runs headless for that many guest seconds.\n"
            "The desktop's ARM-only options are taken and ignored."
         << endl;
}

bool readFile(const string& path, vector<uint8_t>& data) {
    ifstream in(path, ios::binary | ios::ate);
    if (!in) return false;
    const auto end = in.tellg();
    if (end <= 0 || end > (600 << 20)) return false;
    data.resize(size_t(end));
    in.seekg(0);
    return bool(in.read(reinterpret_cast<char*>(data.data()), data.size()));
}

// Writes through a flushed temporary file, so a failed write never leaves half a file.
bool writeFile(const string& path, const void* data, size_t size) {
    // A temporary of its own beside the file, named so the desktop clears one a crash left.
    string temporary = path + ".tmp-XXXXXX";
    const int file = mkstemp(temporary.data());
    if (file < 0) return false;
    fchmod(file, 0644);
    const auto* bytes = static_cast<const char*>(data);
    bool ok = true;
    while (ok && size) {
        const auto n = write(file, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else { bytes += n; size -= size_t(n); }
    }
    if (ok && fsync(file) < 0) ok = false;
    if (close(file) < 0) ok = false;
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        remove(temporary.c_str());
        return false;
    }
    return true;
}

// A card header ID as text; headers before version 5 have none.
string fourCc(uint32_t value) {
    if (!value) return "none";
    string text;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = char(value >> shift);
        text += c >= 0x20 && c < 0x7f ? c : '?';
    }
    return text;
}

// The Cloudpilot device ID for a 68k profile.
const char* cloudpilotDevice(const DeviceProfile5& profile) {
    switch (profile.deviceType) {
        case deviceTypeIIIx: return "PalmIIIx";
        case deviceTypeIIIe: return "PalmIIIe";
        case deviceTypeIIIxe: return "PalmIIIxe";
        case deviceTypeV: return "PalmV";
        case deviceTypeVx: return "PalmVx";
        case deviceTypeM100: return "PalmM100";
        case deviceTypeM105: return "PalmM105";
        case deviceTypeM125: return "PalmM125";
        // The WorkPad c500 is an m500 sold by IBM, on the same ROM.
        case deviceTypeM500: case deviceTypeC500: return "PalmM500";
        case deviceTypeIIIc: return "PalmIIIc";
        case deviceTypeM505: return "PalmM505";
        case deviceTypeM515: return "PalmM515";
        case deviceTypeM130: return "PalmM130";
        case deviceTypeM520: return "PalmM520";
        case deviceTypeDana: return "AlphaSmartDana";
        case deviceTypeDanaWireless: return "DanaWireless";
        default: return nullptr;
    }
}

// A ROM the machine can run: the frontends' header check, then Cloudpilot's full check. Returns
// an error, or "" with profile set. deviceName, if given, must name a 68k profile.
string checkRom(const uint8_t* rom, size_t size, const string& deviceName, const DeviceProfile5*& profile,
                ostream* details) {
    const auto header = deviceProfileInspect68kRom5(size, [&](uint64_t at, uint8_t* out, size_t count) {
        if (at > size || count > size - at) return false;
        memcpy(out, rom + at, count);
        return true;
    });
    if (!header.hasHeader) return "not a 68k Palm ROM";
    if (!header.profile)
        return "unsupported 68k ROM (card header version " + to_string(header.headerVersion) + ")";
    profile = header.profile;
    if (!deviceProfileRuns68k5(profile->deviceType))
        return string(profile->name) + ": 68k board is not qualified";
    if (!deviceName.empty()) {
        profile = deviceProfileNamed5(deviceName.c_str());
        if (!profile || !deviceProfileRuns68k5(profile->deviceType)) return "unknown 68k device " + deviceName;
        if (!deviceProfileTakes68kRom5(*profile, header))
            return string(profile->name) + ": this is a " + header.profile->displayName + " ROM";
    }
    if (profile->capabilities.builtInWifi && !danaSerialToken5(rom, size))
        return string(profile->name) + ": this ROM has no serial number token";
    EmROMReader reader(const_cast<uint8_t*>(rom), size);
    if (!reader.Read()) return "unreadable ROM";
    EmDevice device(cloudpilotDevice(*profile));
    if (!device.Supported() || !device.SupportsROM(reader))
        return string(profile->name) + ": this ROM doesn't run on a " + profile->displayName;
    if (details)
        *details << "device " << profile->name << endl
                 << "card version " << reader.GetCardVersion() << endl
                 << "company " << fourCc(reader.GetCompanyID()) << " hal " << fourCc(reader.GetHalID()) << endl
                 << "rom bytes " << size << endl;
    return "";
}

// Starts the core on a copy of the ROM; the core owns the copy. A Dana Wireless gets its serial
// number in the copy (checkRom made sure the token is there).
bool startCore(const DeviceProfile5& profile, const uint8_t* rom, size_t size) {
    auto* copy = new uint8[size];
    memcpy(copy, rom, size);
    if (profile.capabilities.builtInWifi && !danaWirelessSerial5(copy, size)) {
        delete[] copy;
        return false;
    }
    return gSession->Initialize(new EmDevice(cloudpilotDevice(profile)), copy, size);
}

// A session's metadata lines; contrast is optional. "clock 1" marks a counter in whole 68000
// clocks; sessions without it counted in units of two clocks and are converted on load.
// "modem-call" and "phone-call" are only there when the session was saved with the modem or
// the infrared phone in a call, so a load can tell the guest the line has gone.
// "wifi-mac" is the Dana Wi-Fi adapter's MAC, kept once made; "wifi-adapter" (hex) its settings
// and link, only when it was plugged in, with "wifi-bus" its bus state (address, configuration, IN
// halt, OUT halt, OUT toggle, then what was in flight in hex if anything was). "usb-printer" is the USB printer's bus state (address,
// configuration, halt, bulk toggle), only when it was plugged in. "module-mac", "module-adapter" and
// "module-bus" are the same for the Dana Wireless's built-in Wi-Fi module, the last two only while
// its radio is on.
struct SessionMetadata {
    int contrast{Lcd68k::kDefaultContrast};
    bool wholeClocks{false};
    string modemCall, phoneCall;
    optional<UsbPrism2Adapter::Mac> wifiMac;
    vector<uint8_t> wifiAdapter;
    optional<UsbPrism2Adapter::Bus> wifiBus;
    optional<UsbRawPrinter::Bus> printerBus;
    optional<UsbPrism2Adapter::Mac> moduleMac;
    vector<uint8_t> moduleAdapter;
    optional<UsbPrism2Adapter::Bus> moduleBus;
};

string metadataText(const SessionMetadata& metadata) {
    string text = string(kSessionFormat) + "\n" + kCloudpilotPin + "\ncontrast " + to_string(metadata.contrast) +
                  "\nclock 1\n";
    if (!metadata.modemCall.empty()) text += "modem-call " + metadata.modemCall + "\n";
    if (!metadata.phoneCall.empty()) text += "phone-call " + metadata.phoneCall + "\n";
    if (metadata.wifiMac) text += "wifi-mac " + UsbWifiRuntime::macText(*metadata.wifiMac) + "\n";
    if (!metadata.wifiAdapter.empty()) text += "wifi-adapter " + UsbWifiRuntime::hexText(metadata.wifiAdapter) + "\n";
    if (metadata.wifiBus) text += "wifi-bus " + UsbWifiRuntime::busText(*metadata.wifiBus) + "\n";
    if (metadata.moduleMac) text += "module-mac " + UsbWifiRuntime::macText(*metadata.moduleMac) + "\n";
    if (!metadata.moduleAdapter.empty()) text += "module-adapter " + UsbWifiRuntime::hexText(metadata.moduleAdapter) + "\n";
    if (metadata.moduleBus) text += "module-bus " + UsbWifiRuntime::busText(*metadata.moduleBus) + "\n";
    if (metadata.printerBus) {
        const auto& b = *metadata.printerBus;
        text += "usb-printer " + to_string(b.address) + " " + to_string(b.configuration) + " " + to_string(int(b.halted)) +
                " " + to_string(int(b.bulkToggle)) + "\n";
    }
    return text;
}

string parseMetadata(const void* data, size_t size, SessionMetadata& metadata) {
    // The Wi-Fi adapter's in-flight bytes can take the metadata past 4 KiB.
    if (!data || !size || size > 16384) return "session has no frond-68k metadata";
    istringstream lines(string(static_cast<const char*>(data), size));
    string line;
    if (!getline(lines, line) || line != kSessionFormat) return "session is from another frond-68k format";
    if (!getline(lines, line) || line != kCloudpilotPin) return "session is from another 68k core version";
    while (getline(lines, line)) {
        if (line.rfind("contrast ", 0) == 0) {
            char* end = nullptr;
            const long value = strtol(line.c_str() + 9, &end, 10);
            if (*end || value < 0 || value > 100) return "invalid session contrast";
            metadata.contrast = int(value);
        } else if (line == "clock 1") {
            metadata.wholeClocks = true;
        } else if (line.rfind("modem-call ", 0) == 0 && line.size() > 11) {
            metadata.modemCall = line.substr(11);
        } else if (line.rfind("phone-call ", 0) == 0 && line.size() > 11) {
            metadata.phoneCall = line.substr(11);
        } else if (line.rfind("wifi-mac ", 0) == 0) {
            UsbPrism2Adapter::Mac mac;
            if (!UsbWifiRuntime::parseMac(line.substr(9), mac)) return "invalid session Wi-Fi MAC";
            metadata.wifiMac = mac;
        } else if (line.rfind("wifi-adapter ", 0) == 0) {
            vector<uint8_t> bytes;
            UsbPrism2Adapter check;
            if (!UsbWifiRuntime::parseHex(line.substr(13), bytes) || !check.load(bytes.data(), bytes.size()))
                return "invalid session Wi-Fi adapter state";
            metadata.wifiAdapter = std::move(bytes);
        } else if (line.rfind("wifi-bus ", 0) == 0) {
            UsbPrism2Adapter::Bus bus;
            if (!UsbWifiRuntime::parseBus(line.substr(9), bus)) return "invalid session Wi-Fi adapter state";
            metadata.wifiBus = std::move(bus);
        } else if (line.rfind("module-mac ", 0) == 0) {
            UsbPrism2Adapter::Mac mac;
            if (!UsbWifiRuntime::parseMac(line.substr(11), mac)) return "invalid session Wi-Fi module MAC";
            metadata.moduleMac = mac;
        } else if (line.rfind("module-adapter ", 0) == 0) {
            vector<uint8_t> bytes;
            UsbPrism2Adapter check;
            if (!UsbWifiRuntime::parseHex(line.substr(15), bytes) || !check.load(bytes.data(), bytes.size()))
                return "invalid session Wi-Fi module state";
            metadata.moduleAdapter = std::move(bytes);
        } else if (line.rfind("module-bus ", 0) == 0) {
            UsbPrism2Adapter::Bus bus;
            if (!UsbWifiRuntime::parseBus(line.substr(11), bus)) return "invalid session Wi-Fi module state";
            metadata.moduleBus = std::move(bus);
        } else if (line.rfind("usb-printer ", 0) == 0) {
            istringstream fields(line.substr(12));
            unsigned address, configuration, halted, toggle; string extra;
            UsbRawPrinter::Bus bus;
            if (!(fields >> address >> configuration >> halted >> toggle) || (fields >> extra) || halted > 1 || toggle > 1)
                return "invalid session USB printer state";
            bus = {address, configuration, bool(halted), bool(toggle)};
            UsbRawPrinter check{0};
            if (!check.setBus(bus)) return "invalid session USB printer state";
            metadata.printerBus = bus;
        } else if (!line.empty()) {
            return "unknown session metadata";
        }
    }
    return "";
}

// Native linked sessions are deliberately refused by portable SessionFile5 readers.
RuntimeCards runtimeCards;
constexpr char linkedMagic[] = "FRONDLINKSD1\n";
constexpr char portableMagic[] = "FRONDPORTSD1\n";
bool isPortable(const vector<uint8_t>& data) {
    return data.size()>=sizeof(portableMagic)-1 && !memcmp(data.data(),portableMagic,sizeof(portableMagic)-1);
}
bool isLinked(const vector<uint8_t>& data) {
    return data.size() >= sizeof(linkedMagic)-1 && !memcmp(data.data(),linkedMagic,sizeof(linkedMagic)-1);
}
uint32_t linkedHash(const uint8_t* p,size_t size) {
    uint32_t hash=2166136261u;
    while(size--) hash=(hash ^ *p++)*16777619u;
    return hash;
}

// The current machine as a session file.
bool serializeSession(const DeviceProfile5& profile, const SessionMetadata& metadata, vector<uint8_t>& out) {
    if(!runtimeCards.CanSave()) {
        cerr << "session save refused: unmanaged or failed external media" << endl;
        return false;
    }
    if (!gSession->Save()) return false;
    auto& savestate = gSession->GetSavestate();
    const auto rom = gSession->GetRomImage();
    const string text = metadataText(metadata);
    SessionFile5 session;
    session.SetDeviceName(profile.name)
        .SetDisplayMode(int32_t(profile.defaultDisplay))
        .SetRamSize(profile.minRam)
        .SetNor(rom.first, rom.second)
        .SetMemory(gSession->GetMemorySize(), gSession->GetMemoryPtr())
        .SetSavestate(savestate.GetSize(), savestate.GetBuffer())
        .SetMetadata(text.size(), text.data());
    if (!session.Serialize()) return false;
    const auto* bytes = static_cast<const uint8_t*>(session.GetSerializedSession());
    out.assign(bytes, bytes + session.GetSerializedSessionSize());
    if(runtimeCards.HasCards()) {
        const string manifest=runtimeCards.Manifest();
        if(manifest.size()>8192) return false;
        vector<uint8_t> linked(linkedMagic,linkedMagic+sizeof(linkedMagic)-1);
        auto put=[&](uint32_t x) { for(unsigned i=0;i<4;++i) linked.push_back(x>>(8*i)); };
        put(manifest.size());
        put(linkedHash(out.data(),out.size()));
        put(linkedHash(reinterpret_cast<const uint8_t*>(manifest.data()),manifest.size()));
        linked.insert(linked.end(),manifest.begin(),manifest.end());
        linked.insert(linked.end(),out.begin(),out.end());
        out.swap(linked);
    }
    return true;
}

// A session file checked as far as possible without touching the running machine.
struct CheckedSession {
    SessionFile5 file;
    string manifest;
    const DeviceProfile5* profile{nullptr};
    SessionMetadata metadata;
};

string checkSession(const vector<uint8_t>& data, const string& deviceName, CheckedSession& session, const string& folder="") {
    size_t offset=0;
    if(isLinked(data) || isPortable(data)) {
        offset=sizeof(linkedMagic)-1;
        if(data.size()-offset<12) return "truncated linked session";
        auto get=[&]() { uint32_t x=0; for(unsigned i=0;i<4;++i) x|=uint32_t(data[offset++])<<(8*i); return x; };
        const auto count=get(), machineHash=get(), manifestHash=get();
        if(count>8192 || count>data.size()-offset) return "invalid linked manifest length";
        if(linkedHash(data.data()+offset,count)!=manifestHash ||
           linkedHash(data.data()+offset+count,data.size()-offset-count)!=machineHash) return "damaged linked session";
        session.manifest.assign(reinterpret_cast<const char*>(data.data()+offset),count);
        offset+=count;
        if(isPortable(data) && folder.empty()) return "portable folder required; container conversion is unsupported";
    }
    if (!session.file.Deserialize(data.size()-offset, data.data()+offset)) return "invalid session image";
    const auto* named = deviceProfileNamed5(session.file.GetDeviceName().c_str());
    if (!named || !deviceProfileRuns68k5(named->deviceType)) return "session is not for a 68k device";
    if (!deviceName.empty() && deviceName != named->name)
        return "device " + deviceName + " doesn't match the session's " + named->name;
    if (session.file.GetNandSize() || session.file.GetSdSize() || session.file.GetMediaCount())
        return "session has storage a " + string(named->displayName) + " doesn't have";
    if (session.file.GetDisplayMode() != int32_t(named->defaultDisplay)) return "unsupported display mode";
    if (session.file.GetRamSize() != named->minRam) return "unsupported RAM size";
    if (!session.file.GetMemorySize() || !session.file.GetSavestateSize()) return "session has no machine state";
    if (!session.manifest.empty() && !deviceProfileLinkedCards5(*named)) return "this device has no card slot";
    if (isPortable(data)) {
        string linked;
        const auto error=RuntimeCards::ResolvePortable(named->deviceType,session.manifest,folder,linked);
        if(!error.empty()) return error;
        session.manifest=std::move(linked);
    }
    if (const auto error = parseMetadata(session.file.GetMetadata(), session.file.GetMetadataSize(), session.metadata);
        !error.empty())
        return error;
    if (const auto error = checkRom(static_cast<const uint8_t*>(session.file.GetNor()), session.file.GetNorSize(),
                                    named->name, session.profile, nullptr);
        !error.empty())
        return "session ROM: " + error;
    return "";
}

// A portable folder owns its media files, including ejected cards. Generic readers refuse its magic.
bool readSessionInput(const string& path,vector<uint8_t>& data,string& folder) {
    struct stat st{};
    if(lstat(path.c_str(),&st)) return false;
    if(S_ISDIR(st.st_mode)) {
        folder=std::filesystem::absolute(path).lexically_normal().string();
        return readFile(folder+"/machine.frond",data) && isPortable(data);
    }
    return readFile(path,data);
}
PortablePublication savePortable(const string& path,const DeviceProfile5& profile,const SessionMetadata& metadata) {
    vector<uint8_t> data;
    if(!runtimeCards.HasCards() || !serializeSession(profile,metadata,data) || !isLinked(data)) return PortablePublication::refused;
    const size_t h=sizeof(linkedMagic)-1;
    const uint32_t count=frondSdFile::get32(data.data()+h);
    vector<uint8_t> machine(data.begin()+h+12+count,data.end());
    std::string pattern=path+".stage-XXXXXX";
    vector<char> name(pattern.begin(),pattern.end()); name.push_back(0);
    if(!mkdtemp(name.data())) return PortablePublication::refused;
    const std::filesystem::path staging(name.data());
    auto result=PortablePublication::refused;
    // Only this private directory is removed on failure, never the requested destination.
    try {
        string manifest;
        if(runtimeCards.CopyPortable(staging,manifest)) {
            vector<uint8_t> out(portableMagic,portableMagic+sizeof(portableMagic)-1);
            auto put=[&](uint32_t x){for(unsigned i=0;i<4;++i) out.push_back(x>>(8*i));};
            put(manifest.size()); put(linkedHash(machine.data(),machine.size()));
            put(linkedHash(reinterpret_cast<const uint8_t*>(manifest.data()),manifest.size()));
            out.insert(out.end(),manifest.begin(),manifest.end()); out.insert(out.end(),machine.begin(),machine.end());
            if(writeFile((staging/"machine.frond").string(),out.data(),out.size()))
                result=PublishPortable(staging,path,frondSession::syncDir);
        }
    } catch(...) { /* Keep the publication result if output has already moved. */ }
    std::error_code ec; std::filesystem::remove_all(staging,ec);
    return result;
}

// Decode the real protocol chunks into temporary devices before touching live attachments.
struct CardStateCheck {
    const RuntimeCards::Pair& pair;
    unsigned slots;
    void Load(SavestateLoader<ChunkType>& loader) {
        EmSPISlaveSD first(EmHAL::Slot::sdcard),second(EmHAL::Slot::sdcard2);
        first.Load(loader); second.Load(loader);
        if ((pair.card[0] && (!first.IsBlockInRange(pair.card[0]->backing->Size()/512) || !first.IsProtocol(pair.card[0]->protocol))) ||
            (pair.card[1] && (!second.IsBlockInRange(pair.card[1]->backing->Size()/512) || !second.IsProtocol(pair.card[1]->protocol))))
            loader.NotifyError();
        // One SPI card per slot: 0x1d0 for the first, 0x201 for the Dana's second.
        if(!loader.HasChunk(static_cast<ChunkType>(0x1d0)) || loader.HasChunk(static_cast<ChunkType>(0x201))!=(slots>1) ||
           (slots<2 && pair.card[1]))
            loader.NotifyError();
        auto* chunk=loader.GetChunk(ChunkType::externalStorage);
        if(!chunk) { loader.NotifyError(); return; }
        const auto version=chunk->Get32();
        if(version!=1 && version!=2) { loader.NotifyError(); return; }
        std::string keys[3];
        for(unsigned n=0;n<(version==1?2u:3u);++n) {
            char bytes[ExternalStorage::MAX_KEY_LENGTH+1]{};
            chunk->GetBuffer(bytes,sizeof(bytes));
            auto* end=static_cast<char*>(memchr(bytes,0,sizeof(bytes)));
            if(!end) { loader.NotifyError(); return; }
            keys[n].assign(bytes,end-bytes);
        }
        if(!chunk->IsAtEnd() || !keys[1].empty()) loader.NotifyError();
        for(unsigned n=0;n<2;++n)
            if(keys[n?2:0] != (pair.card[n] && pair.inserted[n]?pair.card[n]->key:"")) loader.NotifyError();
    }
};
string checkCardState(const CheckedSession& session,const RuntimeCards::Pair& pair) {
    if(session.manifest.empty()) return "";
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(session.file.GetSavestate()),
        static_cast<const uint8_t*>(session.file.GetSavestate())+session.file.GetSavestateSize());
    // Reject duplicate chunks and trailing data as well as truncated lengths.
    if(bytes.size()<4 || bytes.size()%4) return "invalid machine chunk table";
    auto word=[&](size_t at) { uint32_t x=0; for(unsigned i=0;i<4;++i) x|=uint32_t(bytes[at+i])<<(8*i); return x; };
    size_t count=word(0),offset=4;
    if(count>(bytes.size()-4)/8) return "invalid machine chunk table";
    offset+=count*8;
    std::vector<uint32_t> ids;
    for(size_t i=0;i<count;++i) {
        auto id=word(4+i*8),length=word(8+i*8);
        if(std::find(ids.begin(),ids.end(),id)!=ids.end() || length>(bytes.size()-offset)/4) return "invalid machine chunk table";
        ids.push_back(id); offset+=size_t(length)*4;
    }
    if(offset!=bytes.size()) return "trailing machine chunk data";
    CardStateCheck target{pair,session.profile?deviceProfileCardSlotCount5(*session.profile):0u}; SavestateLoader<ChunkType> loader;
    return loader.Load(bytes.data(),bytes.size(),target)?"":"invalid card protocol or insertion state";
}

// Replaces the machine with a checked session. On failure the machine is in no useful state.
string applySession(const CheckedSession& session) {
    if (!startCore(*session.profile, static_cast<const uint8_t*>(session.file.GetNor()), session.file.GetNorSize()))
        return "core failed to start";
    if (gSession->GetMemorySize() != session.file.GetMemorySize()) return "session RAM image has the wrong size";
    // LoadMemoryV4 writes into a copy, as the session's buffer is const.
    vector<uint8_t> memory(static_cast<const uint8_t*>(session.file.GetMemory()),
                           static_cast<const uint8_t*>(session.file.GetMemory()) + session.file.GetMemorySize());
    if (!EmMemory::LoadMemoryV4(memory.data(), memory.size())) return "session RAM image is damaged";
    vector<uint8_t> savestate(static_cast<const uint8_t*>(session.file.GetSavestate()),
                              static_cast<const uint8_t*>(session.file.GetSavestate()) + session.file.GetSavestateSize());
    EmHAL::loadedNewerState = false;
    if (!gSession->Load(savestate.size(), savestate.data()))
        return EmHAL::loadedNewerState ? "session machine state was saved by a newer Frond" : "session machine state is damaged";
    gExternalStorage.Remount();
    if(gExternalStorage.RemountFailed()) return "session card remount failed";
    if (!session.metadata.wholeClocks) gSession->ScaleCycles(2);
    return "";
}

// Lines from stdin, read on their own thread and handled by the main loop.
class Console {
   public:
    void Start() {
        thread([this] {
            string line;
            while (getline(cin, line)) {
                lock_guard<mutex> lock(guard);
                lines.push_back(line);
            }
        }).detach();
    }
    bool Next(string& line) {
        lock_guard<mutex> lock(guard);
        if (lines.empty()) return false;
        line = std::move(lines.front());
        lines.pop_front();
        return true;
    }

   private:
    mutex guard;
    deque<string> lines;
};

// Hard keys by replay name, for the buttons a IIIx has.
optional<ButtonEvent::Button> buttonForKey(KeyId key) {
    switch (key) {
        case keyIdHard1: return ButtonEvent::Button::app1;
        case keyIdHard2: return ButtonEvent::Button::app2;
        case keyIdHard3: return ButtonEvent::Button::app3;
        case keyIdHard4: return ButtonEvent::Button::app4;
        case keyIdUp: return ButtonEvent::Button::rockerUp;
        case keyIdDown: return ButtonEvent::Button::rockerDown;
        case keyIdPower: return ButtonEvent::Button::power;
        default: return nullopt;
    }
}

optional<KeyId> keyNamed(const string& name) {
    static const pair<const char*, KeyId> names[] = {
        {"hard1", keyIdHard1}, {"hard2", keyIdHard2}, {"hard3", keyIdHard3}, {"hard4", keyIdHard4},
        {"up", keyIdUp},       {"down", keyIdDown},   {"power", keyIdPower}};
    for (const auto& [text, key] : names)
        if (name == text) return key;
    return nullopt;
}

optional<DanaScan5> danaForLegacyKey(KeyId key) {
    const char* name=key==keyIdPower?"power":key==keyIdUp?"up":key==keyIdDown?"down":
        key==keyIdHard1?"calendar":key==keyIdHard2?"address":key==keyIdHard3?"todo":key==keyIdHard4?"memo":nullptr;
    return name?DanaScan5::Named(name):nullopt;
}

// Timed pen, key and snapshot events, in the ARM runner's replay format, timed in guest
// milliseconds from when the replay was loaded.
class Replay {
   public:
    enum class Kind { penDown, penUp, keyDown, keyUp, snapshot };
    struct Event {
        uint64_t at;
        Kind kind;
        int x{0}, y{0};
        KeyId key{keyInvalid};
        optional<DanaScan5> dana;
        string path;
    };

    bool Load(const string& path, int width, int height, string& error, bool danaDevice=false) {
        ifstream file(path);
        if (!file) {
            error = "cannot open input replay: " + path;
            return false;
        }
        vector<Event> loaded;
        std::array<bool,128> danaHeld{};
        string line;
        size_t number = 0;
        while (getline(file, line)) {
            ++number;
            istringstream tokens(line);
            string time, command, extra;
            if (!(tokens >> time) || time.front() == '#') continue;
            auto fail = [&] {
                error = path + ":" + to_string(number) + ": invalid replay event";
                return false;
            };
            if (!(tokens >> command) || time.find_first_not_of("0123456789") != string::npos || time.size() > 12)
                return fail();
            Event event{stoull(time), Kind::penUp};
            if (!loaded.empty() && event.at < loaded.back().at) return fail();
            if (command == "pen-down" || command == "pen-move") {
                event.kind = Kind::penDown;
                if (!(tokens >> event.x >> event.y) || event.x < 0 || event.x >= width || event.y < 0 ||
                    event.y >= height)
                    return fail();
            } else if (command == "pen-up") {
                event.kind = Kind::penUp;
            } else if (command == "key-down" || command == "key-up") {
                string name;
                if (!(tokens >> name)) return fail();
                const auto key = keyNamed(name);
                if (!key) {
                    if(!danaDevice || name.rfind("dana.",0)!=0 || !(event.dana=DanaScan5::Named(string_view(name).substr(5)))) return fail();
                } else { event.key = *key; if(danaDevice) event.dana=danaForLegacyKey(*key); }
                event.kind = command == "key-down" ? Kind::keyDown : Kind::keyUp;
                if(event.dana) {
                    danaHeld[event.dana->Code()]=command=="key-down";
                    if(std::count(danaHeld.begin(),danaHeld.end(),true)>8) return fail();
                }
            } else if (command == "snapshot") {
                event.kind = Kind::snapshot;
                if (!(tokens >> quoted(event.path)) || event.path.empty()) return fail();
            } else {
                return fail();
            }
            if ((tokens >> extra) && extra.front() != '#') return fail();
            loaded.push_back(event);
        }
        events = std::move(loaded);
        next = 0;
        started = false;
        return true;
    }

    // Events due by guestMs; the first call sets the start.
    template <typename Handle>
    void Dispatch(uint64_t guestMs, Handle handle) {
        if (next >= events.size()) return;
        if (!started) {
            start = guestMs;
            started = true;
        }
        while (next < events.size() && events[next].at <= guestMs - start) {
            if(!handle(events[next])) return;
            ++next;
            cout << "replay event " << next << " at " << guestMs - start << " ms" << endl;
        }
    }

    bool Finished() const { return next >= events.size(); }
    uint64_t LastEventMs() const { return events.empty() ? 0 : events.back().at; }

   private:
    vector<Event> events;
    size_t next{0};
    uint64_t start{0};
    bool started{false};
};

// A database name or four-character code as text, with anything unprintable as '?'.
string printable(const char* text, size_t max) {
    string out;
    for (size_t i = 0; i < max && text[i]; ++i) out += text[i] >= 0x20 && text[i] < 0x7f ? text[i] : '?';
    return out;
}

// Installs a PDB or PRC and, once it's in, does what Palm OS does after a HotSync install.
// Runs at a safe point.
DbInstaller::Result installDatabase(vector<uint8_t>& data) {
    // The name is the header's first 32 bytes; the installer may rewrite the buffer.
    const char* header = reinterpret_cast<const char*>(data.data());
    const string name = data.size() >= 32 ? string(header, strnlen(header, 32)) : string();
    const auto result = DbInstaller(data.size(), data.data()).Install();
    // The database is in whatever the notify does, so a failure there is only logged.
    if (result == DbInstaller::Result::success && !name.empty()) {
        try {
            EmPalmOS::FinishInstall(name);
        } catch (const exception& error) {
            cerr << "install: sync notify failed: " << error.what() << endl;
        }
    }
    return result;
}

// A zip of the RAM databases, or all of them with includeRom. False if Palm OS couldn't list them.
bool backupDatabases(bool includeRom, vector<uint8_t>& out, size_t& count, size_t& failed) {
    DbBackup backup;
    if (!backup.Init(includeRom)) return false;
    count = failed = 0;
    while (backup.IsInProgress()) {
        ++count;
        if (!backup.Save()) ++failed;
    }
    const auto [size, data] = backup.GetArchive();
    if (size <= 0 || !data) return false;
    out.assign(data, data + size);
    return true;
}

// One database as a PDB or PRC file, found by name among RAM and ROM databases.
string exportDatabase(const string& name, vector<uint8_t>& out) {
    DatabaseInfoList databases;
    if (!GetDatabases(databases, GetDatabaseFlags::kDatabaseFlagsNone)) return "Palm OS couldn't list its databases";
    for (const auto& database : databases) {
        if (name != database.dbName) continue;
        zip_t* zip = zip_stream_open(nullptr, 0, 1, 'w');
        if (!zip) return "out of memory";
        DbExporter exporter(zip);
        bool ok = zip_entry_open(zip, "db") == 0 && exporter.Export(database);
        ok = zip_entry_close(zip) == 0 && ok;
        void* archive = nullptr;
        ssize_t size = 0;
        if (ok) zip_stream_copy(zip, &archive, &size);
        zip_stream_close(zip);
        if (!ok || !archive) {
            free(archive);
            return "export failed";
        }
        zip_t* reader = zip_stream_open(static_cast<const char*>(archive), size_t(size), 0, 'r');
        void* bytes = nullptr;
        size_t length = 0;
        ok = reader && zip_entry_open(reader, "db") == 0 && zip_entry_read(reader, &bytes, &length) >= 0;
        if (reader) {
            zip_entry_close(reader);
            zip_stream_close(reader);
        }
        free(archive);
        if (ok && bytes) out.assign(static_cast<uint8_t*>(bytes), static_cast<uint8_t*>(bytes) + length);
        free(bytes);
        return ok ? "" : "export failed";
    }
    return "no database named " + name;
}

// One database by name in a zip, as the desktop's export-db writes it.
string exportDatabaseZip(const string& name, vector<uint8_t>& out) {
    DatabaseInfoList databases;
    if (!GetDatabases(databases, GetDatabaseFlags::kDatabaseFlagsNone)) return "not-ready";
    for (const auto& database : databases) {
        if (name != database.dbName) continue;
        const string entry = name + ((database.dbAttrs & dmHdrAttrResDB) ? ".prc" : ".pdb");
        zip_t* zip = zip_stream_open(nullptr, 0, 1, 'w');
        if (!zip) return "out of memory";
        DbExporter exporter(zip);
        bool ok = zip_entry_open(zip, entry.c_str()) == 0 && exporter.Export(database);
        ok = zip_entry_close(zip) == 0 && ok;
        void* archive = nullptr;
        ssize_t size = 0;
        if (ok) zip_stream_copy(zip, &archive, &size);
        zip_stream_close(zip);
        if (ok && archive) out.assign(static_cast<uint8_t*>(archive), static_cast<uint8_t*>(archive) + size);
        free(archive);
        return ok && !out.empty() ? "" : "export failed";
    }
    return "missing";
}

class Runner {
   public:
    Runner(const Options& options, const DeviceProfile5& profile, const SessionMetadata& metadata)
        : options(options), profile(&profile), metadata(metadata),
          lcd(profile.frontend.width, profile.frontend.height, deviceProfileColourLcd5(profile.deviceType)) {
        if(deviceTypeIsDana5(profile.deviceType)) EmHAL::ReleaseDanaInput(false);
        lcd.SetContrast(options.contrast.value_or(metadata.contrast));
        this->metadata.contrast = lcd.Contrast();
        SetSpeed(options.speed);
        soundOn = options.sound && profile.capabilities.sound;
        smooth = options.smooth;
        fit = options.fit;
        if(deviceTypeIsDana5(profile.deviceType)) {
#ifndef __ANDROID__
            danaView.keyboard="hidden";danaView.physical=true;
#endif
            if(options.danaView) {string error;danaView.Parse(*options.danaView,error);}
            danaLayout=DanaViewLayout::Make(1120,572,danaView);
        }
        desktopMode = bool(options.desktopSocket);
        // The session owns the transports; they live as long as the process.
        serialTransport = new ModemTransport68k(serialLink, modemSyncLink);
        irTransport = new IrPhoneTransport68k(irLink);
        gSession->SetTransportSerial(kUARTSerial, serialTransport);
        gSession->SetTransportSerial(kUARTIR, irTransport);
        // A session saved mid-call: the call is gone by the time modem or phone mode comes on.
        serialTransport->SessionLoaded(metadata.modemCall);
        irTransport->SessionLoaded(metadata.phoneCall);
        piezo.Restart(gSession->GetSystemCycles());
        usbWifi.clock=[this] { return UsbTime(); };
        usbPrinter.clock=[this] { return UsbTime(); };
        moduleWifi.clock=[this] { return UsbTime(); };
        // A session opened at start brings its USB device with it.
        if(deviceTypeIsDana5(profile.deviceType) && Isp()) usbPort=SessionUsbDevice(metadata);
        UsbSessionLoaded(metadata);
        if(usbPort!="off") { string result; if(!UsbPortControl(usbPort,result,true)) usbPort="off"; }
        if(moduleOn) { string result; if(!ModuleControl(true,result,true)) moduleOn=false; }
        EmPalmOS::SetSafePointHandler([this] { RunGuestCall(); });
        pwmHandler = EmHAL::onPwmChange.AddHandler([this](double frequency, double duty) {
            piezo.Change(gCPU68K ? gCPU68K->CycleNow() : gSession->GetSystemCycles(), frequency, duty);
            if (this->options.toneLog) {
                char line[96];
                snprintf(line, sizeof(line), "tone: %.1f Hz duty %.2f at %.3f s", frequency, duty, guestSeconds);
                cout << line << endl;
            }
        });
    }

    ~Runner() {
        usbPrinter.detach();
        usbWifi.detach();
        moduleWifi.detach();
        EmBankRegs::onBeforeDispose.RemoveHandler(usbDisposeHandler);
        StopUsb();
        EmPalmOS::SetSafePointHandler(nullptr);
        EmHAL::onPwmChange.RemoveHandler(pwmHandler);
        if (audio) SDL_CloseAudioDevice(audio);
    }

    // Headless: runs for options.runFor guest seconds as fast as the host can.
    int RunHeadless() {
        for (unsigned n=0; n<2; ++n) if (!options.sdAttach[n].empty()) {
            const auto reply=runtimeCards.Command(n,"attach",options.sdAttach[n]);
            cout << "card: " << reply << endl;
            if (reply.rfind("error:",0)==0) return 1;
        }
        if (options.inputReplay && !LoadReplay(*options.inputReplay)) return 1;
        if (!OpenWav() || !OpenLinks()) return 1;
        while (guestSeconds < options.runFor) {
            if (!Step(100000)) return 1;
            DispatchReplay();
        }
        cout << "ran " << gSession->GetSystemCycles() << " cycles, " << guestSeconds
             << " guest seconds, now at " << gSession->GetClocksPerSecond() << " clocks per second" << endl;
        return Finish();
    }

    int RunWindow() {
        for (unsigned n=0; n<2; ++n) if (!options.sdAttach[n].empty()) {
            const auto reply=runtimeCards.Command(n,"attach",options.sdAttach[n]);
            cout << "card: " << reply << endl;
            if (reply.rfind("error:",0)==0) return 1;
        }
        if (options.inputReplay && !LoadReplay(*options.inputReplay)) return 1;
        if (!OpenWav() || !OpenLinks()) return 1;
        if (options.gdbPort) StartGdb(options.gdbPort);
        OpenWindow();
        if (soundOn) OpenAudio();
        if (options.desktopSocket) desktop = make_unique<DesktopChannel>(*options.desktopSocket);
        else console.Start();
        uint64_t lastDraw = 0;
        double emulatedMs = 0;
        const auto origin = Clock::now();
        auto wallMs = [&] { return chrono::duration<double, milli>(Clock::now() - origin).count(); };
        speedMark = {wallMs(), double(gSession->GetSystemCycles())};
        while (!quit) {
            string line;
            // A command waiting for the guest's event loop holds back the ones after it.
            while (!quit && !desktop && !guestCall && console.Next(line)) Command(line);
            if (window) PollWindow();
            if (desktop) PollDesktop();
            ExpireGuestCall();
            FinishDeferredDelete();
            if (quit) break;
            if (gdb) gdb->Cycle(gDebugger.IsStopped() ? 10 : 0);
            // Hidden on Android: the guest waits and nothing is drawn, while the control socket still
            // answers so the app can save. It carries on from now when shown again. A host call
            // waiting for the event loop (an install from the Peripherals screen) runs the guest
            // without drawing or sound until it is done, and so does an IR print job coming in or
            // an IR printer link up, which only the guest ends.
            if (hidden && !guestCall && !deferredDelete && !PrinterNeedsGuest()) {
                emulatedMs = wallMs();
                SDL_Delay(50);
                continue;
            }

            // Emulate up to the wall clock, in slices short enough to keep input and the console
            // responsive. More than half a second behind starts over from now.
            double now = wallMs();
            // Stopped in the debugger: the guest waits, and carries on from now when let go.
            if (gDebugger.IsStopped()) emulatedMs = now;
            if (now - emulatedMs > 500) emulatedMs = now - 10;
            const double budgetEnd = now + 12;
            while (emulatedMs < now && wallMs() < budgetEnd && !quit && !gDebugger.IsStopped()) {
                const double perMs = gSession->GetClocksPerSecond() / 1000.0;
                const uint32_t want = uint32_t(min(max((now - emulatedMs) * perMs, 1.0), perMs * 4));
                const double before = guestSeconds;
                if (!Step(want)) return 1;
                emulatedMs += (guestSeconds - before) * 1000;
                DispatchReplay();
            }
            UpdateSpeed(wallMs());

            now = wallMs();
            if (now - lastDraw >= 16) {
                Draw();
                lastDraw = uint64_t(now);
            }
            const double ahead = emulatedMs - wallMs();
            if (ahead > 1) SDL_Delay(uint32_t(min(ahead, 8.0)));
        }
        const int status = Finish();
        return saveFailed ? 1 : status;
    }

   private:
    using Clock = chrono::steady_clock;

    // Host commands that call into Palm OS (install, launch, database lists and exports) wait for
    // the guest's event loop, where no app or system call is half done, and run there inside the
    // emulation. One waits at a time; it fails with a reason after kGuestCallSeconds of guest time,
    // or at once while asleep or stopped in the debugger.
    static constexpr double kGuestCallSeconds = 30;
    struct GuestCall {
        function<void()> run;
        function<void(const string&)> refuse;
        double deadline;
    };
    optional<GuestCall> guestCall;

    void Guest(function<void()> run, function<void(const string&)> refuse) {
        if (!gSystemState.IsUIInitialized()) return refuse("Palm OS hasn't finished starting");
        guestCall = GuestCall{std::move(run), std::move(refuse), guestSeconds + kGuestCallSeconds};
        EmPalmOS::RequestSafePoint();
    }

    // The safe point, from EvtGetEvent in the emulated CPU. A call that runs past the cycle limit
    // is abandoned with the CPU's registers put back.
    void RunGuestCall() {
        if (!guestCall) return;
        GuestCall call = std::move(*guestCall);
        guestCall.reset();
        EmSession::HostCallScope scope;
        try {
            call.run();
        } catch (const exception& error) {
            call.refuse(error.what());
        }
    }

    // An application delete waiting for the launcher to quit (EmPalmOS::DeleteDatabaseByName).
    // Like a guest call it holds back later requests, and gives up after kGuestCallSeconds.
    struct DeferredDelete {
        string id;
        double deadline;
    };
    optional<DeferredDelete> deferredDelete;

    void FinishDeferredDelete() {
        if (!deferredDelete || !desktop) return;
        Reply reply;
        if (auto result = EmPalmOS::TakeDeleteResult()) {
            reply = DeleteReply(*result);
        } else if (!EmPalmOS::DeleteWaiting()) {
            reply = {false, "delete", "The Palm was reset before the database could be deleted."};
        } else if (gDebugger.IsStopped() || !gSession->IsPowerOn() || guestSeconds >= deferredDelete->deadline) {
            EmPalmOS::CancelPendingDelete();
            reply = {false, "not-ready", "Palm OS can't take the call now: the launcher didn't quit, nothing was deleted."};
        } else {
            return;
        }
        desktop->Reply(deferredDelete->id, reply.ok, reply.code, reply.message);
        deferredDelete.reset();
    }

    void ExpireGuestCall() {
        if (!guestCall) return;
        string why;
        if (gDebugger.IsStopped()) why = "the CPU is stopped in the debugger";
        else if (!gSession->IsPowerOn()) why = "the Palm is asleep";
        else if (guestSeconds >= guestCall->deadline)
            why = "Palm OS is busy, its event loop didn't run in " + to_string(int(kGuestCallSeconds)) + " s";
        else return;
        GuestCall call = std::move(*guestCall);
        guestCall.reset();
        call.refuse(why);
    }

    // Runs up to cycles and moves guest time on.
    bool Step(uint32_t cycles) {
        ServiceLinks();
        ServiceUsb();
        if(usbBridge.Host()) cycles=std::min(cycles,std::max(1u,uint32_t(gSession->GetClocksPerSecond()/100000)));
        const uint32_t done = gSession->RunEmulation(cycles);
        if (!done) {
            if (gDebugger.IsStopped() && gdb) return true;
            if (gDebugger.IsStopped()) {
                cerr << "the 68k debugger stopped the CPU" << endl;
                return false;
            }
            cerr << "the CPU stopped with no wakeup" << endl;
            return false;
        }
        guestSeconds += double(done) / gSession->GetClocksPerSecond();
        Sound();
        ServiceLinks();
        return true;
    }

    // The GDB stub on 127.0.0.1:port; off until started.
    void StartGdb(uint16_t port) {
        if (gdb) gdb->Stop();
        gdb = make_unique<GdbStub>(gDebugger, port);
        gdb->Listen();
        if (gdb->GetConnectionState() == GdbStub::ConnectionState::socketClosed) {
            gdb.reset();
            cout << "gdb: could not listen on port " << port << endl;
        }
    }

    string GdbState() {
        if (!gdb) return "off";
        return string(gdb->IsDebuggerConnected() ? "connected" : "listening") +
               (gDebugger.IsStopped() ? ", stopped" : ", running");
    }

    bool OpenLinks() {
        for (auto [link, mode, name] : {make_tuple(&serialLink, options.serial, "serial"),
                                        make_tuple(&irLink, options.irda, "infrared")}) {
            string error;
            if (link == &serialLink ? !SetSerial(mode, error) : !SetInfrared(mode, error)) {
                cerr << name << ": " << error << endl;
                // The desktop runs on with the link off and shows why.
                if (!desktopMode) return false;
                (link == &serialLink ? serialError : infraredError) = error;
                continue;
            }
            if (link == &serialLink && serialTransport->ModemMode()) cout << name << ": " << serialTransport->Describe() << endl;
            else if (link == &irLink && (irTransport->PhoneMode() || irTransport->PrinterMode())) cout << name << ": " << irTransport->Describe() << endl;
            else if (link->GetKind() != HostLink68k::Kind::off) cout << name << ": " << link->Describe() << endl;
        }
        if (options.modemSync != "off") {
            string error;
            if (!SetModemSync(options.modemSync, error)) {
                cerr << "modem-sync: " << error << endl;
                if (!desktopMode) return false;
            } else {
                cout << "modem-sync: " << modemSyncLink.Describe() << endl;
            }
        }
        return true;
    }

    // The modem's HotSync link, which a call to the HotSync number reaches. Only on a device with
    // a modem.
    bool SetModemSync(const string& mode, string& error) {
        HostLink68k::Kind kind;
        uint16_t port;
        string path;
        if (!HostLink68k::Parse(mode, kind, port, &path)) {
            error = "use off, pty, tcp:PORT or unix:PATH";
            return false;
        }
        if (kind != HostLink68k::Kind::off && !profile->capabilities.modem) {
            error = string(profile->displayName) + " has no modem";
            return false;
        }
        return modemSyncLink.Open(kind, port, error, path);
    }

    // The serial port's mode: a host link, or modem for the snap-on modem on a device that has one.
    bool SetSerial(const string& mode, string& error) {
        if (mode != "off" && !profile->capabilities.serial) {
            error = "Transport is not qualified for this device.";
            return false;
        }
        if (mode == "modem") {
            if (!profile->capabilities.modem) {
                error = string(profile->displayName) + " has no modem";
                return false;
            }
            if (!serialTransport->SetModemMode(true, error)) return false;
            serialLink.Close();
            SyncDTR();
            return true;
        }
        HostLink68k::Kind kind;
        uint16_t port;
        string path;
        if (!HostLink68k::Parse(mode, kind, port, &path)) {
            error = "use off, pty, tcp:PORT, unix:PATH or modem";
            return false;
        }
        serialTransport->SetModemMode(false, error);
        return serialLink.Open(kind, port, error, path);
    }

    // The IR port's mode: a host link, phone for the core's mobile phone on a device that has one, or
    // printer for the core's IrDA printer. Palm OS before 3.3 has no IrCOMM, so it can't use the phone.
    bool SetInfrared(const string& mode, string& error) {
        if (mode != "off" && !profile->capabilities.infrared) {
            error = "Transport is not qualified for this device.";
            return false;
        }
        if (mode == "printer") {
            irTransport->SetPhoneMode(false, error);
            irTransport->SetPrinterMode(true);
            irLink.Close();
            return true;
        }
        if (mode == "phone") {
            if (!profile->capabilities.irPhone) {
                error = string(profile->displayName) + " has no infrared phone";
                return false;
            }
            if (!RomHasIrComm()) {
                error = "the infrared phone needs Palm OS 3.3 or later";
                return false;
            }
            if (!irTransport->SetPhoneMode(true, error)) return false;
            irLink.Close();
            return true;
        }
        HostLink68k::Kind kind;
        uint16_t port;
        string path;
        if (!HostLink68k::Parse(mode, kind, port, &path)) {
            error = "use off, pty, tcp:PORT, unix:PATH, phone or printer";
            return false;
        }
        irTransport->SetPhoneMode(false, error);
        irTransport->SetPrinterMode(false);
        return irLink.Open(kind, port, error, path);
    }

    // Palm OS 3.3 or later in the running ROM.
    static bool RomHasIrComm() {
        const auto rom = gSession->GetRomImage();
        EmROMReader reader(rom.second, rom.first);
        if (!reader.Read()) return false;
        int major, minor, fix, stage, build;
        reader.Version(major, minor, fix, stage, build);
        return major > 3 || (major == 3 && minor >= 3);
    }

    string InfraredState() const {
        return irTransport->PhoneMode() || irTransport->PrinterMode() ? irTransport->Describe()
                                                                      : LinkState(irLink, irTransport->Host());
    }

    // The metadata to save now, with any call up on the modem or the infrared phone.
    SessionMetadata Saving() const {
        SessionMetadata saving = metadata;
        saving.modemCall = serialTransport->SavedCall();
        saving.phoneCall = irTransport->SavedCall();
        if (usbWifi.hasMac()) saving.wifiMac = usbWifi.mac();
        saving.wifiAdapter.clear();
        if (usbPort == "wifi") saving.wifiAdapter = pendingWifi ? *pendingWifi : usbWifi.adapter.save();
        saving.wifiBus.reset();
        if (usbPort == "wifi") saving.wifiBus = pendingWifi ? pendingWifiBus : optional(usbWifi.adapter.bus());
        saving.printerBus.reset();
        if (usbPort == "printer") saving.printerBus = pendingPrinter ? *pendingPrinter : usbPrinter.printer.bus();
        if (moduleWifi.hasMac()) saving.moduleMac = moduleWifi.mac();
        saving.moduleAdapter.clear();
        saving.moduleBus.reset();
        if (moduleOn && moduleWifi.attached()) {
            saving.moduleAdapter = pendingModule ? *pendingModule : moduleWifi.adapter.save();
            saving.moduleBus = pendingModule ? pendingModuleBus : optional(moduleWifi.adapter.bus());
        }
        return saving;
    }

    // A new modem, or one after a load, takes the guest's DTR as it is now.
    void SyncDTR() {
        serialTransport->SyncDTR(EmHAL::GetDTR(0));
    }

    string SerialState() const {
        return serialTransport->ModemMode() ? ModemState() : LinkState(serialLink, serialTransport->Host());
    }

    // modem idle|dialling|online SPEED, then "; hotsync " and the HotSync link while it's on.
    string ModemState() const {
        const string modem = serialTransport->Describe();
        return modemSyncLink.GetKind() == HostLink68k::Kind::off ? modem : modem + "; hotsync " + modemSyncLink.Describe();
    }

    // Host bytes in and out, and the transports following the guest's line drivers.
    // The printer and the Wi-Fi adapter connect 50 ms after the external port is powered or they're
    // plugged in, like the module below, so a wake's port scans settle before they enumerate and a
    // swap leaves the port empty long enough for the ROM to see the old device go.
    UsbPrinterRuntime usbPrinter{50000000};
    UsbWifiRuntime usbWifi{0,50000000};
    // The external USB port's one device: off, printer or wifi. Kept across loads and resets.
    string usbPort="off";
    // A loaded session's adapter state, put back when the adapter is next plugged in.
    optional<vector<uint8_t>> pendingWifi;
    optional<UsbPrism2Adapter::Bus> pendingWifiBus;
    // A loaded session's printer bus state, the same way.
    optional<UsbRawPrinter::Bus> pendingPrinter;
    // The Dana Wireless's built-in Wi-Fi module on root port 1, and whether its radio is on (network-mode
    // wireless or disabled). Kept across loads and resets; the module's state from a loaded session
    // waits in pendingModule the same way.
    // The module connects 50 ms after it's powered or plugged in again, a power-up within USB's
    // 100 ms attach limit that puts its enumeration between the ROM's polls.
    UsbWifiRuntime moduleWifi{1,50000000};
    bool moduleOn=true;
    optional<vector<uint8_t>> pendingModule;
    optional<UsbPrism2Adapter::Bus> pendingModuleBus;
    UsbHostBridge usbBridge;
    unique_ptr<UsbDanaPort> usbDana;
    UsbHostHooks usbHooks;
    bool usbCable=false;
    string usbDebugMode="off";
    // The bank owner calls this before disposing even the first register bank.
    int usbDisposeHandler=EmBankRegs::onBeforeDispose.AddHandler([this] { StopUsb(); });
    void StopUsb() {
        usbBridge.Stop();
        usbHooks={};
        usbDana.reset();
        usbCable=false;
    }
    void RestoreUsb() {
        if(!deviceTypeIsDana5(profile->deviceType)) return;
        if(usbPort!="off") { string result; UsbPortControl(usbPort,result,true); }
        if(moduleOn) { string result; ModuleControl(true,result,true); }
        string error;
        if(!StartUsbDebug(usbDebugMode,error)) cerr << "usb-debug: error " << error << endl;
    }
    bool PrinterControl(const string& action,const string& argument,string& result) {
        if(!deviceTypeIsDana5(profile->deviceType)) { result="Printer runtime requires Dana.";return false; }
        if(action=="attach") {
            // attach on is usb-port printer; off only unplugs the printer.
            if(argument=="off") { if(usbPort=="printer" && !UsbPortControl("off",result)) return false; }
            else if(argument=="on") { if(!UsbPortControl("printer",result)) return false; }
            else {result="Use attach on|off.";return false;}
            result=usbPrinter.status();return true;
        }
        return usbPrinter.command(action,argument,result);
    }
    // usb-port off|printer|wifi: the one device on the Dana's external USB port.
    // The device already there stays plugged in unless force (plug in again after a load or reset).
    bool UsbPortControl(const string& choice,string& result,bool force=false) {
        if(!deviceTypeIsDana5(profile->deviceType)) { result="This device has no USB host port.";return false; }
        if(choice!="off" && choice!="printer" && choice!="wifi") { result="Use off, printer or wifi.";return false; }
        if(choice=="wifi" && !profile->capabilities.usbWifi) { result="This device's Wi-Fi is built in; use network-mode.";return false; }
        if(!force && choice==usbPort) { result=UsbPortState(); return true; }
        auto* dc=Isp();
        if(choice!="off" && !dc) { result="Dana controller unavailable.";return false; }
        if(choice!="printer") usbPrinter.detach();
        if(choice!="wifi") usbWifi.detach();
        if(choice=="printer") {
            usbPrinter.attach(*dc);
            if(pendingPrinter) { usbPrinter.printer.setBus(*pendingPrinter); pendingPrinter.reset(); }
        }
        if(choice=="wifi") {
            usbWifi.attach(*dc);
            if(pendingWifi) { usbWifi.adapter.load(pendingWifi->data(),pendingWifi->size()); pendingWifi.reset(); }
            if(pendingWifiBus) { usbWifi.adapter.setBus(*pendingWifiBus); pendingWifiBus.reset(); }
        }
        usbPort=choice;
        result=UsbPortState();
        return true;
    }
    // "off", "printer" and the printer's status, or "wifi" and the adapter's.
    string UsbPortState() const {
        if(usbPort=="printer") return "printer "+usbPrinter.status();
        if(usbPort=="wifi") return "wifi "+usbWifi.status();
        return "off";
    }
    // The built-in Wi-Fi module's radio on or off. It's on the bus while the board powers it (PK0).
    // The module already there stays unless force (plug in again after a load or reset).
    bool ModuleControl(bool on,string& result,bool force=false) {
        if(!profile->capabilities.builtInWifi) { result="This device has no built-in Wi-Fi.";return false; }
        if(!force && on==moduleOn && on==moduleWifi.attached()) { result=ModuleState(); return true; }
        auto* dc=Isp();
        if(on && !dc) { result="Dana controller unavailable.";return false; }
        moduleWifi.detach();
        if(on) {
            moduleWifi.attach(*dc);
            if(pendingModule) { moduleWifi.adapter.load(pendingModule->data(),pendingModule->size()); pendingModule.reset(); }
            if(pendingModuleBus) { moduleWifi.adapter.setBus(*pendingModuleBus); pendingModuleBus.reset(); }
        }
        moduleOn=on;
        result=ModuleState();
        return true;
    }
    // "off", or the module's power, link, address and counts.
    string ModuleState() const { return moduleOn && moduleWifi.attached() ? moduleWifi.status() : "off"; }
    // The network mode as the T|X names it: wireless while a Wi-Fi adapter is the port's device or
    // the built-in module's radio is on.
    string NetworkState() const {
        if(profile->capabilities.builtInWifi) return moduleOn ? "wireless" : "disabled";
        return usbPort=="wifi" ? "wireless" : "disabled";
    }
    EmRegsISPDana* Isp() const { return dynamic_cast<EmRegsISPDana*>(EmBankRegs::GetSubBank(0x1f000000,2)); }
    // The device a session was saved with on the external port.
    static string SessionUsbDevice(const SessionMetadata& m) { return !m.wifiAdapter.empty() ? "wifi" : m.printerBus ? "printer" : "off"; }
    // A session's MAC and device state. The device stays connected through the load only if the port
    // still has the same one; slirp starts again, so host connections drop.
    void UsbSessionLoaded(const SessionMetadata& loaded) {
        if(loaded.wifiMac) usbWifi.setMac(*loaded.wifiMac);
        if(loaded.moduleMac) moduleWifi.setMac(*loaded.moduleMac);
        pendingWifi.reset(); pendingWifiBus.reset(); pendingPrinter.reset();
        pendingModule.reset(); pendingModuleBus.reset();
        usbWifi.sessionLoaded();
        moduleWifi.sessionLoaded();
        if(!deviceTypeIsDana5(profile->deviceType)) return;
        // The module stays connected through the load when the session had its radio on too.
        if(profile->capabilities.builtInWifi) {
            if(moduleOn && !loaded.moduleAdapter.empty()) { pendingModule=loaded.moduleAdapter; pendingModuleBus=loaded.moduleBus; }
            else if(auto* dc=Isp()) dc->DropLoadedPort(1);
        }
        const string device=SessionUsbDevice(loaded);
        if(device!=usbPort) { if(auto* dc=Isp()) dc->DropLoadedPort(0); return; }
        if(device=="wifi") { pendingWifi=loaded.wifiAdapter; pendingWifiBus=loaded.wifiBus; }
        if(device=="printer") pendingPrinter=loaded.printerBus;
    }
    uint64_t UsbTime() const { return uint64_t(guestSeconds*1000000000.0); }
    void ServiceUsb() {
        usbBridge.Service();
        if(!gDebugger.IsStopped()) { usbWifi.poll(); moduleWifi.poll(); usbPrinter.poll(); }
        if(!gDebugger.IsStopped() && usbHooks.tick) usbHooks.tick(usbHooks.context,UsbTime());
        for(auto& m:usbBridge.TakeMessages()) cerr << m << endl;
    }
    void UsbReplaced() {
        if(usbHooks.replaced) usbHooks.replaced(usbHooks.context);
    }
    bool StartUsbDebug(const string& mode,string& error) {
        if(!deviceTypeIsDana5(profile->deviceType)) { error="USB debug role requires Dana.";return false; }
        UsbHostBridge::Config config;
        if(!HostSerial::Parse(mode,config)) { error="Invalid USB debug transport.";return false; }
        if(config.kind!=HostSerial::Kind::off && config.kind!=HostSerial::Kind::tcp) {
            error="USB debug role supports localhost TCP only.";return false;
        }
        StopUsb();
        usbDebugMode="off";
        if(config.kind==HostSerial::Kind::off) return true;
        auto* dc=dynamic_cast<EmRegsISPDana*>(EmBankRegs::GetSubBank(0x1f000000,2));
        if(!dc) { error="Dana controller unavailable.";return false; }
        usbDana=make_unique<UsbDanaPort>(*dc);
        if(!usbBridge.Start(UsbHostBridge::Backend{
            [this] { return usbDana->Port(); },
            [this](bool attached) { usbCable=attached;usbDana->SetCable(attached); },
            [this] { return usbCable; }, [] { return false; },
            [] { return gDebugger.IsStopped(); }, [this] { return UsbTime(); },
            [] {}, [this](const UsbHostHooks* hooks) { usbHooks=hooks?*hooks:UsbHostHooks{}; }
        },config,error)) {
            StopUsb();
            return false;
        }
        usbDebugMode=HostSerial::Format(usbBridge.GetConfig());
        return true;
    }
    void ServiceLinks() {
        if (serialLink.GetKind() == HostLink68k::Kind::off && irLink.GetKind() == HostLink68k::Kind::off &&
            !serialTransport->ModemMode() && !irTransport->PhoneMode() && !irTransport->PrinterMode())
            return;
        const bool on = gSession->IsPowerOn();
        irLink.SetAsleep(!on);
        serialLink.Poll();
        irLink.Poll();
        modemSyncLink.Poll();
        serialTransport->Service(on && EmHAL::GetLineDriverState(kUARTSerial));
        irTransport->Service(on && EmHAL::GetLineDriverState(kUARTIR));
    }

    string LinkState(const HostLink68k& link, const HostTransport68k& transport) const {
        return link.Describe() + (link.GetKind() == HostLink68k::Kind::off ? ""
                                  : string(", port ") + (transport.IsOpen() ? "open" : "closed") +
                                        ", rx " + to_string(link.Received()) + " tx " + to_string(link.Sent()));
    }

    bool OpenWav() {
        if (!options.audioOutput) return true;
        if (wav.Open(*options.audioOutput)) return wavOpen = true;
        cerr << "cannot write audio output " << *options.audioOutput << endl;
        return false;
    }

    // SDL audio for the window; without a device the tones still go to --audio-output.
    void OpenAudio() {
        if (audio) return;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            cerr << "no audio: " << SDL_GetError() << endl;
            return;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = Piezo68k::kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = uint16_t(options.audioPeriod);
        audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!audio) {
            cerr << "no audio device: " << SDL_GetError() << endl;
            return;
        }
        SDL_PauseAudioDevice(audio, 0);
    }

    // The piezo's samples since the last slice, to the WAV file and the audio device. The
    // device queue is kept between 40 and 250 ms; samples past that are dropped.
    void Sound() {
        const bool toDevice = soundOn && audio && !hidden;
        if (!wavOpen && !toDevice) return piezo.Render(gSession->GetSystemCycles(), gSession->GetClocksPerSecond(), nullptr);
        const bool playing = piezo.Playing();
        samples.clear();
        piezo.Render(gSession->GetSystemCycles(), gSession->GetClocksPerSecond(), &samples);
        if (wavOpen) wav.Write(samples);
        if (!toDevice || samples.empty()) return;
        const uint32_t queued = SDL_GetQueuedAudioSize(audio) / 2;
        if (!playing && !piezo.Playing() && queued == 0) return;
        if (queued > Piezo68k::kRate / 4) return;
        if (queued < Piezo68k::kRate / 50) {
            const vector<int16_t> lead(Piezo68k::kRate / 25 - queued, 0);
            SDL_QueueAudio(audio, lead.data(), uint32_t(lead.size() * 2));
        }
        SDL_QueueAudio(audio, samples.data(), uint32_t(samples.size() * 2));
    }

    void SetSound(bool on) {
        soundOn = on && profile->capabilities.sound;
        if (soundOn && window) OpenAudio();
        if (!on && audio) SDL_ClearQueuedAudio(audio);
    }

    string SoundState() const {
        char text[128];
        snprintf(text, sizeof(text), "%s, %s, tones %llu, last %.1f Hz", soundOn ? "on" : "off",
                 audio ? "device open" : "no device", static_cast<unsigned long long>(piezo.Changes()),
                 piezo.LastFrequency());
        return text;
    }

    uint64_t GuestMs() const { return uint64_t(guestSeconds * 1000); }

    void SetSpeed(Speed value) {
        speed = value;
        gSession->SetClockFactor(speed == Speed::original ? 1 : kUncappedFactor);
    }

    // Measured over about a second of wall time; one cycle is one 68000 clock.
    void UpdateSpeed(double wallMs) {
        if (wallMs - speedMark.first < 1000) return;
        const double cycles = double(gSession->GetSystemCycles()) - speedMark.second;
        measuredHz = cycles / ((wallMs - speedMark.first) / 1000);
        speedMark = {wallMs, double(gSession->GetSystemCycles())};
    }

    string SpeedState() const {
        char text[160];
        const double limit = gSession->GetClocksPerSecond();
        snprintf(text, sizeof(text), "%.2f MHz, limit %.2f MHz, %u%%, %s", measuredHz / 1e6, limit / 1e6,
                 limit > 0 ? unsigned(100 * measuredHz / limit + 0.5) : 0u,
                 speed == Speed::original ? "original" : "uncapped");
        return text;
    }

    bool ReplaceReplay(Replay replacement) {
        if(deviceTypeIsDana5(profile->deviceType)) {
            if(!danaInput.ReleaseSource(DanaSource5::replay,
                [](const auto& batch) { return EmHAL::DispatchDanaInput(batch); })) return false;
        } else ReleaseInput();
        replay=std::move(replacement); return true;
    }

    bool LoadReplay(const string& path) {
        string error;
        Replay replacement;
        if (!replacement.Load(path, lcd.Width(), InputHeight(), error, deviceTypeIsDana5(profile->deviceType))) {
            cerr << error << endl;
            cout << error << endl;
            return false;
        }
        if(!ReplaceReplay(std::move(replacement))) {
            cout << "input replay refused: keyboard busy" << endl; return false;
        }
        return true;
    }

    void DispatchReplay() {
        replay.Dispatch(GuestMs(), [&](const Replay::Event& event) {
            switch (event.kind) {
                case Replay::Kind::penDown: gSession->QueuePenEvent(PenEvent::down(event.x, event.y)); break;
                case Replay::Kind::penUp: gSession->QueuePenEvent(PenEvent::up()); break;
                case Replay::Kind::keyDown:
                case Replay::Kind::keyUp:
                    if(event.dana) return DanaKey(DanaSource5::replay,*event.dana,event.kind==Replay::Kind::keyDown);
                    Key(event.key, event.kind == Replay::Kind::keyDown); break;
                case Replay::Kind::snapshot: {
                    bool blank = false;
                    if (!SaveFrame(event.path, blank)) cerr << "cannot capture replay snapshot: " << event.path << endl;
                    else if (blank) cout << "replay snapshot lcd-off, saved a black frame: " << event.path << endl;
                    break;
                }
            }
            return true;
        });
    }

    bool DanaKey(DanaSource5 source,DanaScan5 scan,bool down) {
        return deviceTypeIsDana5(profile->deviceType) && danaInput.Key(source,scan,down,
            [](const auto& batch) { return EmHAL::DispatchDanaInput(batch); });
    }
    bool DanaText(const string& text) {
        return deviceTypeIsDana5(profile->deviceType) && danaInput.Text(text,
            [](const auto& batch) { return EmHAL::DispatchDanaInput(batch); });
    }

    string DanaInputCommand(const string& command,const string& argument) {
        if(!deviceTypeIsDana5(profile->deviceType)) return "unsupported-device";
        if(command=="dana-text") {
            if(!DanaInput5::TextTransitions(argument)) return "unsupported-text";
            return DanaText(argument)?"accepted":"busy";
        }
        auto scan=DanaScan5::Named(argument);
        if(!scan) return "unsupported-key";
        return DanaKey(DanaSource5::console,*scan,command=="dana-down")?"accepted":"busy";
    }

    // Every source of a key (mouse, keyboard, fingers) goes through here; the key is pressed by the
    // first holder and released by the last.
    void Key(KeyId key, bool down) {
        if(deviceTypeIsDana5(profile->deviceType)) {
            if(auto scan=danaForLegacyKey(key)) {
                if(down)FlushDanaHost();
                DanaKey(DanaSource5::screen,*scan,down);
            }
            return;
        }
        if (down ? !keyHolds.Press(key) : !keyHolds.Release(key)) return;
        SetKey(key, down);
    }

    void SetKey(KeyId key, bool down) {
        const auto button = buttonForKey(key);
        if (!button) return;
        gSession->QueueButtonEvent(ButtonEvent(*button, down ? ButtonEvent::Type::press : ButtonEvent::Type::release));
        if (down) pressed |= deviceKeyBit(key);
        else pressed &= ~deviceKeyBit(key);
    }

    // The LCD as a PNG; black with the LCD off, as the ARM runner does.
    bool SaveFrame(const string& path, bool& blank) {
        blank = !lcd.Update();
        vector<uint8_t> black;
        const void* pixels = lcd.Pixels();
        if (blank) {
            black.assign(lcd.Width() * lcd.Height() * 4, 0);
            for (size_t i = 3; i < black.size(); i += 4) black[i] = 0xff;
            pixels = black.data();
        }
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<void*>(pixels), lcd.Width(),
                                                                  lcd.Height(), 32, lcd.Width() * 4,
                                                                  SDL_PIXELFORMAT_RGBA32);
        if (!surface) return false;
        const bool saved = IMG_SavePNG(surface, path.c_str()) == 0;
        SDL_FreeSurface(surface);
        return saved;
    }

    // The whole window as drawn: LCD, silkscreen and button bar.
    bool SaveWindow(const string& path) {
        if (!renderer) return false;
        Draw(false);
        // Reads are relative to the viewport, so read the whole output.
        SDL_RenderSetScale(renderer, 1, 1);
        SDL_RenderSetViewport(renderer, nullptr);
        int width = 0, height = 0;
        if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0) return false;
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surface) return false;
        const bool saved = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, surface->pixels,
                                                surface->pitch) == 0 &&
                           IMG_SavePNG(surface, path.c_str()) == 0;
        SDL_FreeSurface(surface);
        SDL_RenderPresent(renderer);
        return saved;
    }

    int Finish() {
        if (wavOpen && !wav.Close()) {
            cerr << "failed to write audio output" << endl;
            return 1;
        }
        wavOpen = false;
        if (options.framebufferOutput) {
            bool blank = false;
            if (!SaveFrame(*options.framebufferOutput, blank)) {
                cerr << "failed to save framebuffer: " << SDL_GetError() << endl;
                return 1;
            }
            if (blank) cout << "framebuffer output with the LCD off, saved a black frame" << endl;
        }
        return 0;
    }

    // Console commands, one per line; arguments may be quoted.
    void Command(const string& line) {
        istringstream words(line);
        string command;
        if (!(words >> command)) return;
        string requested;
        auto position=words.tellg();
        words >> quoted(requested); words.clear(); words.seekg(position);
        if (auto refusal=peripheralRefusal68k(*profile,command,requested); !refusal.empty())
            return void(cout << "unsupported: " << refusal << endl);
        auto argument = [&](string& value) { return bool(words >> quoted(value)) && !value.empty(); };
        string path;
        if (command.rfind("sd-",0)==0 || command.rfind("sd2-",0)==0) {
            string args; getline(words,args);
            const bool second=command.rfind("sd2-",0)==0;
            const auto begin=args.find_first_not_of(" ");
            args=begin==string::npos ? "" : args.substr(begin);
            cout << "card: " << runtimeCards.Command(second,command.substr(second?4:3),args) << endl;
        } else if(command=="dana-view"||command=="dana-view-state") {
            if(!deviceTypeIsDana5(profile->deviceType))return void(cout<<"unsupported: Dana view"<<endl);
            if(command=="dana-view") {
                string value,error,extra;
                if(!(words>>quoted(value))||(words>>extra)||!SetDanaView(value,error))
                    return void(cout<<"Dana view refused: "<<error<<endl);
            }
            cout<<DanaViewDescription()<<endl;
        } else if (command == "dana-down" || command == "dana-up" || command == "dana-text") {
            string value,extra;
            if(!(words >> quoted(value)) || (words >> extra)) return void(cout << "input refused: argument" << endl);
            const auto result=DanaInputCommand(command,value);
            cout << (result=="accepted"?"input accepted":"input refused: "+result) << endl;
        } else if (command == "quit") {
            ReleaseInput();
            quit = true;
        } else if (command == "export-portable" || command == "copy-session" || command == "duplicate-session") {
            if(!runtimeCards.Slots()) return void(cout << "unsupported: This command is for independent cards." << endl);
            if(!argument(path)) return void(cout << "usage: " << command << " <new-folder>" << endl);
            const auto result=savePortable(path,*profile,metadata);
            cout << (result==PortablePublication::durable?"wrote portable folder ":
                result==PortablePublication::publishedUncertain?"portable export error: output published, durability uncertain ":
                "portable export refused: new folder and managed media required ") << path << endl;
        } else if (command == "peek-session") {
            if(!argument(path)) return;
            vector<uint8_t> data; string folder; CheckedSession checked; RuntimeCards::Pair pair;
            string error=readSessionInput(path,data,folder)?checkSession(data,profile->name,checked,folder):"cannot read session";
            if(error.empty()) error=runtimeCards.Stage(checked.manifest,pair);
            if(error.empty()) error=checkCardState(checked,pair);
            if(!error.empty()) cout << "peek refused: " << error << endl;
            else cout << "session media: " << bool(pair.card[0])+bool(pair.card[1]) << " SD cards; slots SD0="
                      << (pair.card[0]?(pair.inserted[0]?"inserted":"ejected"):"empty") << " SD2="
                      << (pair.card[1]?(pair.inserted[1]?"inserted":"ejected"):"empty") << endl;
        } else if (command == "save-session") {
            if (!argument(path)) return void(cout << "usage: save-session <file>" << endl);
            vector<uint8_t> data;
            if (!serializeSession(*profile, Saving(), data) || !writeFile(path, data.data(), data.size()))
                cout << "failed to save session" << endl;
            else
                cout << "wrote session to " << path << (runtimeCards.HasCards()?" (linked external SD backings; not portable)":"") << endl;
        } else if (command == "load-session" || command == "import-portable") {
            if (!argument(path)) return void(cout << "usage: load-session <file>" << endl);
            LoadSession(path);
        } else if (command == "reset" || command == "reset-hard" || command == "reset-noext") {
            ReleaseInput();
            UsbReplaced();
            irTransport->DeviceReset();
            gSession->Reset(command == "reset"        ? EmSession::ResetType::soft
                            : command == "reset-hard" ? EmSession::ResetType::hard
                                                      : EmSession::ResetType::noext);
            piezo.Restart(gSession->GetSystemCycles());
            cout << command << " done" << endl;
        } else if (command == "input-replay") {
            if (!argument(path)) return void(cout << "usage: input-replay <file>" << endl);
            if (LoadReplay(path)) cout << "input replay loaded" << endl;
        } else if (command == "snapshot") {
            if (!argument(path)) return void(cout << "usage: snapshot <file>" << endl);
            bool blank = false;
            if (!SaveFrame(path, blank)) cout << "snapshot failed" << endl;
            else cout << (blank ? "snapshot lcd-off: " : "snapshot saved: ") << path << endl;
        } else if (command == "window-snapshot") {
            if (!argument(path)) return void(cout << "usage: window-snapshot <file>" << endl);
            cout << (SaveWindow(path) ? "window snapshot saved: " : "window snapshot failed: ") << path << endl;
        } else if (command == "hidden") {
            // For tests: the state SDL's background event sets on Android.
            string value;
            if (words >> value) {
                if (value != "on" && value != "off") return void(cout << "usage: hidden [on|off]" << endl);
                hidden = value == "on";
                if (hidden) ReleaseInput();
            }
            cout << "hidden: " << (hidden ? "on" : "off") << endl;
        } else if (command == "cycles") {
            cout << "cycles: " << gSession->GetSystemCycles() << endl;
        } else if (command == "audio-on" || command == "audio-off") {
            SetSound(command == "audio-on");
            cout << "sound: " << SoundState() << endl;
        } else if (command == "sound-state") {
            cout << "sound: " << SoundState() << endl;
        } else if (command == "irprinter") {
            string action,argument,result;words >> action;getline(words >> ws,argument);
            bool ok=irTransport->PrinterControl().Command(irTransport->Printer(),action,argument,result);
            cout << "irprinter: " << (ok?"":"error ") << result << endl;
        } else if (command == "printer") {
            string action,argument,result;words >> action;getline(words >> ws,argument);
            bool ok=PrinterControl(action,argument,result);
            cout << "printer: " << (ok?"":"error ") << result << endl;
        } else if (command == "usb-port") {
            string choice,result;
            if(!(words >> choice)) { cout << "usb-port: " << (deviceTypeIsDana5(profile->deviceType) ? UsbPortState() : "off") << endl; return; }
            bool ok=UsbPortControl(choice,result);
            cout << "usb-port: " << (ok?"":"error ") << result << endl;
        } else if (command == "wifi-module") {
            string choice,result;
            if(!(words >> choice)) { cout << "wifi-module: " << ModuleState() << endl; return; }
            if(choice!="on" && choice!="off") { cout << "wifi-module: error Use on or off." << endl; return; }
            bool ok=ModuleControl(choice=="on",result);
            cout << "wifi-module: " << (ok?"":"error ") << result << endl;
        } else if (command == "usb-debug-mode") {
            string mode,error;words >> mode;
            if(!StartUsbDebug(mode,error)) cout << "usb-debug: error " << error << endl;
            else cout << "usb-debug: " << usbBridge.Describe() << endl;
        } else if (command == "serial-mode" || command == "irda-mode") {
            const bool serial = command == "serial-mode";
            string mode, error;
            if (!(words >> mode))
                return void(cout << "usage: " << command << " off|pty|tcp:PORT|unix:PATH" << (serial ? "|modem" : "|phone|printer") << endl);
            if (serial ? !SetSerial(mode, error) : !SetInfrared(mode, error))
                return void(cout << (serial ? "serial: " : "infrared: ") << "error " << error << endl);
            cout << (serial ? "serial: " + SerialState() : "infrared: " + InfraredState()) << endl;
        } else if (command == "serial-state") {
            cout << "serial: " << SerialState() << endl;
        } else if (command == "modem-sync") {
            string mode, error;
            if (!(words >> mode)) return void(cout << "usage: modem-sync off|pty|tcp:PORT|unix:PATH" << endl);
            if (!SetModemSync(mode, error)) return void(cout << "modem-sync: error " << error << endl);
            cout << "modem-sync: " << modemSyncLink.Describe() << endl;
        } else if (command == "irda-state") {
            cout << "infrared: " << InfraredState() << endl;
        } else if (command == "irda-stats") {
            cout << "infrared: " << (irTransport->PhoneMode() || irTransport->PrinterMode() ? irTransport->Stats() : "off") << endl;
        } else if (command == "hotsync") {
            // The cradle's HotSync button, pressed and let go.
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::cradle, ButtonEvent::Type::press));
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::cradle, ButtonEvent::Type::release));
            cout << "hotsync: pressed" << endl;
        } else if (command == "gdb-start") {
            // gdb-start [port]: the GDB stub on 127.0.0.1, 2159 by default.
            string text;
            long port = 2159;
            if (words >> text) {
                char* end = nullptr;
                port = strtol(text.c_str(), &end, 10);
                if (*end || port < 1 || port > 65535) return void(cout << "usage: gdb-start [port]" << endl);
            }
            StartGdb(uint16_t(port));
            if (gdb) cout << "gdb: " << GdbState() << " on 127.0.0.1:" << port << endl;
        } else if (command == "gdb-stop") {
            if (gdb) gdb->Stop();
            gdb.reset();
            gDebugger.SetBreakMode(Debugger::BreakMode::all);
            cout << "gdb: off" << endl;
        } else if (command == "gdb-state") {
            cout << "gdb: " << GdbState() << endl;
        } else if (command == "gdb-app") {
            // gdb-app <ELF or COFF> [database]: relocates the app's symbols to where its code is.
            string name;
            if (!argument(path)) return void(cout << "usage: gdb-app <executable> [database]" << endl);
            if (!gdb) return void(cout << "gdb: start the stub first" << endl);
            const bool named = argument(name);
            vector<uint8_t> data;
            if (!readFile(path, data)) return void(cout << "gdb: cannot read " << path << endl);
            debug_support::SetApp(data.data(), data.size(), named ? name.c_str() : nullptr, *gdb, gDebugger);
            cout << "gdb-app: done" << endl;
        } else if (command == "backlight-state") {
            cout << "backlight: " << (gSession->IsPowerOn() && EmHAL::GetLCDBacklightOn() ? "on" : "off") << endl;
        } else if (command == "speed-state") {
            cout << "speed: " << SpeedState() << endl;
        } else if (command == "bus-wait") {
            // Wait states per byte/word bus cycle, from the chip selects as the guest set them.
            const EmAddressBank& rom = EmMemGetBank(EmBankROM::GetMemoryStart());
            const EmAddressBank& ram = EmMemGetBank(0);
            cout << "bus wait: rom " << int(rom.waitByte) << "/" << int(rom.waitWord) << ", ram "
                 << int(ram.waitByte) << "/" << int(ram.waitWord);
            // SDRAM waits by page instead (EmBusTiming.h): CAS latency, continuous page mode,
            // the address bits within a row and the bank address bits (255 for none).
            if (ram.sdram) {
                const EmSdramTiming& sdram = EmSdram::Get();
                cout << ", sdram cl " << (sdram.casLatency2 ? 2 : 1) << " cpm " << int(sdram.continuousPage)
                     << " page " << int(sdram.pageShift) << " bank " << int(sdram.bankHigh) << "/"
                     << int(sdram.bankLow);
            }
            cout << endl;
        } else if (command == "bus-timing") {
            // LCD DMA and DRAM refresh as the guest programmed them, in system clocks.
            const EmBusSteal& steal = EmBusTiming::Get();
            cout << "bus timing: lcd line " << steal.lcdLineClocks << " dma " << steal.lcdDmaClocks
                 << ", refresh period " << steal.refreshPeriod << " hold " << steal.refreshClocks << endl;
            // The LCD frame rate those give: the line times LYMAX + 1 lines.
            const uint16 lines = EmMemGet16(0xfffffa0a) + 1;
            if (steal.lcdLineClocks) {
                char frame[64];
                snprintf(frame, sizeof(frame), "lcd frame: %.1f Hz",
                         double(EmHAL::GetSystemClockFrequency()) / (double(steal.lcdLineClocks) * lines));
                cout << frame << endl;
            }
            // The DragonBall registers it comes from (same addresses on the EZ and VZ, except LRRA:
            // 8 bits at $FA29 on the EZ, 10 bits at $FA28 on the VZ).
            const bool vz = gSession->GetDevice().Supports68VZ328();
            char regs[160];
            snprintf(regs, sizeof(regs),
                     "bus registers: pllcr %04x csd %04x drammc %04x dramc %04x lpicf %02x lpxcd %02x lckcon %02x "
                     "lrra %0*x lxmax %u lymax %u",
                     EmMemGet16(0xfffff200), EmMemGet16(0xfffff116), EmMemGet16(0xfffffc00), EmMemGet16(0xfffffc02),
                     EmMemGet8(0xfffffa20), EmMemGet8(0xfffffa25), EmMemGet8(0xfffffa27), vz ? 4 : 2,
                     vz ? EmMemGet16(0xfffffa28) : EmMemGet8(0xfffffa29), EmMemGet16(0xfffffa08),
                     EmMemGet16(0xfffffa0a));
            cout << regs << endl;
            if (vz) {
                // VZ only: SDRAM control, LCD DMA control, chip select A and CSCTRL1.
                snprintf(regs, sizeof(regs), "bus registers vz: sdctrl %04x dmacr %02x csa %04x csctrl1 %04x",
                         EmMemGet16(0xfffffc04), EmMemGet8(0xfffffa39), EmMemGet16(0xfffff110),
                         EmMemGet16(0xfffff10a));
                cout << regs << endl;
            }
        } else if (command == "speed") {
            string value;
            words >> value;
            if (value != "original" && value != "uncapped") return void(cout << "usage: speed original|uncapped" << endl);
            SetSpeed(value == "original" ? Speed::original : Speed::uncapped);
            cout << "speed: " << value << endl;
        } else if (command == "display") {
            Display(words);
        } else if (command == "launch") {
            string name;
            if (!argument(name)) return void(cout << "usage: launch <app name>" << endl);
            Guest([name] {
                cout << (gSession->LaunchAppByName(name) ? "launch: started " : "launch: could not start ") << name
                     << endl;
            }, [name](const string& why) { cout << "launch: could not start " << name << ": " << why << endl; });
        } else if (command == "db-list") {
            // One line per RAM database: type, creator, attributes in hex and the name.
            Guest([] {
                DatabaseInfoList databases;
                if (!GetDatabases(databases, GetDatabaseFlags::kOnlyRamDatabases))
                    return void(cout << "db-list: Palm OS is not ready for the database list" << endl);
                for (const auto& database : databases) {
                    char fields[48];
                    snprintf(fields, sizeof(fields), "db: %s %s %04x ", fourCc(database.type).c_str(),
                             fourCc(database.creator).c_str(), unsigned(database.dbAttrs));
                    cout << fields << printable(database.dbName, sizeof(database.dbName)) << endl;
                }
                cout << "db-list: " << databases.size() << " RAM databases" << endl;
            }, [](const string& why) { cout << "db-list: " << why << endl; });
        } else if (command == "db-export" || command == "db-export-all") {
            // A zip of the RAM databases, with the ROM ones too for db-export-all.
            if (!argument(path)) return void(cout << "usage: " << command << " <zip file>" << endl);
            const bool all = command == "db-export-all";
            Guest([path, all] {
                vector<uint8_t> archive;
                size_t count = 0, failed = 0;
                if (!backupDatabases(all, archive, count, failed))
                    return void(cout << "export failed: Palm OS couldn't list its databases" << endl);
                if (!writeFile(path, archive.data(), archive.size())) return void(cout << "export failed: cannot write " << path << endl);
                cout << "export written to " << path << ": " << count << " databases";
                if (failed) cout << ", " << failed << " failed";
                cout << endl;
            }, [](const string& why) { cout << "export failed: " << why << endl; });
        } else if (command == "db-export-db") {
            // One database by name as a PDB or PRC file.
            string name;
            if (!argument(name) || !argument(path)) return void(cout << "usage: db-export-db <name> <file>" << endl);
            Guest([name, path] {
                vector<uint8_t> data;
                if (const auto error = exportDatabase(name, data); !error.empty()) return void(cout << "export failed: " << error << endl);
                if (!writeFile(path, data.data(), data.size())) return void(cout << "export failed: cannot write " << path << endl);
                cout << "export written to " << path << ": " << data.size() << " bytes" << endl;
            }, [](const string& why) { cout << "export failed: " << why << endl; });
        } else if (command == "install") {
            if (!argument(path)) return void(cout << "usage: install <file>" << endl);
            vector<uint8_t> data;
            if (!readFile(path, data)) return void(cout << "unable to read " << path << endl);
            Guest([data = std::move(data)]() mutable {
                const auto result = installDatabase(data);
                cout << (result == DbInstaller::Result::success       ? "installation successful"
                         : result == DbInstaller::Result::needsReboot ? "installation successful, needs reset"
                                                                      : "installation failed")
                     << endl;
            }, [](const string& why) { cout << "installation failed: " << why << endl; });
        } else {
            cout << "unknown command: " << command << endl;
        }
    }

    // The desktop control socket: one request at a time, the machine's state changes pushed as
    // events. After quit, the core leaves once the reply is sent.
    void PollDesktop() {
        StateChanged("serial", lastSerial, SerialDescription());
        StateChanged("infrared", lastInfrared, InfraredDescription());
        DesktopChannel::Request request;
        // One request at a time: the next waits while one waits for the guest's event loop.
        switch (guestCall || deferredDelete ? DesktopChannel::Poll::idle : desktop->Receive(request)) {
            case DesktopChannel::Poll::closed: return DesktopLost();
            case DesktopChannel::Poll::handled:
            case DesktopChannel::Poll::idle: break;
            case DesktopChannel::Poll::request: {
                // Window events queued before the request go first, so a focus loss lifts the pen
                // before a save.
                if (window) PollWindow();
                // Before the UI is up the command itself answers not-ready.
                if (GuestCommand(request.command) && gSystemState.IsUIInitialized()) {
                    Guest([this, request] {
                        Reply result;
                        try {
                            result = DesktopCommand(request.command, request.argument);
                        } catch (const exception& error) {
                            result = {false, "failed", error.what()};
                        }
                        // An application delete waiting for the launcher to quit answers later.
                        if (result.code == "pending") {
                            deferredDelete = DeferredDelete{request.id, guestSeconds + kGuestCallSeconds};
                            return;
                        }
                        desktop->Reply(request.id, result.ok, result.code, result.message);
                    }, [this, id = request.id](const string& why) {
                        desktop->Reply(id, false, "not-ready", "Palm OS can't take the call now: " + why + ".");
                    });
                    break;
                }
                Reply result;
                try {
                    result = DesktopCommand(request.command, request.argument);
                } catch (const exception& error) {
                    result = {false, "failed", error.what()};
                }
                desktop->Reply(request.id, result.ok, result.code, result.message);
                break;
            }
        }
        if (!desktop->Flush()) DesktopLost();
        else if (desktopQuit && desktop->OutputEmpty()) quit = true;
    }

    // The socket closed. When the desktop went away without asking to quit, the session is saved
    // where its close would have saved it, between emulation steps like a save request. A host
    // call still waiting for the event loop is dropped.
    void DesktopLost() {
        quit = true;
        if (!options.saveOnDisconnect || desktopQuit || !desktop->Lost() || desktop->QuitPending()) return;
        vector<uint8_t> data;
        saveFailed = !serializeSession(*profile, Saving(), data) ||
                     !writeFile(*options.saveOnDisconnect, data.data(), data.size());
        if (saveFailed)
            cerr << "desktop: the launcher went away and the session could not be saved" << endl;
        else
            cout << "desktop: the launcher went away, session saved to " << *options.saveOnDisconnect << endl;
    }

    // Pushes a link's state when it changes; clients that never use the link never see it.
    void StateChanged(const char* name, string& last, const string& state) {
        if (state == last || (state == "off" && last.empty())) return;
        last = state;
        desktop->Event(name, state);
    }

    // "off", "off <error>" after a failed change, or "pty|tcp|unix <endpoint> connected|waiting".
    string SerialDescription() const {
        return serialTransport->ModemMode() ? ModemState() : LinkDescription(serialLink, serialError);
    }

    string InfraredDescription() const {
        return irTransport->PhoneMode() || irTransport->PrinterMode() ? irTransport->Describe()
                                                                      : LinkDescription(irLink, infraredError);
    }

    static string LinkDescription(const HostLink68k& link, const string& error) {
        const auto state = link.Describe();
        return state == "off" && !error.empty() ? "off " + error : state;
    }

    struct Reply {
        bool ok{false};
        string code{"failed"}, message;
    };

    // The desktop commands that call into Palm OS, so run at the guest's event loop.
    static bool GuestCommand(const string& command) {
        return command == "install" || command == "export" || command == "export-db" || command == "db-list" ||
               command == "launch-app" || command == "delete-db";
    }

    static Reply DeleteReply(EmPalmOS::DeleteResult result) {
        switch (result) {
            case EmPalmOS::DeleteResult::deleted: return {true, "deleted", "Database deleted."};
            case EmPalmOS::DeleteResult::pending: return {false, "pending", ""};
            case EmPalmOS::DeleteResult::notReady:
                return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
            case EmPalmOS::DeleteResult::missing: return {false, "missing", "There is no Palm database with that name."};
            case EmPalmOS::DeleteResult::rom: return {false, "rom", "That database is in ROM and can't be deleted."};
            case EmPalmOS::DeleteResult::running: return {false, "running", "That application is running. Switch to another one first."};
            case EmPalmOS::DeleteResult::open: return {false, "open", "That database is open. Close its application first."};
            default: return {false, "delete", "The database could not be deleted."};
        }
    }

    // The same commands, codes and replies as the ARM runner's DesktopControl. What a IIIx doesn't
    // have answers "unsupported", or reports it absent for the state queries the desktop asks every
    // device.
    Reply DesktopCommand(const string& command, const string& argument) {
        if (command == "export-portable" || command == "copy-session" || command == "duplicate-session") {
            if (!runtimeCards.Slots())
                return {false, "unsupported", "This command is for independent cards."};
            if (argument.empty() || argument.front() != '/' || argument.find_first_of("\r\n") != string::npos || argument.find('\0') != string::npos)
                return {false, "argument", "An absolute new folder path is required."};
            const auto result = savePortable(argument, *profile, metadata);
            if (result == PortablePublication::durable)
                return {true, "portable", runtimeCards.Slots() > 1 ? "Portable folder saved with both card slots." :
                                                                   "Portable folder saved with its card."};
            if (result == PortablePublication::publishedUncertain)
                return {false, "durability-uncertain", "Output published, but its directory flush failed. Keep it and verify it before use."};
            return {false, "portable", "Portable export refused. A new folder and managed cards are required."};
        }
        if(command=="keyboard-grab") {
            if(argument!="on"&&argument!="off")return {false,"argument","Use on or off."};
            SetKeyboardGrab(argument=="on");
            return {true,"keyboard-grab",string(keyboardGrabWanted?"on":"off")+(keyboardGrabbed?" grabbed":"")};
        }
        if(command=="dana-view"||command=="dana-view-state") {
            if(!deviceTypeIsDana5(profile->deviceType))return {false,"unsupported","This device has no Dana view."};
            string error;
            if(command=="dana-view-state"&&!argument.empty())return {false,"argument","This command takes no argument."};
            if(command=="dana-view"&&!SetDanaView(argument,error))return {false,"argument",error};
            return {true,"dana-view",DanaViewDescription()};
        }
        if ((command.rfind("sd-",0)==0 || command.rfind("sd2-",0)==0) && runtimeCards.Slots()) {
            const bool second=command.rfind("sd2-",0)==0;
            auto reply=runtimeCards.Command(second,command.substr(second?4:3),argument);
            return {reply.rfind("error:",0)!=0,"card",reply};
        }
        if (auto refusal=peripheralRefusal68k(*profile,command,argument); !refusal.empty())
            return {false,"unsupported",refusal};
        static const char* const fileCommands[] = {"save", "install", "export", "export-db", "input-replay"};
        static const char* const argumentCommands[] = {
            "launch-app",  "delete-db", "serial-mode", "irda-mode", "display",   "hold",           "keyboard-attach",
            "keyboard-key", "network-mode", "sound-mode", "speed", "microphone-mode", "usb-mode",
            "bluetooth-mode", "gdb-start", "usb-port", "printer", "irprinter"};
        const bool card = command.rfind("sd-", 0) == 0 || command.rfind("ms-", 0) == 0;
        if(command=="dana-down" || command=="dana-up" || command=="dana-text") {
            const auto result=DanaInputCommand(command,argument);
            return {result=="accepted",result,"Dana input: "+result};
        }
        bool fileCommand = false, takesArgument = card;
        for (const char* name : fileCommands) fileCommand = fileCommand || command == name;
        for (const char* name : argumentCommands) takesArgument = takesArgument || command == name;
        if (fileCommand && (argument.empty() || argument.front() != '/'))
            return {false, "argument", "An absolute file path is required."};
        if (!fileCommand && !takesArgument && !argument.empty())
            return {false, "argument", "This command takes no argument."};
        const bool ready = gSystemState.IsUIInitialized();
        const Reply notReady{false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};

        if (command == "device-state") return {true, "device", profile->name};
        if (command == "window-id") {
            const string id = x11WindowId(window);
            if (id.empty()) return {false, "unsupported", "The window is not an X11 window."};
            return {true, "window", id};
        }
        if (command == "guest-ready") {
            if (!ready) return {false, "not-ready", "Palm OS has not reached a supported application boundary."};
            return {true, "guest-ready", "Palm OS can accept application commands."};
        }
        if (command == "replay-state")
            return {true, replay.Finished() ? "finished" : "running", to_string(replay.LastEventMs())};
        if (command == "input-replay") {
            string error;
            Replay replacement;
            if (!replacement.Load(argument, lcd.Width(), InputHeight(), error, deviceTypeIsDana5(profile->deviceType))) return {false, "replay", error};
            if(!ReplaceReplay(std::move(replacement))) return {false,"busy","Dana keyboard busy."};
            return {true, "loaded", "Input replay loaded."};
        }
        if (command == "save") {
            vector<uint8_t> data;
            if (!serializeSession(*profile, Saving(), data) || !writeFile(argument, data.data(), data.size()))
                return {false, "save", "Could not save device state."};
            return {true, "saved", runtimeCards.HasCards()?"Linked session saved. Keep both external SD backings and their generation files.":"Session saved."};
        }
        if (command == "install") {
            vector<uint8_t> data;
            if (!readFile(argument, data)) return {false, "read", "Could not read the application database."};
            if (!ready) return {false, "not-ready", "Wait for Palm OS to finish starting, then try again."};
            switch (installDatabase(data)) {
                case DbInstaller::Result::success: return {true, "installed", "Application installed."};
                case DbInstaller::Result::needsReboot:
                    return {true, "needs-reset", "Installed. Reset the Palm to finish installation."};
                case DbInstaller::Result::failureDbIsCorrupt: return {false, "corrupt", "The Palm database is corrupt."};
                case DbInstaller::Result::failureNotEnoughMemory:
                    return {false, "memory", "The Palm does not have enough memory."};
                case DbInstaller::Result::failureDbIsOpen:
                    return {false, "open", "Close this application on the Palm before replacing it."};
                case DbInstaller::Result::failedCouldNotOverwrite:
                    return {false, "overwrite", "The existing database could not be replaced."};
                default: return {false, "install", "Application installation failed."};
            }
        }
        if (command == "export" || command == "export-db") {
            string destination = argument, database;
            if (command == "export-db") {
                const auto split = argument.find('\n');
                if (split == string::npos || split == 0 || split + 1 == argument.size() ||
                    argument.size() - split - 1 > 31 || argument.find('\n', split + 1) != string::npos)
                    return {false, "argument", "Choose an absolute ZIP path and one Palm database name."};
                destination = argument.substr(0, split);
                database = argument.substr(split + 1);
            }
            if (!ready) return notReady;
            vector<uint8_t> archive;
            if (database.empty()) {
                size_t count = 0, failed = 0;
                if (!backupDatabases(false, archive, count, failed)) return notReady;
            } else if (const auto error = exportDatabaseZip(database, archive); !error.empty()) {
                if (error == "not-ready") return notReady;
                if (error == "missing") return {false, "missing", "There is no Palm database with that name."};
                return {false, "export", "The database could not be exported."};
            }
            if (!writeFile(destination, archive.data(), archive.size()))
                return {false, "write", "Could not write the export file."};
            return {true, "exported", database.empty() ? "RAM databases exported." : "Database exported."};
        }
        if (command == "db-list") {
            // One line per RAM database: type, creator and attributes in hex, then the hex name.
            DatabaseInfoList databases;
            if (!ready || !GetDatabases(databases, GetDatabaseFlags::kOnlyRamDatabases)) return notReady;
            string text;
            for (const auto& database : databases) {
                char fields[32];
                snprintf(fields, sizeof(fields), "%08x %08x %04x ", unsigned(database.type),
                         unsigned(database.creator), unsigned(database.dbAttrs & 0xffff));
                text += fields + DesktopChannel::Encode(string(database.dbName, strnlen(database.dbName, sizeof(database.dbName)))) + "\n";
            }
            cout << "db-list: " << databases.size() << " RAM databases" << endl;
            return {true, "databases", text};
        }
        if (command == "launch-app") {
            if (argument.empty() || argument.size() > 31)
                return {false, "argument", "Enter a Palm application database name (up to 31 bytes)."};
            if (!ready) return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
            if (!gSession->LaunchAppByName(argument))
                return {false, "launch",
                        "Could not launch that application. Check its database name and wait for Palm OS to finish "
                        "starting."};
            return {true, "launched", "Application launched."};
        }
        if (command == "delete-db") {
            if (argument.empty() || argument.size() > 31)
                return {false, "argument", "Enter a Palm database name (up to 31 bytes)."};
            if (!ready) return notReady;
            return DeleteReply(EmPalmOS::DeleteDatabaseByName(argument));
        }
        if (command == "reset" || command == "hard-reset") {
            ReleaseInput();
            UsbReplaced();
            irTransport->DeviceReset();
            gSession->Reset(command == "reset" ? EmSession::ResetType::soft : EmSession::ResetType::hard);
            piezo.Restart(gSession->GetSystemCycles());
            return {true, "done", "Done."};
        }
        if (command == "hotsync") {
            // The cradle's HotSync button, pressed and let go.
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::cradle, ButtonEvent::Type::press));
            gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::cradle, ButtonEvent::Type::release));
            return {true, "hotsync", "HotSync button pressed."};
        }
        if (command == "serial-state") return {true, "serial", SerialDescription()};
        if (command == "irda-state") return {true, "infrared", InfraredDescription()};
        if (command == "serial-mode" || command == "irda-mode") {
            const bool serial = command == "serial-mode";
            HostLink68k::Kind kind;
            uint16_t port;
            string path;
            if (serial && argument == "modem") {
                serialError.clear();
                if (!profile->capabilities.modem) return {false, "unsupported", string(profile->displayName) + " has no modem."};
                if (!SetSerial(argument, serialError)) return {false, "serial", serialError};
                return {true, "serial", SerialDescription()};
            }
            if (!serial && argument == "phone") {
                infraredError.clear();
                if (!profile->capabilities.irPhone)
                    return {false, "unsupported", string(profile->displayName) + " has no infrared phone."};
                if (!RomHasIrComm()) return {false, "unsupported", "The infrared phone needs Palm OS 3.3 or later."};
                if (!SetInfrared(argument, infraredError)) return {false, "infrared", infraredError};
                return {true, "infrared", InfraredDescription()};
            }
            if (!serial && argument == "printer") {
                infraredError.clear();
                if (!SetInfrared(argument, infraredError)) return {false, "infrared", infraredError};
                return {true, "infrared", InfraredDescription()};
            }
            if (!HostLink68k::Parse(argument, kind, port, &path))
                return {false, "argument", serial ? "Use off, pty, tcp:PORT, unix:PATH or modem." : "Use off, pty, tcp:PORT, unix:PATH, phone or printer."};
            HostLink68k& link = serial ? serialLink : irLink;
            string& error = serial ? serialError : infraredError;
            error.clear();
            if (serial) serialTransport->SetModemMode(false, error);
            else {
                irTransport->SetPhoneMode(false, error);
                irTransport->SetPrinterMode(false);
            }
            // A failed open leaves the link off, and its state shows why.
            if (!link.Open(kind, port, error, path)) return {false, serial ? "serial" : "infrared", error};
            return {true, serial ? "serial" : "infrared", link.Describe()};
        }
        if (command == "serial-stats") return {true, "serial", serialTransport->Stats()};
        if (command == "modem-sync") {
            if (!profile->capabilities.modem) return {false, "unsupported", string(profile->displayName) + " has no modem."};
            string error;
            if (argument.empty()) return {true, "modem-sync", modemSyncLink.Describe()};
            if (!SetModemSync(argument, error)) return {false, "argument", "Use off, pty, tcp:PORT or unix:PATH. " + error};
            return {true, "modem-sync", modemSyncLink.Describe()};
        }
        if (command == "irda-stats") {
            if (irTransport->PhoneMode() || irTransport->PrinterMode()) return {true, "infrared", irTransport->Stats()};
            return {true, "infrared", "rx=" + to_string(irLink.Received()) + " tx=" + to_string(irLink.Sent()) +
                                          " port=" + (irTransport->Host().IsOpen() ? "open" : "closed")};
        }
        if (command == "speed-state") return {true, "speed", SpeedState()};
        if (command == "speed") {
            if (argument != "original" && argument != "uncapped") return {false, "argument", "Use original or uncapped."};
            SetSpeed(argument == "original" ? Speed::original : Speed::uncapped);
            return {true, "speed", argument};
        }
        if (command == "display") {
            string error;
            if (!DesktopDisplay(argument, error)) return {false, "argument", error};
            return {true, "display", "contrast " + to_string(lcd.Contrast()) + ", smooth " + (smooth ? "on" : "off")};
        }
        if (command == "backlight-state")
            return {true, "backlight", gSession->IsPowerOn() && EmHAL::GetLCDBacklightOn() ? "on" : "off"};
        if (command == "sound-state") return {true, "sound", soundOn && audio ? "on" : "off"};
        if (command == "sound-mode") {
            if (argument != "on" && argument != "off") return {false, "argument", "Use on or off."};
            SetSound(argument == "on");
            return {true, "sound", soundOn && audio ? "on" : "off"};
        }
        if (command == "audio-on" || command == "audio-off") {
            SetSound(command == "audio-on");
            return {true, "done", "Done."};
        }
        if (command == "gdb-start") {
            long port = 2159;
            if (!argument.empty()) {
                char* end = nullptr;
                port = strtol(argument.c_str(), &end, 10);
                if (*end || port < 1 || port > 65535) return {false, "argument", "Use a TCP port from 1 to 65535."};
            }
            StartGdb(uint16_t(port));
            if (!gdb) return {false, "gdb", "Could not listen on port " + to_string(port) + "."};
            return {true, "gdb", GdbState()};
        }
        if (command == "gdb-stop") {
            if (gdb) gdb->Stop();
            gdb.reset();
            gDebugger.SetBreakMode(Debugger::BreakMode::all);
            return {true, "gdb", "off"};
        }
        if (command == "gdb-state") return {true, "gdb", GdbState()};
        if (command == "quit") {
            ReleaseInput();
            desktopQuit = true;
            return {true, "done", "Done."};
        }

        // State queries the desktop asks every device, answered as absent.
        if (command == "network-state") return {true, "network", NetworkState()};
        if (command == "hold-state") return {true, "hold", "unavailable"};
        if (command == "keyboard-state") return {true, "keyboard", "detached"};
        if (command == "irprinter") {
            if (!profile->capabilities.infrared) return {false,"unsupported","This device has no infrared port."};
            auto split=argument.find(' ');
            string result;
            bool ok=irTransport->PrinterControl().Command(irTransport->Printer(),argument.substr(0,split),
                                                          split==string::npos?"":argument.substr(split+1),result);
            return {ok,"irprinter",result};
        }
        if (command == "printer") {
            auto split=argument.find(' ');
            string result;
            bool ok=PrinterControl(argument.substr(0,split),split==string::npos?"":argument.substr(split+1),result);
            return {ok,"printer",result};
        }
        if (command == "usb-port" || command == "usb-port-state") {
            if (!deviceTypeIsDana5(profile->deviceType)) {
                if (command == "usb-port-state" || argument == "off") return {true, "usb-port", "off"};
                return {false, "unsupported", "This device has no USB host port."};
            }
            string result;
            if (command == "usb-port-state") return {true, "usb-port", UsbPortState()};
            const bool ok = UsbPortControl(argument, result);
            return {ok, ok ? "usb-port" : "argument", result};
        }
        if (command == "usb-state") return {true, "usb", "off"};
        if (command == "microphone-state") return {true, "microphone", "off"};
        if (command == "microphone-epoch") return {true, "microphone", "0"};
        if (command == "sd-state") return {true, "card", "none"};
        if (command == "wifi-module-state") return {true, "wifi-module", ModuleState()};
        if (command == "network-mode") {
            // On the Dana Wireless, wireless and disabled turn the built-in module's radio on and off.
            if (profile->capabilities.builtInWifi && (argument == "wireless" || argument == "disabled")) {
                string result;
                if (!ModuleControl(argument == "wireless", result)) return {false, "network", result};
                return {true, "network", NetworkState()};
            }
            if (profile->capabilities.builtInWifi) return {false, "argument", "Use disabled or wireless."};
            // On the Dana, wireless plugs in the USB Wi-Fi adapter and disabled unplugs it.
            if (profile->capabilities.usbWifi && (argument == "wireless" || argument == "disabled")) {
                string result;
                if (argument == "wireless" ? !UsbPortControl("wifi", result)
                                           : usbPort == "wifi" && !UsbPortControl("off", result))
                    return {false, "network", result};
                return {true, "network", NetworkState()};
            }
            if (argument == "disabled") return {true, "network", "disabled"};
            if (profile->capabilities.usbWifi) return {false, "argument", "Use disabled or wireless."};
            return {false, "unsupported", "This device has no emulated network."};
        }
        if (command == "keyboard-attach") {
            if (argument != "on" && argument != "off") return {false, "argument", "Use on or off."};
            if (argument == "off") return {true, "keyboard", "detached"};
            return {false, "unsupported", "This device has no keyboard."};
        }
        if (command == "hold") {
            if (argument != "on" && argument != "off") return {false, "argument", "Use on or off."};
            return {false, "unsupported", "This device has no supported Hold switch."};
        }
        if (command == "keyboard-key") return {false, "unsupported", "This device has no keyboard."};
        if (command == "usb-mode") return {false, "unsupported", "USB is not supported on this device."};
        if (command == "bluetooth-state" || command == "bluetooth-mode")
            return {false, "unsupported", "This device has no Bluetooth radio."};
        if (command == "microphone-mode") return {false, "unsupported", "This device has no emulated microphone."};
        if (command == "guest-orientation" || command == "rotate")
            return {false, "unsupported", "This device has no screen rotation."};
        if (command == "replay-snapshots") return {false, "unsupported", "This runner keeps no replay snapshot results."};
        if (command.rfind("sd-", 0) == 0) return {false, "unsupported", "This device has no SD slot."};
        if (command.rfind("ms-", 0) == 0) return {false, "unsupported", "This device has no Memory Stick slot."};
        return {false, "command", "Unknown desktop command."};
    }

    // contrast=0..100 and smooth=on|off; fit, keyboard and jog are for ARM devices.
    bool DesktopDisplay(const string& text, string& error) {
        istringstream words(text);
        string word;
        optional<int> contrast;
        optional<bool> smoothing;
        optional<WindowFit> fitting;
        while (words >> word) {
            const auto equals = word.find('=');
            const string name = word.substr(0, equals), value = equals == string::npos ? "" : word.substr(equals + 1);
            char* end = nullptr;
            const long number = strtol(value.c_str(), &end, 10);
            if (name == "contrast" && !value.empty() && !*end && number >= 0 && number <= 100) {
                contrast = int(number);
            } else if (name == "smooth" && (value == "on" || value == "off")) {
                smoothing = value == "on";
            } else if (WindowFit parsed; name == "fit" && parseWindowFit(value, parsed)) {
                fitting = parsed;
            } else if (equals != string::npos && (name == "keyboard" || name == "jog")) {
                error = "This device has no " + name + " controls.";
                return false;
            } else {
                error = "Invalid display setting: " + word;
                return false;
            }
        }
        if (!contrast && !smoothing && !fitting) {
            error = "Use contrast=, smooth= or fit=.";
            return false;
        }
        if (contrast) {
            lcd.SetContrast(*contrast);
            metadata.contrast = lcd.Contrast();
        }
        if (smoothing) {
            smooth = *smoothing;
            if (lcdTexture) SDL_SetTextureScaleMode(lcdTexture, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        }
        // Only a window opened fitted can change how it fits; a fixed-scale window stays as it is.
        if (fitting && fit != WindowFit::none && *fitting != fit) {
            fit = *fitting;
            Layout();
        }
        return true;
    }

    // display contrast=0..100; smooth=on|off for the window's LCD scaling.
    void Display(istringstream& words) {
        string word;
        bool any = false;
        while (words >> word) {
            const auto equals = word.find('=');
            const string name = word.substr(0, equals), value = equals == string::npos ? "" : word.substr(equals + 1);
            char* end = nullptr;
            const long number = strtol(value.c_str(), &end, 10);
            if (name == "contrast" && !value.empty() && !*end && number >= 0 && number <= 100) {
                lcd.SetContrast(int(number));
                metadata.contrast = lcd.Contrast();
            } else if (name == "smooth" && (value == "on" || value == "off")) {
                smooth = value == "on";
                if (lcdTexture) SDL_SetTextureScaleMode(lcdTexture, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
            } else {
                return void(cout << "display: invalid setting " << word << endl);
            }
            any = true;
        }
        if (!any) return void(cout << "display: use contrast= or smooth=" << endl);
        cout << "display: contrast " << lcd.Contrast() << ", smooth " << (smooth ? "on" : "off") << endl;
    }

    // Checks the file first; a session that fails after that puts the old machine back.
    void LoadSession(const string& path) {
        vector<uint8_t> data;
        string folder;
        if (!readSessionInput(path, data,folder)) return void(cout << "session refused: cannot read " << path << "; machine unchanged" << endl);
        CheckedSession session;
        if (const auto error = checkSession(data, profile->name, session,folder); !error.empty())
            return void(cout << "session refused: " << error << "; machine unchanged" << endl);
        vector<uint8_t> backup;
        if (!serializeSession(*profile, metadata, backup))
            return void(cout << "session refused: the running machine can't be saved first; machine unchanged" << endl);
        RuntimeCards::Pair next;
        if(const auto error=runtimeCards.Stage(session.manifest,next); !error.empty())
            return void(cout << "session refused: " << error << "; machine unchanged" << endl);
        if(const auto error=checkCardState(session,next); !error.empty())
            return void(cout << "session refused: " << error << "; machine unchanged" << endl);
        const auto old=runtimeCards.Snapshot();
        runtimeCards.Install(next);
        const auto wifiBefore=usbPort=="wifi" ? usbWifi.adapter.save() : vector<uint8_t>{};
        const auto wifiBusBefore=usbPort=="wifi" ? optional(usbWifi.adapter.bus()) : nullopt;
        const auto printerBefore=usbPort=="printer" ? optional<UsbRawPrinter::Bus>(usbPrinter.printer.bus()) : nullopt;
        const bool moduleBefore=moduleOn && moduleWifi.attached();
        const auto moduleStateBefore=moduleBefore ? moduleWifi.adapter.save() : vector<uint8_t>{};
        const auto moduleBusBefore=moduleBefore ? optional(moduleWifi.adapter.bus()) : nullopt;
        auto error=applySession(session);
        if(error.empty() && !runtimeCards.Matches(next)) error="manifest and protocol insertion disagree";
        if (!error.empty()) {
            runtimeCards.Install(old);
            CheckedSession previous;
            if (!checkSession(backup, profile->name, previous).empty() || !applySession(previous).empty()) {
                cerr << "the running machine couldn't be restored" << endl;
                quit = true;
                return;
            }
            if(!wifiBefore.empty()) { pendingWifi=wifiBefore; pendingWifiBus=wifiBusBefore; }
            pendingPrinter=printerBefore;
            if(moduleBefore) { pendingModule=moduleStateBefore; pendingModuleBus=moduleBusBefore; }
            RestoreUsb();
            piezo.Restart(gSession->GetSystemCycles());
            serialTransport->SessionLoaded("");
            irTransport->SessionLoaded("");
            SyncDTR();
            return void(cout << "session refused: " << error << "; machine restored"
                                  << (usbDebugMode!="off"?"; USB peer must reconnect and reset":"") << endl);
        }
        UsbSessionLoaded(session.metadata);
        RestoreUsb();
        ReleaseInput(false);
        metadata = session.metadata;
        lcd.SetContrast(metadata.contrast);
        SetSpeed(speed);
        piezo.Restart(gSession->GetSystemCycles());
        serialTransport->SessionLoaded(metadata.modemCall);
        irTransport->SessionLoaded(metadata.phoneCall);
        SyncDTR();
        // A session on an older ROM can't keep the phone.
        if (irTransport->PhoneMode() && !RomHasIrComm()) {
            irTransport->SetPhoneMode(false, infraredError);
            infraredError = "the infrared phone needs Palm OS 3.3 or later";
        }
        cout << "loaded session " << path << endl;
    }

    // Opens the window at the fixed scale, or resizable and fitted with fit set.
    void OpenWindow() {
        const auto& controls = deviceControls(profile->art);
        const float s = options.windowScale;
        const int width = int(lcd.Width() * s);
        const int height = deviceTypeIsDana5(profile->deviceType) ? int(160*s+252) :
            int((InputHeight() + deviceControlsHeight(controls, lcd.Width())) * s);
        window = SDL_CreateWindow(profile->displayName, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                                  fit == WindowFit::none && !deviceTypeIsDana5(profile->deviceType) ? 0 : SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) {
            cerr << "no window: " << SDL_GetError() << endl;
            return;
        }
        CreateRenderer();
#ifndef __ANDROID__
        // On Android this would bring up the soft keyboard; keys still come in as key events.
        SDL_StartTextInput();
#endif
    }

    void CreateRenderer() {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) {
            cerr << "no renderer: " << SDL_GetError() << endl;
            return;
        }
        lcdTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, lcd.Width(),
                                       lcd.Height());
        if (lcdTexture) SDL_SetTextureScaleMode(lcdTexture, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        Layout();
    }

    // A lost renderer (Android after its context goes) is made again with its textures.
    void ResetRenderer() {
        for (auto* texture : {lcdTexture, silkscreenTexture, controlsTexture})
            if (texture) SDL_DestroyTexture(texture);
        lcdTexture = silkscreenTexture = controlsTexture = nullptr;
        if (renderer) SDL_DestroyRenderer(renderer);
        renderer = nullptr;
        CreateRenderer();
    }

    int InputHeight() const { return lcd.Height() + profile->frontend.silkscreenHeight; }

    int DeviceHeight() const { return InputHeight() + deviceControlsHeight(deviceControls(profile->art), lcd.Width()); }

    // Where the device goes in the window and the scale the art is drawn at. Fixed: the window
    // scale from the top left. Fitted: WindowLayout's place, drawn in device pixels through the
    // viewport and render scale, with the art made at the output's own pixel size.
    void Layout() {
        if (!renderer) return;
        int windowWidth = 0, windowHeight = 0, outputWidth = 0, outputHeight = 0;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
        pixelsX = windowWidth > 0 ? float(outputWidth) / windowWidth : 1;
        pixelsY = windowHeight > 0 ? float(outputHeight) / windowHeight : 1;
        if(deviceTypeIsDana5(profile->deviceType)) {
            CancelDanaPointers();
            danaLayout=DanaViewLayout::Make(outputWidth,outputHeight,danaView);
            if(controlsTexture)SDL_DestroyTexture(controlsTexture);
            controlsTexture=nullptr;controlsDirty=true;
            NotifyDanaView();
            return;
        }
        const float s = options.windowScale;
        if (fit == WindowFit::none) {
            layout = WindowLayout{s, s, 0, 0, int(lcd.Width() * s), int(DeviceHeight() * s)};
            artScale = s;
        } else {
            layout = fitWindowLayout(fit, lcd.Width(), DeviceHeight(), windowWidth, windowHeight);
            artScale = max(layout.scaleX * pixelsX, layout.scaleY * pixelsY);
        }
        if (silkscreenTexture) SDL_DestroyTexture(silkscreenTexture);
        silkscreenTexture = nullptr;
        const auto& silkscreen = deviceSilkscreen(profile->art);
        if (silkscreen.count) silkscreenTexture = createDeviceSilkscreen(renderer, silkscreen, artScale);
        if (silkscreenTexture && fit != WindowFit::none) SDL_SetTextureScaleMode(silkscreenTexture, SDL_ScaleModeLinear);
        if (controlsTexture) SDL_DestroyTexture(controlsTexture);
        controlsTexture = nullptr;
        if (fit != WindowFit::none) {
            const auto& controls = deviceControls(profile->art);
            controlsTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET,
                                                int(lcd.Width() * artScale),
                                                int(deviceControlsHeight(controls, lcd.Width()) * artScale));
            if (controlsTexture) SDL_SetTextureScaleMode(controlsTexture, SDL_ScaleModeLinear);
        }
        controlsDirty = true;
    }

    void Draw(bool present = true) {
        if (!renderer || hidden) return;
        const bool fitted = fit != WindowFit::none;
        const float s = fitted ? 1 : options.windowScale;
        const auto& controls = deviceControls(profile->art);
        lcd.Update();
        SDL_RenderSetViewport(renderer, nullptr);
        SDL_RenderSetScale(renderer, 1, 1);
        const uint32_t colour = fitted ? options.windowBackground : 0;
        SDL_SetRenderDrawColor(renderer, colour >> 16, (colour >> 8) & 0xff, colour & 0xff, 0xff);
        SDL_RenderClear(renderer);
        if(deviceTypeIsDana5(profile->deviceType)) {
            DrawDana();
            if(present)SDL_RenderPresent(renderer);
            return;
        }
        if (fitted) {
            // The viewport is in output pixels at scale 1; the scale then maps device pixels into it.
            const SDL_Rect viewport{int(lround(layout.x * pixelsX)), int(lround(layout.y * pixelsY)),
                                    int(lround(layout.width * pixelsX)), int(lround(layout.height * pixelsY))};
            SDL_RenderSetViewport(renderer, &viewport);
            SDL_RenderSetScale(renderer, layout.scaleX * pixelsX, layout.scaleY * pixelsY);
        }
        if (lcdTexture) {
            SDL_UpdateTexture(lcdTexture, nullptr, lcd.Pixels(), lcd.Width() * 4);
            const SDL_Rect dest{0, 0, int(lcd.Width() * s), int(lcd.Height() * s)};
            SDL_RenderCopy(renderer, lcdTexture, nullptr, &dest);
        }
        if (silkscreenTexture) {
            const SDL_Rect dest{0, int(lcd.Height() * s), int(lcd.Width() * s),
                                int((InputHeight() - lcd.Height()) * s)};
            SDL_RenderCopy(renderer, silkscreenTexture, nullptr, &dest);
        }
        if (!fitted) {
            drawDeviceControls(renderer, controls, lcd.Width(), InputHeight(), s, pressed);
        } else if (controlsTexture) {
            // Drawn once into a texture at the output's pixel size, again when a key changes.
            if (controlsDirty || pressed != shownPressed) {
                float sx, sy;
                SDL_RenderGetScale(renderer, &sx, &sy);
                SDL_Rect viewport;
                SDL_RenderGetViewport(renderer, &viewport);
                if (SDL_SetRenderTarget(renderer, controlsTexture) == 0) {
                    SDL_RenderSetScale(renderer, 1, 1);
                    drawDeviceControls(renderer, controls, lcd.Width(), 0, artScale, pressed);
                    SDL_SetRenderTarget(renderer, nullptr);
                    SDL_RenderSetScale(renderer, 1, 1);
                    SDL_RenderSetViewport(renderer, &viewport);
                    SDL_RenderSetScale(renderer, sx, sy);
                    controlsDirty = false;
                    shownPressed = pressed;
                }
            }
            const SDL_Rect dest{0, InputHeight(), lcd.Width(), deviceControlsHeight(controls, lcd.Width())};
            SDL_RenderCopy(renderer, controlsTexture, nullptr, &dest);
        } else {
            drawDeviceControls(renderer, controls, lcd.Width(), InputHeight(), 1, pressed);
        }
        if (present) SDL_RenderPresent(renderer);
    }

    // The held back ctrl and shift presses only go down, so this doesn't retry releases.
    void FlushDanaHost() {
        for(const auto& change:danaHostKeys.Flush())
            if(const auto scan=DanaScan5::Named(change.name))DanaKey(change.source,*scan,true);
    }
    // Full screen on the desktop asks the window to grab the keyboard, so the desktop's own shortcuts
    // reach the Dana. The grab is only held while the window has the focus, and is tried again
    // when it comes back, not in a loop.
    void SetKeyboardGrab(bool wanted) {
        keyboardGrabWanted=wanted;
        ApplyKeyboardGrab();
    }
    void ApplyKeyboardGrab() {
        if(!window)return;
        const bool want=keyboardGrabWanted&&windowFocused;
        if(want==keyboardGrabbed)return;
        SDL_SetWindowKeyboardGrab(window,want?SDL_TRUE:SDL_FALSE);
        keyboardGrabbed=want&&SDL_GetWindowKeyboardGrab(window)==SDL_TRUE;
        if(want&&!keyboardGrabbed)cerr<<"Keyboard grab refused: "<<SDL_GetError()<<endl;
    }
    void ApplyDanaHost(const vector<DanaHostChange>& changes) {
        for(const auto& change:changes) {
            const auto scan=DanaScan5::Named(change.name);
            if(!scan) continue;
            const auto key=make_pair(int(change.source),scan->Code());
            if(!change.down)danaReleases.insert_or_assign(key,*scan);
            RetryDanaReleases();
            if(change.down && (danaReleases.count(key)||!DanaKey(change.source,*scan,true)))cerr<<"Dana keyboard busy"<<endl;
        }
    }
    bool ApplyDanaScreen(const vector<DanaTransition5>& batch) {
        // A held host Ctrl goes to the guest before a key from the screen, so Ctrl and a click give ctrl+key.
        for(const auto& change:batch)if(change.down){FlushDanaHost();break;}
        const bool accepted=danaInput.Keys(DanaSource5::screen,batch,
            [](const auto& changes) {return EmHAL::DispatchDanaInput(changes);});
        if(accepted)controlsDirty=true;
        return accepted;
    }
    void RetryDanaReleases() {
        for(auto it=danaReleases.begin();it!=danaReleases.end();) {
            if(DanaKey(DanaSource5(it->first.first),it->second,false))it=danaReleases.erase(it);else ++it;
        }
        danaScreen.Flush([this](const auto& batch){return ApplyDanaScreen(batch);});
    }
    void CancelDanaPointers() {
        if(danaPen)gSession->QueuePenEvent(PenEvent::up());
        danaPen=false;danaPointers.clear();
        danaScreen.Cancel([this](const auto& batch){return ApplyDanaScreen(batch);});
        SDL_CaptureMouse(SDL_FALSE);
    }
    string DanaViewDescription() const {return danaView.Describe(danaLayout.resolved);}
    void NotifyDanaView() {
        if(desktop)StateChanged("dana-view",lastDanaView,DanaViewDescription());
    }
    bool SetDanaView(const string& argument,string& error) {
        auto next=danaView;
        if(!next.Parse(argument,error))return false;
        CancelDanaPointers();danaView=next;
        if(renderer)Layout();
        else danaLayout=DanaViewLayout::Make(1120,572,danaView);
        NotifyDanaView();return true;
    }
    void DrawDana() {
        if(lcdTexture) {
            SDL_UpdateTexture(lcdTexture,nullptr,lcd.Pixels(),lcd.Width()*4);
            SDL_RenderSetClipRect(renderer,&danaLayout.viewport);
            SDL_RenderCopyF(renderer,lcdTexture,nullptr,&danaLayout.screen);
            SDL_RenderSetClipRect(renderer,nullptr);
        }
        const auto& dest=danaLayout.keyboard;
        if(dest.w<=0||dest.h<=0||danaLayout.resolved=="hidden")return;
        DanaKeysHeld held;
        for(const auto& key:danaKeys5)if(danaInput.Held(*DanaScan5::Named(key.name)))held.Set(key.name,true);
        if(controlsDirty||held.Count()!=danaShown.Count()||DanaHeldChanged(held)) {
            if(!controlsTexture)controlsTexture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_TARGET,dest.w,dest.h);
            if(controlsTexture&&SDL_SetRenderTarget(renderer,controlsTexture)==0) {
                SDL_RenderSetScale(renderer,1,1);
                SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);
                const SDL_Rect local{0,0,dest.w,dest.h};
                if(danaLayout.resolved=="full")drawDanaKeyboard(renderer,local,held);
                else {
                    auto keys=danaCompactKeys(danaView.page,danaLayout.portrait);
                    vector<DanaKeyArt> caps;vector<bool> down;
                    for(const auto& key:keys) {
                        caps.push_back(key.art);
                        down.push_back(!key.chord.empty()&&danaInput.Held(key.chord.back()));
                    }
                    drawDanaKeyCaps(renderer,local,danaLayout.portrait?390:510,danaLayout.portrait?252:180,caps.data(),caps.size(),down);
                }
                SDL_SetRenderTarget(renderer,nullptr);
                SDL_RenderSetScale(renderer,1,1);
                controlsDirty=false;danaShown=held;
            }
        }
        if(controlsTexture)SDL_RenderCopy(renderer,controlsTexture,nullptr,&dest);
    }
    bool DanaHeldChanged(const DanaKeysHeld& held) const {
        for(size_t i=0;i<danaKeyboardArt().count;++i)if(held.Held(i)!=danaShown.Held(i))return true;
        return false;
    }
    int DanaHit(double x,double y) const {
        const auto& r=danaLayout.keyboard;
        if(danaLayout.resolved=="hidden"||r.w<=0||r.h<=0||x<r.x||y<r.y||x>=r.x+r.w||y>=r.y+r.h)return -1;
        if(danaLayout.resolved=="full")return danaKeyAt((x-r.x)*600/r.w,(y-r.y)*292/r.h);
        const auto keys=danaCompactKeys(danaView.page,danaLayout.portrait);
        x=(x-r.x)*(danaLayout.portrait?390:510)/r.w;y=(y-r.y)*(danaLayout.portrait?252:180)/r.h;
        for(size_t i=0;i<keys.size();++i) {
            const auto& b=keys[i].art.bounds;
            if(x>=b.x&&y>=b.y&&x<b.x+b.w&&y<b.y+b.h)return int(i);
        }
        return -1;
    }
    bool DanaPointerEvent(const SDL_Event& event) {
        const bool finger=event.type==SDL_FINGERDOWN||event.type==SDL_FINGERUP||event.type==SDL_FINGERMOTION;
        const bool mouse=event.type==SDL_MOUSEBUTTONDOWN||event.type==SDL_MOUSEBUTTONUP||event.type==SDL_MOUSEMOTION;
        if(event.type==SDL_MOUSEWHEEL) {
            if(danaView.pointer=="pan"||(SDL_GetMouseState(nullptr,nullptr)&SDL_BUTTON_MMASK)) {
                static const double zooms[]={1,1.5,2,3};
                double zoom=event.wheel.y>0?3:1;
                if(event.wheel.y>0)for(auto z:zooms) {if(z>danaView.zoom){zoom=z;break;}}
                if(event.wheel.y<0)for(auto z:zooms)if(z<danaView.zoom)zoom=z;
                if(event.wheel.y) {danaView.zoom=zoom;Layout();}
            }
            return true;
        }
        if(!finger&&!mouse)return false;
        if(mouse&&(event.type==SDL_MOUSEMOTION?event.motion.which:event.button.which)==SDL_TOUCH_MOUSEID)return true;
        if(finger&&SDL_GetTouchDeviceType(event.tfinger.touchId)!=SDL_TOUCH_DEVICE_DIRECT)return true;
        const bool down=event.type==SDL_FINGERDOWN||event.type==SDL_MOUSEBUTTONDOWN;
        const bool up=event.type==SDL_FINGERUP||event.type==SDL_MOUSEBUTTONUP;
        const auto touch=finger?int64_t(event.tfinger.touchId):std::numeric_limits<int64_t>::min();
        const auto id=finger?int64_t(event.tfinger.fingerId):int64_t(event.type==SDL_MOUSEMOTION?0:event.button.button);
        int ww=0,wh=0;SDL_GetWindowSize(window,&ww,&wh);
        const double x=finger?event.tfinger.x*ww*pixelsX:((event.type==SDL_MOUSEMOTION?event.motion.x:event.button.x)+.5)*pixelsX;
        const double y=finger?event.tfinger.y*wh*pixelsY:((event.type==SDL_MOUSEMOTION?event.motion.y:event.button.y)+.5)*pixelsY;
        auto apply=[this](const auto& batch){return ApplyDanaScreen(batch);};
        if(down) {
            if(!finger&&id!=SDL_BUTTON_LEFT&&id!=SDL_BUTTON_MIDDLE)return true;
            const int hit=DanaHit(x,y);
            if(hit>=0&&(finger||id!=SDL_BUTTON_MIDDLE)) {
                if(danaLayout.resolved=="full") {
                    const auto scan=*DanaScan5::Named(danaKeyboardArt().keys[hit].name);
                    if(!danaScreen.Down(touch,id,{scan},apply))return true;
                } else {
                    const auto key=danaCompactKeys(danaView.page,danaLayout.portrait)[hit];
                    if(!key.action.empty()) {
                        if(auto scan=DanaScan5::Named(key.action))danaScreen.Modifier(*scan,apply);
                        else {
                            CancelDanaPointers();danaView.page=key.action;controlsDirty=true;NotifyDanaView();
                        }
                        return true;
                    }
                    if(!danaScreen.Down(touch,id,key.chord,apply))return true;
                }
                danaPointers[{touch,id}]={hit,false,false,x,y};
            } else {
                int gx=0,gy=0;
                if(!danaLayout.GuestPoint(x,y,gx,gy))return true;
                const bool pan=danaView.pointer=="pan"||(!finger&&id==SDL_BUTTON_MIDDLE);
                if(!pan&&danaPen)return true;
                if(pan)CancelDanaPointers();
                danaPointers[{touch,id}]={-1,!pan,pan,x,y};
                if(!pan){danaPen=true;gSession->QueuePenEvent(PenEvent::down(gx,gy));}
            }
            if(mouse)SDL_CaptureMouse(SDL_TRUE);
        } else {
            // A mouse motion has no button field; move each captured mouse owner.
            for(auto it=danaPointers.begin();it!=danaPointers.end();) {
                if(it->first.first!=touch||(finger||up ? it->first.second!=id:false)) {++it;continue;}
                auto& pointer=it->second;
                bool release=up;
                if(pointer.pan&&!up) {
                    danaView.panX=std::clamp(danaLayout.panX+(pointer.x-x)/danaLayout.scale,0.,560.);
                    danaView.panY=std::clamp(danaLayout.panY+(pointer.y-y)/danaLayout.scale,0.,160.);
                    int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                    danaLayout=DanaViewLayout::Make(w,h,danaView);NotifyDanaView();
                } else if(pointer.pen&&!up) {
                    int gx=0,gy=0;
                    if(danaLayout.GuestPoint(x,y,gx,gy))gSession->QueuePenEvent(PenEvent::down(gx,gy));else release=true;
                } else if(pointer.key>=0&&!up&&DanaHit(x,y)!=pointer.key)release=true;
                pointer.x=x;pointer.y=y;
                if(release) {
                    if(pointer.pen){gSession->QueuePenEvent(PenEvent::up());danaPen=false;}
                    danaScreen.Up(it->first.first,it->first.second,apply);
                    it=danaPointers.erase(it);
                } else ++it;
            }
            if(mouse&&danaPointers.empty())SDL_CaptureMouse(SDL_FALSE);
        }
        return true;
    }

    // A window point in device pixels; false outside the device when fitted.
    bool DevicePoint(int windowX, int windowY, int& x, int& y) const {
        if (fit == WindowFit::none) {
            x = int(windowX / options.windowScale);
            y = int(windowY / options.windowScale);
            return true;
        }
        const float fx = (windowX - layout.x) / layout.scaleX, fy = (windowY - layout.y) / layout.scaleY;
        x = int(floor(fx));
        y = int(floor(fy));
        return fx >= 0 && fy >= 0 && x < lcd.Width() && y < DeviceHeight();
    }

    // The IR printer has a job coming in or a link up (frondIr::IrPrinter::NeedsGuest).
    bool PrinterNeedsGuest() {
        return irTransport && irTransport->PrinterNeedsGuest();
    }

    // Lifts the pen and any held key, when the window loses focus or the app is hidden.
    // keepReplay leaves a replay that hasn't started yet, like one from --input-replay.
    void ReleaseInput(bool cancel=true, bool keepReplay=false) {
        if (!keepReplay) replay=Replay();
        if(deviceTypeIsDana5(profile->deviceType)) {
            CancelDanaPointers();danaScreen.Clear();danaReleases.clear();
            danaInput.Clear();danaHostKeys.Clear();danaRepeats.Clear();EmHAL::ReleaseDanaInput(cancel);
        }
        if (penDown) gSession->QueuePenEvent(PenEvent::up());
        penDown = false;
        mouseKey = keyInvalid;
        touch.ReleaseAll();
        keyHolds.Clear();
        for (KeyId key : {keyIdHard1, keyIdHard2, keyIdHard3, keyIdHard4, keyIdUp, keyIdDown, keyIdPower})
            if (pressed & deviceKeyBit(key)) SetKey(key, false);
    }

    void PollWindow() {
        const auto& controls = deviceControls(profile->art);
        if(deviceTypeIsDana5(profile->deviceType))RetryDanaReleases();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if(deviceTypeIsDana5(profile->deviceType) && DanaPointerEvent(event))continue;
            switch (event.type) {
                case SDL_QUIT:
                    ReleaseInput();
                    // The desktop decides: it saves, then sends quit.
                    if (desktop) desktop->Event("close");
                    else quit = true;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) Layout();
                    // Hidden events from opening the window must keep balanced input from the loaded session
                    // and a replay from --input-replay.
                    if(deviceTypeIsDana5(profile->deviceType) && (event.window.event==SDL_WINDOWEVENT_HIDDEN||
                        event.window.event==SDL_WINDOWEVENT_MINIMIZED))ReleaseInput(windowPolled,!windowPolled);
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {windowFocused=true;ApplyKeyboardGrab();}
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        // Initial focus events must keep balanced input from the loaded session and the replay.
                        if(windowFocused) ReleaseInput(windowPolled,!windowPolled);
                        windowFocused=false;
                        ApplyKeyboardGrab();
                    }
                    break;
                case SDL_RENDER_TARGETS_RESET:
                    controlsDirty = true;
                    break;
                case SDL_RENDER_DEVICE_RESET:
                    ResetRenderer();
                    break;
                case SDL_APP_DIDENTERBACKGROUND:
                    hidden = true;
                    ReleaseInput();
                    break;
                case SDL_APP_DIDENTERFOREGROUND:
                    hidden = false;
                    Layout();
                    break;
                case SDL_FINGERDOWN:
                case SDL_FINGERMOTION:
                case SDL_FINGERUP: {
                    const auto& f = event.tfinger;
                    if (event.type == SDL_FINGERUP) {
                        touch.Up(f.touchId, f.fingerId);
                        break;
                    }
                    int windowWidth = 0, windowHeight = 0;
                    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                    float fx = 0, fy = 0;
                    RawFingers::Take(f, fx, fy);
                    int x = 0, y = 0;
                    const bool inside = DevicePoint(int(floor(fx * windowWidth)), int(floor(fy * windowHeight)), x, y);
                    if (event.type == SDL_FINGERDOWN) touch.Down(f.touchId, f.fingerId, x, y, inside, penDown);
                    else touch.Move(f.touchId, f.fingerId, x, y);
                    break;
                }
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEMOTION: {
                    // SDL's touch-to-mouse copies of the fingers above.
                    if ((event.type == SDL_MOUSEMOTION ? event.motion.which : event.button.which) == SDL_TOUCH_MOUSEID) break;
                    const bool down = event.type == SDL_MOUSEBUTTONDOWN;
                    if (event.type != SDL_MOUSEMOTION && event.button.button != SDL_BUTTON_LEFT) break;
                    const int mx = event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x;
                    const int my = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
                    int x = 0, y = 0;
                    const bool inside = DevicePoint(mx, my, x, y);
                    x = clamp(x, 0, lcd.Width() - 1);
                    if (event.type == SDL_MOUSEBUTTONUP) {
                        if (penDown) gSession->QueuePenEvent(PenEvent::up());
                        if (mouseKey != keyInvalid) Key(mouseKey, false);
                        penDown = false;
                        mouseKey = keyInvalid;
                    } else if (down) {
                        if (!inside) break;
                        if (y < InputHeight()) {
                            if (touch.PenActive()) break;
                            penDown = true;
                            gSession->QueuePenEvent(PenEvent::down(x, y));
                        } else {
                            mouseKey = deviceControlAt(controls, lcd.Width(), InputHeight(), x, y);
                            if (mouseKey != keyInvalid) Key(mouseKey, true);
                        }
                    } else if (penDown) {
                        gSession->QueuePenEvent(PenEvent::down(x, clamp(y, 0, InputHeight() - 1)));
                    }
                    break;
                }
                case SDL_TEXTINPUT: {
                    const auto c = static_cast<unsigned char>(event.text.text[0]);
                    if (deviceTypeIsDana5(profile->deviceType)) break;
                    if (c && c < 0x80) gSession->QueueKeyboardEvent(char(c));
                    break;
                }
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    const bool keyDown = event.type == SDL_KEYDOWN;
                    if (desktop) {
                        if (const auto name = shortcutName(event.key.keysym.sym, event.key.keysym.mod,
                                                          deviceTypeIsDana5(profile->deviceType))) {
                            if (keyDown && !event.key.repeat) desktop->Event("shortcut", *name);
                            break;
                        }
                    }
                    if(deviceTypeIsDana5(profile->deviceType)) {
                        // SDL on X11 can drop a quick key's release and report its next press as a repeat
                        // when a burst shares a timestamp. A repeat sooner than any real auto-repeat
                        // delay is that press, so it goes in as a release and a press.
                        const bool retap=keyDown && danaRepeats.Retap(event.key.keysym.scancode,event.key.repeat!=0,SDL_GetTicks());
                        if(event.key.repeat && !retap) break;
                        string name=SDL_GetScancodeName(event.key.keysym.scancode);
                        for(char& c:name) c=char(std::tolower(static_cast<unsigned char>(c)));
                        switch(event.key.keysym.scancode) {
                            case SDL_SCANCODE_LSHIFT: name="lshift"; break;
                            case SDL_SCANCODE_RSHIFT: name="rshift"; break;
                            case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: name="ctrl"; break;
                            case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: name="command"; break;
                            case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: name="function"; break;
                            case SDL_SCANCODE_RETURN: name="enter"; break;
                            case SDL_SCANCODE_CAPSLOCK: name="caps"; break;
                            case SDL_SCANCODE_APPLICATION: name="apps"; break;
                            default: break;
                        }
                        if(retap) ApplyDanaHost(danaHostKeys.Event(event.key.keysym.scancode,false,name));
                        ApplyDanaHost(danaHostKeys.Event(event.key.keysym.scancode,keyDown,name));
                        break;
                    }
                    KeyId key = keyInvalid;
                    switch (event.key.keysym.sym) {
                        case SDLK_F1: key = keyIdHard1; break;
                        case SDLK_F2: key = keyIdHard2; break;
                        case SDLK_F3: key = keyIdHard3; break;
                        case SDLK_F4: key = keyIdHard4; break;
                        case SDLK_UP: key = keyIdUp; break;
                        case SDLK_DOWN: key = keyIdDown; break;
                        case SDLK_ESCAPE: key = keyIdPower; break;
                        default: break;
                    }
                    if (key != keyInvalid) {
                        if (!event.key.repeat) Key(key, keyDown);
                        break;
                    }
                    if (!keyDown) break;
                    switch (event.key.keysym.sym) {
                        case SDLK_RETURN: gSession->QueueKeyboardEvent('\n'); break;
                        case SDLK_BACKSPACE: gSession->QueueKeyboardEvent(chrBackspace); break;
                        case SDLK_TAB: gSession->QueueKeyboardEvent(chrHorizontalTabulation); break;
                        case SDLK_LEFT: gSession->QueueKeyboardEvent(chrLeftArrow); break;
                        case SDLK_RIGHT: gSession->QueueKeyboardEvent(chrRightArrow); break;
                        default: break;
                    }
                    break;
                }
            }
        }
        windowPolled=true;
    }

    const Options& options;
    const DeviceProfile5* profile;
    SessionMetadata metadata;
    Lcd68k lcd;
    Replay replay;
    Console console;
    Speed speed{Speed::original};
    double guestSeconds{0};
    double measuredHz{0};
    pair<double, double> speedMark{0, 0};
    bool quit{false};
    bool smooth{false};
    SDL_Window* window{nullptr};
    bool windowFocused{false}, windowPolled{false}, keyboardGrabWanted{false}, keyboardGrabbed{false};
    SDL_Renderer* renderer{nullptr};
    SDL_Texture* lcdTexture{nullptr};
    SDL_Texture* silkscreenTexture{nullptr};
    SDL_Texture* controlsTexture{nullptr};
    WindowFit fit{WindowFit::none};
    WindowLayout layout;
    float pixelsX{1}, pixelsY{1}, artScale{1};
    bool controlsDirty{true};
    DeviceKeyMask shownPressed{0};
    // Android has the app in the background.
    bool hidden{false};
    DeviceKeyMask pressed{0};
    bool penDown{false};
    KeyId mouseKey{keyInvalid};
    KeyHolds68k keyHolds;
    DanaInput5 danaInput;
    DanaViewState danaView;
    DanaViewLayout danaLayout;
    DanaScreenKeys danaScreen;
    DanaKeysHeld danaShown;
    string lastDanaView;
    bool danaPen{false};
    struct DanaPointer {int key;bool pen,pan;double x,y;};
    map<pair<int64_t,int64_t>,DanaPointer> danaPointers;
    map<pair<int,uint8_t>,DanaScan5> danaReleases;
    DanaHostKeys danaHostKeys;
    DanaRepeats danaRepeats;
    // Fingers: held buttons and the pen, besides the mouse.
    struct TouchHost : TouchSink68k {
        explicit TouchHost(Runner& runner) : runner(runner) {}
        bool DirectTouch(int64_t touch) override { return SDL_GetTouchDeviceType(touch) == SDL_TOUCH_DEVICE_DIRECT; }
        KeyId KeyAt(int x, int y) override {
            return deviceControlAt(deviceControls(runner.profile->art), runner.lcd.Width(), runner.InputHeight(), x, y);
        }
        void PenDown(int x, int y) override { gSession->QueuePenEvent(PenEvent::down(x, y)); }
        void PenUp() override { gSession->QueuePenEvent(PenEvent::up()); }
        void KeyDown(KeyId key) override { runner.Key(key, true); }
        void KeyUp(KeyId key) override { runner.Key(key, false); }
        Runner& runner;
    } touchHost{*this};
    Touch68k touch{touchHost, lcd.Width(), InputHeight()};
    Piezo68k piezo;
    WavWriter68k wav;
    bool wavOpen{false};
    vector<int16_t> samples;
    SDL_AudioDeviceID audio{0};
    bool soundOn{true};
    EmEvent<double, double>::HandleT pwmHandler{0};
    unique_ptr<GdbStub> gdb;
    unique_ptr<DesktopChannel> desktop;
    bool desktopQuit{false};
    // The save when the desktop went away failed; the core exits 1.
    bool saveFailed{false};
    bool desktopMode{false};
    string lastSerial, lastInfrared, serialError, infraredError;
    HostLink68k serialLink, irLink, modemSyncLink;
    ModemTransport68k* serialTransport{nullptr};
    IrPhoneTransport68k* irTransport{nullptr};
};

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        auto number = [&](double low, double high, double& out) {
            const char* v = value();
            char* end = nullptr;
            out = v ? strtod(v, &end) : 0;
            return v && !*end && out >= low && out <= high;
        };
        double n = 0;
        if (arg == "--rom") {
            const char* v = value();
            if (!v || !options.image.empty()) return false;
            options.image = v;
        } else if (arg == "--device") {
            const char* v = value();
            if (!v) return false;
            options.device = v;
        } else if(arg=="--dana-view") {
            const char* v=value();DanaViewState state;string error;
            if(!v||!state.Parse(v,error))return false;
            options.danaView=v;
        } else if (arg == "--check-rom") {
            options.checkOnly = true;
        } else if (arg == "--run-for") {
            if (!number(0.001, 3600, options.runFor)) return false;
        } else if (arg == "--framebuffer-output") {
            const char* v = value();
            if (!v) return false;
            options.framebufferOutput = v;
        } else if (arg == "--input-replay") {
            const char* v = value();
            if (!v) return false;
            options.inputReplay = v;
        } else if (arg == "--speed") {
            const char* v = value();
            if (!v || (strcmp(v, "original") && strcmp(v, "uncapped"))) return false;
            options.speed = strcmp(v, "original") ? Speed::uncapped : Speed::original;
        } else if (arg == "--contrast") {
            if (!number(0, 100, n) || n != int(n)) return false;
            options.contrast = int(n);
        } else if (arg == "--window-scale") {
            if (!number(1, 4, n)) return false;
            options.windowScale = float(n);
        } else if (arg == "--no-sound" || arg == "-q") {
            options.sound = false;
        } else if (arg == "--audio-output") {
            const char* v = value();
            if (!v) return false;
            options.audioOutput = v;
        } else if (arg == "--serial" || arg == "--irda") {
            const char* v = value();
            HostLink68k::Kind kind;
            uint16_t port;
            string path;
            if (!v || (!(arg == "--serial" && string(v) == "modem") &&
                       !(arg == "--irda" && (string(v) == "phone" || string(v) == "printer")) &&
                       !HostLink68k::Parse(v, kind, port, &path)))
                return false;
            (arg == "--serial" ? options.serial : options.irda) = v;
        } else if (arg == "--modem-sync") {
            const char* v = value();
            HostLink68k::Kind kind;
            uint16_t port;
            string path;
            if (!v || !HostLink68k::Parse(v, kind, port, &path)) return false;
            options.modemSync = v;
        } else if (arg == "--gdb") {
            if (!number(1, 65535, n) || n != int(n)) return false;
            options.gdbPort = uint16_t(n);
        } else if (arg == "--tone-log") {
            options.toneLog = true;
        } else if (arg == "--smooth-lcd") {
            options.smooth = true;
        } else if (arg == "--window-fit") {
            const char* v = value();
            if (!v || !parseWindowFit(v, options.fit)) return false;
        } else if (arg == "--window-background") {
            const char* v = value();
            if (!v || !parseWindowColour(v, options.windowBackground)) return false;
        } else if (arg == "--audio-period") {
            if (!number(64, 8192, n) || n != int(n)) return false;
            options.audioPeriod = int(n);
        } else if (arg == "--desktop-socket") {
            const char* v = value();
            if (!v) return false;
            options.desktopSocket = v;
        } else if (arg == "--save-on-disconnect") {
            const char* v = value();
            if (!v || v[0] != '/') return false;
            options.saveOnDisconnect = v;
        } else if (arg == "--work-folder" || arg == "--nand" || arg == "--engine" || arg == "--jog-controls" ||
                   arg == "--usb" || arg == "--bluetooth") {
            // The desktop's options for ARM devices and their cards; a IIIx has none of these.
            if (!value()) return false;
            cout << "frond-68k: ignoring " << arg << endl;
        } else if (arg == "--sd-attach" || arg == "--sd2-attach") {
            const char* v=value(); if (!v) return false;
            options.sdAttach[arg=="--sd2-attach"] = v;
        } else if (arg == "--card-swap" || arg == "--card-identity" || arg == "--card-protected") {
            options.refused = arg;
            return false;
        } else if (arg == "--fast-audio" || arg == "--hide-keyboard" ||
                   arg == "--hide-jog-controls") {
            cout << "frond-68k: ignoring " << arg << endl;
        } else if (!arg.empty() && arg[0] != '-' && options.image.empty()) {
            options.image = arg;
        } else {
            return false;
        }
    }
    return !options.image.empty();
}

}  // namespace

int main(int argc, char** argv) {
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    if (argc == 2 && string(argv[1]) == "--version") return cout << "frond-68k 0.15.0" << endl, 0;
    Options options;
    if (!parseOptions(argc, argv, options)) {
        if (options.refused.empty()) return usage(), 2;
        cerr << "frond-68k: " << options.refused << " is not supported: 68k devices have no SD card or Memory Stick slot" << endl;
        return 2;
    }
    if (options.desktopSocket && (options.runFor > 0 || options.checkOnly)) {
        cerr << "--desktop-socket cannot be combined with --run-for or --check-rom" << endl;
        return 2;
    }
    if (options.saveOnDisconnect && !options.desktopSocket) {
        cerr << "--save-on-disconnect needs --desktop-socket" << endl;
        return 2;
    }
    if (options.saveOnDisconnect) {
        string error;
        if (!LockSessionForCore(*options.saveOnDisconnect, error)) {
            cerr << "frond-68k: " << error << endl;
            return 1;
        }
    }
    // Networking only goes the way the hardware did it, over the serial port.
    Feature::SetNetworkRedirection(false);

    vector<uint8_t> image;
    string folder;
    if (!readSessionInput(options.image, image,folder)) {
        cerr << "cannot read ROM " << options.image << endl;
        return 1;
    }

    const DeviceProfile5* profile = nullptr;
    SessionMetadata metadata;
    const bool isSession = isPortable(image) || isLinked(image) || SessionFile5::IsSessionFile(image.size(), image.data());
    if (isSession) {
        if (options.checkOnly) {
            cerr << "--check-rom takes a ROM" << endl;
            return 2;
        }
        CheckedSession session;
        string error = checkSession(image, options.device, session,folder);
        RuntimeCards::Pair next;
        if(error.empty() && !startCore(*session.profile,static_cast<const uint8_t*>(session.file.GetNor()),session.file.GetNorSize())) error="core failed to start";
        if(error.empty()) runtimeCards.SetDevice(session.profile->deviceType);
        if(error.empty()) error=runtimeCards.Stage(session.manifest,next);
        if(error.empty()) error=checkCardState(session,next);
        if(error.empty()) { runtimeCards.Install(next); error=applySession(session); }
        if(error.empty() && !runtimeCards.Matches(next)) error="manifest and protocol insertion disagree";
        if (!error.empty()) {
            cerr << "session refused: " << error << endl;
            return 1;
        }
        profile = session.profile;
        metadata = session.metadata;
    } else {
        ostringstream details;
        if (const auto error = checkRom(image.data(), image.size(), options.device, profile, &details); !error.empty()) {
            cerr << error << endl;
            return 1;
        }
        cout << details.str() << "network redirect " << (Feature::GetNetworkRedirection() ? "on" : "off") << endl;
        if (options.checkOnly) return 0;
        runtimeCards.SetDevice(profile->deviceType);
        if (!startCore(*profile, image.data(), image.size())) {
            cerr << "core failed to start" << endl;
            return 1;
        }
    }
    if ((!profile->capabilities.sound && (options.audioOutput || options.toneLog)) ||
        (!profile->capabilities.serial && options.serial!="off") ||
        (!profile->capabilities.infrared && options.irda!="off")) {
        cerr << "requested peripheral is not qualified for " << profile->name << endl;
        return 1;
    }
    if(options.danaView&&!deviceTypeIsDana5(profile->deviceType)) {
        cerr<<"--dana-view requires a Dana"<<endl;return 1;
    }
    image.clear();
    image.shrink_to_fit();
    cerr << "frond-68k: " << profile->name << " (" << profile->displayName << ") from "
         << (isSession ? "session " : "ROM ") << options.image << endl;

    if (SDL_Init(options.runFor > 0 ? 0 : SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        cerr << "SDL failed to start: " << SDL_GetError() << endl;
        return 1;
    }
    IgnoreGoneXWindows();
    // Before any window exists, so nothing queued is flushed; it sees window points either way.
    RawFingers::Install();
    int status;
    {
        Runner runner(options, *profile, metadata);
        try {
            status = options.runFor > 0 ? runner.RunHeadless() : runner.RunWindow();
        } catch (const exception& error) {
            cerr << error.what() << endl;
            status = 1;
        }
    }
    runtimeCards.Install(RuntimeCards::Pair{});
    cout.flush();
#ifdef __ANDROID__
    // SDL_main returns to the activity, which ends the process.
    return status;
#else
    // The console thread may still be blocked on stdin, so leave without unwinding it.
    _exit(status);
#endif
}
