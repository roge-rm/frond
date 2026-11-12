// Reliable BCSP payload ownership at the H4 provider boundary.
#ifndef FROND_BCSP_H4_BRIDGE_H
#define FROND_BCSP_H4_BRIDGE_H
#include "BcspTransport.h"
#include <string>
#include <map>
#include <algorithm>
#include <bitset>

namespace csr {
template<class Backend> class BcspH4Bridge {
 public:
    using Bytes=BcspTransport::Bytes;
    struct ControlObservation {Bytes request;uint64_t at;};
    BcspH4Bridge(BcspTransport& transport,Backend& backend):transport(transport),backend(backend){}
    const std::string& Error() const{return error;}
    const std::deque<ControlObservation>& Controls() const{return controls;}
    uint64_t UnretainedControls() const{return unretainedControls;}
    // Caller freezes this object and the UART together for checkpoints.
    void Pump(uint64_t now) {
        if(!error.empty() || transport.State()!=BcspTransport::Link::active)return;
        if(resetFailed && host.empty()) {
            // Retain the genuine failure event until its reliable delivery is ACKed.
            if(!transport.Outstanding() && !transport.HasWire())Fail("Controller HCI Reset failed; radio restart required");
            return;
        }
        // This bridge is the only producer. Check the saved frame before any
        // Queue can reuse its three-bit sequence after a cumulative ACK.
        if(resetReply>=0 && !transport.ReliablePending(uint8_t(resetReply))){resetReply=-1;resetAcknowledged=true;}
        if(resetPending && now-resetAt>=3000000000ull){Fail("Controller HCI Reset timed out");return;}
        // UART-to-controller ownership transfers once per reliable delivery.
        if(const auto* packet=transport.Peek()) {
            if(packet->channel==2) {
                if(controls.size()<8)controls.push_back({packet->payload,now});
                else ++unretainedControls;
                transport.Pop();
                // Transport receipt is ACKed, no CSR semantic reply is emitted.
                // No channel2 request has yet been proven safe to implement.
                unsupportedControl=true;
            } else {
                const auto& p=packet->payload;
                const bool command=packet->channel==5;
                if((command && (p.size()<3 || p.size()!=size_t(3+p[2]))) ||
                   (!command && (p.size()<4 || p.size()!=size_t(4+p[2]+(size_t(p[3])<<8))))) {
                    Fail("Malformed TG50 HCI packet");return;
                }
                if(!command) {
                    const size_t n=p.size()-4;
                    if((p[1]&0xc0)==0xc0){Fail("Reserved guest ACL broadcast flags");return;}
                    if(!aclPackets || !aclMtu){Fail("ACL sent before controller buffer negotiation");return;}
                    if(n>aclMtu){Fail("Guest ACL exceeds negotiated controller buffer");return;}
                    // The controller pool is shared by all connection handles.
                    if(aclOutstanding>=aclPackets)goto receive;
                }
                Bytes h4{uint8_t(command?1:2)};h4.insert(h4.end(),p.begin(),p.end());
                const unsigned op=command?unsigned(p[0]|(unsigned(p[1])<<8)):0;
                if(op==0x0c03 && p.size()!=3){Fail("Malformed HCI Reset request");return;}
                // Only late host completions may cross an admitted reset boundary.
                if((resetPending || resetFailed || (resetEpoch && !resetAcknowledged)) && op!=0x0c35)goto receive;
                ReceiveDebt completed;unsigned remaining=rxOutstanding,active=rxActive;
                if(op==0x0c35) {
                    completed=rxDebt;
                    if(p.size()<4 || p.size()!=size_t(4+4*p[3])){Fail("Malformed host ACL completion");return;}
                    h4={1,0x35,0x0c,1,0};
                    for(size_t at=4;at<p.size();at+=4) {
                        const unsigned handle=p[at]|(unsigned(p[at+1])<<8),count=p[at+2]|(unsigned(p[at+3])<<8);
                        auto i=completed.find(handle);unsigned originals=0;
                        if(count && (i==completed.end() || count>i->second.size() || count>remaining)) {
                            Fail("Host completed unsubmitted ACL fragments");return;
                        }
                        for(unsigned j=0;j<count;++j){const auto token=i->second.front();originals+=((token&3)==1);active-=!(token&2);i->second.pop_front();}
                        if(i!=completed.end() && i->second.empty())completed.erase(i);
                        remaining-=count;
                        if(originals){++h4[4];h4.insert(h4.end(),{uint8_t(handle),uint8_t(handle>>8),uint8_t(originals),uint8_t(originals>>8)});}
                    }
                    h4[3]=uint8_t(h4.size()-4);
                    // Host completions have no HCI response. Return one provider
                    // packet only after all of its serial fragments are released.
                    if(!h4[4]){rxDebt=std::move(completed);rxOutstanding=remaining;rxActive=active;transport.Pop();goto receive;}
                }
                if(op==0x0c31 && (p.size()!=4 || p[3]>1 || flowRequest>=0 || rxActive || hostAt)) {
                    Fail("Unsupported host flow-control transition");return;
                }
                if(op==0x0c33 && (p.size()!=10 || !Word(p,3) || !Word(p,6) ||
                   Word(p,6)>4096 || bufferRequest || rxActive || hostAt)) {
                    Fail("Unsupported host ACL buffer contract");return;
                }
                if(command && op!=0x0c35 && commandsPending.size()==64){Fail("Too many pending HCI commands");return;}
                if(!backend.Connected()){Fail("Bluetooth controller disconnected");return;}
                // Write closes its endpoint on overflow, so test capacity first.
                if(h4.size()<=Backend::capacity-backend.Queued()) {
                    if(!backend.Write(h4.data(),h4.size())){Fail("Bluetooth controller write failed");return;}
                    if(command && op!=0x0c35){commandsPending.push_back(uint16_t(op));if(commandCredit)--commandCredit;}
                    if(!command){++aclOutstanding;++debt[uint16_t(p[0]|((p[1]&15)<<8))];}
                    if(op==0x0c35){rxDebt=std::move(completed);rxOutstanding=remaining;rxActive=active;}
                    if(op==0x0c03) {
                        ForgetAclLinks();
                        resetPending=true;resetAt=now;resetEpoch=resetAcknowledged=false;resetReply=-1;
                        // Submitted BCSP fragments remain ordered and retain late-ACK
                        // tokens. The unsubmitted ACL suffix creates no credit.
                        RetireHostCredits();
                        if(!host.empty() && host[0]==2)ClearHost();
                    }
                    if(op==0x0c31)flowRequest=p[3];
                    if(op==0x0c33){bufferRequest=true;requestedMtu=Word(p,3);requestedPackets=Word(p,6);}
                    transport.Pop();
                }
            }
        }
receive:
        // Flush writes even when provider input is backpressured.
        backend.Poll(backend.Pending()<=Backend::capacity-(Backend::maxFrame+4*4096-1));
        if(!backend.Connected()){Fail("Bluetooth controller disconnected");return;}
        // Assemble one complete H4 packet, retain it until Queue accepts it.
        // Existing Consume guarantees complete H4 frames in Pending().
        if(host.empty() && backend.Pending()) {
            host.push_back(uint8_t(backend.Read()));
            const size_t header=host[0]==4?3:host[0]==2?5:host[0]==3?4:0;
            if(!header){Fail("Invalid controller H4 type");return;}
            while(host.size()<header) {
                const auto c=backend.Read();
                if(c==0x100){Fail("Incomplete controller H4 header");return;}
                host.push_back(uint8_t(c));
            }
            const size_t size=host[0]==4?3+host[2]:host[0]==2?5+host[3]+(size_t(host[4])<<8):4+host[3];
            while(host.size()<size) {
                const auto c=backend.Read();
                if(c==0x100){Fail("Incomplete controller H4 packet");return;}
                host.push_back(uint8_t(c));
            }
        }
        if(!host.empty()) {
            if(host[0]==3){Fail("TG50 SCO transport is unqualified");return;}
            // Consume at most one cancelled old-generation original per pump.
            // Events still reach the guest in their original stream order.
            if(host[0]==2 && resetPending){ClearHost();return;}
            if(host[0]==4) {
                if(!hostObserved){if(!ObserveEvent(now))return;hostObserved=true;}
                uint8_t sequence=0;
                if(transport.Queue(5,host.data()+1,host.size()-1,false,&sequence)) {
                    if(host.size()==7 && host[1]==14 && host[4]==3 && host[5]==0x0c && !host[6])resetReply=sequence;
                    ClearHost();
                }
            } else {
                const uint8_t pb=(host[2]>>4)&3,bc=host[2]>>6;
                if((pb!=1 && pb!=2) || bc==3){Fail("Unsupported controller ACL packet flags");return;}
                if(hostFlow && (!hostMtu || !hostPackets)){Fail("Controller ACL received before a successful host buffer declaration");return;}
                if(hostFlow && rxActive>=hostPackets)return;
                const size_t limit=hostFlow?std::min(size_t(hostMtu),BcspTransport::maxPayload-4):BcspTransport::maxPayload-4;
                const size_t count=std::min(limit,host.size()-5-hostAt);
                Bytes fragment{host[1],uint8_t((host[2]&0xcf)|(hostAt?0x10:(pb<<4))),
                               uint8_t(count),uint8_t(count>>8)};
                fragment.insert(fragment.end(),host.begin()+5+hostAt,host.begin()+5+hostAt+count);
                // Keep one current and one retired maximum host-buffer pool.
                if(hostFlow && rxOutstanding>=8192){Fail("Host ACL completion history exhausted; radio restart required");return;}
                if(transport.Queue(6,fragment.data(),fragment.size())) {
                    hostAt+=count;
                    if(hostFlow){++rxOutstanding;++rxActive;rxDebt[uint16_t(host[1]|((host[2]&15)<<8))].push_back(hostAt==host.size()-5);}
                    if(hostAt==host.size()-5)ClearHost();
                }
            }
        }
    }
    bool UnsupportedControl() const{return unsupportedControl;}
    int ResetStatus() const{return resetStatus;}
    const std::vector<uint16_t>& CommandsPending() const{return commandsPending;}
    unsigned CommandCredit() const{return commandCredit;}
    unsigned ObservedAclLinks() const{return unsigned(observedAcl.count());}
    bool AclHistoryKnown() const{return aclHistoryKnown;}
    // Host observations are separate from portable transport and packet credits.
    void ForgetAclLinks(){observedAcl.reset();aclHistoryKnown=false;}
    void ControllerResetObserved(){observedAcl.reset();aclHistoryKnown=true;}
    void ClearLocal() {ForgetAclLinks();ClearHost();commandsPending.clear();commandCredit=1;debt.clear();aclOutstanding=aclPackets=aclMtu=0;ClearHostCredits();error.clear();unsupportedControl=false;resetPending=resetFailed=resetEpoch=resetAcknowledged=false;resetReply=-1;resetStatus=-1;}
    void Reset() {
        backend.Close();ClearLocal();
        // Diagnostic observations survive reset until explicitly exported.
    }
 private:
    BcspTransport& transport;Backend& backend;
    Bytes host;
    size_t hostAt{};
    bool hostObserved{},resetPending{},resetFailed{},resetEpoch{},resetAcknowledged{};
    int resetReply{-1};
    uint64_t resetAt{};
    int resetStatus{-1};
    uint16_t aclMtu{},aclPackets{};
    unsigned aclOutstanding{};
    std::map<uint16_t,unsigned> debt;
    std::bitset<4096> observedAcl;
    bool aclHistoryKnown{};
    std::vector<uint16_t> commandsPending;
    unsigned commandCredit{1};
    using ReceiveDebt=std::map<uint16_t,std::deque<uint8_t>>;
    ReceiveDebt rxDebt;
    unsigned rxOutstanding{},rxActive{};
    bool hostFlow{},bufferRequest{};
    int flowRequest{-1};
    uint16_t hostMtu{},hostPackets{},requestedMtu{},requestedPackets{};
    static unsigned Word(const Bytes& p,size_t at){return p[at]|(unsigned(p[at+1])<<8);}
    void RetireHostCredits(){for(auto& row:rxDebt)for(auto& token:row.second)token|=2;rxActive=0;}
    void ClearHostCredits(bool retire=false){if(retire)RetireHostCredits();else{rxDebt.clear();rxOutstanding=rxActive=0;}hostFlow=false;hostMtu=hostPackets=0;bufferRequest=false;flowRequest=-1;}
    void ClearHost(){host.clear();hostAt=0;hostObserved=false;}
    void CommitHostEpoch(){
        if(!resetEpoch || !resetAcknowledged)return;
        // The real reset invalidates controller debt. The host has received that
        // result and successfully declared its new receive epoch, so no old
        // completion token can credit a packet in the new pool.
        rxDebt.clear();rxOutstanding=rxActive=0;resetEpoch=resetAcknowledged=false;
    }
    bool ObserveEvent(uint64_t) {
        const auto* p=host.data()+1;const size_t n=host.size()-1;
        if((p[0]==0x0e && n>=5) || (p[0]==0x0f && n==6)) {
            const size_t at=p[0]==0x0e?3:4;
            const unsigned op=p[at]|(unsigned(p[at+1])<<8);
            commandCredit=p[p[0]==0x0e?2:3];
            const auto i=std::find(commandsPending.begin(),commandsPending.end(),op);
            if(i!=commandsPending.end())commandsPending.erase(i);
        }
        if(p[0]==0x0e && n>=6) {
            const unsigned op=p[3]|(unsigned(p[4])<<8);
            if(op==0x0c31 && flowRequest>=0){if(!p[5]){CommitHostEpoch();hostFlow=flowRequest;}flowRequest=-1;}
            if(op==0x0c33 && bufferRequest){if(!p[5]){CommitHostEpoch();hostMtu=requestedMtu;hostPackets=requestedPackets;}bufferRequest=false;}
            if(op==0x1005 && !p[5]) {
                if(n!=13){Fail("Malformed Read Buffer Size completion");return false;}
                const unsigned mtu=p[6]|(unsigned(p[7])<<8),packets=p[9]|(unsigned(p[10])<<8);
                if(!mtu || !packets || aclOutstanding){Fail("Unsupported controller ACL buffer contract");return false;}
                aclMtu=mtu;aclPackets=packets;
            } else if(op==0x0c03) {
                if(n!=6){Fail("Malformed HCI Reset completion");return false;}
                const bool expected=resetPending;
                resetStatus=p[5];
                if(resetPending && p[5])resetFailed=true;
                resetPending=false;
                if(!p[5]){
                    if(expected)ControllerResetObserved();else ForgetAclLinks();
                    debt.clear();aclOutstanding=0;RetireHostCredits();
                    // Keep only actual announced host capacity; flow and credit
                    // epochs reset. A successful new declaration can replace it.
                    hostFlow=false;bufferRequest=false;flowRequest=-1;
                    resetEpoch=true;resetAcknowledged=false;resetReply=-1;
                }
            }
        }
        if(p[0]==0x13) {
            if(n<3 || n!=size_t(3+4*p[2])){Fail("Malformed controller ACL completion");return false;}
            auto candidate=debt;unsigned total=aclOutstanding;
            for(size_t at=3;at<n;at+=4) {
                const unsigned handle=p[at]|(unsigned(p[at+1])<<8),count=p[at+2]|(unsigned(p[at+3])<<8);
                auto i=candidate.find(handle);
                if(count && (i==candidate.end() || count>i->second || count>total)) {
                    Fail("Controller completed unsubmitted ACL packets");return false;
                }
                if(i!=candidate.end()){i->second-=count;if(!i->second)candidate.erase(i);}total-=count;
            }
            debt=std::move(candidate);aclOutstanding=total;
        }
        if(p[0]==5 && n==6 && !p[2]) {
            const unsigned handle=p[3]|(unsigned(p[4])<<8);auto i=debt.find(handle);
            if(i!=debt.end()){aclOutstanding-=i->second;debt.erase(i);}
            // Retired tokens still occupy guest buffers, but cannot credit a new link.
            auto r=rxDebt.find(handle);if(r!=rxDebt.end())for(auto& token:r->second){rxActive-=!(token&2);token|=2;}
        }
        ObserveAclEvent(p,n);
        return true;
    }
    void ObserveAclEvent(const uint8_t* p,size_t n){
        if(p[0]==0x10){ForgetAclLinks();return;}
        // Events before the reset result has reached the guest belong to an
        // uncertain epoch. They still follow the unchanged BCSP delivery path.
        if(resetPending || resetFailed || (resetEpoch && !resetAcknowledged)){
            if(p[0]==3 || p[0]==5)ForgetAclLinks();
            return;
        }
        if(p[0]==3){
            if(n!=13){ForgetAclLinks();return;}
            if(p[2] || p[11]!=1)return;
            const unsigned handle=p[3]|(unsigned(p[4])<<8);
            if(handle>0x0eff){ForgetAclLinks();return;}
            observedAcl.set(handle);
        }else if(p[0]==5){
            if(n!=6){ForgetAclLinks();return;}
            if(p[2])return;
            const unsigned handle=p[3]|(unsigned(p[4])<<8);
            if(handle>0x0eff){ForgetAclLinks();return;}
            observedAcl.reset(handle);
        }
    }
    std::deque<ControlObservation> controls;
    uint64_t unretainedControls{};
    bool unsupportedControl{};
    std::string error;
    void Fail(const char* message){ForgetAclLinks();error=message;backend.Close();transport.Stop();}
};
}
#endif
