#include "RawPrintFile.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>

namespace fs = std::filesystem;
static bool failCleanup=false, failWrite=false;
static unsigned writes=0, cleanupCalls=0;
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_unlinkat(int, const char*, int);
extern "C" ssize_t __wrap_write(int fd, const void* bytes, size_t size) {
    if(!failWrite) return __real_write(fd,bytes,size);
    if(writes++==0) return __real_write(fd,bytes,std::min(size,size_t(2)));
    errno=EIO;return -1;
}
extern "C" int __wrap_unlinkat(int fd, const char* path, int flags) {
    if(!failCleanup) return __real_unlinkat(fd,path,flags);
    ++cleanupCalls;errno=EACCES;return -1;
}
struct Output {
    RawPrintFile file;
    unsigned calls=0;
    explicit Output(const std::string& path):file(path) {}
    static RawPrintJob::Publication publish(void* context,const uint8_t* bytes,size_t size) {
        auto& self=*static_cast<Output*>(context);++self.calls;
        return RawPrintFile::Output(&self.file,bytes,size);
    }
};
int main(int argc,char** argv) {
    assert(argc==2);
    const fs::path root=argv[1];
    // A failed earlier run can leave its folders behind.
    fs::remove_all(root);fs::create_directories(root);
    for(unsigned test=0;test<4;++test) {
        const auto directory=root/std::to_string(test);
        assert(fs::create_directory(directory));
        Output output((directory/"job.raw").string());
        if(test==1) output.file.fault=RawPrintFile::Fault::publish;
        if(test==2) output.file.fault=RawPrintFile::Fault::close;
        const uint8_t payload[]={0,42,255,12};
        RawPrintJob job(8);assert(job.begin());assert(job.append(payload,sizeof(payload)));
        writes=cleanupCalls=0;failWrite=test==0;failCleanup=true;
        const auto state=job.finish(Output::publish,&output);
        failWrite=failCleanup=false;
        assert(state==RawPrintJob::State::uncertain);
        assert(output.calls==1 && cleanupCalls==1 && fs::exists(output.file.path)==(test==3));
        fs::path staging;
        for(const auto& entry:fs::directory_iterator(directory)) {
            if(entry.path().filename()=="job.raw") continue;
            assert(staging.empty());staging=entry.path();
        }
        assert(!staging.empty() && staging.filename().string().find(".frond-print-")==0);
        assert(output.file.error.find(staging.string())!=std::string::npos);
        struct stat st{};assert(stat(staging.c_str(),&st)==0 && (st.st_mode&0777)==0600);
        std::ifstream stream(staging,std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)),{});
        assert(bytes==std::string(reinterpret_cast<const char*>(payload),test==0?2:sizeof(payload)));
        assert(job.finish(Output::publish,&output)==RawPrintJob::State::uncertain);
        assert(output.calls==1 && cleanupCalls==1 && fs::file_size(staging)==bytes.size());
        fs::remove_all(directory);
    }
    {
        // Write fails, staging is removed, then the directory flush fails: nothing was published or kept.
        const auto directory=root/"directory-flush";
        assert(fs::create_directory(directory));
        Output output((directory/"job.raw").string());
        output.file.fault=RawPrintFile::Fault::directory;
        const uint8_t payload[]={0,42,255,12};
        RawPrintJob job(8);assert(job.begin());assert(job.append(payload,sizeof(payload)));
        writes=cleanupCalls=0;failWrite=true;
        const auto state=job.finish(Output::publish,&output);
        failWrite=false;
        assert(state==RawPrintJob::State::rejected);
        assert(output.calls==1 && fs::is_empty(directory));
        assert(output.file.error.find((directory/".frond-print-").string())!=std::string::npos);
        assert(output.file.error.find("may not be durable")!=std::string::npos);
        assert(job.finish(Output::publish,&output)==RawPrintJob::State::rejected && output.calls==1);
        fs::remove_all(directory);
    }
    {
        // A parent the user can only search, as / and /data are for an Android app: the job still lands.
        const auto parent=root/"search-only", directory=parent/"print";
        assert(fs::create_directory(parent) && fs::create_directory(directory));
        fs::permissions(parent,fs::perms::owner_write|fs::perms::owner_exec);
        Output output((directory/"job.raw").string());
        const uint8_t payload[]={7,8,9};
        RawPrintJob job(8);assert(job.begin());assert(job.append(payload,sizeof(payload)));
        const auto state=job.finish(Output::publish,&output);
        assert(state==RawPrintJob::State::published && output.file.error.empty());
        assert(fs::file_size(directory/"job.raw")==sizeof(payload));
        fs::permissions(parent,fs::perms::owner_all);
        fs::remove_all(parent);
    }
    {
        // A refused hard link, as on Android: the staging file is renamed into place instead.
        const auto directory=root/"no-link";
        assert(fs::create_directory(directory));
        Output output((directory/"job.raw").string());
        output.file.fault=RawPrintFile::Fault::link;
        const uint8_t payload[]={1,2,3,4,5};
        RawPrintJob job(8);assert(job.begin());assert(job.append(payload,sizeof(payload)));
        cleanupCalls=0;failCleanup=true;
        const auto state=job.finish(Output::publish,&output);
        failCleanup=false;
        assert(state==RawPrintJob::State::published && output.file.error.empty());
        assert(fs::file_size(directory/"job.raw")==sizeof(payload));
        unsigned entries=0;for(const auto& entry:fs::directory_iterator(directory)) {(void)entry;++entries;}
        assert(entries==1 && cleanupCalls==0);
        // The rename never replaces a file that is already there.
        Output again((directory/"job.raw").string());
        again.file.fault=RawPrintFile::Fault::link;
        RawPrintJob second(8);assert(second.begin());assert(second.append(payload,2));
        assert(second.finish(Output::publish,&again)==RawPrintJob::State::rejected);
        assert(fs::file_size(directory/"job.raw")==sizeof(payload));
        fs::remove_all(directory);
    }
    std::cout<<"PASS rename when links are refused; search-only parent; partial write, refused publication and close failure retain private staging with uncertain state and path; directory flush failure after cleanup is rejected with the path; no callback replay\n";
}
