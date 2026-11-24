// Fixed guest-time input replay for private workloads. No wall-time governor.
// Restores a session, or cold boots a NOR image, on any ARM board and runs it headless.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>

#include "SDL.h"
#include "SDL_image.h"
#include "IrKeyboardMapping.h"
#include "audio_queue.h"
#include "session/session_file5.h"
#include "soc_IMX1.h"
#include "soc_PXA.h"
#include "device.h"
#include "deviceSonyTG50.h"
#include "device_configuration.h"
#include "device_profile5.h"
#include "rom_info5.h"
#include "MachineSession.h"
#include "MsProCards.h"
#include "sdcard.h"
#include "NandFile.h"
#include "host-profile.h"

namespace {
SoC* clockSoc = nullptr;
uint64_t clockStart = 0, epoch = 3875817600ull; // 2026-10-26 00:00:00, Palm local epoch
uint64_t number(const std::string& s, uint64_t maximum) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected unsigned integer: " + s);
    const auto n = std::stoull(s);
    if (n > maximum) throw std::runtime_error("integer exceeds limit: " + s);
    return n;
}
struct Event { uint64_t ms; std::string kind; int x=0, y=0; KeyId key=keyInvalid; };
// The runner's --input-replay pen, key and snapshot lines. Pen bounds are the screen with its
// silkscreen. "-" is no input. The Zodiac also takes "stick X Y" (each -511 to 511) and
// "card-insert" (the --sd card goes in then, not at the start).
std::vector<Event> replay(const char* path, uint64_t end, int width=320, int height=480, bool zodiac=false) {
    std::vector<Event> events;
    if (std::string(path)=="-") return events;
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read replay");
    const std::map<std::string, KeyId> keys = {
        {"hard1",keyIdHard1},{"hard2",keyIdHard2},{"hard3",keyIdHard3},{"hard4",keyIdHard4},
        {"home",keyIdHard1},{"home-key",keyIdHome},{"calendar",keyIdHard2},{"contacts",keyIdHard3},
        {"web",keyIdHard4},{"up",keyIdUp},{"down",keyIdDown},
        {"left",keyIdLeft},{"right",keyIdRight},{"center",keyIdSelect},{"power",keyIdPower},
        {"voice",keyIdVoice},{"graffiti",keyIdGraffiti},{"back",keyIdBack},{"hold",keyIdHold},
        {"hotsync",keyIdHotSync},{"jog-up",keyIdJogUp},{"jog-down",keyIdJogDown},{"jog-press",keyIdJogPress},
        {"function",keyIdFunction},{"bluetooth",keyIdBluetooth},{"trigger-left",keyIdTriggerLeft},
        {"trigger-right",keyIdTriggerRight},{"stick-press",keyIdStickPress}};
    std::string line;
    while (std::getline(input,line)) {
        std::istringstream tokens(line);
        std::string at, extra;
        if (!(tokens >> at) || at[0]=='#') continue;
        Event e{number(at,end),""};
        if (!events.empty() && e.ms < events.back().ms) throw std::runtime_error("unordered replay");
        if (!(tokens >> e.kind)) throw std::runtime_error("missing replay command");
        if (e.kind=="pen-down" || e.kind=="pen-move") {
            if (!(tokens>>e.x>>e.y) || e.x<0 || e.x>=width || e.y<0 || e.y>=height)
                throw std::runtime_error("invalid pen coordinates");
        } else if (e.kind=="key-down" || e.kind=="key-up") {
            if (!(tokens>>extra)) throw std::runtime_error("invalid key");
            if (keys.count(extra)) e.key=keys.at(extra);
            else {
                const auto id=number(extra,keyIdCount-1);
                if (!id) throw std::runtime_error("invalid key");
                e.key=KeyId(id);
            }
        } else if (e.kind=="snapshot") {
            // Accept native replay syntax but always redirect into the private output directory.
            if (!(tokens>>std::quoted(extra)) || extra.empty()) throw std::runtime_error("missing snapshot path");
        } else if (e.kind=="keyboard-down" || e.kind=="keyboard-up") {
            if (!(tokens>>std::quoted(extra))) throw std::runtime_error("invalid keyboard key");
            e.x=irKeyboardCode(SDL_GetScancodeFromName(extra.c_str()));
            if (!e.x) throw std::runtime_error("invalid keyboard key");
        } else if (zodiac && e.kind=="stick") {
            if (!(tokens>>e.x>>e.y) || e.x<-511 || e.x>511 || e.y<-511 || e.y>511)
                throw std::runtime_error("invalid stick offset");
        } else if (e.kind!="pen-up" && e.kind!="keyboard-attach" && e.kind!="keyboard-detach" && !(zodiac && e.kind=="card-insert")) throw std::runtime_error("unsupported replay command");
        if (zodiac && e.kind.rfind("keyboard-",0)==0) throw std::runtime_error("the Zodiac has no keyboard");
        if ((tokens>>extra) && extra[0]!='#') throw std::runtime_error("extra replay arguments");
        events.push_back(e);
    }
    if (input.bad()) throw std::runtime_error("replay read failed");
    return events;
}
uint64_t hashBytes(uint64_t hash, const void* data, size_t count) {
    auto bytes=static_cast<const uint8_t*>(data);
    for (size_t i=0;i<count;++i) { hash^=bytes[i]; hash*=1099511628211ull; }
    return hash;
}
constexpr uint64_t hashInit=14695981039346656037ull;

// A machine folder has no bytes to read here: loadPath opens it.
std::vector<uint8_t> readFile(const char* path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path,ec)) return {};
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if (!input || input.tellg()<=0 || input.tellg()>512*1024*1024)
        throw std::runtime_error("cannot read session or NOR image");
    std::vector<uint8_t> data(size_t(input.tellg()));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("incomplete session");
    return data;
}

// What a run starts from: the images the SoC runs on (kept alive for it) and the state to restore.
struct Machine {
    DeviceType5 device{deviceTypeInvalid};
    uint32_t ramSize{};
    std::vector<uint8_t> nor, nand, memory, savestate;
    std::vector<frondMachine::Media> media;
    std::filesystem::path folder;  // the work copy a folder session's cards and NAND run from
    bool legacy{};                 // a v6 TX session, restored as this tool always did
};

bool pxaBoard(DeviceBoard5 board) {
    return board==DeviceBoard5::pxa255E2 || board==DeviceBoard5::pxa270Tx ||
           board==DeviceBoard5::pxa270Zire72 || board==DeviceBoard5::pxa250SonyTG50;
}

