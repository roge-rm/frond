// Genuine HCI reset of one retained virtual controller, outside guest transport.
#ifndef FROND_CSR_CONTROLLER_RESET_H
#define FROND_CSR_CONTROLLER_RESET_H
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
namespace csr {
template<class Backend> class ControllerReset {
 public:
    bool Pending() const{return pending;}
    const std::string& Error() const{return error;}
    void Clear(){pending=sent=false;error.clear();}
    void Begin(uint64_t now,const std::vector<uint16_t>& oldCommands,unsigned credit){
        if(pending)return;
        pending=true;sent=false;started=now;commands=oldCommands;commandCredit=credit;error.clear();
    }
    void Pump(Backend& backend,uint64_t now){
        if(!pending)return;
        // Reserve one receive syscall batch; old complete frames are drained below.
        backend.Poll(backend.Pending()<=Backend::capacity-(Backend::maxFrame+16384-1));
        if(!backend.Connected()){Fail(backend,"Bluetooth controller disconnected during radio reset");return;}
        if(!sent && commands.empty() && commandCredit && Backend::capacity-backend.Queued()>=4){
            const uint8_t command[]{1,3,0x0c,0};
            if(!backend.Write(command,sizeof(command))){Fail(backend,"Bluetooth controller reset write failed");return;}
            sent=true;backend.Poll(false);
        }
        // Discard only this fenced transport generation. No guest events or
        // completion credits are manufactured, and no ACL suffix is replayed.
        for(unsigned i=0;i<32 && backend.Pending();++i){
            const auto type=backend.Read();
            const unsigned header=type==4?2:type==2?4:type==3?3:0;
            if(!header){Fail(backend,"Invalid controller packet during radio reset");return;}
            uint8_t h[4]{};for(unsigned j=0;j<header;++j)h[j]=uint8_t(backend.Read());
            const unsigned n=type==4?h[1]:type==2?h[2]+(unsigned(h[3])<<8):h[2];
            uint8_t event[4]{};
            for(unsigned j=0;j<n;++j){const auto c=backend.Read();if(c==0x100){Fail(backend,"Incomplete controller packet during radio reset");return;}if(j<4)event[j]=uint8_t(c);}
            if(type==4 && ((h[0]==14 && n>=3) || (h[0]==15 && n==4))){
                const unsigned op=h[0]==14?event[1]+(unsigned(event[2])<<8):event[2]+(unsigned(event[3])<<8);
                commandCredit=event[h[0]==14?0:1];
                const auto old=std::find(commands.begin(),commands.end(),op);
                if(old!=commands.end()){
                    if(op==0x0c03 && (h[0]!=14 || n!=4 || event[3])){Fail(backend,"Controller rejected prior radio reset");return;}
                    commands.erase(old);
                }else if(op==0x0c03){
                    if(!sent){Fail(backend,"Unexpected controller reset completion");return;}
                    if(h[0]!=14 || n!=4 || event[3]){Fail(backend,"Controller rejected radio reset");return;}
                    pending=false;return;
                }
            }
        }
        if(now-started>=3000000000ull)Fail(backend,"Controller radio reset timed out");
    }
 private:
    bool pending{},sent{};uint64_t started{};unsigned commandCredit{};std::vector<uint16_t> commands;std::string error;
    void Fail(Backend& backend,const char* message){error=message;pending=false;backend.Close();}
};
}
#endif
