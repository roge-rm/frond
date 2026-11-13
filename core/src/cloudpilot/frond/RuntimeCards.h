#pragma once
#include <chrono>
#include "ExternalStorage.h"
#include "device_profile5.h"
#include "MsBackend.h"
#include "../../uarm/uarm/SessionIo.h"
#include <sys/file.h>
#include <iomanip>
#include <sstream>

// Linked sessions retain live external backings. Completed writes never rewind with machine state.
// The Dana has two SD slots (0 and 2), the VZ Palms one (0), the T415 one Memory Stick (1); the
// device is set before any card moves.
class RuntimeCards {
    struct Card {
        int fd{-1};
        std::string key, path;
        uint32_t serial{};
        CardImage::Protocol protocol{CardImage::Protocol::sd};
        uint64_t baseDev{},baseIno{},baseMtime{};
        bool protectedCard{}, failed{};
        std::shared_ptr<FileCardBacking> backing;
        std::shared_ptr<CardImage> image;
        // A stick commits its generation when a save, a protect change or exit needs it, and at
        // least every two seconds, not on every page write.
        bool pending{};
        std::chrono::steady_clock::time_point lastCommit{};
        bool Settle() {
            if (failed) return false;
            if (pending) {
                if (!backing->Commit()) { failed = true; return false; }
                pending = false;
            }
            lastCommit = std::chrono::steady_clock::now();
            return true;
        }
        ~Card() { if (fd >= 0) ::close(fd); }
    };
    std::shared_ptr<Card> cards[2];
    std::shared_ptr<Card> stick;
    int32_t deviceType{deviceTypeDana};
    static bool ValidGeometry(int32_t deviceType, unsigned id, const std::string& protocol, uint64_t bytes) {
        if (protocol != "sd" && protocol != "mmc") return false;
        return deviceProfileCardSizeValid5(deviceType, DeviceCardSlot5(id),
            protocol == "mmc" ? DeviceCardFamily5::legacyMmc : DeviceCardFamily5::sdsc, bytes);
    }
    static EmHAL::Slot slot(unsigned n) { return n ? EmHAL::Slot::sdcard2 : EmHAL::Slot::sdcard; }
    static void BindImage(const std::shared_ptr<Card>& next, bool deferred = false) {
        auto* live = next.get();
        next->image = std::make_shared<CardImage>(next->backing, next->protectedCard, [live, deferred] {
            if (live->failed) return false;
            if (deferred) {
                live->pending = true;
                if (std::chrono::steady_clock::now() - live->lastCommit < std::chrono::seconds(2)) return true;
                return live->Settle();
            }
            if (!live->backing->Commit()) { live->failed = true; return false; }
            return true;
        }, next->serial, next->protocol);
    }
    std::shared_ptr<Card> Open(unsigned n, const std::string& inputPath, const std::string& key,
                               const std::string& policy, std::string& error, const std::string& protocol="sd") {
        if (n >= Slots() || (protocol != "sd" && protocol != "mmc")) { error="error: invalid slot or protocol"; return {}; }
        const auto path=std::filesystem::path(inputPath).lexically_normal().string();
        auto fail = [&](const std::string& why) -> std::shared_ptr<Card> { error=why; return {}; };
        auto next = std::make_shared<Card>();
        next->protocol = protocol=="mmc" ? CardImage::Protocol::mmc : CardImage::Protocol::sd;
        next->key = key; next->path = path; next->protectedCard = policy == "protected";
        // CID is 32 bits, so refuse a serial collision as well as duplicate identities.
        next->serial = uint32_t(std::stoul(key.substr(0, 8), nullptr, 16));

        next->fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        struct stat mine{}, named{};
        if (next->fd < 0 || flock(next->fd, LOCK_EX | LOCK_NB) || fstat(next->fd, &mine) ||
            stat(path.c_str(), &named) || !S_ISREG(mine.st_mode) || mine.st_dev != named.st_dev || mine.st_ino != named.st_ino)
            return fail("error: cannot lock card file");
        next->baseDev=mine.st_dev; next->baseIno=mine.st_ino;
        next->baseMtime=uint64_t(mine.st_mtim.tv_sec)*1000000000ull+mine.st_mtim.tv_nsec;
        frondSdFile::Info info;
        std::vector<uint32_t> table;
        if (frondMachine::readSdHeader(path, info, &table) != frondMachine::SiblingStatus::ok ||
            !ValidGeometry(deviceType, n ? 2 : 0, protocol, info.payloadBytes) ||
            !EmHAL::SupportsImageInSlot(slot(n), uint32_t(info.payloadBytes / 512)) ||
            frondMachine::checkSdPayload(path, info, table) != frondMachine::SiblingStatus::ok)
            return fail("error: invalid SD file or geometry (32 to 2048 MiB, representable SDSC; MMC 32/64/128/256 MiB)");
        FileCardOptions options;
        options.rejectInvalidRoot = true; options.identity = key; options.chunkCrcs = table;
        next->backing = FileCardBacking::Open(path, info.payloadBytes, frondSdFile::headerBytes, options);
        if (!next->backing) return fail("error: cannot open card backing");
        BindImage(next);
        return next;
    }
    static bool ValidStickBytes(int32_t deviceType, uint64_t stored) {
        const auto* layout = frondMsLayout::byStoredBytes(stored);
        return layout && deviceProfileMsSizeValid5(deviceType, layout->megabytes);
    }
    // A v2 .frondms of one of the device's stick sizes, with boot blocks the Sony driver takes.
    std::shared_ptr<Card> OpenStick(const std::string& inputPath, const std::string& key,
                                    const std::string& policy, std::string& error) {
        const auto path=std::filesystem::path(inputPath).lexically_normal().string();
        auto fail = [&](const std::string& why) -> std::shared_ptr<Card> { error=why; return {}; };
        auto next = std::make_shared<Card>();
        next->key = key; next->path = path; next->protectedCard = policy == "protected";
        next->serial = uint32_t(std::stoul(key.substr(0, 8), nullptr, 16));
        next->fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        struct stat mine{}, named{};
        if (next->fd < 0 || flock(next->fd, LOCK_EX | LOCK_NB) || fstat(next->fd, &mine) ||
            stat(path.c_str(), &named) || !S_ISREG(mine.st_mode) || mine.st_dev != named.st_dev || mine.st_ino != named.st_ino)
            return fail("error: cannot lock card file");
        next->baseDev=mine.st_dev; next->baseIno=mine.st_ino;
        next->baseMtime=uint64_t(mine.st_mtim.tv_sec)*1000000000ull+mine.st_mtim.tv_nsec;
        frondSdFile::Info info;
        std::vector<uint32_t> table;
        if (frondMachine::readSdHeader(path, info, &table) != frondMachine::SiblingStatus::ok || !info.classic ||
            info.version != frondSdFile::versionV2 || !ValidStickBytes(deviceType, info.stored()) ||
            !EmHAL::SupportsImageInSlot(EmHAL::Slot::memorystick, uint32_t(info.stored() / 512)) ||
            frondMachine::checkSdPayload(path, info, table) != frondMachine::SiblingStatus::ok)
            return fail("error: invalid Memory Stick file or size (a v2 .frondms of 4 to 128 MB)");
        FileCardOptions options;
        options.rejectInvalidRoot = true; options.identity = key; options.chunkCrcs = table;
        next->backing = FileCardBacking::Open(path, info.stored(), frondSdFile::headerBytes, options);
        if (!next->backing) return fail("error: cannot open card backing");
        BindImage(next, true);
        if (!FrondMsBackend::Valid(*next->image)) return fail("error: the stick's boot blocks or pages are not valid");
        return next;
    }
    static bool SameFile(const Card& c, const std::string& path) {
        struct stat named{}, owned{};
        return !lstat(path.c_str(), &named) && S_ISREG(named.st_mode) && !fstat(c.fd, &owned) &&
               named.st_dev == owned.st_dev && named.st_ino == owned.st_ino;
    }