uint32_t ramSizeFromMemorySize(uint32_t memorySize) {
    return memorySize ? (1u << (31 - __builtin_clz(memorySize))) : 0;
}

// A v6 TX session restores from its bytes. Any other session is copied into OUTPUT/machine
// first, so its cards and NAND run on copies and the original is never written.
Machine loadSession(const char* path, const std::vector<uint8_t>& data, const std::filesystem::path& output) {
    Machine m;
    uint32_t version=0; std::string headerDevice;
    frondSession::peekHeader(data.data(),data.size(),version,headerDevice);
    SessionFile5 legacy;
    auto bytes=[](std::vector<uint8_t>& out, const void* data, size_t size) {
        const auto* p=static_cast<const uint8_t*>(data);
        out.assign(p,p+size);
    };
    if (version==6 && legacy.Deserialize(data.size(),data.data()) && legacy.GetDeviceName()=="palm-tx") {
        if (legacy.GetSdSize()) throw std::runtime_error("use a session without an SD image for this benchmark");
        m.legacy=true;
        m.device=DeviceType5::deviceTypeTX;
        m.ramSize=legacy.GetRamSize()?legacy.GetRamSize():32*1024*1024;
        bytes(m.nor,legacy.GetNor(),legacy.GetNorSize());
        bytes(m.nand,legacy.GetNand(),legacy.GetNandSize());
        bytes(m.memory,legacy.GetMemory(),legacy.GetMemorySize());
        bytes(m.savestate,legacy.GetSavestate(),legacy.GetSavestateSize());
        if (m.memory.empty() || m.savestate.empty()) throw std::runtime_error("complete RAM and peripheral state required");
        return m;
    }
    const auto work=output/"machine";
    if (mkdir(work.c_str(),0700)) throw std::runtime_error("cannot make the work folder");
    frondSession::Contents contents;
    std::filesystem::path folder;
    if (!frondSession::loadPath(std::filesystem::absolute(path),contents,folder,frondSession::msPhysicalBytes,work))
        throw std::runtime_error("session did not load");
    const auto* profile=deviceProfileNamed5(contents.deviceName.c_str());
    if (!profile || !deviceProfileRunnerEligible5(profile->deviceType) || !pxaBoard(profile->board))
        throw std::runtime_error("this session's device runs in frond-68k or has no board: "+contents.deviceName);
    m.device=DeviceType5(profile->deviceType);
    m.ramSize=contents.ramSize?contents.ramSize:ramSizeFromMemorySize(contents.memory.size());
    m.nor=std::move(contents.nor); m.nand=std::move(contents.nand);
    m.memory=std::move(contents.memory); m.savestate=std::move(contents.savestate);
    m.media=std::move(contents.media);
    m.folder=folder;
    // A NAND that lives in the machine folder is attached after the SoC is made; the SoC needs a buffer of the right size first.
    if (m.nand.empty())
        for (const auto& media : m.media)
            if (media.kind==frondMachine::kindNand) m.nand.assign(deviceConfigurationGetNandSize(m.device),0xff);
    // A card the session carries opens in the card store, in the slot as saved. The work copy takes its writes.
    if (std::any_of(m.media.begin(),m.media.end(),[](const auto& media) { return media.kind==frondMachine::kindSd; })) {
        frondSession::Contents card;
        card.media=m.media;
        if (!frondNative::attachFolderMedia(card,folder)) throw std::runtime_error("the session's card did not open");
    }
    if (m.savestate.empty() || m.memory.empty()) throw std::runtime_error("complete RAM and peripheral state required");
    if (m.device==deviceTypeTG50 && m.memory.size()!=tg50RamBackingSize+2*MEMORY_BUFFER_GRANULARITY)
        throw std::runtime_error("TG50 session requires complete RAM and CPU/peripheral state");
    return m;
}

// A cold boot from a NOR image, like the runner's `frond NOR --device NAME [--nand NAND]`. Without
// a NAND image a device with NAND gets a blank one, as in the runner. The NAND runs from memory, so
// the file is never written.
Machine loadNor(std::vector<uint8_t> data, const std::string& deviceName, const std::string& nandPath) {
    Machine m;
    RomInfo5 rom(data.data(),data.size());
    DeviceType5 device=rom.IsValid()?rom.GetDeviceType():deviceTypeInvalid;
    if (!deviceName.empty()) {
        const auto* profile=deviceProfileNamed5(deviceName.c_str());
        if (!profile || profile->deviceType==deviceTypeInvalid) throw std::runtime_error("unknown device "+deviceName);
        if (device!=deviceTypeInvalid && device!=profile->deviceType) throw std::runtime_error("device does not match the ROM");
        device=DeviceType5(profile->deviceType);
    }
    if (!rom.IsValid() || device==deviceTypeInvalid) throw std::runtime_error("not a session or a NOR image Frond knows");
    if (!deviceProfileRunnerEligible5(device) || !pxaBoard(deviceProfile5(device)->board))
        throw std::runtime_error("this ROM's device runs in frond-68k or has no board");
    if (!nandPath.empty()) m.nand=readFile(nandPath.c_str());
    else m.nand.assign(deviceConfigurationGetNandSize(device),0xff);
    if (deviceProfileValidateFirmware5(device,data.size(),m.nand.size(),false,!nandPath.empty(),rom.GetFirmwareVariant())!=DeviceFirmwareError5::none)
        throw std::runtime_error("invalid firmware geometry for the device (the TX needs --nand)");
    m.device=device;
    m.ramSize=rom.GetRecommendedRamSize();
    if (!deviceConfigurationSupportsRamSize(device,m.ramSize)) throw std::runtime_error("unsupported RAM size");
    m.nor=std::move(data);
    return m;
}

