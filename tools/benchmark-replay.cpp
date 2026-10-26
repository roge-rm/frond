// Fixed guest-time input replay for private TX workloads. No wall-time governor.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "SDL.h"
#include "SDL_image.h"
#include "audio_queue.h"
#include "session/session_file5.h"
#include "soc_PXA.h"
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
std::vector<Event> replay(const char* path, uint64_t end) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read replay");
    const std::map<std::string, KeyId> keys = {
        {"home",keyIdHard1},{"calendar",keyIdHard2},{"contacts",keyIdHard3},
        {"web",keyIdHard4},{"up",keyIdUp},{"down",keyIdDown},
        {"left",keyIdLeft},{"right",keyIdRight},{"center",keyIdSelect},{"power",keyIdPower}};
    std::vector<Event> events;
    std::string line;
    while (std::getline(input,line)) {
        std::istringstream tokens(line);
        std::string at, extra;
        if (!(tokens >> at) || at[0]=='#') continue;
        Event e{number(at,end),""};
        if (!events.empty() && e.ms < events.back().ms) throw std::runtime_error("unordered replay");
        if (!(tokens >> e.kind)) throw std::runtime_error("missing replay command");
        if (e.kind=="pen-down" || e.kind=="pen-move") {
            if (!(tokens>>e.x>>e.y) || e.x<0 || e.x>=320 || e.y<0 || e.y>=480)
                throw std::runtime_error("invalid pen coordinates");
        } else if (e.kind=="key-down" || e.kind=="key-up") {
            if (!(tokens>>extra) || !keys.count(extra)) throw std::runtime_error("invalid key");
            e.key=keys.at(extra);
        } else if (e.kind=="snapshot") {
            // Accept native replay syntax but always redirect into the private output directory.
            if (!(tokens>>std::quoted(extra)) || extra.empty()) throw std::runtime_error("missing snapshot path");
        } else if (e.kind!="pen-up") throw std::runtime_error("unsupported replay command");
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
}
// This executable replaces timeutil.o: guest RTC is tied to restored virtual time.
uint64_t palmEpochSeconds() {
    return epoch + (clockSoc ? (clockSoc->GetTime()-clockStart)/1000000000ull : 0);
}

