#ifndef FROND_FILE_CARD_BACKING_H
#define FROND_FILE_CARD_BACKING_H

#include "CardBacking.h"
#include "FrondSdImage.h"

#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

// File-backed copy-on-write generations. Durable payload is a sibling
// .frondsd / .frondms or a tiny test file. Working files are .gen-* in the
// same folder. The dirty index is a bounded memory cache plus a disk index.

struct FileCardOptions {
    // uARM saves adopt the durable base without Commit. With no root pointer,
    // retain crash leftovers and resume that base; missing committed history is unknowable.
    bool recoverRootlessBase = false;
    bool rejectInvalidRoot = false;
    bool requireRoot = false;
    uint64_t minimumRoot = 0;
    uint32_t pageSize = 4096;
    size_t cacheBytes = 16u << 20;
    size_t indexCacheEntries = 4096;
    uint32_t maxGenerations = 8;
    std::string identity;  // card identity; generations only apply to the same card and file
    // CRC32 per chunk of the durable payload (.frondsd v2). Each chunk is checked the first time
    // it is read from the durable file; a mismatch fails the read. Empty means no check.
    std::vector<uint32_t> chunkCrcs;
    uint32_t chunkBytes = 1u << 20;
    // An Open between HoldGenerations and Return or Drop: leaves the .held- files alone.
    bool keepHeld = false;
};

struct FileCardIoHooks {
    uint64_t bytesRead = 0;
    uint64_t bytesWritten = 0;
    int reads = 0, writes = 0, flushes = 0, closes = 0, renames = 0;
    bool failCompact = false;
    int failReadAfter = -1, failWriteAfter = -1, failFlushAfter = -1, failCloseAfter = -1,
        failRenameAfter = -1;
};

class FileCardBacking : public CardBacking {
   public:
    static constexpr uint64_t kTomb = ~uint64_t{0};
    static constexpr uint32_t kHeaderBytes = 64;
    static constexpr uint32_t kRootBytes = 64;
    static constexpr uint32_t kIndexRecBytes = 16;

    static std::unique_ptr<FileCardBacking> Open(const std::filesystem::path& durable,
                                                 uint64_t payloadSize, uint64_t payloadOffset,
                                                 const FileCardOptions& options = {},
                                                 FileCardIoHooks* hooks = nullptr) {
        auto impl = std::make_shared<Impl>();
        if (!impl->open(durable, payloadSize, payloadOffset, options, hooks)) return nullptr;
        auto backing = std::unique_ptr<FileCardBacking>(new FileCardBacking(std::move(impl)));
        return backing;
    }

    ~FileCardBacking() override = default;

    uint64_t Size() const override { return impl_->payloadSize; }
    uint32_t PageSize() const { return impl_->pageSize; }
    uint64_t RootGenerationId() const { return impl_->root->id; }
    uint64_t WorkingGenerationId() const { return impl_->working->id; }
    std::vector<std::filesystem::path> OwnedPaths() const { return impl_->ownedPaths(); }
    const std::filesystem::path& DurablePath() const { return impl_->durablePath; }
    // The durable file this backing has open is the one at `path` now (same device and inode),
    // not one a rename has since replaced.
    bool DurableIs(const std::filesystem::path& path) const {
        struct stat open{}, named{};
        return impl_->durableFd >= 0 && fstat(impl_->durableFd, &open) == 0 && ::stat(path.c_str(), &named) == 0 &&
               open.st_dev == named.st_dev && open.st_ino == named.st_ino;
    }
    // A snapshot handle still refers to this backing.
    bool Pinned() const { return impl_.use_count() > 1; }

    // Exact grammar shared by namespace cleanup and pristine-root checks.
    static bool GeneratedName(const std::string& tag, const std::string& name) {
        const auto prefix = ".gen-" + tag + "-";
        if (name.compare(0, prefix.size(), prefix)) return false;
        auto rest = name.substr(prefix.size());
        if (rest == "root") return true;
        auto decimal = [](const std::string& v) {
            if (v.empty() || v[0] < '1' || v[0] > '9') return false;
            uint64_t n = 0;
            for (char c : v) {
                if (c < '0' || c > '9' || n > (UINT64_MAX - (c - '0')) / 10) return false;
                n = n * 10 + (c - '0');
            }
            return true;
        };
        for (const auto* tmp : {"tmp-idx-", "tmp-root-", "tmp-"}) {
            const std::string start(tmp);
            if (rest.compare(0, start.size(), start)) continue;
            const auto pair = rest.substr(start.size());
            const auto dash = pair.find('-');
            return dash != std::string::npos && decimal(pair.substr(0, dash)) && decimal(pair.substr(dash + 1));
        }
        if (rest.size() > 4 && rest.substr(rest.size() - 4) == ".idx") rest.resize(rest.size() - 4);
        return decimal(rest);
    }

    // A temporary name can also spell a neighbour's generation. Preserve that
    // entry whenever the neighbour's durable file exists.
    static bool NeighbourName(const std::filesystem::path& durable, const std::string& name,
                              bool longerOnly = false) {
        auto dir = durable.parent_path();
        if (dir.empty()) dir = ".";
        if (name.compare(0, 5, ".gen-")) return false;
        for (size_t dash = name.find('-', 5); dash != std::string::npos; dash = name.find('-', dash + 1)) {
            const auto tag = name.substr(5, dash - 5);
            if (tag == durable.filename().string() ||
                (longerOnly && tag.size() <= durable.filename().string().size()) ||
                !GeneratedName(tag, name)) continue;
            std::error_code ec;
            if (std::filesystem::exists(dir / tag, ec) || ec) return true;
        }
        return false;
    }