  public:
    // Called before any card is staged or attached, while no card is held.
    void SetDevice(int32_t type) { deviceType = type; }
    // The device has a Memory Stick slot in frond-68k.
    bool StickSlot() const {
        const auto* p = deviceProfile5(deviceType);
        return p && p->board == DeviceBoard5::vz328 && deviceProfileStorageSlot5(*p) == DeviceStorageSlot5::memoryStick;
    }
    // The device takes manifests of linked cards.
    static bool LinkedDevice(const DeviceProfile5& p) {
        return deviceProfileLinkedCards5(p) ||
               (p.board == DeviceBoard5::vz328 && deviceProfileStorageSlot5(p) == DeviceStorageSlot5::memoryStick);
    }
    unsigned Slots() const {
        const auto* p = deviceProfile5(deviceType);
        return p && deviceProfileLinkedCards5(*p) ? deviceProfileCardSlotCount5(*p) : 0;
    }
    struct Pair {
        std::shared_ptr<Card> card[2];
        bool inserted[2]{};
        std::shared_ptr<Card> stick;
        bool stickInserted{};
    };
    Pair Snapshot() const {
        Pair p;
        for(unsigned n=0;n<2;++n) { p.card[n]=cards[n]; p.inserted[n]=cards[n] && gExternalStorage.IsMounted(slot(n)); }
        p.stick=stick; p.stickInserted=stick && gExternalStorage.IsMounted(EmHAL::Slot::memorystick);
        return p;
    }
    bool CanSave() const {
        if (stick && !stick->Settle()) return false;
        size_t count=0;
        for(const auto& c:{cards[0],cards[1],stick}) if(c) {
            struct stat st{};
            if(fstat(c->fd,&st) || uint64_t(st.st_mtim.tv_sec)*1000000000ull+st.st_mtim.tv_nsec!=c->baseMtime)
                return false;
            if(c->failed || !c->backing->DurableIs(c->path) || gExternalStorage.GetImage(c->key)!=c->image.get()) return false;
            ++count;
        }
        return count==gExternalStorage.ImageCount();
    }
    bool HasCards() const { return bool(cards[0]) || bool(cards[1]) || bool(stick); }
    std::string Manifest() const {
        if (stick) stick->Settle();
        std::ostringstream out; out << "frond-linked-sd 3\n";
        for(unsigned n=0;n<2;++n) if(cards[n]) {
            const auto& c=*cards[n];
            out << (n?2:0) << " sd " << std::quoted(c.path) << " " << c.key << " "
                << c.serial << " " << c.backing->Size() << " " << c.protectedCard << " "
                << gExternalStorage.IsMounted(slot(n)) << " " << c.baseDev << " " << c.baseIno << " " << c.baseMtime << " " << (c.protocol==CardImage::Protocol::mmc?"mmc":"sd") << " " << c.backing->RootGenerationId() << "\n";
        }
        if(stick) {
            const auto& c=*stick;
            out << "1 ms " << std::quoted(c.path) << " " << c.key << " " << c.serial << " " << c.backing->Size() << " "
                << c.protectedCard << " " << gExternalStorage.IsMounted(EmHAL::Slot::memorystick) << " " << c.baseDev << " "
                << c.baseIno << " " << c.baseMtime << " sd " << c.backing->RootGenerationId() << "\n";
        }
        return out.str();
    }
    // Called on the stopped guest thread, with both source locks held throughout the copy.
    bool CopyPortable(const std::filesystem::path& dir, std::string& manifest) const {
        if(!CanSave() || stick) return false;
        std::ostringstream out; out << "frond-portable-sd 3\n";
        for(unsigned n=0;n<2;++n) if(cards[n]) {
            const auto& c=*cards[n];
            const std::string name=n?"sd2.frondsd":"sd0.frondsd";
            std::array<uint8_t,frondMachine::identityBytes> identity{};
            if(!frondSession::writeSdFromReader(dir/name,c.backing->Size(),
                [&](uint64_t off,uint64_t size,void* dest){return c.backing->Read(off,size,dest);},identity)) return false;
            frondSdFile::Info info;
            if(frondMachine::readSdHeader(dir/name,info)!=frondMachine::SiblingStatus::ok) return false;
            FileCardOptions proof; proof.rejectInvalidRoot=true; proof.identity=c.key;
            auto copied=FileCardBacking::Open(dir/name,c.backing->Size(),frondSdFile::headerBytes,proof);
            if(!copied) return false;
            out << (n?2:0) << " sd " << name << " " << c.key << " " << c.serial << " "
                << c.backing->Size() << " " << c.protectedCard << " "
                << gExternalStorage.IsMounted(slot(n)) << " " << info.crc << " " << (c.protocol==CardImage::Protocol::mmc?"mmc":"sd") << "\n";
        }
        manifest=out.str();
        return CanSave();
    }
    // Portable identity is the guest identity. Ownership is the new directory and locked inode.
    // Resolve only fixed sibling names; Stage performs the full pair and root checks afterwards.
    static std::string ResolvePortable(int32_t deviceType,const std::string& text,const std::filesystem::path& dir,
                                       std::string& linked) {
        if(text.size()>8192 || text.find('\0')!=std::string::npos) return "invalid portable manifest";
        std::istringstream in(text); std::string line;
        if(!std::getline(in,line) || line!="frond-portable-sd 3") return "unknown portable manifest";
        const bool v2=line=="frond-portable-sd 3";
        std::ostringstream out; out << "frond-linked-sd 3\n";
        unsigned seen=0;
        while(std::getline(in,line)) {
            unsigned id,protect,insert; uint64_t serial,size,crc;
            std::string kind,name,key,extra,protocol="sd"; std::istringstream row(line);
            if(!(row>>id>>kind>>name>>key>>serial>>size>>protect>>insert>>crc) || (v2 && !(row>>protocol)) || (protocol!="sd" && protocol!="mmc") || row>>extra ||
                (id!=0 && id!=2) || kind!="sd" || name!=(id?"sd2.frondsd":"sd0.frondsd") ||
                !ValidGeometry(deviceType,id,protocol,size) || (seen&(1u<<id)) || protect>1 || insert>1) return "invalid portable descriptor";
            seen|=1u<<id;
            const auto path=std::filesystem::absolute(dir/name).lexically_normal();
            struct stat st{}; frondSdFile::Info info;
            if(lstat(path.c_str(),&st) || !S_ISREG(st.st_mode) ||
                frondMachine::readSdHeader(path,info)!=frondMachine::SiblingStatus::ok ||
                info.version!=frondSdFile::versionV2 || info.payloadBytes!=size || info.crc!=crc)
                return "invalid portable media header or checksum";
            out << id << " sd " << std::quoted(path.string()) << " " << key << " " << serial << " "
                << size << " " << protect << " " << insert << " " << st.st_dev << " " << st.st_ino << " "
                << uint64_t(st.st_mtim.tv_sec)*1000000000ull+st.st_mtim.tv_nsec << " " << protocol << " 0\n";
        }
        if(!seen) return "empty portable manifest";
        linked=out.str(); return "";
    }
    std::string Stage(const std::string& text, Pair& next) {
        if(text.empty()) return "";
        if(text.size()>8192 || text.find('\0')!=std::string::npos) return "invalid card manifest";
        std::istringstream in(text); std::string line;
        if(!std::getline(in,line) || line!="frond-linked-sd 3") return "unknown card manifest";
        const bool v2=line=="frond-linked-sd 3";
        while(std::getline(in,line)) {
            std::istringstream row(line); unsigned id,protect,insert; uint64_t size,serial,dev,ino,mtime,history;
            std::string kind,path,key,extra,protocol="sd";
            if(!(row>>id>>kind>>std::quoted(path)>>key>>serial>>size>>protect>>insert>>dev>>ino>>mtime) || (v2 && !(row>>protocol>>history)) || (protocol!="sd" && protocol!="mmc") || row>>extra ||
                (id!=0 && id!=1 && id!=2) || kind!=(id==1?"ms":"sd") || path.empty() || path[0]!='/' || path.size()>4096 ||
                path.find_first_of("\r\n")!=std::string::npos || key.size()!=32 ||
                key.find_first_not_of("0123456789abcdef")!=std::string::npos || protect>1 || insert>1 ||
                (id==1 ? (protocol!="sd" || !StickSlot() || !ValidStickBytes(deviceType,size)) : !ValidGeometry(deviceType,id,protocol,size)) ||
                serial!=std::stoul(key.substr(0,8),nullptr,16)) return "invalid card descriptor";
            path=std::filesystem::path(path).lexically_normal().string();
            const bool ms=id==1;
            unsigned n=id==2;
            std::shared_ptr<Card>& held=ms?next.stick:next.card[n];
            bool& heldInserted=ms?next.stickInserted:next.inserted[n];
            if(held) return "duplicate card slot, identity or serial";
            for(const auto* other:{&next.card[0],&next.card[1],&next.stick})
                if(other!=&held && *other && ((*other)->key==key || (*other)->serial==serial))
                    return "duplicate card slot, identity or serial";
            for(auto* c:{&cards[0],&cards[1],&stick}) if(*c && (*c)->key==key &&
                (*c)->protocol==(protocol=="mmc"?CardImage::Protocol::mmc:CardImage::Protocol::sd) && SameFile(**c,path)) {
                if((*c)->path!=path) return "linked card generation namespace changed";
                if((*c)->protectedCard==bool(protect) && (*c)->path==path) held=*c;
                else {
                    auto copy=std::make_shared<Card>(); copy->fd=dup((*c)->fd);
                    if(copy->fd<0) return "cannot retain card lock";
                    copy->protocol=(*c)->protocol; copy->key=key; copy->path=path; copy->serial=(*c)->serial;
                    copy->baseDev=(*c)->baseDev; copy->baseIno=(*c)->baseIno; copy->baseMtime=(*c)->baseMtime;
                    copy->protectedCard=protect; copy->failed=(*c)->failed; copy->backing=(*c)->backing;
                    BindImage(copy); held=std::move(copy);
                }
            }
            FileCardOptions preflight; preflight.rejectInvalidRoot=true; preflight.requireRoot=true;
            preflight.minimumRoot=held?std::max(history,held->backing->RootGenerationId()):history; preflight.identity=key;
            if(!FileCardBacking::Open(path,size,frondSdFile::headerBytes,preflight))
                return "linked card committed history missing or invalid";
            std::string error;
            if(!held) held=ms?OpenStick(path,key,protect?"protected":"writable",error):Open(n,path,key,protect?"protected":"writable",error,protocol);
            if(!held) return error;
            if(held->baseDev!=dev || held->baseIno!=ino || held->baseMtime!=mtime)
                return "linked card source was replaced or modified";
            if(held->failed || held->backing->Size()!=size) return "card geometry or I/O fault";
            FileCardOptions check; check.rejectInvalidRoot=true; check.requireRoot=true;
            check.minimumRoot=std::max(history,held->backing->RootGenerationId()); check.identity=key;
            if(!FileCardBacking::Open(path,size,frondSdFile::headerBytes,check))
                return "linked card committed history missing or invalid";
            heldInserted=insert;
        }
        return "";
    }
    void Install(const Pair& p) {
        for(auto* c:{&cards[0],&cards[1],&stick}) if(*c) gExternalStorage.RemoveImage((*c)->key);
        for(unsigned n=0;n<2;++n) {
            cards[n]=p.card[n];
            if(cards[n]) gExternalStorage.AddImage(cards[n]->key,cards[n]->image);
        }
        stick=p.stick;
        if(stick) gExternalStorage.AddImage(stick->key,stick->image);
    }
    bool Matches(const Pair& p) const {
        for(unsigned n=0;n<2;++n)
            if(gExternalStorage.GetImageKeyInSlot(slot(n)) != (p.card[n] && p.inserted[n]?p.card[n]->key:"")) return false;
        return gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::memorystick) == (p.stick && p.stickInserted?p.stick->key:"");
    }
    ~RuntimeCards() {
        if (stick) stick->Settle();
        for (auto& card : cards) if (card) gExternalStorage.RemoveImage(card->key);
        if (stick) gExternalStorage.RemoveImage(stick->key);
    }
    // The Memory Stick's commands (ms-state, ms-attach, ms-eject, ms-insert, ms-protect, ms-export, ms-take-out).
    std::string StickCommand(const std::string& op, const std::string& argument) {
        constexpr auto msSlot = EmHAL::Slot::memorystick;
        if (!StickSlot()) return "error: unsupported device";
        if (op == "state" && argument.empty())
            return !stick ? "none" : (gExternalStorage.IsMounted(msSlot) ? "inserted " : "ejected ") +
                stick->key + (stick->protectedCard ? " protected" : " writable") + (stick->failed ? " io-error" : "");
        if (op == "attach" || (op == "insert" && !argument.empty())) {
            if (stick) return "error: take out the current stick first";
            std::istringstream words(argument);
            std::string path, key, policy, extra;
            if (!(words >> std::quoted(path) >> key >> policy) || (words >> extra) || path.empty() || path[0] != '/' ||
                path.size()>4096 || path.find_first_of("\r\n")!=std::string::npos ||
                key.size() != 32 || key.find_first_not_of("0123456789abcdef") != std::string::npos ||
                (policy != "protected" && policy != "writable"))
                return "error: use absolute-path 32-lowercase-hex-identity writable|protected";
            for (auto& other : cards) if (other && (other->key == key || other->serial == uint32_t(std::stoul(key.substr(0,8),nullptr,16))))
                return "error: duplicate identity or serial";
            std::string error;
            auto next=OpenStick(path,key,policy,error);
            if (!next) return error;
            if (!gExternalStorage.AddImage(key, next->image)) return "error: duplicate image key";
            if (!gExternalStorage.Mount(key, msSlot)) {
                gExternalStorage.RemoveImage(key);
                return "error: cannot mount stick";
            }
            stick = std::move(next);
            return "inserted";
        }
        if (!stick) return "error: empty slot";
        if (op == "export") {
            std::istringstream words(argument); std::string dest, extra;
            if (!(words >> std::quoted(dest)) || (words >> extra) || dest.empty() || dest[0]!='/')
                return "error: use an absolute destination path";
            int fd=::open(dest.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if (fd<0) return "error: destination must be a new file";
            std::array<uint8_t, frondMachine::identityBytes> identity{};
            const bool ok=frondSession::writeSdFromReader(dest,stick->backing->Size(),
                [&](uint64_t off,uint64_t count,void* p){return stick->backing->Read(off,count,p);},identity,1,
                frondSession::msShape(stick->backing->Size()));
            ::close(fd);
            if (!ok) { ::unlink(dest.c_str()); return "error: stick export failed"; }
            if (!frondSession::syncDir(std::filesystem::path(dest).parent_path())) return "error: export directory flush failed";
            return "exported";
        }
        if (op == "protect") {
            if (argument != "on" && argument != "off") return "error: use on or off";
            if (gExternalStorage.IsMounted(msSlot)) return "error: eject first";
            const auto key = stick->key;
            gExternalStorage.RemoveImage(key);
            if (!stick->Settle()) return "error: stick I/O failed";
            stick->protectedCard = argument == "on";
            BindImage(stick, true);
            gExternalStorage.AddImage(key, stick->image);
            return argument == "on" ? "protected" : "writable";
        }
        if (!argument.empty()) return "error: this command takes no argument";
        if (op == "eject") return gExternalStorage.Unmount(msSlot) ? "ejected" : "error: already ejected";
        if (op == "insert") return gExternalStorage.Mount(stick->key, msSlot) ? "inserted" : "error: already inserted";
        if (op == "take-out") {
            if (gExternalStorage.IsMounted(msSlot)) return "error: eject first";
            if (stick->failed) return "error: stick I/O failed; backing retained";
            std::array<uint8_t, frondMachine::identityBytes> identity{};
            if (!stick->protectedCard && !frondSession::writeSdFromReader(stick->path, stick->backing->Size(),
                [&](uint64_t off, uint64_t count, void* dest) { return stick->backing->Read(off, count, dest); }, identity, 1,
                frondSession::msShape(stick->backing->Size())))
                return "error: could not settle stick file";
            if (!stick->protectedCard && !frondSession::syncDir(std::filesystem::path(stick->path).parent_path()))
                return "error: stick directory flush failed; backing retained";
            gExternalStorage.RemoveImage(stick->key);
            const auto path = stick->path;
            const bool protect = stick->protectedCard;
            stick.reset();
            if (!protect) FileCardBacking::DiscardGenerations(path);
            return "taken-out";
        }
        return "error: unsupported stick command";
    }
    std::string Command(unsigned n, const std::string& op, const std::string& argument) {
        if (!Slots()) return "error: unsupported device";
        if (n >= Slots()) return "error: this device has one card slot";
        auto& card = cards[n];
        if (op == "state" && argument.empty())
            return !card ? "none" : (gExternalStorage.IsMounted(slot(n)) ? "inserted " : "ejected ") +
                card->key + (card->protectedCard ? " protected" : " writable") + (card->failed ? " io-error" : "") + (card->protocol==CardImage::Protocol::mmc?" mmc":"");
        if (op == "attach" || (op == "insert" && !argument.empty())) {
            if (card) return "error: take out the current card first";
            std::istringstream words(argument);
            std::string path, key, policy, extra, protocol="sd";
            if (!(words >> std::quoted(path) >> key >> policy) || ((words >> protocol) && (words >> extra)) || (protocol!="sd" && protocol!="mmc") || path.empty() || path[0] != '/' || path.size()>4096 || path.find_first_of("\r\n")!=std::string::npos ||
                key.size() != 32 || key.find_first_not_of("0123456789abcdef") != std::string::npos ||
                (policy != "protected" && policy != "writable"))
                return "error: use absolute-path 32-lowercase-hex-identity writable|protected [sd|mmc]";
            for (auto& other : cards) if (other && (other->key == key || other->serial == uint32_t(std::stoul(key.substr(0,8),nullptr,16))))
                return "error: duplicate identity or serial";
            std::string error;
            auto next=Open(n,path,key,policy,error,protocol);
            if (!next) return error;
            if (!gExternalStorage.AddImage(key, next->image)) return "error: duplicate image key";
            if (!gExternalStorage.Mount(key, slot(n))) {
                gExternalStorage.RemoveImage(key);
                return "error: cannot mount card";
            }
            card = std::move(next);
            return "inserted";
        }
        if (!card) return "error: empty slot";
        if (op == "export") {
            std::istringstream words(argument); std::string dest, extra;
            if (!(words >> std::quoted(dest)) || (words >> extra) || dest.empty() || dest[0]!='/')
                return "error: use an absolute destination path";
            int fd=::open(dest.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if (fd<0) return "error: destination must be a new file";
            std::array<uint8_t, frondMachine::identityBytes> identity{};
            const bool ok=frondSession::writeSdFromReader(dest,card->backing->Size(),
                [&](uint64_t off,uint64_t count,void* p){return card->backing->Read(off,count,p);},identity);
            ::close(fd);
            if (!ok) { ::unlink(dest.c_str()); return "error: card export failed"; }
            if (!frondSession::syncDir(std::filesystem::path(dest).parent_path())) return "error: export directory flush failed";
            return "exported";
        }
        if (!argument.empty()) return "error: this command takes no argument";
        if (op == "eject") return gExternalStorage.Unmount(slot(n)) ? "ejected" : "error: already ejected";
        if (op == "insert") return gExternalStorage.Mount(card->key, slot(n)) ? "inserted" : "error: already inserted";
        if (op == "take-out") {
            if (gExternalStorage.IsMounted(slot(n))) return "error: eject first";
            if (card->failed) return "error: card I/O failed; backing retained";
            // Settle the live generations into an ordinary portable .frondsd before releasing it.
            std::array<uint8_t, frondMachine::identityBytes> identity{};
            if (!card->protectedCard && !frondSession::writeSdFromReader(card->path, card->backing->Size(),
                [&](uint64_t off, uint64_t count, void* dest) { return card->backing->Read(off, count, dest); }, identity))
                return "error: could not settle card file";
            if (!card->protectedCard && !frondSession::syncDir(std::filesystem::path(card->path).parent_path()))
                return "error: card directory flush failed; backing retained";
            gExternalStorage.RemoveImage(card->key);
            const auto path = card->path;
            const bool protect = card->protectedCard;
            card.reset();
            if (!protect) FileCardBacking::DiscardGenerations(path);
            return "taken-out";
        }
        return "error: unsupported card command";
    }
};