int main(int argc, char** argv) {
    try {
        if (argc==4 && std::string(argv[1])=="--check-replay") {
            replay(argv[2],number(argv[3],600000)); return 0;
        }
        if (argc!=7 && argc!=8)
            throw std::runtime_error("usage: benchmark-replay session replay milliseconds mips sound:on|off NEW-output-dir [palm-epoch-seconds]");
        const auto duration=number(argv[3],600000), mips=number(argv[4],1000);
        if (!duration || !mips) throw std::runtime_error("duration and MIPS must be positive");
        const std::string soundArg=argv[5];
        if (soundArg!="on" && soundArg!="off") throw std::runtime_error("sound must be on or off");
        const bool sound=soundArg=="on";
        if (argc==8) epoch=number(argv[7],UINT32_MAX-600);
        const auto events=replay(argv[2],duration);
        const std::filesystem::path output=argv[6];
        // Atomic exclusive mkdir, private from creation (unlike create_directory + chmod).
        if (mkdir(output.c_str(),0700)) throw std::runtime_error("output must be a new directory in an existing parent");
        std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
        if (!input || input.tellg()<=0 || input.tellg()>512*1024*1024)
            throw std::runtime_error("cannot read session");
        std::vector<uint8_t> data(size_t(input.tellg()));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("incomplete session");
        SessionFile5 session;
        if (!session.Deserialize(data.size(),data.data()) || session.GetDeviceType()!=int(DeviceType5::deviceTypeTX))
            throw std::runtime_error("complete TX session required");
        if (session.GetSdSize()) throw std::runtime_error("use a session without an SD image for this benchmark");
        std::vector<uint8_t> nor(session.GetNorSize()),nand(session.GetNandSize());
        std::memcpy(nor.data(),session.GetNor(),nor.size());
        std::memcpy(nand.data(),session.GetNand(),nand.size());
        SocPXA soc(DeviceType5::deviceTypeTX,session.GetRamSize()?session.GetRamSize():32*1024*1024,
                   nor.data(),nor.size(),nand.data(),nand.size(),-1,2);
        if (session.GetMemorySize()!=soc.GetMemoryData().size || !session.GetSavestateSize())
            throw std::runtime_error("complete RAM and peripheral state required");
        std::memcpy(soc.GetMemoryData().data,session.GetMemory(),session.GetMemorySize());
        std::vector<uint8_t> state(session.GetSavestateSize());
        std::memcpy(state.data(),session.GetSavestate(),state.size());
        if (!soc.Load(state.size(),state.data())) throw std::runtime_error("restore failed");
        if (soc.SdInserted() || soc.networkSimulation.GetMode()!=NetworkSimulation::disabled)
            throw std::runtime_error("use an ejected SD and disabled network fixture");
        clockStart=soc.GetTime(); clockSoc=&soc;
        auto queue=audioQueueCreate(8192);
        soc.SetAudioQueue(queue); soc.SetPcmOutputEnabled(sound); soc.SetPcmSuspended(false);
        std::ofstream frames(output/"frames.csv"), inputs(output/"inputs.csv");
        frames << "guest_ns,wall_ns,cpu_seconds,hash,changed\n";
        inputs << "requested_ms,guest_ns,wall_ns,kind\n";
        std::vector<uint32_t> frame(320*480);
        uint64_t lastHash=hashInit, frameSequence=hashInit, pcmHash=hashInit;
        uint64_t frameCount=0, changed=0, pcmFrames=0, pcmZeros=0, cycles=0;
        unsigned peak=0, snapshots=0;
        bool haveFrame=false;
        size_t next=0;
        HostProfile hostProfile(output);
        const auto start=std::chrono::steady_clock::now();
        const auto cpuStart=std::clock();
        auto wallNs=[&]() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count(); };
        auto cpuSeconds=[&]() { return double(std::clock()-cpuStart)/CLOCKS_PER_SEC; };
        auto observe=[&]() {
            uint32_t first=0,last=0;
            if (auto pending=soc.GetPendingFrame(first,last)) {
                std::memcpy(frame.data(),pending,frame.size()*sizeof(uint32_t));
                const auto hash=hashBytes(hashInit,frame.data(),frame.size()*sizeof(uint32_t));
                const bool different=!haveFrame || hash!=lastHash;
                const uint64_t guest=soc.GetTime()-clockStart;
                frames << guest << ',' << wallNs() << ',' << cpuSeconds() << ',' << std::hex << hash << std::dec << ',' << different << '\n';
                frameSequence=hashBytes(frameSequence,&guest,sizeof(guest));
                frameSequence=hashBytes(frameSequence,&hash,sizeof(hash));
                lastHash=hash; haveFrame=true; ++frameCount; changed+=different;
                soc.ResetPendingFrame();
            }
        };
        while (true) {
            observe();
            const auto elapsed=soc.GetTime()-clockStart;
            while (next<events.size() && events[next].ms*1000000<=elapsed) {
                const auto& e=events[next++];
                inputs << e.ms << ',' << elapsed << ',' << wallNs() << ',' << e.kind << '\n';
                if (e.kind=="pen-down" || e.kind=="pen-move") soc.PenDown(e.x,e.y);
                else if (e.kind=="pen-up") soc.PenUp();
                else if (e.kind=="key-down") soc.KeyDown(e.key);
                else if (e.kind=="key-up") soc.KeyUp(e.key);
                else {
                    if (!haveFrame) throw std::runtime_error("snapshot before first LCD frame");
                    auto surface=SDL_CreateRGBSurfaceWithFormatFrom(frame.data(),320,480,32,320*4,SDL_PIXELFORMAT_RGBA32);
                    const auto path=output/("snapshot-"+std::to_string(++snapshots)+".png");
                    const bool ok=surface && IMG_SavePNG(surface,path.c_str())==0;
                    if (surface) SDL_FreeSurface(surface);
                    if (!ok) throw std::runtime_error("snapshot failed");
                }
            }
            if (elapsed>=duration*1000000) break;
            const auto count=soc.Run(mips*1000,mips*1000000); // fixed 1 ms work requests
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
        hostProfile.Stop();
        frames.flush(); inputs.flush();
        if (!frames || !inputs) throw std::runtime_error("trace write failed");
        std::ofstream result(output/"summary.json");
        result << std::setprecision(12) << "{\"version\":1,\"mips\":" << mips << ",\"sound\":" << (sound?"true":"false")
               << ",\"palm_epoch\":" << epoch << ",\"guest_ns\":" << soc.GetTime()-clockStart << ",\"cycles\":" << cycles
               << ",\"wall_seconds\":" << wall << ",\"cpu_seconds\":" << cpu
               << ",\"lcd_frames\":" << frameCount << ",\"changed_frames\":" << changed
               << ",\"pcm_frames\":" << pcmFrames << ",\"pcm_zero_frames\":" << pcmZeros << ",\"pcm_peak\":" << peak
               << ",\"frame_sequence_hash\":\"" << std::hex << frameSequence << "\",\"pcm_hash\":\"" << pcmHash << "\"}\n";
        result.flush(); if (!result) throw std::runtime_error("summary write failed");
        clockSoc=nullptr;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