    // Generations moved aside under ".held-" names (which no load or discard looks at), for a move
    // onto `durable` that may still fail. Open descriptors on them stay valid across the renames.
    struct HeldGenerations {
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;  // original, held
        // The move failed: they go back under their own names, the root last, so a crash part way
        // never leaves a root whose generations are still held.
        void Return() {
            std::error_code ec;
            for (int rootPass = 0; rootPass < 2; ++rootPass)
                for (const auto& [original, held] : moved) {
                    const auto name = original.filename().string();
                    const bool root = name.size() > 5 && name.compare(name.size() - 5, 5, "-root") == 0;
                    if (root == bool(rootPass)) std::filesystem::rename(held, original, ec);
                }
            moved.clear();
        }
        // The move worked: they go.
        void Drop() {
            std::error_code ec;
            for (const auto& entry : moved) std::filesystem::remove(entry.second, ec);
            moved.clear();
        }
    };
    // As DiscardGenerations, but moved aside, to Return or Drop.
    static HeldGenerations HoldGenerations(const std::filesystem::path& durable) {
        SettleHeld(durable);
        auto dir = durable.parent_path();
        if (dir.empty()) dir = ".";
        std::error_code ec;
        HeldGenerations held;
        std::vector<std::filesystem::path> found;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (GeneratedName(durable.filename().string(), name) && !NeighbourName(durable, name))
                found.push_back(it->path());
        }
        for (const auto& path : found) {
            const auto aside = dir / (".held-" + path.filename().string());
            std::error_code renamed;
            std::filesystem::rename(path, aside, renamed);
            if (!renamed) held.moved.emplace_back(path, aside);
        }
        return held;
    }

    // Held generations a crash left behind (between HoldGenerations and Return or Drop). They come
    // back under their names when their root binds the durable file there now and no live root
    // does; otherwise the durable file has replaced them and they go.
    static void SettleHeld(const std::filesystem::path& durable) {
        auto dir = durable.parent_path();
        if (dir.empty()) dir = ".";
        const auto tag = durable.filename().string();
        std::error_code ec;
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> found;  // held, original
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (name.compare(0, 6, ".held-")) continue;
            const auto original = name.substr(6);
            if (GeneratedName(tag, original) && !NeighbourName(durable, original))
                found.emplace_back(it->path(), dir / original);
        }
        if (found.empty()) return;
        const auto rootName = ".gen-" + tag + "-root";
        const bool restore = !std::filesystem::exists(dir / rootName, ec) && !ec &&
                             RootBinds(dir / (".held-" + rootName), durable);
        for (const auto& [held, original] : found) {
            std::error_code done;
            if (restore && !std::filesystem::exists(original, done) && !done)
                std::filesystem::rename(held, original, done);
            else
                std::filesystem::remove(held, done);
        }
    }

    // True when the v2 root at `rootPath` was written for the durable file now at `durable`.
    static bool RootBinds(const std::filesystem::path& rootPath, const std::filesystem::path& durable) {
        uint8_t buf[kRootBytes]{};
        const int fd = ::open(rootPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool read = ::pread(fd, buf, kRootBytes, 0) == ssize_t(kRootBytes);
        ::close(fd);
        struct stat st {};
        if (!read || std::memcmp(buf, "FRONDRT\0", 8) != 0 || frondSdFile::get32(buf + 8) != 2 ||
            stat(durable.c_str(), &st) != 0)
            return false;
        const uint64_t mtime = uint64_t(st.st_mtim.tv_sec) * 1000000000ull + uint64_t(st.st_mtim.tv_nsec);
        return frondSdFile::get64(buf + 28) == uint64_t(st.st_dev) &&
               frondSdFile::get64(buf + 36) == uint64_t(st.st_ino) && frondSdFile::get64(buf + 44) == mtime;
    }

    // Removes recognised, unambiguous namespace entries of a closed backing.
    static void DiscardGenerations(const std::filesystem::path& durable) {
        SettleHeld(durable);
        auto dir = durable.parent_path();
        if (dir.empty()) dir = ".";
        std::error_code ec;
        std::vector<std::filesystem::path> doomed;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (GeneratedName(durable.filename().string(), name) && !NeighbourName(durable, name))
                doomed.push_back(it->path());
        }
        for (const auto& path : doomed) std::filesystem::remove(path, ec);
    }

    std::shared_ptr<const CardSnapshot> Snapshot() override {
        if (!impl_->flushWorking()) return nullptr;
        return std::make_shared<Pin>(impl_, impl_->working);
    }

    bool Commit() override {
        if (!impl_->flushWorking()) return false;
        if (impl_->working != impl_->root && !impl_->freeze(impl_->working.get())) return false;
        // Compaction must succeed before publishing a chain the loader cannot open.
        if (!impl_->reloadable(impl_->working.get())) return impl_->compact();
        if (!impl_->publishRoot(impl_->working)) return false;
        impl_->root = impl_->working;
        if (impl_->liveCount() > impl_->maxGenerations) Compact();
        return true;
    }

    bool Rollback() override {
        impl_->dropCache();
        impl_->working = impl_->root;
        return true;
    }

    bool Restore(const CardSnapshot& snapshot) override {
        const auto* pin = dynamic_cast<const Pin*>(&snapshot);
        if (!pin || pin->impl != impl_ || !pin->gen) return false;
        impl_->dropCache();
        impl_->working = pin->gen;
        return true;
    }

    bool Compact() override { return impl_->compact(); }

    bool ForEachOverlay(const OverlayFn& fn) const override {
        return impl_->forEachOverlay(impl_->working.get(), fn);
    }
    // Pages of a pinned generation that differ from the durable file, in page order.
    bool ForEachOverlayIn(const CardSnapshot& snapshot, const OverlayFn& fn) const {
        const auto* pin = dynamic_cast<const Pin*>(&snapshot);
        if (!pin || pin->impl != impl_ || !pin->gen) return false;
        return impl_->forEachOverlay(pin->gen.get(), fn);
    }
    // Replaces the chunk table after the durable file was rewritten in place.
    bool SetChunkCrcs(std::vector<uint32_t> table) { return impl_->setChunkCrcs(std::move(table)); }
    // Stops checking a chunk, for one a failed journal apply left part old, part new.
    void TrustChunk(uint64_t index) { impl_->trustChunk(index); }

    uint32_t LiveGenerations() const override { return impl_->liveCount(); }

   protected:
    bool ReadBytes(size_t offset, size_t length, void* dest) const override {
        return impl_->readWorking(offset, length, dest);
    }
    bool WriteBytes(size_t offset, size_t length, const void* src) override {
        return impl_->writeWorking(offset, length, static_cast<const uint8_t*>(src));
    }

   private:
    struct Gen;
    struct Impl;

    class Pin final : public CardSnapshot {
       public:
        Pin(std::shared_ptr<Impl> impl, std::shared_ptr<Gen> gen)
            : impl(std::move(impl)), gen(std::move(gen)) {}
        uint64_t Size() const override { return impl->payloadSize; }
        uint64_t GenerationId() const override { return gen->id; }
        std::shared_ptr<Impl> impl;
        std::shared_ptr<Gen> gen;

       protected:
        bool ReadBytes(size_t offset, size_t length, void* dest) const override {
            return impl->readFrom(gen.get(), offset, length, dest);
        }
    };

    struct Gen {
        uint64_t id{};
        std::shared_ptr<Gen> parent;
        std::filesystem::path file;
        std::filesystem::path idxFile;
        int fd{-1};
        int idxFd{-1};
        uint64_t dataEnd{kHeaderBytes};
        uint64_t overlayCount{};
        uint64_t indexOffset{};
        uint64_t idxBase{};
        uint64_t idxCount{};
        bool frozen{};
        bool durable{};
        bool published{};
        bool ownedFile{};
        bool ownedIndex{};
        std::vector<std::pair<uint64_t, uint64_t>> memIndex;
        Impl* owner{};

        ~Gen() {
            // A name is only removed while it still points at the file this generation has open: after
            // a move, a new backing on the same durable file can own a generation of the same name.
            const bool fileSame = sameFile(fd, file);
            const bool idxSame = idxFd == fd ? fileSame : sameFile(idxFd, idxFile);
            if (owner && !durable) owner->closeGen(*this);
            fd = -1;
            idxFd = -1;
            if (ownedFile && !durable && !file.empty()) {
                std::error_code ec;
                if (fileSame) std::filesystem::remove(file, ec);
                if (ownedIndex && !idxFile.empty() && idxSame) std::filesystem::remove(idxFile, ec);
            }
        }

        // True when `path` names the file open on `fd`. Without a descriptor it can't tell, so the
        // name is left alone.
        static bool sameFile(int fd, const std::filesystem::path& path) {
            if (fd < 0) return false;
            struct stat open {}, named {};
            if (fstat(fd, &open) != 0 || stat(path.c_str(), &named) != 0) return false;
            return open.st_dev == named.st_dev && open.st_ino == named.st_ino;
        }
    };

    struct CacheEnt {
        uint64_t page{};
        std::vector<uint8_t> data;
        bool dirty{};
    };

    struct Impl : std::enable_shared_from_this<Impl> {
        std::filesystem::path durablePath;
        std::filesystem::path dir;
        std::string tag;

        std::string genPrefix() const { return ".gen-" + tag + "-"; }
        std::filesystem::path genPath(const std::string& rest) const { return dir / (genPrefix() + rest); }
        uint64_t payloadOffset{};
        uint64_t payloadSize{};
        uint32_t pageSize{4096};
        size_t cacheBytes{16u << 20};
        size_t indexLimit{4096};
        uint32_t maxGenerations{8};
        FileCardIoHooks* hooks{};
        uint64_t nextId{1};
        bool strictRoot{};
        bool boundRoot{};
        int durableFd{-1};
        uint64_t bindDev{}, bindIno{}, bindMtime{}, bindKey{};
        std::shared_ptr<Gen> root;
        std::shared_ptr<Gen> working;
        std::list<CacheEnt> lru;
        std::map<uint64_t, std::list<CacheEnt>::iterator> cache;
        size_t cacheUsed{};
        std::vector<uint32_t> chunkCrcs;
        uint32_t chunkBytes{1u << 20};
        // 1 once a chunk has matched its CRC. Pins may read from another thread.
        std::unique_ptr<std::atomic<uint8_t>[]> chunkOk;

        bool setChunkCrcs(std::vector<uint32_t> table) {
            if (table.empty()) {
                chunkCrcs.clear();
                chunkOk.reset();
                return true;
            }
            if (chunkBytes == 0 || chunkBytes % pageSize ||
                table.size() != (payloadSize + chunkBytes - 1) / chunkBytes)
                return false;
            auto ok = std::make_unique<std::atomic<uint8_t>[]>(table.size());
            for (size_t i = 0; i < table.size(); ++i) ok[i] = 0;
            chunkCrcs = std::move(table);
            chunkOk = std::move(ok);
            return true;
        }

        void trustChunk(uint64_t index) {
            if (chunkOk && index < chunkCrcs.size()) chunkOk[index].store(1, std::memory_order_release);
        }

        bool verifyChunks(uint64_t off, size_t n) {
            if (chunkCrcs.empty() || n == 0) return true;
            const uint64_t first = off / chunkBytes, last = (off + n - 1) / chunkBytes;
            for (uint64_t c = first; c <= last; ++c) {
                if (c >= chunkCrcs.size()) return false;
                if (chunkOk[c].load(std::memory_order_acquire)) continue;
                const uint64_t start = c * chunkBytes;
                const size_t len = static_cast<size_t>(std::min<uint64_t>(chunkBytes, payloadSize - start));
                std::vector<uint8_t> buf(len);
                if (!ioRead(durableFd, buf.data(), len, payloadOffset + start)) return false;
                if (mz_crc32(0, buf.data(), len) != chunkCrcs[c]) return false;
                chunkOk[c].store(1, std::memory_order_release);
            }
            return true;
        }

        // Reads durable payload bytes, checking their chunks first.
        bool readDurable(uint64_t off, size_t n, uint8_t* dest) {
            if (!verifyChunks(off, n)) return false;
            return ioRead(durableFd, dest, n, payloadOffset + off);
        }

        ~Impl() {
            dropCache();
            for (auto* g = root.get(); g; g = g->parent.get()) g->ownedFile = false;
            working.reset();
            root.reset();
            if (durableFd >= 0) {
                ::close(durableFd);
                durableFd = -1;
            }
        }

        static uint64_t uniqueSeq() {
            static std::atomic<uint64_t> n{1};
            return n++;
        }

        bool failRead() {
            if (!hooks) return false;
            ++hooks->reads;
            return hooks->failReadAfter >= 0 && hooks->reads > hooks->failReadAfter;
        }
        bool failWrite() {
            if (!hooks) return false;
            ++hooks->writes;
            return hooks->failWriteAfter >= 0 && hooks->writes > hooks->failWriteAfter;
        }
        bool failFlush() {
            if (!hooks) return false;
            ++hooks->flushes;
            return hooks->failFlushAfter >= 0 && hooks->flushes > hooks->failFlushAfter;
        }
        bool failClose() {
            if (!hooks) return false;
            ++hooks->closes;
            return hooks->failCloseAfter >= 0 && hooks->closes > hooks->failCloseAfter;
        }
        bool failRename() {
            if (!hooks) return false;
            ++hooks->renames;
            return hooks->failRenameAfter >= 0 && hooks->renames > hooks->failRenameAfter;
        }

        bool ioRead(int fd, void* buf, size_t n, uint64_t off) {
            if (failRead()) return false;
            auto* p = static_cast<uint8_t*>(buf);
            size_t got = 0;
            while (got < n) {
                const ssize_t r = pread(fd, p + got, n - got, static_cast<off_t>(off + got));
                if (r <= 0) return false;
                got += static_cast<size_t>(r);
            }
            if (hooks) hooks->bytesRead += n;
            return true;
        }

        bool ioWrite(int fd, const void* buf, size_t n, uint64_t off) {
            if (failWrite()) return false;
            auto* p = static_cast<const uint8_t*>(buf);
            size_t put = 0;
            while (put < n) {
                const ssize_t w = pwrite(fd, p + put, n - put, static_cast<off_t>(off + put));
                if (w <= 0) return false;
                put += static_cast<size_t>(w);
            }
            if (hooks) hooks->bytesWritten += n;
            return true;
        }

        bool ioFlush(int fd) {
            if (failFlush()) return false;
            return fsync(fd) == 0;
        }

        bool ioClose(int& fd) {
            if (fd < 0) return true;
            const int cur = fd;
            fd = -1;
            const int rc = ::close(cur);
            if (failClose()) return false;
            return rc == 0;
        }

        bool ioRename(const std::filesystem::path& from, const std::filesystem::path& to) {
            if (failRename()) return false;
            std::error_code ec;
            std::filesystem::rename(from, to, ec);
            return !ec;
        }

        bool installGeneration(const std::filesystem::path& from, const std::filesystem::path& to) {
            if (failRename()) return false;
            // Reserve the destination before rename, including on filesystems without hardlinks.
            const int fd = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0) return false;
            ::close(fd);
            std::error_code ec;
            std::filesystem::rename(from, to, ec);
            if (!ec) return true;
            std::filesystem::remove(to, ec);
            return false;
        }

        bool flushDir() {
            const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd < 0) return false;
            const int rc = fsync(dfd);
            ::close(dfd);
            return rc == 0;
        }

        std::filesystem::path tempName(const std::string& rest) const {
            return genPath(rest + std::to_string(uniqueSeq()) + "-" + std::to_string(::getpid()));
        }

        void closeGen(Gen& g) {
            if (g.idxFd >= 0 && g.idxFd != g.fd) {
                ::close(g.idxFd);
                g.idxFd = -1;
            }
            if (g.fd >= 0) {
                ::close(g.fd);
                g.fd = -1;
            }
        }

        std::vector<std::filesystem::path> ownedPaths() const {
            std::vector<std::filesystem::path> paths;
            paths.push_back(durablePath);
            const auto rootPtr = genPath("root");
            std::error_code ec;
            if (std::filesystem::exists(rootPtr, ec)) paths.push_back(rootPtr);
            for (auto* g = working.get(); g; g = g->parent.get()) {
                if (!g->durable && !g->file.empty()) paths.push_back(g->file);
                if (!g->idxFile.empty()) paths.push_back(g->idxFile);
            }
            return paths;
        }

        uint32_t liveCount() const {
            uint32_t n = 0;
            for (auto* g = working.get(); g; g = g->parent.get()) ++n;
            return n;
        }

        bool open(const std::filesystem::path& durable, uint64_t size, uint64_t off,
                  const FileCardOptions& options, FileCardIoHooks* io) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(durable, ec)) return false;
            const auto fileSize = std::filesystem::file_size(durable, ec);
            if (ec || fileSize < off) return false;
            if (size == 0) size = uint64_t(fileSize) - off;
            if (size > fileSize - off) return false;
            if (options.pageSize == 0) return false;
            durablePath = durable;
            dir = durable.parent_path();
            if (dir.empty()) dir = ".";
            tag = durable.filename().string();
            if (tag.empty()) return false;
            if (!options.keepHeld) SettleHeld(durable);
            payloadOffset = off;
            payloadSize = size;
            pageSize = options.pageSize;
            cacheBytes = std::max(options.cacheBytes, size_t(options.pageSize));
            indexLimit = std::max(options.indexCacheEntries, size_t(1));
            maxGenerations = std::max(options.maxGenerations, 1u);
            chunkBytes = options.chunkBytes;
            if (!setChunkCrcs(options.chunkCrcs)) return false;
            hooks = io;
            strictRoot = options.rejectInvalidRoot;
            durableFd = ::open(durable.c_str(), O_RDWR | O_CLOEXEC);
            if (durableFd < 0) return false;
            auto base = std::make_shared<Gen>();
            base->id = 0;
            base->file = durable;
            base->fd = durableFd;
            base->durable = true;
            base->published = true;
            base->frozen = true;
            base->owner = this;
            root = base;
            working = base;
            struct stat st {};
            if (fstat(durableFd, &st) != 0) return false;
            bindDev = uint64_t(st.st_dev);
            bindIno = uint64_t(st.st_ino);
            bindMtime = uint64_t(st.st_mtim.tv_sec) * 1000000000ull + uint64_t(st.st_mtim.tv_nsec);
            if (!bindingKey(options.identity, bindKey)) return false;
            // Strict providers retain every invalid namespace. Legacy providers discard
            // roots belonging to a replaced base file or card identity.
            if (!loadRoot()) {
                if (boundRoot && !options.rejectInvalidRoot) return false;
                if (options.rejectInvalidRoot) {
                    if (options.requireRoot || options.minimumRoot) return false;
                    // A new base has no namespace entries, including abandoned generations.
                    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                        if (it->path().filename().string().find(genPrefix()) == 0 &&
                            !NeighbourName(durablePath, it->path().filename().string(), true)) return false;
                    if (ec) return false;
                    if (!publishRoot(base)) return false;
                }
                root = base;
                working = base;
                if (!options.rejectInvalidRoot) {
                    // Only the explicit saved-base lifecycle may ignore rootless leftovers.
                    const bool missing = !std::filesystem::exists(genPath("root"), ec);
                    if (ec) return false;
                    const bool recover = missing && options.recoverRootlessBase &&
                                         !options.requireRoot && !options.minimumRoot;
                    if (missing && !recover) {
                        if (ec) return false;
                        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                            if (it->path().filename().string().find(genPrefix()) == 0 &&
                                !NeighbourName(durablePath, it->path().filename().string(), true)) return false;
                        if (ec) return false;
                    }
                    if (!recover) DiscardGenerations(durablePath);
                }
            }
            if (root->id < options.minimumRoot) return false;
            nextId = 1;
            for (auto* g = working.get(); g; g = g->parent.get()) {
                if (g->id >= nextId) nextId = g->id == UINT64_MAX ? UINT64_MAX : g->id + 1;
                if (!g->durable) g->ownedFile = true;
            }
            // Retained pending files and indexes reserve their IDs, including peers whose
            // names are ambiguous with this namespace. Never reuse or wrap those IDs.
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                auto name = it->path().filename().string();
                if (!GeneratedName(tag, name)) continue;
                auto id = name.substr(genPrefix().size());
                if (id.size() > 4 && id.substr(id.size() - 4) == ".idx") id.resize(id.size() - 4);
                if (id.empty() || id[0] < '1' || id[0] > '9') continue;
                uint64_t value = 0;
                for (char c : id) value = value * 10 + (c - '0');
                if (value >= nextId) nextId = value == UINT64_MAX ? UINT64_MAX : value + 1;
            }
            return !ec;
        }

        // FNV-1a of the card identity and the durable file's header bytes before the payload.
        bool bindingKey(const std::string& identity, uint64_t& key) {
            key = 1469598103934665603ull;
            auto mix = [&](const uint8_t* p, size_t n) {
                for (size_t i = 0; i < n; ++i) key = (key ^ p[i]) * 1099511628211ull;
            };
            mix(reinterpret_cast<const uint8_t*>(identity.data()), identity.size());
            const uint8_t sep = 0;
            mix(&sep, 1);
            uint8_t chunk[64];
            for (uint64_t off = 0; off < payloadOffset;) {
                const size_t n = size_t(std::min<uint64_t>(sizeof(chunk), payloadOffset - off));
                if (!ioRead(durableFd, chunk, n, off)) return false;
                mix(chunk, n);
                off += n;
            }
            return true;
        }

        bool loadRoot() {
            std::error_code ec;
            const auto rootPath = genPath("root");
            boundRoot = true;
            if (!std::filesystem::is_regular_file(rootPath, ec)) {
                std::error_code existsError;
                const auto status = std::filesystem::symlink_status(rootPath, existsError);
                if (status.type() == std::filesystem::file_type::not_found) boundRoot = false;
                return false;
            }
            uint8_t buf[kRootBytes]{};
            const int rfd = ::open(rootPath.c_str(), O_RDONLY | O_CLOEXEC);
            if (rfd < 0) return false;
            // The unbound 32-byte v1 pointer belongs to the legacy replacement
            // policy. Current v2 read/schema failures must retain their history.
            if (!strictRoot && std::filesystem::file_size(rootPath, ec) == 32 && !ec) {
                const bool legacy = ioRead(rfd, buf, 32, 0) && std::memcmp(buf, "FRONDRT\0", 8) == 0 &&
                                    frondSdFile::get32(buf + 8) == 1;
                ::close(rfd);
                if (legacy) boundRoot = false;
                return false;
            }
            const bool read = std::filesystem::file_size(rootPath, ec) == kRootBytes && !ec &&
                              ioRead(rfd, buf, kRootBytes, 0);
            ::close(rfd);
            if (!read || std::memcmp(buf, "FRONDRT\0", 8) != 0 || frondSdFile::get32(buf + 8) != 2) return false;
            if (frondSdFile::get64(buf + 20) != payloadSize || frondSdFile::get64(buf + 28) != bindDev ||
                frondSdFile::get64(buf + 36) != bindIno || frondSdFile::get64(buf + 44) != bindMtime ||
                frondSdFile::get64(buf + 52) != bindKey || frondSdFile::get32(buf + 60) != pageSize) {
                boundRoot = false;
                return false;
            }
            boundRoot = true;
            const uint64_t id = frondSdFile::get64(buf + 12);
            if (id == 0) return true;
            auto loaded = loadChain(id);
            if (!loaded) return false;
            root = loaded;
            working = loaded;
            return true;
        }

        uint64_t recordBudget() const {
            const uint64_t pages = payloadSize / pageSize + (payloadSize % pageSize != 0);
            return std::min<uint64_t>(pages, strictRoot ? (2048ull << 20) / 4096 : UINT64_MAX / 16) * 16;
        }

        bool reloadable(const Gen* g) const {
            uint64_t records = 0, depth = 0;
            for (; g && !g->durable; g = g->parent.get()) {
                if (++depth > 256 || g->overlayCount > recordBudget() - records ||
                    (strictRoot && g->overlayCount > (2048ull << 20) / 4096)) return false;
                records += g->overlayCount;
            }
            return true;
        }

        std::shared_ptr<Gen> loadChain(uint64_t id) {
            if (id == 0) return root;
            std::shared_ptr<Gen> child;
            std::shared_ptr<Gen> latest;
            std::set<uint64_t> seen;
            // Normal commits compact at eight generations. Allow pinned histories up to
            // 256 files, with at most sixteen full-card indexes of validation work.
            uint64_t records = 0;
            const uint64_t budget = recordBudget();
            while (id != 0) {
                if (seen.size() >= 256 || !seen.insert(id).second || id == UINT64_MAX) return nullptr;
                auto g = loadFrozen(id);
                if (!g) return nullptr;
                if (g->overlayCount > budget - records) return nullptr;
                records += g->overlayCount;
                if (!latest) latest = g;
                if (child) child->parent = g;
                child = g;
                uint8_t header[kHeaderBytes];
                if (!ioRead(g->fd, header, kHeaderBytes, 0)) return nullptr;
                const uint64_t parentId = frondSdFile::get64(header + 32);
                if (parentId >= id) return nullptr;
                if (parentId == 0) {
                    g->parent = root;
                    break;
                }
                id = parentId;
            }
            return latest;
        }

        std::shared_ptr<Gen> loadFrozen(uint64_t id) {
            auto g = std::make_shared<Gen>();
            g->id = id;
            g->file = genPath(std::to_string(id));
            g->fd = ::open(g->file.c_str(), O_RDWR | O_CLOEXEC);
            if (g->fd < 0) return nullptr;
            g->owner = this;
            uint8_t header[kHeaderBytes];
            if (!ioRead(g->fd, header, kHeaderBytes, 0)) return nullptr;
            if (std::memcmp(header, "FRONDGEN", 8) != 0) return nullptr;
            const uint32_t ver = frondSdFile::get32(header + 8);
            const uint32_t ps = frondSdFile::get32(header + 12);
            const uint64_t payload = frondSdFile::get64(header + 16);
            const uint64_t gid = frondSdFile::get64(header + 24);
            if (ver != 1 || ps != pageSize || payload != payloadSize || gid != id) return nullptr;
            g->overlayCount = frondSdFile::get64(header + 40);
            g->indexOffset = frondSdFile::get64(header + 48);
            struct stat st{};
            const uint64_t pages = payloadSize / pageSize + (payloadSize % pageSize != 0);
            if (fstat(g->fd, &st) || !S_ISREG(st.st_mode) || st.st_size < kHeaderBytes ||
                g->overlayCount > pages || (strictRoot && g->overlayCount > (2048ull << 20) / 4096) || g->indexOffset < kHeaderBytes ||
                g->indexOffset > uint64_t(st.st_size) ||
                g->overlayCount > (uint64_t(st.st_size) - g->indexOffset) / kIndexRecBytes ||
                g->indexOffset + g->overlayCount * kIndexRecBytes != uint64_t(st.st_size) ||
                (g->indexOffset - kHeaderBytes) % pageSize || (frondSdFile::get64(header + 56) != 0 && frondSdFile::get64(header + 56) != kHeaderBytes)) return nullptr;
            uint64_t previous = 0;
            for (uint64_t i = 0; i < g->overlayCount; ++i) {
                uint8_t rec[kIndexRecBytes];
                if (!ioRead(g->fd, rec, sizeof(rec), g->indexOffset + i * kIndexRecBytes)) return nullptr;
                const auto page = frondSdFile::get64(rec), offset = frondSdFile::get64(rec + 8);
                if (page >= pages || (i && page <= previous)) return nullptr;
                previous = page;
                if (offset != kTomb && (offset < kHeaderBytes || offset > g->indexOffset ||
                    g->indexOffset - offset < pageSize || (offset - kHeaderBytes) % pageSize)) return nullptr;
            }
            g->idxFd = g->fd;
            g->idxBase = g->indexOffset;
            g->idxCount = g->overlayCount;
            g->frozen = true;
            g->published = true;
            g->dataEnd = g->indexOffset ? g->indexOffset : kHeaderBytes;
            return g;
        }

        bool ensureWorking() {
            if (working != root && working.use_count() == 1 && !working->frozen) return true;
            auto child = std::make_shared<Gen>();
            if (nextId == UINT64_MAX) return false;
            child->id = nextId++;
            child->parent = working;
            child->owner = this;
            child->file = genPath(std::to_string(child->id));
            child->idxFile = genPath(std::to_string(child->id) + ".idx");
            const auto tmp = tempName("tmp-");
            const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0) return false;
            uint8_t header[kHeaderBytes]{};
            std::memcpy(header, "FRONDGEN", 8);
            frondSdFile::put32(header + 8, 1);
            frondSdFile::put32(header + 12, pageSize);
            frondSdFile::put64(header + 16, payloadSize);
            frondSdFile::put64(header + 24, child->id);
            frondSdFile::put64(header + 32, working->id);
            frondSdFile::put64(header + 56, kHeaderBytes);
            if (!ioWrite(fd, header, kHeaderBytes, 0) || !ioFlush(fd)) {
                ::close(fd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            if (!installGeneration(tmp, child->file)) {
                ::close(fd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            child->ownedFile = true;
            if (!flushDir()) {
                ::close(fd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                std::filesystem::remove(child->file, ec);
                return false;
            }
            child->fd = fd;
            child->dataEnd = kHeaderBytes;
            working = child;
            return true;
        }

        enum class Probe { miss, hit, error };

        Probe lookup(const Gen* g, uint64_t page, uint64_t& offset, bool& tomb) const {
            const auto it =
                std::lower_bound(g->memIndex.begin(), g->memIndex.end(), page,
                                 [](const std::pair<uint64_t, uint64_t>& e, uint64_t p) { return e.first < p; });
            if (it != g->memIndex.end() && it->first == page) {
                offset = it->second;
                tomb = offset == kTomb;
                return Probe::hit;
            }
            if (g->idxCount == 0 || g->idxFd < 0) return Probe::miss;
            uint64_t lo = 0, hi = g->idxCount;
            while (lo < hi) {
                const uint64_t mid = lo + (hi - lo) / 2;
                uint8_t rec[kIndexRecBytes];
                if (!const_cast<Impl*>(this)->ioRead(g->idxFd, rec, kIndexRecBytes,
                                                     g->idxBase + mid * kIndexRecBytes))
                    return Probe::error;
                const uint64_t recPage = frondSdFile::get64(rec);
                const uint64_t recOff = frondSdFile::get64(rec + 8);
                if (recPage == page) {
                    offset = recOff;
                    tomb = recOff == kTomb;
                    return Probe::hit;
                }
                if (recPage < page) lo = mid + 1;
                else hi = mid;
            }
            return Probe::miss;
        }

        bool readPageFromGen(const Gen* g, uint64_t page, uint8_t* dest) const {
            for (const Gen* cur = g; cur; cur = cur->parent.get()) {
                if (cur->durable) {
                    const uint64_t off = page * uint64_t(pageSize);
                    if (off >= payloadSize) return false;
                    const size_t n = static_cast<size_t>(std::min(uint64_t(pageSize), payloadSize - off));
                    if (!const_cast<Impl*>(this)->readDurable(off, n, dest)) return false;
                    if (n < pageSize) std::memset(dest + n, 0, pageSize - n);
                    return true;
                }
                uint64_t offset = 0;
                bool tomb = false;
                const auto found = lookup(cur, page, offset, tomb);
                if (found == Probe::error) return false;
                if (found == Probe::miss) continue;
                if (tomb) {
                    const uint64_t off = page * uint64_t(pageSize);
                    const size_t n = static_cast<size_t>(std::min(uint64_t(pageSize), payloadSize - off));
                    if (!const_cast<Impl*>(this)->readDurable(off, n, dest)) return false;
                    if (n < pageSize) std::memset(dest + n, 0, pageSize - n);
                    return true;
                }
                if (!const_cast<Impl*>(this)->ioRead(cur->fd, dest, pageSize, offset)) return false;
                return true;
            }
            return false;
        }

        bool cacheGet(uint64_t page, uint8_t* dest, bool* dirty) {
            const auto it = cache.find(page);
            if (it == cache.end()) return false;
            lru.splice(lru.begin(), lru, it->second);
            std::memcpy(dest, it->second->data.data(), pageSize);
            if (dirty) *dirty = it->second->dirty;
            return true;
        }

        bool cachePut(uint64_t page, const uint8_t* data, bool dirty) {
            const auto it = cache.find(page);
            if (it != cache.end()) {
                lru.splice(lru.begin(), lru, it->second);
                std::memcpy(it->second->data.data(), data, pageSize);
                it->second->dirty = dirty;
                return true;
            }
            while (cacheUsed + pageSize > cacheBytes && !lru.empty()) {
                auto last = std::prev(lru.end());
                if (last->dirty && !flushPage(last->page, last->data.data())) return false;
                cacheUsed -= last->data.size();
                cache.erase(last->page);
                lru.erase(last);
            }
            CacheEnt ent;
            ent.page = page;
            ent.data.assign(data, data + pageSize);
            ent.dirty = dirty;
            lru.push_front(std::move(ent));
            cache[page] = lru.begin();
            cacheUsed += pageSize;
            return true;
        }

        void dropCache() {
            lru.clear();
            cache.clear();
            cacheUsed = 0;
        }

        bool spillIndex(Gen* g) {
            if (g->memIndex.empty()) return true;
            const auto tmp = tempName("tmp-idx-");
            const int tfd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (tfd < 0) return false;
            uint8_t countBuf[8]{};
            if (!ioWrite(tfd, countBuf, 8, 0)) {
                ::close(tfd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            size_t mi = 0;
            uint64_t di = 0;
            uint64_t out = 0;
            auto writeRec = [&](uint64_t page, uint64_t off) {
                uint8_t rec[kIndexRecBytes];
                frondSdFile::put64(rec, page);
                frondSdFile::put64(rec + 8, off);
                if (!ioWrite(tfd, rec, kIndexRecBytes, 8 + out * kIndexRecBytes)) return false;
                ++out;
                return true;
            };
            uint8_t drec[kIndexRecBytes];
            bool haveDisk = false;
            uint64_t dPage = 0, dOff = 0;
            auto nextDisk = [&]() {
                haveDisk = false;
                if (g->idxFd < 0 || di >= g->idxCount) return true;
                if (!ioRead(g->idxFd, drec, kIndexRecBytes, g->idxBase + di * kIndexRecBytes)) return false;
                dPage = frondSdFile::get64(drec);
                dOff = frondSdFile::get64(drec + 8);
                haveDisk = true;
                ++di;
                return true;
            };
            if (!nextDisk()) {
                ::close(tfd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            while (mi < g->memIndex.size() || haveDisk) {
                if (mi < g->memIndex.size() && (!haveDisk || g->memIndex[mi].first < dPage)) {
                    if (!writeRec(g->memIndex[mi].first, g->memIndex[mi].second)) {
                        ::close(tfd);
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                        return false;
                    }
                    ++mi;
                } else if (haveDisk && (mi >= g->memIndex.size() || dPage < g->memIndex[mi].first)) {
                    if (!writeRec(dPage, dOff)) {
                        ::close(tfd);
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                        return false;
                    }
                    if (!nextDisk()) {
                        ::close(tfd);
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                        return false;
                    }
                } else {
                    if (!writeRec(g->memIndex[mi].first, g->memIndex[mi].second)) {
                        ::close(tfd);
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                        return false;
                    }
                    ++mi;
                    if (!nextDisk()) {
                        ::close(tfd);
                        std::error_code ec;
                        std::filesystem::remove(tmp, ec);
                        return false;
                    }
                }
            }
            frondSdFile::put64(countBuf, out);
            if (!ioWrite(tfd, countBuf, 8, 0) || !ioFlush(tfd)) {
                ::close(tfd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            if (g->idxFd >= 0 && g->idxFd != g->fd) {
                ::close(g->idxFd);
                g->idxFd = -1;
            }
            if (!(g->ownedIndex ? ioRename(tmp, g->idxFile) : installGeneration(tmp, g->idxFile))) {
                ::close(tfd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            g->ownedIndex = true;
            g->idxFd = tfd;
            g->idxBase = 8;
            g->idxCount = out;
            g->memIndex.clear();
            return true;
        }

        bool remember(Gen* g, uint64_t page, uint64_t offset) {
            auto it = std::lower_bound(g->memIndex.begin(), g->memIndex.end(), page,
                                       [](const std::pair<uint64_t, uint64_t>& e, uint64_t p) { return e.first < p; });
            if (it != g->memIndex.end() && it->first == page) it->second = offset;
            else g->memIndex.insert(it, {page, offset});
            if (g->memIndex.size() >= indexLimit) return spillIndex(g);
            return true;
        }

        bool flushPage(uint64_t page, const uint8_t* data) {
            if (!ensureWorking()) return false;
            Gen* g = working.get();
            std::vector<uint8_t> basePage(pageSize);
            const uint64_t off = page * uint64_t(pageSize);
            const size_t n = static_cast<size_t>(std::min(uint64_t(pageSize), payloadSize - off));
            if (!readDurable(off, n, basePage.data())) return false;
            if (n < pageSize) std::memset(basePage.data() + n, 0, pageSize - n);
            const bool matchesBase = std::memcmp(data, basePage.data(), pageSize) == 0;
            bool parentHas = false;
            if (g->parent && !g->parent->durable) {
                uint64_t o = 0;
                bool tomb = false;
                for (auto* p = g->parent.get(); p && !p->durable; p = p->parent.get()) {
                    const auto found = lookup(p, page, o, tomb);
                    if (found == Probe::error) return false;
                    if (found == Probe::hit) {
                        parentHas = true;
                        break;
                    }
                }
            }
            if (matchesBase && !parentHas) return remember(g, page, kTomb);
            const uint64_t at = g->dataEnd;
            if (!ioWrite(g->fd, data, pageSize, at)) return false;
            g->dataEnd += pageSize;
            return remember(g, page, at);
        }

        bool flushWorking() {
            for (auto& ent : lru) {
                if (!ent.dirty) continue;
                if (!flushPage(ent.page, ent.data.data())) return false;
                ent.dirty = false;
            }
            if (working && working != root && !working->memIndex.empty()) {
                if (!spillIndex(working.get())) return false;
            }
            if (working && working->fd >= 0 && working != root && !working->durable) {
                if (!ioFlush(working->fd)) return false;
                if (working->idxFd >= 0 && working->idxFd != working->fd && !ioFlush(working->idxFd)) return false;
            }
            return true;
        }

        bool freeze(Gen* g) {
            if (g->frozen) return true;
            if (!spillIndex(g)) return false;
            g->indexOffset = g->dataEnd;
            g->overlayCount = g->idxCount;
            for (uint64_t i = 0; i < g->idxCount; ++i) {
                uint8_t rec[kIndexRecBytes];
                if (!ioRead(g->idxFd, rec, kIndexRecBytes, g->idxBase + i * kIndexRecBytes)) return false;
                if (!ioWrite(g->fd, rec, kIndexRecBytes, g->indexOffset + i * kIndexRecBytes)) return false;
            }
            uint8_t header[kHeaderBytes]{};
            if (!ioRead(g->fd, header, kHeaderBytes, 0)) return false;
            frondSdFile::put64(header + 40, g->overlayCount);
            frondSdFile::put64(header + 48, g->indexOffset);
            if (!ioWrite(g->fd, header, kHeaderBytes, 0) || !ioFlush(g->fd)) return false;
            g->idxFd = g->fd;
            g->idxBase = g->indexOffset;
            g->frozen = true;
            return true;
        }

        bool publishRoot(const std::shared_ptr<Gen>& g) {
            uint8_t buf[kRootBytes]{};
            std::memcpy(buf, "FRONDRT\0", 8);
            frondSdFile::put32(buf + 8, 2);
            frondSdFile::put64(buf + 12, g->id);
            frondSdFile::put64(buf + 20, payloadSize);
            frondSdFile::put64(buf + 28, bindDev);
            frondSdFile::put64(buf + 36, bindIno);
            frondSdFile::put64(buf + 44, bindMtime);
            frondSdFile::put64(buf + 52, bindKey);
            frondSdFile::put32(buf + 60, pageSize);
            const auto tmp = tempName("tmp-root-");
            const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0) return false;
            if (!ioWrite(fd, buf, kRootBytes, 0) || !ioFlush(fd)) {
                ::close(fd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            int published = fd;
            if (!ioClose(published)) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            const auto dest = genPath("root");
            if (!ioRename(tmp, dest)) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            g->published = true;
            if (!flushDir()) {
                g->ownedFile = false;
                return false;
            }
            return true;
        }

        bool readFrom(const Gen* g, size_t offset, size_t length, void* dest) const {
            auto* out = static_cast<uint8_t*>(dest);
            while (length) {
                const uint64_t page = uint64_t(offset) / pageSize;
                const size_t skip = offset % pageSize;
                const size_t n = std::min(length, size_t(pageSize) - skip);
                std::vector<uint8_t> buf(pageSize);
                if (!readPageFromGen(g, page, buf.data())) return false;
                std::memcpy(out, buf.data() + skip, n);
                offset += n;
                out += n;
                length -= n;
            }
            return true;
        }

        bool readWorking(size_t offset, size_t length, void* dest) {
            auto* out = static_cast<uint8_t*>(dest);
            while (length) {
                const uint64_t page = uint64_t(offset) / pageSize;
                const size_t skip = offset % pageSize;
                const size_t n = std::min(length, size_t(pageSize) - skip);
                std::vector<uint8_t> buf(pageSize);
                if (!cacheGet(page, buf.data(), nullptr) && !readPageFromGen(working.get(), page, buf.data()))
                    return false;
                if (!cacheGet(page, buf.data(), nullptr) && !cachePut(page, buf.data(), false)) return false;
                std::memcpy(out, buf.data() + skip, n);
                offset += n;
                out += n;
                length -= n;
            }
            return true;
        }

        bool writeWorking(size_t offset, size_t length, const uint8_t* src) {
            if (!ensureWorking()) return false;
            while (length) {
                const uint64_t page = uint64_t(offset) / pageSize;
                const size_t skip = offset % pageSize;
                const size_t n = std::min(length, size_t(pageSize) - skip);
                std::vector<uint8_t> buf(pageSize);
                if (n != pageSize || skip != 0) {
                    if (!cacheGet(page, buf.data(), nullptr) && !readPageFromGen(working.get(), page, buf.data()))
                        return false;
                }
                std::memcpy(buf.data() + skip, src, n);
                if (!cachePut(page, buf.data(), true)) return false;
                offset += n;
                src += n;
                length -= n;
            }
            return true;
        }

        bool loadAllIndex(const Gen* g, std::vector<std::pair<uint64_t, uint64_t>>& out) const {
            out = g->memIndex;
            for (uint64_t i = 0; i < g->idxCount; ++i) {
                uint8_t rec[kIndexRecBytes];
                if (!const_cast<Impl*>(this)->ioRead(g->idxFd, rec, kIndexRecBytes,
                                                     g->idxBase + i * kIndexRecBytes))
                    return false;
                const uint64_t page = frondSdFile::get64(rec);
                const uint64_t off = frondSdFile::get64(rec + 8);
                auto it = std::lower_bound(out.begin(), out.end(), page,
                                           [](const std::pair<uint64_t, uint64_t>& e, uint64_t p) {
                                               return e.first < p;
                                           });
                if (it != out.end() && it->first == page) continue;
                out.insert(it, {page, off});
            }
            return true;
        }

        bool coalescedOverlay(const Gen* g, std::vector<std::pair<uint64_t, uint64_t>>& pages) const {
            std::vector<const Gen*> chain;
            for (const Gen* cur = g; cur; cur = cur->parent.get()) {
                if (cur->durable) break;
                chain.push_back(cur);
            }
            std::reverse(chain.begin(), chain.end());
            pages.clear();
            for (const Gen* cur : chain) {
                std::vector<std::pair<uint64_t, uint64_t>> idx;
                if (!loadAllIndex(cur, idx)) return false;
                for (auto& rec : idx) {
                    auto it = std::lower_bound(pages.begin(), pages.end(), rec.first,
                                               [](const std::pair<uint64_t, uint64_t>& e, uint64_t p) {
                                                   return e.first < p;
                                               });
                    if (it != pages.end() && it->first == rec.first) *it = rec;
                    else pages.insert(it, rec);
                }
            }
            return true;
        }

        bool forEachOverlay(const Gen* gen, const OverlayFn& fn) const {
            std::vector<std::pair<uint64_t, uint64_t>> pages;
            if (!coalescedOverlay(gen, pages)) return false;
            std::vector<uint8_t> buf(pageSize);
            for (auto& rec : pages) {
                if (rec.second == kTomb) continue;
                if (!readPageFromGen(gen, rec.first, buf.data())) return false;
                const uint64_t off = rec.first * uint64_t(pageSize);
                const uint64_t n = std::min(uint64_t(pageSize), payloadSize - off);
                if (!fn(off, n, buf.data())) return false;
            }
            return true;
        }

        bool compact() {
            if (hooks && hooks->failCompact) return false;
            if (!flushWorking()) return false;
            if (working != root && !freeze(working.get())) return false;
            std::vector<std::pair<uint64_t, uint64_t>> pages;
            if (!coalescedOverlay(working.get(), pages)) return false;
            auto child = std::make_shared<Gen>();
            if (nextId == UINT64_MAX) return false;
            child->id = nextId++;
            child->parent = working;
            while (child->parent && !child->parent->durable) child->parent = child->parent->parent;
            if (!child->parent) return false;
            child->owner = this;
            child->file = genPath(std::to_string(child->id));
            const auto tmp = tempName("tmp-");
            const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0) return false;
            uint8_t header[kHeaderBytes]{};
            std::memcpy(header, "FRONDGEN", 8);
            frondSdFile::put32(header + 8, 1);
            frondSdFile::put32(header + 12, pageSize);
            frondSdFile::put64(header + 16, payloadSize);
            frondSdFile::put64(header + 24, child->id);
            if (!ioWrite(fd, header, kHeaderBytes, 0)) {
                ::close(fd);
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            child->fd = fd;
            child->dataEnd = kHeaderBytes;
            uint64_t overlay = 0;
            std::vector<uint8_t> buf(pageSize);
            for (auto& rec : pages) {
                if (rec.second == kTomb) continue;
                if (!readPageFromGen(working.get(), rec.first, buf.data())) {
                    std::error_code ec;
                    std::filesystem::remove(tmp, ec);
                    return false;
                }
                if (!ioWrite(fd, buf.data(), pageSize, child->dataEnd)) {
                    std::error_code ec;
                    std::filesystem::remove(tmp, ec);
                    return false;
                }
                rec.second = child->dataEnd;
                child->dataEnd += pageSize;
                ++overlay;
            }
            child->indexOffset = child->dataEnd;
            child->overlayCount = overlay;
            uint64_t slot = 0;
            for (auto& rec : pages) {
                if (rec.second == kTomb) continue;
                uint8_t irec[kIndexRecBytes];
                frondSdFile::put64(irec, rec.first);
                frondSdFile::put64(irec + 8, rec.second);
                if (!ioWrite(fd, irec, kIndexRecBytes, child->indexOffset + slot * kIndexRecBytes)) {
                    std::error_code ec;
                    std::filesystem::remove(tmp, ec);
                    return false;
                }
                ++slot;
            }
            frondSdFile::put64(header + 40, child->overlayCount);
            frondSdFile::put64(header + 48, child->indexOffset);
            if (!ioWrite(fd, header, kHeaderBytes, 0) || !ioFlush(fd)) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            if (!installGeneration(tmp, child->file)) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            child->ownedFile = true;
            if (!flushDir()) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                std::filesystem::remove(child->file, ec);
                return false;
            }
            child->idxFd = fd;
            child->idxBase = child->indexOffset;
            child->idxCount = child->overlayCount;
            child->frozen = true;
            if (!reloadable(child.get())) return false;
            if (!publishRoot(child)) {
                std::error_code ec;
                if (!child->published) std::filesystem::remove(child->file, ec);
                return false;
            }
            dropCache();
            working = child;
            root = std::move(child);
            return true;
        }
    };

    explicit FileCardBacking(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

#endif
