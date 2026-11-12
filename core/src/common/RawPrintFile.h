#pragma once
#include "RawPrintJob.h"
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <cerrno>
#include <string>

// POSIX native output. Each finish needs a caller-chosen unique absolute destination.
class RawPrintFile {
 public:
    enum class Fault { none, write, flush, close, publish, directory, link };
    std::string path, error;
    Fault fault=Fault::none;
    explicit RawPrintFile(std::string destination): path(std::move(destination)) {}
    static RawPrintJob::Publication Output(void* context,const uint8_t* bytes,size_t size) {
        return static_cast<RawPrintFile*>(context)->publish(bytes,size);
    }
 private:
#ifdef O_PATH
    static constexpr int WalkFlags=O_PATH;
#else
    static constexpr int WalkFlags=O_RDONLY;
#endif
    static int closeFd(int& fd) { int old=fd;fd=-1;return old<0?0:close(old); }
    // Android apps may not make hard links, so a refused link falls back to a rename that
    // won't replace an existing file. moved tells the caller the staging name is gone.
    bool link(int dir,const std::string& from,const std::string& to,bool& moved) const {
        moved=false;
        if(fault!=Fault::link && linkat(dir,from.c_str(),dir,to.c_str(),0)==0) return true;
        if(fault!=Fault::link && errno!=EPERM && errno!=EACCES) return false;
#if defined(__linux__) && defined(SYS_renameat2)
        moved=syscall(SYS_renameat2,dir,from.c_str(),dir,to.c_str(),1u /* RENAME_NOREPLACE */)==0;
#endif
        return moved;
    }
    struct Guard { int& fd; ~Guard() { closeFd(fd); } };
    struct Staging { int& dir; const std::string& name; ~Staging() { if(dir>=0) unlinkat(dir,name.c_str(),0); } };
    RawPrintJob::Publication publish(const uint8_t* bytes,size_t size) {
        using P=RawPrintJob::Publication;
        error="Invalid absolute output path.";
        if(size>16*1024*1024 || (size && !bytes)) {error="Raw output limit exceeded.";return P::rejected;}
        if(path.empty() || path[0]!='/' || path.size()>4096 || path.find('\0')!=std::string::npos) return P::rejected;
        // The walk only needs search permission on each parent (Android apps can't read / or /data),
        // so it opens path-only descriptors and then the final folder itself for its fsync.
        int dir=open("/",WalkFlags|O_DIRECTORY|O_CLOEXEC);
        Guard directoryGuard{dir};
        if(dir<0) { error="Cannot open output directory."; return P::rejected; }
        size_t start=1;
        std::string name;
        while(start<path.size()) {
            auto end=path.find('/',start);
            name=path.substr(start,end==std::string::npos?end:end-start);
            if(name.empty() || name=="." || name==".." || name.size()>255) { closeFd(dir);return P::rejected; }
            if(end==std::string::npos) break;
            int next=openat(dir,name.c_str(),WalkFlags|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            closeFd(dir);dir=next;
            if(dir<0) { error="Cannot open output directory.";return P::rejected; }
            start=end+1;
        }
        if(start>=path.size()) {closeFd(dir);return P::rejected;}
        {
            int folder=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            closeFd(dir);dir=folder;
            if(dir<0) { error="Cannot open output directory.";return P::rejected; }
        }
        unsigned char random[16];
        int entropy=open("/dev/urandom",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        Guard entropyGuard{entropy};
        bool seeded=entropy>=0 && read(entropy,random,sizeof(random))==sizeof(random);
        if(closeFd(entropy)!=0) seeded=false;
        if(!seeded) {closeFd(dir);error="Cannot create output identity.";return P::rejected;}
        std::string temporary=".frond-print-";
        for(auto c:random) {temporary+="0123456789abcdef"[c>>4];temporary+="0123456789abcdef"[c&15];}
        const std::string stagingPath=path.substr(0,path.rfind('/')+1)+temporary;
        int fd=openat(dir,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        Guard fileGuard{fd};
        if(fd<0) {closeFd(dir);error="Cannot create private output.";return P::rejected;}
        Staging staging{dir,temporary};
        bool ok=true;
        for(size_t offset=0;offset<size;) {
            ssize_t n=fault==Fault::write?-1:write(fd,bytes+offset,size-offset);
            if(n<0 && errno==EINTR && fault!=Fault::write) continue;
            if(n<=0) {ok=false;break;} offset+=n;
        }
        if(fsync(fd)!=0 || fault==Fault::flush) ok=false;
        if(closeFd(fd)!=0 || fault==Fault::close) ok=false;
        if(!ok) error="Output write, flush or close failed.";
        bool published=false,moved=false;
        if(ok) {
            published=fault!=Fault::publish && link(dir,temporary,name,moved);
            if(!published) error="Output publication refused; choose a new destination.";
        }
        bool cleaned=moved || unlinkat(dir,temporary.c_str(),0)==0;
        bool durable=fsync(dir)==0 && fault!=Fault::directory;
        bool closed=closeFd(dir)==0;
        if(published) {
            if(!cleaned || !durable || !closed) {
                error="Output exists; durability uncertain. Do not retry automatically.";
                if(!cleaned) error+=" Private staging file remains at "+stagingPath+".";
                return P::uncertain;
            }
            error.clear();return P::published;
        }
        // Nothing was published. Only a staging file that is still there makes the result uncertain.
        if(!cleaned) {error+=" Private staging file remains at "+stagingPath+". Do not retry automatically.";return P::uncertain;}
        if(!durable || !closed) error+=" Private staging file "+stagingPath+" was removed; the removal may not be durable.";
        return P::rejected;
    }
};