// Writes through our own FILE: on Android SDL_RWFromFile sends a relative path to the app's
// internal storage through JNI, which a command-line program doesn't have, and crashes.
bool savePng(const std::filesystem::path& path, std::vector<uint32_t>& frame, int width, int height) {
    auto surface=SDL_CreateRGBSurfaceWithFormatFrom(frame.data(),width,height,32,width*4,SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    FILE* file=std::fopen(path.c_str(),"wb");
    SDL_RWops* rw=file?SDL_RWFromFP(file,SDL_TRUE):nullptr;
    if (file && !rw) std::fclose(file);
    const bool ok=rw && IMG_SavePNG_RW(surface,rw,1)==0;
    SDL_FreeSurface(surface);
    return ok;
}

// Each thread's name and time on a CPU in ns, from /proc (Linux and Android; empty elsewhere), for the
// "threads" line: the Zodiac run's emu thread and its fill worker separately.
std::map<std::string,std::pair<std::string,uint64_t>> threadCpuNs() {
    std::map<std::string,std::pair<std::string,uint64_t>> out;
    std::error_code error;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task",error)) {
        std::ifstream comm(task.path()/"comm"), sched(task.path()/"schedstat");
        std::string name; uint64_t ns=0;
        if (!std::getline(comm,name) || !(sched >> ns)) continue;
        out[task.path().filename().string()]={name,ns};
    }
    return out;
}
// A Zodiac cold boot: its own machine, no session. Counts and hashes as for the PXA boards;
// instructions are the machine's retired count and jit_instructions those its Fast engine ran in
// Dynarmic blocks. Its runs have no host time limit and its input is stamped with guest time, so
// they repeat. A --sd card runs from memory, so the file is never written.
// BENCH_ZODIAC_SAVE=MS:FILE saves the machine at the first slice boundary at or after MS into FILE (the
// guest ns since the start, then the state) and stops there, or carries on with BENCH_ZODIAC_SAVE_CONTINUE=1.
// BENCH_ZODIAC_SAVE_CONTINUE=reload carries on from the state it saved, loaded in place, which lets go of the
// held keys and pen as a load does. BENCH_ZODIAC_LOAD=FILE starts from such a file
// instead of the cold boot, with the same replay; events before the saved time are skipped and summary
// counts are from the load. Either engine can load a state the other saved.
uint64_t eventMs() { return clockSoc ? (clockSoc->GetTime()-clockStart)/1000000 : 0; }
// BENCH_ZODIAC_SD2=FILE: a card in slot 2 (the W4200's SD slot) from the start, in sdCardStore(1) in
// memory. The run's end lists the sectors that differ from the file on stderr.
std::vector<uint8_t> secondCard;
void listChangedSectors() {
    auto& store=sdCardStore(1);
    std::vector<uint32_t> sectors;
    std::vector<uint8_t> sector(512);
    for (uint32_t i=0; i<store.sectorCount(); ++i)
        if (store.read(i,sector.data()) && std::memcmp(sector.data(),secondCard.data()+size_t(i)*512,512)) sectors.push_back(i);
    std::fprintf(stderr,"sd2 changed %zu sectors:",sectors.size());
    for (size_t i=0; i<sectors.size() && i<256; ++i) std::fprintf(stderr," %u",sectors[i]);
    std::fprintf(stderr,"%s\n",sectors.size()>256?" ...":"");
}
void runZodiac(std::vector<uint8_t>& rom, DeviceType5 device, const char* replayArg, uint64_t duration,
               const char* mipsArg, bool sound, const std::filesystem::path& output, const std::string& sdPath) {
    const uint64_t mips=std::string(mipsArg)=="original"?deviceProfileOriginalHz5(device)/1000000:number(mipsArg,3000);
    if (!mips) throw std::runtime_error("MHz must be positive");
    const auto display=deviceConfigurationDisplayConfigForDevice(device,deviceConfigurationDefaultDisplayMode(device));
    const auto events=replay(replayArg,duration,display.width,display.height,true);
    const bool cardLater=std::any_of(events.begin(),events.end(),[](const Event& e) { return e.kind=="card-insert"; });
    if (cardLater && sdPath.empty()) throw std::runtime_error("card-insert needs --sd");
    std::vector<uint8_t> card;
    if (!sdPath.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sdPath,ec)) throw std::runtime_error("--sd needs an SD image file");
        std::ifstream input(sdPath,std::ios::binary);
        card.assign(std::istreambuf_iterator<char>(input),{});
        if (input.bad() || card.empty()) throw std::runtime_error("cannot read the SD image, or it is empty");
        if (card.size()%512) throw std::runtime_error("the SD image isn't a whole number of sectors");
    }
    if (mkdir(output.c_str(),0700)) throw std::runtime_error("output must be a new directory in an existing parent");
    cpuSetFastEngine(false);
    std::string why;
    std::unique_ptr<SocIMX1> soc(SocIMX1::Create(rom.data(),rom.size(),why));
    if (!soc) throw std::runtime_error("Tapwave Zodiac 2: "+why);
    const char* jit=std::getenv("FROND_JIT");
    const bool fast=jit && std::string(jit)=="1";
    if (fast && !soc->SetFastEngine(true)) throw std::runtime_error("the Zodiac's Fast engine isn't available");
    soc->SetHostTimeLimit(false);
    // BENCH_BATTERY: the battery setting, as --battery takes it (Maximum without).
    if (const char* spec=std::getenv("BENCH_BATTERY"); spec && *spec) {
        BatteryPower battery;
        if (!BatteryPower::Parse(spec,battery)) throw std::runtime_error("BENCH_BATTERY is max, level N or host N, then charging or battery");
        soc->SetBattery(battery);
    }
    clockStart=soc->GetTime(); clockSoc=soc.get();
    SoC::SetEventClock(eventMs);
    if (!card.empty()) {
        sdCardInitializeWithBytes(std::move(card),"benchmark-replay");
        if (!cardLater) soc->SdInsert();
    }
    if (const char* second=std::getenv("BENCH_ZODIAC_SD2")) {
        secondCard=readFile(second);
        if (secondCard.empty() || secondCard.size()%512) throw std::runtime_error("BENCH_ZODIAC_SD2 needs an SD image");
        sdCardStore(1).initializeWithBytes(std::vector<uint8_t>(secondCard),"benchmark-replay-2");
        if (!soc->Sd2Insert() || !soc->SecondCardAttached()) throw std::runtime_error("BENCH_ZODIAC_SD2: slot 2 refused the card");
    }
    auto queue=audioQueueCreate(8192);
    soc->SetAudioQueue(queue); soc->SetPcmOutputEnabled(sound); soc->SetPcmSuspended(false);
    size_t next=0;
    uint64_t saveAt=UINT64_MAX;
    std::string savePath;
    if (const char* save=std::getenv("BENCH_ZODIAC_SAVE")) {
        const std::string text=save;
        const auto colon=text.find(':');
        if (colon==std::string::npos || colon+1==text.size()) throw std::runtime_error("BENCH_ZODIAC_SAVE needs MS:FILE");
        saveAt=number(text.substr(0,colon).c_str(),UINT32_MAX)*1000000; savePath=text.substr(colon+1);
    }
    const char* goOn=std::getenv("BENCH_ZODIAC_SAVE_CONTINUE");
    const bool reloadAfterSave=goOn && std::string(goOn)=="reload";
    const bool continueAfterSave=goOn && (std::string(goOn)=="1" || reloadAfterSave);
    // BENCH_ZODIAC_MUSIC_HOOKS=1 turns the music hooks on (events counted, not printed), =log prints them too.
    const char* musicHooks=std::getenv("BENCH_ZODIAC_MUSIC_HOOKS");
    const bool musicLog=musicHooks && std::string(musicHooks)=="log";
    if (musicHooks && (std::string(musicHooks)=="1" || musicLog) && !soc->SetMusicHooks(true,musicLog))
        throw std::runtime_error("BENCH_ZODIAC_MUSIC_HOOKS: refused");
    if (const char* load=std::getenv("BENCH_ZODIAC_LOAD")) {
        auto state=readFile(load);
        uint64_t savedNs=0;
        if (state.size()<=sizeof(savedNs)) throw std::runtime_error("BENCH_ZODIAC_LOAD: not a saved state");
        std::memcpy(&savedNs,state.data(),sizeof(savedNs));
        if (!soc->Load(state.size()-sizeof(savedNs),state.data()+sizeof(savedNs))) throw std::runtime_error("BENCH_ZODIAC_LOAD: refused");
        clockStart=soc->GetTime()-savedNs;
        // The run that saved took these before saving.
        while (next<events.size() && events[next].ms*1000000<=savedNs) ++next;
        std::fprintf(stderr,"loaded at %.3f s\n",savedNs/1e9);
    }
    const uint64_t resumedNs=soc->GetTime()-clockStart;
    const uint64_t retiredStart=soc->Status().retired, jitStart=soc->JitRetired();
    const size_t pixels=size_t(zodiacPanelWidth)*zodiacPanelHeight;
    std::vector<uint32_t> frame(pixels);
    uint64_t lastHash=hashInit, frameSequence=hashInit, pcmHash=hashInit, frameCount=0, changed=0, pcmFrames=0, cycles=0;
    unsigned snapshots=0;
    bool haveFrame=false;
    const char* traceEnv=std::getenv("BENCH_ZODIAC_TRACE");
    const bool trace=traceEnv!=nullptr;
    const uint64_t traceStepMs=trace&&*traceEnv?number(traceEnv,100000):250;
    uint64_t traceNext=0;
    bool lastLcd=true;  // changed frames and handed-over events, guest time
    // BENCH_ZODIAC_WAV=FILE keeps the sound (44100 Hz stereo, 16-bit) as a WAV. BENCH_ZODIAC_PACE=FILE writes a line
    // per 20 ms of guest time: guest ms, wall ms, PCM frames and SSI1 underrun words so far.
    const char* wavEnv=std::getenv("BENCH_ZODIAC_WAV");
    std::vector<uint32_t> wav;
    std::ofstream pace;
    if (const char* paceEnv=std::getenv("BENCH_ZODIAC_PACE")) pace.open(paceEnv);
    const uint64_t underrunStart=soc->PcmUnderrunWords();
    uint64_t paceNext=0;
    const auto threadsStart=threadCpuNs();
    const auto start=std::chrono::steady_clock::now();
    for (;;) {
        uint32_t first=0,last=0;
        if (auto pending=soc->GetPendingFrame(first,last)) {
            std::memcpy(frame.data(),pending,pixels*sizeof(uint32_t));
            const auto hash=hashBytes(hashInit,frame.data(),pixels*sizeof(uint32_t));
            const uint64_t guest=soc->GetTime()-clockStart;
            frameSequence=hashBytes(frameSequence,&guest,sizeof(guest));
            frameSequence=hashBytes(frameSequence,&hash,sizeof(hash));
            changed+=!haveFrame || hash!=lastHash;
            if (trace && (!haveFrame || hash!=lastHash)) std::fprintf(stderr,"frame %.3f ms hash %llx\n",guest/1e6,(unsigned long long)hash);
            lastHash=hash; haveFrame=true; ++frameCount;
            soc->ResetPendingFrame();
        }
        if (soc->Halted()) break;
        const auto elapsed=soc->GetTime()-clockStart;
        while (next<events.size() && events[next].ms*1000000<=elapsed) {
            const auto& e=events[next++];
            if (trace) std::fprintf(stderr,"event %s at %.3f ms (due %llu)\n",e.kind.c_str(),elapsed/1e6,(unsigned long long)e.ms);
            if (e.kind=="pen-down" || e.kind=="pen-move") soc->PenDown(e.x,e.y);
            else if (e.kind=="pen-up") soc->PenUp();
            else if (e.kind=="key-down") soc->KeyDown(e.key);
            else if (e.kind=="key-up") soc->KeyUp(e.key);
            else if (e.kind=="stick") {
                // The orientation is polled here, at stick events only; the stick follows it.
                const unsigned orientation=soc->GuestOrientation();
                if (trace) std::fprintf(stderr,"stick %d %d, orientation %u\n",e.x,e.y,orientation);
                soc->AnalogStick(e.x/511.0f,e.y/511.0f);
            }
            else if (e.kind=="card-insert") soc->SdInsert();
            else {
                // The panel as it is, portrait (the host turns it), black before the first frame.
                const auto path=output/("snapshot-"+std::to_string(++snapshots)+".png");
                if (!savePng(path,frame,zodiacPanelWidth,zodiacPanelHeight)) throw std::runtime_error("snapshot failed");
            }
        }
        if (elapsed>=saveAt) {
            if (!soc->Save()) throw std::runtime_error("BENCH_ZODIAC_SAVE: save failed");
            const auto buffer=soc->GetSavestate();
            std::ofstream out(savePath,std::ios::binary);
            out.write(reinterpret_cast<const char*>(&elapsed),sizeof(elapsed));
            out.write(static_cast<const char*>(buffer.data),std::streamsize(buffer.size));
            out.flush(); if (!out) throw std::runtime_error("BENCH_ZODIAC_SAVE: write failed");
            std::fprintf(stderr,"saved at %.3f s ready reads %llu\n",elapsed/1e9,(unsigned long long)soc->YmfReadyReads());
            if (!continueAfterSave) break;
            saveAt=UINT64_MAX;
            if (reloadAfterSave) {
                std::vector<uint8_t> copy(static_cast<const uint8_t*>(buffer.data),static_cast<const uint8_t*>(buffer.data)+buffer.size);
                if (!soc->Load(copy.size(),copy.data()))
                    throw std::runtime_error("BENCH_ZODIAC_SAVE_CONTINUE: reload refused");
                clockStart=soc->GetTime()-elapsed;
                std::fprintf(stderr,"loaded at %.3f s\n",elapsed/1e9);
            }
        }
        if (elapsed>=duration*1000000) break;
        cycles+=soc->Run(mips*1000,mips*1000000);
        uint32_t pcm[8192];
        const auto n=audioQueuePopChunk(queue,pcm,8192);
        pcmHash=hashBytes(pcmHash,pcm,n*sizeof(uint32_t)); pcmFrames+=n;
        if (wavEnv) wav.insert(wav.end(),pcm,pcm+n);
        if (pace.is_open() && soc->GetTime()-clockStart>=paceNext) {
            const auto now=soc->GetTime()-clockStart;
            pace << now/1000000 << ' ' << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count() << ' '
                 << pcmFrames << ' ' << soc->PcmUnderrunWords()-underrunStart << '\n';
            paceNext=now-now%20000000+20000000;
        }
        if (trace) {
            const bool lcd=soc->LcdEnabled();
            if (lcd!=lastLcd) { std::fprintf(stderr,"lcd %s at %.3f ms wall %.3f\n",lcd?"on":"off",(soc->GetTime()-clockStart)/1e6,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()); lastLcd=lcd; }
        }
        if (trace && elapsed>=traceNext) {
            const auto st=soc->Status();
            std::fprintf(stderr,"state %.0f ms pc %08x retired %llu idleHclk %llu wall %.3f jit %llu runs %llu skips %llu haltskips %llu ready reads %llu asp polls %llu rounds %llu cycles %llu cross %llu refused %llu lag %llu boundaries %llu ends %llu defer %llu\n",elapsed/1e6,st.pc,(unsigned long long)st.retired,(unsigned long long)st.idleHclk,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),(unsigned long long)soc->JitRetired(),(unsigned long long)soc->JitRuns(),(unsigned long long)soc->DelaySkips(),(unsigned long long)soc->DelayHaltSkips(),(unsigned long long)soc->YmfReadyReads(),(unsigned long long)soc->AspPollSkips(),(unsigned long long)soc->AspPollRounds(),(unsigned long long)soc->AspPollCycles(),(unsigned long long)soc->CrossSpans(),(unsigned long long)soc->CrossRefusals(),(unsigned long long)soc->LagSubmits(),(unsigned long long)soc->LagBoundaries(),(unsigned long long)soc->CrossEnds(),(unsigned long long)soc->DeferredSubmits());
            traceNext=elapsed+traceStepMs*1000000;
        }
    }
    const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    {
        std::string line;
        for (const auto& [tid,thread] : threadCpuNs()) {
            const auto before=threadsStart.find(tid);
            const uint64_t ns=thread.second-(before==threadsStart.end()?0:before->second.second);
            if (ns>=1000000) line+=" "+thread.first+" "+std::to_string(ns/1000000)+" ms";
        }
        if (!line.empty()) std::fprintf(stderr,"threads cpu:%s\n",line.c_str());
    }
    if (wavEnv) {
        const uint32_t bytes=uint32_t(wav.size()*4), rate=44100, byteRate=rate*4, fmtSize=16, riffSize=36+bytes;
        const uint16_t format=1, channels=2, align=4, bits=16;
        std::ofstream out(wavEnv,std::ios::binary);
        out.write("RIFF",4); out.write((const char*)&riffSize,4); out.write("WAVEfmt ",8); out.write((const char*)&fmtSize,4);
        out.write((const char*)&format,2); out.write((const char*)&channels,2); out.write((const char*)&rate,4);
        out.write((const char*)&byteRate,4); out.write((const char*)&align,2); out.write((const char*)&bits,2);
        out.write("data",4); out.write((const char*)&bytes,4); out.write((const char*)wav.data(),bytes);
        out.flush(); if (!out) throw std::runtime_error("BENCH_ZODIAC_WAV: write failed");
    }
    if (pace.is_open()) { pace.flush(); if (!pace) throw std::runtime_error("BENCH_ZODIAC_PACE: write failed"); }
    if (std::getenv("BENCH_ZODIAC_PACE")) std::fprintf(stderr,"ssi underrun words %llu\n",(unsigned long long)(soc->PcmUnderrunWords()-underrunStart));
    const uint64_t guestNs=soc->GetTime()-clockStart-resumedNs, instructions=soc->Status().retired-retiredStart;
    const uint64_t jitInstructions=soc->JitRetired()-jitStart;
    if (soc->Halted()) std::fprintf(stderr,"stopped: %s\n",soc->StopText().c_str());
    if (!secondCard.empty()) listChangedSectors();
    if (trace) std::fprintf(stderr,"lcd %s at the end\n%s\nmemory hash %llx\npcm hash %llx\nready last read at %lld ns\nready reads %llu\n",soc->LcdEnabled()?"on":"off",
                            soc->ChipSummary().c_str(),(unsigned long long)soc->MemoryHash(),(unsigned long long)pcmHash,
                            (long long)(soc->YmfLastReadyReadNs()-clockStart),(unsigned long long)soc->YmfReadyReads());
    std::ofstream result(output/"summary.json");
    result << std::setprecision(12) << "{\"version\":1,\"device\":\"" << deviceName5(device) << "\",\"cold_boot\":true"
           << ",\"engine\":\"" << (fast?"fast":"exact") << "\",\"jit_async\":false,\"mips\":" << mips << ",\"sound\":" << (sound?"true":"false")
           << ",\"guest_ns\":" << guestNs << ",\"cycles\":" << cycles << ",\"instructions\":" << instructions
           << ",\"jit_instructions\":" << jitInstructions << ",\"halted\":" << (soc->Halted()?"true":"false") << ",\"wall_seconds\":" << wall
           << ",\"lcd_frames\":" << frameCount << ",\"changed_frames\":" << changed << ",\"pcm_frames\":" << pcmFrames
           << ",\"frame_sequence_hash\":\"" << std::hex << frameSequence << "\",\"pcm_hash\":\"" << pcmHash << "\"}\n";
    result.flush(); if (!result) throw std::runtime_error("summary write failed");
    std::printf("%s cold boot: guest %.3f s, %llu cycles, wall %.3f s, %llu instructions (%llu in the JIT)%s, %llu frames, "
                "frame hash %llx, %llu PCM frames, PCM hash %llx\n",deviceName5(device),guestNs/1e9,(unsigned long long)cycles,wall,
                (unsigned long long)instructions,(unsigned long long)jitInstructions,soc->Halted()?", stopped":"",
                (unsigned long long)frameCount,(unsigned long long)frameSequence,(unsigned long long)pcmFrames,(unsigned long long)pcmHash);
    if (soc->MusicHooksOn()) {
        const auto& m=soc->MusicHooks().stats();
        std::printf("music hooks: %llu calls, %llu opens, %llu bad files, %llu plays unmatched, %zu events waiting, %llu dropped\n",
                    (unsigned long long)m.calls,(unsigned long long)m.opens,(unsigned long long)m.badFiles,
                    (unsigned long long)m.unmatchedPlays,soc->MusicHooks().pending(),(unsigned long long)m.dropped);
    }
    SoC::SetEventClock(nullptr);
    clockSoc=nullptr;
}
}
// This executable replaces timeutil.o: guest RTC is tied to restored virtual time.
uint64_t palmEpochSeconds() {
    return epoch + (clockSoc ? (clockSoc->GetTime()-clockStart)/1000000000ull : 0);
}

