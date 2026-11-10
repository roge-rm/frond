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
#include "soc_PXA.h"
#include "device.h"
#include "deviceSonyTG50.h"
#include "device_configuration.h"
#include "device_profile5.h"
#include "rom_info5.h"
#include "MachineSession.h"
#include "MsProCards.h"
#include "NandFile.h"
#include "host-profile.h"

namespace {
SocPXA* clockSoc = nullptr;
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
// silkscreen. "-" is no input.
std::vector<Event> replay(const char* path, uint64_t end, int width=320, int height=480) {
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
        {"hotsync",keyIdHotSync},{"jog-up",keyIdJogUp},{"jog-down",keyIdJogDown},{"jog-press",keyIdJogPress}};
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
        } else if (e.kind!="pen-up" && e.kind!="keyboard-attach" && e.kind!="keyboard-detach") throw std::runtime_error("unsupported replay command");
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
    for (const auto& media : m.media)
        if (media.kind==frondMachine::kindSd) throw std::runtime_error("use a session without an SD card for this benchmark");
    if (m.savestate.empty() || m.memory.empty()) throw std::runtime_error("complete RAM and peripheral state required");
    if (m.device==deviceTypeTG50 && m.memory.size()!=tg50RamBackingSize+2*MEMORY_BUFFER_GRANULARITY)
        throw std::runtime_error("TG50 session requires complete RAM and CPU/peripheral state");
    return m;
}

// A cold boot from a NOR image, like the runner's `frond NOR --device NAME`.
Machine loadNor(std::vector<uint8_t> data, const std::string& deviceName) {
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
    if (device==DeviceType5::deviceTypeTX) throw std::runtime_error("a TX cold boot needs its NAND: use a session");
    if (!deviceProfileRunnerEligible5(device) || !pxaBoard(deviceProfile5(device)->board))
        throw std::runtime_error("this ROM's device runs in frond-68k or has no board");
    if (deviceProfileValidateFirmware5(device,data.size(),0,false,false,rom.GetFirmwareVariant())!=DeviceFirmwareError5::none)
        throw std::runtime_error("invalid firmware geometry for the device");
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
        if (argc==4 && std::string(argv[1])=="--check-replay") {
            replay(argv[2],number(argv[3],600000)); return 0;
        }
        std::string deviceName;
        int skip=0;
        if (argc>2 && std::string(argv[1])=="--device") { deviceName=argv[2]; skip=2; }
        char** arg=argv+skip;  // arg[1] is SESSION
        const int count=argc-skip;
        if (count!=7 && count!=8)
            throw std::runtime_error("usage: benchmark-replay [--device NAME] SESSION|NOR REPLAY|- MILLISECONDS MIPS|original on|off NEW-output-dir [palm-epoch-seconds]");
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
        if (!coldBoot && !deviceName.empty()) throw std::runtime_error("--device is for a NOR image; a session names its device");
        Machine m;
        if (coldBoot) { m=loadNor(std::move(data),deviceName); data={}; }
        const auto checkInputs=[&](DeviceType5 device) {
            const uint64_t mips=std::string(arg[4])=="original"?deviceProfileOriginalMips5(device):number(arg[4],1000);
            if (!mips) throw std::runtime_error("MIPS must be positive");
            const auto display=deviceConfigurationDisplayConfigForDevice(device,deviceConfigurationDefaultDisplayMode(device));
            if (display.width<=0 || display.height<=0) throw std::runtime_error("device has no display");
            return std::make_pair(mips,replay(arg[2],duration,display.width,display.height+display.graffitiHeight));
        };
        // A cold boot's MIPS and replay are checked before the output directory is made.
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
            if (soc->SdInserted() || soc->networkSimulation.GetMode()!=NetworkSimulation::disabled)
                throw std::runtime_error("use an ejected SD and disabled network fixture");
        }
        m.memory={}; m.savestate={};
        clockStart=soc->GetTime(); clockSoc=soc.get();
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
            const auto count=soc->Run(mips*1000,mips*1000000); // fixed 1 ms work requests
            if (!count) throw std::runtime_error("guest made no progress");
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
        const double hostMips=cycles/wall/1e6;
        std::ofstream result(output/"summary.json");
        result << std::setprecision(12) << "{\"version\":1,\"device\":\"" << deviceName5(device) << "\",\"cold_boot\":" << (coldBoot?"true":"false")
               << ",\"jit_async\":" << (jitAsync?"true":"false") << ",\"mips\":" << mips << ",\"sound\":" << (sound?"true":"false")
               << ",\"palm_epoch\":" << epoch << ",\"guest_ns\":" << guestNs << ",\"cycles\":" << cycles
               << ",\"wall_seconds\":" << wall << ",\"cpu_seconds\":" << cpu
               << ",\"cpu_user_seconds\":" << user << ",\"cpu_system_seconds\":" << system << ",\"host_mips\":" << hostMips
               << ",\"lcd_frames\":" << frameCount << ",\"changed_frames\":" << changed
               << ",\"pcm_frames\":" << pcmFrames << ",\"pcm_zero_frames\":" << pcmZeros << ",\"pcm_peak\":" << peak
               << ",\"frame_sequence_hash\":\"" << std::hex << frameSequence << "\",\"pcm_hash\":\"" << pcmHash << "\"}\n";
        result.flush(); if (!result) throw std::runtime_error("summary write failed");
        std::printf("%s %s: guest %.3f s, %llu cycles, wall %.3f s, host CPU %.3f s (user %.3f, sys %.3f), %.1f MIPS, "
                    "%llu frames, frame hash %llx, %llu PCM frames, PCM hash %llx\n",
                    deviceName5(device),coldBoot?"cold boot":"session",guestNs/1e9,(unsigned long long)cycles,wall,cpu,user,system,
                    hostMips,(unsigned long long)frameCount,(unsigned long long)frameSequence,
                    (unsigned long long)pcmFrames,(unsigned long long)pcmHash);
        std::fflush(stdout);
        clockSoc=nullptr;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