int main(int argc, char** argv) {
    // Fast compiles where asked for, so runs repeat. BENCH_JIT_ASYNC=1 compiles on the compile
    // thread as the app does: its time is closer to the app's, but Fast hashes may change from run to run.
    const char* asyncEnv=std::getenv("BENCH_JIT_ASYNC");
    const bool jitAsync=asyncEnv && *asyncEnv && std::string(asyncEnv)!="0";
    cpuSetJitSync(!jitAsync);
    try {
        // --check-replay REPLAY MILLISECONDS [DEVICE]: parse only, with the device's screen and commands.
        if ((argc==4 || argc==5) && std::string(argv[1])=="--check-replay") {
            const auto* profile=argc==5?deviceProfileNamed5(argv[4]):nullptr;
            if (argc==5 && (!profile || profile->deviceType==deviceTypeInvalid)) throw std::runtime_error(std::string("unknown device ")+argv[4]);
            if (!profile) { replay(argv[2],number(argv[3],600000)); return 0; }
            const auto device=DeviceType5(profile->deviceType);
            const auto display=deviceConfigurationDisplayConfigForDevice(device,deviceConfigurationDefaultDisplayMode(device));
            const bool zodiac=profile->board==DeviceBoard5::imx1Zodiac;
            replay(argv[2],number(argv[3],600000),display.width,display.height+(zodiac?0:display.graffitiHeight),zodiac);
            return 0;
        }
        std::string deviceName, nandPath, sdPath;
        int skip=0;
        while (argc>skip+2 && (std::string(argv[skip+1])=="--device" || std::string(argv[skip+1])=="--nand" ||
                               std::string(argv[skip+1])=="--sd")) {
            const std::string option=argv[skip+1];
            (option=="--device"?deviceName:option=="--nand"?nandPath:sdPath)=argv[skip+2];
            skip+=2;
        }
        char** arg=argv+skip;  // arg[1] is SESSION
        const int count=argc-skip;
        if (count!=7 && count!=8)
            throw std::runtime_error("usage: benchmark-replay [--device NAME] [--nand NAND] [--sd CARD (Zodiac)] SESSION|NOR REPLAY|- MILLISECONDS MHZ|original on|off NEW-output-dir [palm-epoch-seconds]");
        const auto duration=number(arg[3],600000);
        if (!duration) throw std::runtime_error("duration must be positive");
        const std::string soundArg=arg[5];
        if (soundArg!="on" && soundArg!="off") throw std::runtime_error("sound must be on or off");
        const bool sound=soundArg=="on";
        if (count==8) epoch=number(arg[7],UINT32_MAX-600);
        const std::filesystem::path output=arg[6];
        auto data=readFile(arg[1]);
        uint32_t version=0; std::string headerDevice;
        const bool folderSession=data.empty();
        const bool coldBoot=!folderSession && !frondSession::peekHeader(data.data(),data.size(),version,headerDevice);
        if (!coldBoot && (!deviceName.empty() || !nandPath.empty()))
            throw std::runtime_error("--device and --nand are for a NOR image; a session names its device");
        Machine m;
        if (coldBoot && !deviceName.empty()) {
            const auto* profile=deviceProfileNamed5(deviceName.c_str());
            if (profile && profile->board==DeviceBoard5::imx1Zodiac) {
                runZodiac(data,DeviceType5(profile->deviceType),arg[2],duration,arg[4],soundArg=="on",arg[6],sdPath);
                return 0;
            }
        }
        if (!sdPath.empty()) throw std::runtime_error("--sd is for a Zodiac cold boot only");
        if (coldBoot) { m=loadNor(std::move(data),deviceName,nandPath); data={}; }
        const auto checkInputs=[&](DeviceType5 device) {
            const uint64_t mips=std::string(arg[4])=="original"?deviceProfileOriginalHz5(device)/1000000:number(arg[4],3000);
            if (!mips) throw std::runtime_error("MHz must be positive");
            const auto display=deviceConfigurationDisplayConfigForDevice(device,deviceConfigurationDefaultDisplayMode(device));
            if (display.width<=0 || display.height<=0) throw std::runtime_error("device has no display");
            return std::make_pair(mips,replay(arg[2],duration,display.width,display.height+display.graffitiHeight));
        };
        // A cold boot's MHz and replay are checked before the output directory is made.
        if (coldBoot) checkInputs(m.device);
        // Atomic exclusive mkdir, private from creation (unlike create_directory + chmod).
        if (mkdir(output.c_str(),0700)) throw std::runtime_error("output must be a new directory in an existing parent");
        if (!coldBoot) { m=loadSession(arg[1],data,output); data={}; }
        const auto device=m.device;
        const auto [mips,events]=checkInputs(device);
        const auto display=deviceConfigurationDisplayConfigForDevice(device,deviceConfigurationDefaultDisplayMode(device));
        const int width=display.width, height=display.height;

        const auto board=deviceProfile5(device)->board;
        const bool pxa270=board==DeviceBoard5::pxa270Tx || board==DeviceBoard5::pxa270Zire72;
        auto soc=std::make_unique<SocPXA>(device,m.ramSize,m.nor.data(),m.nor.size(),
                   m.nand.empty()?nullptr:m.nand.data(),m.nand.size(),-1,pxa270?2:deviceGetSocRev());
        const bool original=std::string(arg[4])=="original";
        soc->SetFollowCoreClock(original);
        for (const auto& media : m.media)
            if (media.kind==frondMachine::kindNand && !frondNand::attachFile(soc->GetNand(),m.folder,media))
                throw std::runtime_error("session NAND did not attach");
        if (!coldBoot) {
            if (m.memory.size()>soc->GetMemoryData().size ||
                ((m.legacy || device==DeviceType5::deviceTypeTX) && m.memory.size()!=soc->GetMemoryData().size))
                throw std::runtime_error("complete RAM and peripheral state required");
            std::memcpy(soc->GetMemoryData().data,m.memory.data(),m.memory.size());
            // A stick runs from the work copy, in the slot as saved.
            auto* stick=soc->GetMemoryStick();
            for (const auto& media : m.media) {
                if (media.kind==frondMachine::kindMs) {
                    if (!frondNative::attachClassicFile(stick,m.folder/frondMachine::msSibling,media.identity,true,media.writeProtected))
                        throw std::runtime_error("session Memory Stick did not attach");
                } else if (media.kind==frondMachine::kindMsPro) {
                    frondSession::Contents pro;
                    pro.media={media};
                    if (!frondNative::attachFolderMedia(pro,m.folder) || !frondNative::attachMsPro(stick,false))
                        throw std::runtime_error("session Memory Stick Pro did not attach");
                }
            }
            if (!soc->Load(m.savestate.size(),m.savestate.data())) throw std::runtime_error("restore failed");
            if (!m.legacy) soc->ReleaseKeyboard();
            // A Memory Stick device has no SD slot: an SD flag from the shared card store is dropped.
            if (stick) soc->SdEject();
            const bool hasSd=std::any_of(m.media.begin(),m.media.end(),[](const auto& media) { return media.kind==frondMachine::kindSd; });
            if (hasSd && soc->SdInserted() && !soc->SdRemount()) throw std::runtime_error("the session and its card image do not match");
            if ((soc->SdInserted() && !hasSd) || soc->networkSimulation.GetMode()!=NetworkSimulation::disabled)
                throw std::runtime_error("use an ejected SD and disabled network fixture");
        }
        m.memory={}; m.savestate={};
        clockStart=soc->GetTime(); clockSoc=soc.get();
        // Guest instructions run and those run in the JIT (Fast only), counted from here.
        const uint64_t instructionsStart=soc->GetRunInstructions();
        uint64_t jitBlocks=0, jitStart=0;
        const bool fast=cpuJitExecutionCounts(soc->GetCpu(),&jitBlocks,&jitStart);
        auto queue=audioQueueCreate(8192);
        soc->SetAudioQueue(queue); soc->SetPcmOutputEnabled(sound); soc->SetPcmSuspended(false);
        std::ofstream frames(output/"frames.csv"), inputs(output/"inputs.csv");
        frames << "guest_ns,wall_ns,cpu_seconds,hash,changed\n";
        inputs << "requested_ms,guest_ns,wall_ns,kind\n";
        std::vector<uint32_t> frame(size_t(width)*height);
        uint64_t lastHash=hashInit, frameSequence=hashInit, pcmHash=hashInit;
        uint64_t frameCount=0, changed=0, pcmFrames=0, pcmZeros=0, cycles=0;
        unsigned peak=0, snapshots=0;
        bool haveFrame=false;
        size_t next=0;
        // Host and guest ns of each 1 ms work request, for the pause figures.
        struct Slice { uint64_t host, guest; };
        std::vector<Slice> slices; slices.reserve(size_t(duration)+16);
        // BENCH_DUMP_RAM=1 writes guest RAM at the start and end, for tools that read guest data.
        const bool dumpRam=std::getenv("BENCH_DUMP_RAM")!=nullptr;
        auto dump=[&](const char* name) {
            std::ofstream out(output/name,std::ios::binary);
            out.write(reinterpret_cast<const char*>(soc->GetMemoryData().data),soc->GetMemoryData().size);
        };
        if (dumpRam) dump("ram-start.bin");
        HostProfile hostProfile(output);
        rusage usageStart{};
        getrusage(RUSAGE_SELF,&usageStart);
        const auto start=std::chrono::steady_clock::now();
        const auto cpuStart=std::clock();
        auto wallNs=[&]() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count(); };
        auto cpuSeconds=[&]() { return double(std::clock()-cpuStart)/CLOCKS_PER_SEC; };
        auto observe=[&]() {
            uint32_t first=0,last=0;
            if (auto pending=soc->GetPendingFrame(first,last)) {
                std::memcpy(frame.data(),pending,frame.size()*sizeof(uint32_t));
                const auto hash=hashBytes(hashInit,frame.data(),frame.size()*sizeof(uint32_t));
                const bool different=!haveFrame || hash!=lastHash;
                const uint64_t guest=soc->GetTime()-clockStart;
                frames << guest << ',' << wallNs() << ',' << cpuSeconds() << ',' << std::hex << hash << std::dec << ',' << different << '\n';
                frameSequence=hashBytes(frameSequence,&guest,sizeof(guest));
                frameSequence=hashBytes(frameSequence,&hash,sizeof(hash));
                lastHash=hash; haveFrame=true; ++frameCount; changed+=different;
                soc->ResetPendingFrame();
            }
        };
        while (true) {
            observe();
            const auto elapsed=soc->GetTime()-clockStart;
            while (next<events.size() && events[next].ms*1000000<=elapsed) {
                const auto& e=events[next++];
                inputs << e.ms << ',' << elapsed << ',' << wallNs() << ',' << e.kind << '\n';
                if (e.kind=="pen-down" || e.kind=="pen-move") soc->PenDown(e.x,e.y);
                else if (e.kind=="pen-up") soc->PenUp();
                else if (e.kind=="key-down") soc->KeyDown(e.key);
                else if (e.kind=="key-up") soc->KeyUp(e.key);
                else if (e.kind=="keyboard-down" || e.kind=="keyboard-up") soc->KeyboardKey(uint8_t(e.x),e.kind=="keyboard-down");
                else if (e.kind=="keyboard-attach" || e.kind=="keyboard-detach") soc->AttachKeyboard(e.kind=="keyboard-attach");
                else {
                    // Before the first LCD frame (a cold boot with the LCD still off) it saves black.
                    if (!haveFrame) std::fprintf(stderr,"snapshot %u at %llu ms: no LCD frame yet, saved black\n",
                                                 snapshots+1,(unsigned long long)e.ms);
                    const auto path=output/("snapshot-"+std::to_string(++snapshots)+".png");
                    if (!savePng(path,frame,width,height)) throw std::runtime_error("snapshot failed");
                }
            }
            if (elapsed>=duration*1000000) break;
            const auto sliceStart=wallNs(); const auto sliceGuest=soc->GetTime();
            // Fixed 1 ms work requests; Original runs at the guest's own clock as it sets it.
            const uint64_t hz=original&&soc->CoreHz()?soc->CoreHz():mips*1000000;
            const auto count=soc->Run(hz/1000,hz);
            if (!count) throw std::runtime_error("guest made no progress");
            slices.push_back({uint64_t(wallNs()-sliceStart),uint64_t(soc->GetTime()-sliceGuest)});
            cycles+=count;
            uint32_t pcm[8192];
            const auto n=audioQueuePopChunk(queue,pcm,8192);
            pcmHash=hashBytes(pcmHash,pcm,n*sizeof(uint32_t)); pcmFrames+=n;
            for (size_t i=0;i<n;++i) {
                pcmZeros+=pcm[i]==0;
                peak=std::max({peak,unsigned(std::abs(int(int16_t(pcm[i])))),unsigned(std::abs(int(int16_t(pcm[i]>>16))))});
            }
        }
        const double wall=wallNs()/1e9, cpu=cpuSeconds();
        rusage usageEnd{};
        getrusage(RUSAGE_SELF,&usageEnd);
        auto seconds=[](const timeval& a, const timeval& b) { return double(b.tv_sec-a.tv_sec)+double(b.tv_usec-a.tv_usec)/1e6; };
        const double user=seconds(usageStart.ru_utime,usageEnd.ru_utime), system=seconds(usageStart.ru_stime,usageEnd.ru_stime);
        hostProfile.Stop();
        if (dumpRam) dump("ram-end.bin");
        frames.flush(); inputs.flush();
        if (!frames || !inputs) throw std::runtime_error("trace write failed");
        const uint64_t guestNs=soc->GetTime()-clockStart;
        const uint64_t instructions=soc->GetRunInstructions()-instructionsStart;
        uint64_t jitInstructions=0;
        if (fast && cpuJitExecutionCounts(soc->GetCpu(),&jitBlocks,&jitInstructions)) jitInstructions-=jitStart;
        const double hostMips=instructions/wall/1e6;  // guest instructions per host second
        std::ofstream result(output/"summary.json");
        result << std::setprecision(12) << "{\"version\":1,\"device\":\"" << deviceName5(device) << "\",\"cold_boot\":" << (coldBoot?"true":"false")
               << ",\"engine\":\"" << (fast?"fast":"exact") << "\",\"jit_async\":" << (jitAsync?"true":"false") << ",\"mips\":" << mips << ",\"sound\":" << (sound?"true":"false")
               << ",\"palm_epoch\":" << epoch << ",\"guest_ns\":" << guestNs << ",\"cycles\":" << cycles
               << ",\"instructions\":" << instructions << ",\"jit_instructions\":" << jitInstructions
               << ",\"wall_seconds\":" << wall << ",\"cpu_seconds\":" << cpu
               << ",\"cpu_user_seconds\":" << user << ",\"cpu_system_seconds\":" << system << ",\"host_mips\":" << hostMips
               << ",\"lcd_frames\":" << frameCount << ",\"changed_frames\":" << changed
               << ",\"pcm_frames\":" << pcmFrames << ",\"pcm_zero_frames\":" << pcmZeros << ",\"pcm_peak\":" << peak
               << ",\"frame_sequence_hash\":\"" << std::hex << frameSequence << "\",\"pcm_hash\":\"" << pcmHash << "\"}\n";
        result.flush(); if (!result) throw std::runtime_error("summary write failed");
        std::printf("%s %s: guest %.3f s, %llu cycles, wall %.3f s, host CPU %.3f s (user %.3f, sys %.3f), %.1f MIPS, "
                    "%llu instructions (%llu in the JIT), %llu frames, frame hash %llx, %llu PCM frames, PCM hash %llx\n",
                    deviceName5(device),coldBoot?"cold boot":"session",guestNs/1e9,(unsigned long long)cycles,wall,cpu,user,system,
                    hostMips,(unsigned long long)instructions,(unsigned long long)jitInstructions,(unsigned long long)frameCount,(unsigned long long)frameSequence,
                    (unsigned long long)pcmFrames,(unsigned long long)pcmHash);
        std::fflush(stdout);
        // Pauses on the guest thread: the longest work requests, the most host time in any 20
        // in a row, and underruns of a real-time player that runs the guest up to BUF ms ahead
        // of the sound it plays (refilling BUF after each underrun).
        if (!slices.empty()) {
            std::vector<uint64_t> hosts; hosts.reserve(slices.size());
            for (const auto& sl : slices) hosts.push_back(sl.host);
            std::vector<uint64_t> sorted=hosts; std::sort(sorted.begin(),sorted.end());
            auto pct=[&](double p) { return sorted[std::min(sorted.size()-1,size_t(p*sorted.size()))]/1e6; };
            uint64_t window=0, worst=0;
            size_t worstAt=0, maxAt=0, over5=0;
            for (size_t i=0;i<hosts.size();++i) {
                window+=hosts[i]; if (i>=20) window-=hosts[i-20];
                if (window>worst) { worst=window; worstAt=i; }
                if (hosts[i]>hosts[maxAt]) maxAt=i;
                over5+=hosts[i]>5000000;
            }
            auto underruns=[&](double bufMs) {
                const double buf=bufMs*1e6; double real=-buf, guest=0; unsigned n=0;
                for (const auto& sl : slices) {
                    if (guest-real>buf) real=guest-buf;
                    real+=double(sl.host); guest+=double(sl.guest);
                    if (real>guest) { ++n; real=guest-buf; }
                }
                return n;
            };
            std::printf("pauses: %zu slices, max %.3f ms, p99 %.3f, p99.9 %.3f, worst 20 %.2f ms, underruns at 10 ms %u, 40 ms %u, "
                        "over 5 ms %zu, max at slice %zu, worst 20 ending at %zu\n",
                        hosts.size(),sorted.back()/1e6,pct(0.99),pct(0.999),worst/1e6,underruns(10),underruns(40),over5,maxAt,worstAt);
            std::fflush(stdout);
        }
        clockSoc=nullptr;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
