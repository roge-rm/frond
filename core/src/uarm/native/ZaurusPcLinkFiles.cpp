#ifndef _WIN32
#include "ZaurusPcLinkFiles.h"

#include <algorithm>
#include <array>
#include <iconv.h>
#include <random>
#include <stdexcept>
#include <string_view>

namespace zaurusPcLink {
namespace {

using Bytes = std::vector<uint8_t>;

// SMB1 (MS-CIFS) over the NetBIOS session service (RFC 1002).
constexpr uint8_t smbClose = 0x04, smbDelete = 0x06, smbRename = 0x07,
                  smbReadAndX = 0x2e, smbWriteAndX = 0x2f, smbTransaction2 = 0x32,
                  smbNegotiate = 0x72, smbSessionSetup = 0x73, smbTreeConnect = 0x75,
                  smbNtCreateAndX = 0xa2;
constexpr uint16_t maxBuffer = 16384, directoryInfo = 0x0101, closeAtEnd = 0x0002,
                   continueFromLast = 0x0008, searchAttributes = 0x0016;
constexpr uint32_t capLargeFiles = 0x0008, capNtSmbs = 0x0010;
constexpr size_t headerSize = 32;
constexpr uint16_t ioSize = 8192;
constexpr std::string_view disk = "Internal_Disk";

[[noreturn]] void InvalidReply() { throw std::runtime_error("Invalid reply from Zaurus PC Link."); }

struct View {
    const uint8_t* bytes;
    size_t size;

    View Slice(size_t offset, size_t length) const {
        if (offset > size || length > size - offset) InvalidReply();
        return {bytes + offset, length};
    }
    uint8_t U8(size_t offset) const { return *Slice(offset, 1).bytes; }
    uint16_t U16(size_t offset) const {
        auto value = Slice(offset, 2).bytes;
        return uint16_t(value[0]) | uint16_t(value[1]) << 8;
    }
    uint32_t U32(size_t offset) const {
        return uint32_t(U16(offset)) | uint32_t(U16(offset + 2)) << 16;
    }
    uint64_t U64(size_t offset) const {
        return uint64_t(U32(offset)) | uint64_t(U32(offset + 4)) << 32;
    }
};

View ViewOf(const Bytes& bytes) { return {bytes.data(), bytes.size()}; }

void Put16(Bytes& dst, uint16_t value) {
    dst.push_back(uint8_t(value));
    dst.push_back(uint8_t(value >> 8));
}
void Put32(Bytes& dst, uint32_t value) {
    Put16(dst, uint16_t(value));
    Put16(dst, uint16_t(value >> 16));
}
void Put64(Bytes& dst, uint64_t value) {
    Put32(dst, uint32_t(value));
    Put32(dst, uint32_t(value >> 32));
}
void PutString(Bytes& dst, std::string_view value) {
    dst.insert(dst.end(), value.begin(), value.end());
    dst.push_back(0);
}

Bytes AndX() { return {0xff, 0, 0, 0}; }

std::string CodePage(std::string_view value, bool fromGuest) {
    if (std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte < 0x80; }))
        return std::string(value);
    std::string encoded(4 * value.size(), '\0');
    auto converter = iconv_open(fromGuest ? "UTF-8" : "CP932", fromGuest ? "CP932" : "UTF-8");
    if (converter == (iconv_t)-1) throw std::runtime_error("Couldn't open the Zaurus filename encoding.");
    // iconv advances the pointer but leaves the source bytes alone.
    char* input = const_cast<char*>(value.data());
    char* output = encoded.data();
    size_t inputLeft = value.size(), outputLeft = encoded.size();
    const size_t result = iconv(converter, &input, &inputLeft, &output, &outputLeft);
    iconv_close(converter);
    if (result == size_t(-1) || inputLeft)
        throw std::runtime_error("The Zaurus can't use this file name.");
    encoded.resize(encoded.size() - outputLeft);
    return encoded;
}

bool ValidName(std::string_view name) {
    return !name.empty() && name.back() != '.' && name.back() != ' ' &&
           std::none_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 32 || c == 127 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos;
    });
}

std::string DiskPath(std::string_view relative, bool directory) {
    std::string path = "\\" + std::string(disk);
    if (relative.empty() && directory) return path;
    if (relative.size() > 1024) throw std::runtime_error("The Zaurus path is too long.");
    for (;;) {
        const auto slash = relative.find('/');
        const auto part = relative.substr(0, slash);
        if (!ValidName(part)) throw std::runtime_error("The Zaurus can't use this file name.");
        auto encoded = CodePage(part, false);
        // CP932 can encode a yen sign as the path separator.
        if (!ValidName(CodePage(encoded, true))) throw std::runtime_error("The Zaurus can't use this file name.");
        if (encoded.size() > 255) throw std::runtime_error("The Zaurus file name is too long.");
        path += '\\';
        path += encoded;
        if (slash == std::string_view::npos) return path;
        relative.remove_prefix(slash + 1);
    }
}

void PutNetbiosName(Bytes& bytes, std::string_view name, uint8_t suffix) {
    bytes.push_back(32);
    for (size_t i = 0; i != 16; ++i) {
        const uint8_t value = i == 15 ? suffix : i < name.size() ? uint8_t(name[i]) : ' ';
        bytes.push_back('A' + (value >> 4));
        bytes.push_back('A' + (value & 15));
    }
    bytes.push_back(0);
}

struct Frame {
    uint8_t type;
    Bytes bytes;
};

class SmbError : public std::runtime_error {
   public:
    SmbError(uint8_t command, uint32_t status, bool nt)
        : std::runtime_error(Describe(command, status, nt)), status(status), nt(nt) {}

    bool EmptySearch() const {
        return nt ? status == 0x80000006 || status == 0xc000000f
                  : (status & 255) == 1 && (status >> 16 == 2 || status >> 16 == 18);
    }

   private:
    static std::string Describe(uint8_t command, uint32_t status, bool nt) {
        const auto dosClass = status & 255;
        const auto dosCode = status >> 16;
        if (nt ? status == 0xc0000035 : dosClass == 1 && (dosCode == 80 || dosCode == 183))
            return "A file with this name already exists on the Zaurus.";
        if (nt ? status == 0xc000007f : (dosClass == 3 && dosCode == 39) || (dosClass == 1 && dosCode == 112))
            return "The Zaurus disk is full.";
        if (nt ? status == 0xc0000043 : dosClass == 1 && dosCode == 32)
            return "The Zaurus file is in use.";
        if (nt ? status == 0xc0000022 : dosClass == 1 && dosCode == 5)
            return "The Zaurus denied access to the file or folder.";
        if (nt ? status == 0xc0000034 || status == 0xc000003a : dosClass == 1 && (dosCode == 2 || dosCode == 3))
            return "The Zaurus file or folder could not be found.";
        char detail[64];
        std::snprintf(detail, sizeof(detail), "Zaurus PC Link error %02x/%08x.", unsigned(command), unsigned(status));
        return detail;
    }

    uint32_t status;
    bool nt;
};

struct Reply {
    Bytes bytes;

    View Header() const { return ViewOf(bytes); }
    View Words() const {
        auto message = Header();
        return message.Slice(headerSize + 1, message.U8(headerSize) * 2);
    }
    View DataAt(size_t offset, size_t length) const {
        auto message = Header();
        const auto position = headerSize + 1 + Words().size;
        const auto first = position + 2;
        auto data = message.Slice(first, message.U16(position));
        if (offset < first) InvalidReply();
        return data.Slice(offset - first, length);
    }
};

struct OpenFile {
    uint16_t id;
    uint64_t size;
};

struct Transaction {
    Bytes parameters;
    Bytes data;
};

}  // namespace

struct Client::Impl {
    explicit Impl(Stream& stream) : stream(stream) {
        Bytes names;
        PutNetbiosName(names, "ZAURUS", 0x20);
        PutNetbiosName(names, "FROND", 0);
        stream.CheckCancelled();
        SendFrame(0x81, names);
        const auto session = ReadFrame();
        if (session.type != 0x82 || !session.bytes.empty())
            throw std::runtime_error("The Zaurus rejected the PC Link connection.");

        Bytes dialect{0x02};
        PutString(dialect, "NT LM 0.12");
        const auto negotiated = Exchange(smbNegotiate, {}, dialect);
        const auto words = negotiated.Words();
        if (words.U16(0) != 0 || words.size != 34)
            throw std::runtime_error("The Zaurus did not offer the PC Link file protocol.");
        const auto serverBuffer = words.U32(7);
        if (serverBuffer < 1024) InvalidReply();
        bufferSize = uint16_t(std::min<uint32_t>(serverBuffer, maxBuffer));
        capabilities = words.U32(19) & (capLargeFiles | capNtSmbs);
        if (!(capabilities & capNtSmbs))
            throw std::runtime_error("The Zaurus did not offer the PC Link file protocol.");
        OpenSession(words.U32(15));
    }

    void OpenSession(uint32_t key) {
        Bytes words = AndX();
        Put16(words, maxBuffer);
        Put16(words, 1);  // One in-flight request.
        Put16(words, 0);  // First virtual circuit.
        Put32(words, key);
        Put16(words, 0);  // Anonymous session, both password lengths are zero.
        Put16(words, 0);
        Put32(words, 0);
        Put32(words, capabilities);
        Bytes data{0, 0};  // Account and domain.
        PutString(data, "Frond");
        PutString(data, "Frond");
        uid = Exchange(smbSessionSetup, words, data).Header().U16(28);

        words = AndX();
        Put16(words, 0);  // Connect an additional tree.
        Put16(words, 1);  // Empty share password.
        data = {0};
        PutString(data, "\\\\ZAURUS\\home");
        PutString(data, "?????");
        tid = 0xffff;
        tid = Exchange(smbTreeConnect, words, data).Header().U16(24);
    }

    std::vector<Entry> List(const std::string& remote) {
        const auto path = DiskPath(remote, true);
        std::vector<Entry> entries;
        uint16_t searchId = 0;
        bool first = true;
        for (unsigned page = 0; page < 1024; ++page) {
            Bytes parameters;
            if (first) {
                Put16(parameters, searchAttributes);
                Put16(parameters, 256);
                Put16(parameters, closeAtEnd);
                Put16(parameters, directoryInfo);
                Put32(parameters, 0);
                PutString(parameters, path + "\\*");
            } else {
                Put16(parameters, searchId);
                Put16(parameters, 256);
                Put16(parameters, directoryInfo);
                Put32(parameters, 0);  // Continue the established search cursor.
                Put16(parameters, closeAtEnd | continueFromLast);
                PutString(parameters, "");
            }

            Transaction reply;
            try {
                reply = Transact(first ? 1 : 2, parameters);
            } catch (const SmbError& error) {
                if (error.EmptySearch()) return entries;
                throw;
            }
            auto response = ViewOf(reply.parameters);
            if (first) {
                searchId = response.U16(0);
                response = response.Slice(2, response.size - 2);
                first = false;
            }
            const auto count = response.U16(0);
            const bool end = response.U16(2) != 0;
            if (count > 256 || (!count && !end)) InvalidReply();
            ReadEntries(entries, ViewOf(reply.data), count);
            if (end) return entries;
        }
        throw std::runtime_error("The Zaurus folder has too many files to list.");
    }

    void Send(const std::string& remote, FILE* source) {
        const auto path = DiskPath(remote, false);
        char suffix[24];
        std::snprintf(suffix, sizeof(suffix), ".frond-%08x%08x", unsigned(std::random_device{}()), unsigned(std::random_device{}()));
        const auto slash = remote.rfind('/');
        const auto staged = DiskPath(slash == std::string::npos ? suffix : remote.substr(0, slash + 1) + suffix, false);
        const auto file = Open(staged, true);
        bool open = true;
        try {
            WriteFile(file.id, source);
            Close(file.id);
            open = false;

            Bytes attributes;
            Put16(attributes, searchAttributes);
            Bytes fromTo{4};
            PutString(fromTo, staged);
            fromTo.push_back(4);
            PutString(fromTo, path);
            Exchange(smbRename, attributes, fromTo);
        } catch (...) {
            if (open) {
                try { Close(file.id, false); } catch (...) {}
            }
            try { Delete(staged); } catch (...) {}
            throw;
        }
    }

    void Receive(const std::string& remote, FILE* target) {
        const auto file = Open(DiskPath(remote, false), false);
        for (uint64_t position = 0; position < file.size;) {
            const uint16_t capacity = uint16_t(std::min<uint64_t>(TransferSize(), file.size - position));
            Bytes words = AndX();
            Put16(words, file.id);
            Put32(words, uint32_t(position));
            Put16(words, capacity);
            Put16(words, 0);  // Minimum count, unused for disk files.
            Put32(words, 0);  // Timeout, unused for disk files.
            Put16(words, 0);  // Remaining.
            AppendHighOffset(words, position);

            const auto reply = Exchange(smbReadAndX, words, {});
            const auto read = reply.Words();
            const auto count = read.U16(10);
            if (!count || count > capacity || read.U16(14))
                throw std::runtime_error("The Zaurus file changed while it was being copied.");
            const auto data = reply.DataAt(read.U16(12), count);
            if (std::fwrite(data.bytes, 1, count, target) != count)
                throw std::runtime_error("Couldn't write the received file.");
            position += count;
        }
        Close(file.id);
    }

   private:
    void SendFrame(uint8_t type, const Bytes& bytes) {
        const auto size = bytes.size();
        Bytes frame{type, uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
        frame.insert(frame.end(), bytes.begin(), bytes.end());
        stream.Send(frame.data(), frame.size());
    }

    Frame ReadFrame() {
        for (;;) {
            std::array<uint8_t, 4> header{};
            stream.Receive(header.data(), header.size());
            if (header[1] & 0xfe) InvalidReply();
            const size_t length = size_t(header[1]) << 16 | size_t(header[2]) << 8 | header[3];
            if (header[0] == 0x85 && length == 0) continue;  // NetBIOS keep-alive.
            if (length > maxBuffer) InvalidReply();
            Frame frame{header[0], Bytes(length)};
            if (length) stream.Receive(frame.bytes.data(), frame.bytes.size());
            return frame;
        }
    }

    Reply Exchange(uint8_t command, const Bytes& words, const Bytes& data, bool checkCancelled = true) {
        if (checkCancelled) stream.CheckCancelled();
        constexpr uint16_t pid = 42;
        constexpr uint16_t longNames = 1;
        Bytes message{0xff, 'S', 'M', 'B', command};
        Put32(message, 0);
        message.push_back(0x18);  // Case-insensitive files and canonical paths.
        Put16(message, longNames);
        message.resize(24);
        Put16(message, tid);
        Put16(message, pid);
        Put16(message, uid);
        Put16(message, ++mid);
        message.push_back(uint8_t(words.size() / 2));
        message.insert(message.end(), words.begin(), words.end());
        Put16(message, uint16_t(data.size()));
        message.insert(message.end(), data.begin(), data.end());
        SendFrame(0, message);
        return ReadReply(command);
    }

    Reply ReadReply(uint8_t command) {
        auto frame = ReadFrame();
        auto view = ViewOf(frame.bytes);
        if (frame.type != 0 || view.size < headerSize + 3 || view.U32(0) != 0x424d53ff ||
            view.U8(4) != command || !(view.U8(9) & 0x80) || view.U16(30) != mid)
            InvalidReply();
        if (const auto status = view.U32(5))
            throw SmbError(command, status, (view.U16(10) & 0x4000) != 0);
        return {std::move(frame.bytes)};
    }

    Transaction Transact(uint16_t subcommand, const Bytes& parameters) {
        constexpr uint16_t parameterOffset = headerSize + 1 + 30 + 2 + 3;
        Bytes words;
        Put16(words, uint16_t(parameters.size()));
        Put16(words, 0);  // No request data.
        Put16(words, 10);  // Maximum response parameter bytes.
        Put16(words, maxBuffer - 128);
        Put16(words, 0);  // Maximum setup count and reserved.
        Put16(words, 0);  // Flags.
        Put32(words, 0);  // Timeout.
        Put16(words, 0);  // Reserved.
        Put16(words, uint16_t(parameters.size()));
        Put16(words, parameterOffset);
        Put16(words, 0);  // Data count.
        Put16(words, 0);  // Data offset.
        words.insert(words.end(), {1, 0});  // Setup count and reserved.
        Put16(words, subcommand);
        Bytes data(3, 0);  // Align parameters to four bytes.
        data.insert(data.end(), parameters.begin(), parameters.end());

        Transaction transaction;
        auto reply = Exchange(smbTransaction2, words, data);
        for (;;) {
            const auto response = reply.Words();
            const size_t parameterTotal = response.U16(0), dataTotal = response.U16(2);
            if (parameterTotal > 10 || dataTotal > maxBuffer - 128) InvalidReply();
            const size_t parameterCount = response.U16(6), dataCount = response.U16(12);
            if (!parameterCount && !dataCount) InvalidReply();
            AppendFragment(transaction.parameters, reply, parameterTotal, parameterCount, response.U16(8), response.U16(10));
            AppendFragment(transaction.data, reply, dataTotal, dataCount, response.U16(14), response.U16(16));
            if (transaction.parameters.size() == parameterTotal && transaction.data.size() == dataTotal)
                return transaction;
            reply = ReadReply(smbTransaction2);
        }
    }

    static void AppendFragment(Bytes& destination, const Reply& reply, size_t total, size_t count,
                               size_t offset, size_t displacement) {
        if (destination.size() > total || count > total - destination.size()) InvalidReply();
        if (!count) return;
        if (displacement != destination.size()) InvalidReply();
        const auto fragment = reply.DataAt(offset, count);
        destination.insert(destination.end(), fragment.bytes, fragment.bytes + fragment.size);
    }

    static void ReadEntries(std::vector<Entry>& entries, View data, uint16_t count) {
        constexpr size_t nameOffset = 64;
        for (uint16_t i = 0; i < count; ++i) {
            const size_t next = data.U32(0), nameLength = data.U32(60);
            auto name = data.Slice(nameOffset, nameLength);
            if (name.size && name.bytes[name.size - 1] == 0) --name.size;
            auto utf8 = CodePage(std::string_view(reinterpret_cast<const char*>(name.bytes), name.size), true);
            if (ValidName(utf8)) {
                if (entries.size() == 65536) throw std::runtime_error("The Zaurus folder has too many files to list.");
                entries.push_back({std::move(utf8), (data.U32(56) & 0x10) != 0});
            }
            if (i + 1 < count) {
                if (next < nameOffset + nameLength || next >= data.size) InvalidReply();
                data = data.Slice(next, data.size - next);
            }
        }
    }

    OpenFile Open(const std::string& path, bool create) {
        // Samba 2.0 also needs the specific data rights when opening an NT1 file.
        constexpr uint32_t readAccess = 0x80000081, writeAccess = 0x40000082;
        Bytes words = AndX();
        words.push_back(0);  // Reserved.
        Put16(words, uint16_t(path.size() + 1));
        Put32(words, 0);  // No oplock.
        Put32(words, 0);  // Path is relative to the share.
        Put32(words, create ? writeAccess : readAccess);
        Put64(words, 0);  // No preallocation.
        Put32(words, 0x80);  // Normal file attributes.
        Put32(words, create ? 0 : 1);  // Exclusive write or shared read.
        Put32(words, create ? 2 : 1);  // Create new or open existing.
        Put32(words, 0x40);  // Require a regular file.
        Put32(words, 2);  // Impersonation.
        words.push_back(0);  // Security flags.
        Bytes data;
        PutString(data, path);
        const auto reply = Exchange(smbNtCreateAndX, words, data);
        const auto response = reply.Words();
        if (response.U8(67)) throw std::runtime_error("Select a file on the Zaurus.");
        return {response.U16(5), response.U64(55)};
    }

    void Close(uint16_t fileId, bool checkCancelled = true) {
        Bytes words;
        Put16(words, fileId);
        Put32(words, 0xffffffff);  // Leave last-write time unchanged.
        Exchange(smbClose, words, {}, checkCancelled);
    }

    void Delete(const std::string& path) {
        Bytes words, data{4};
        Put16(words, searchAttributes);
        PutString(data, path);
        Exchange(smbDelete, words, data, false);
    }

    void AppendHighOffset(Bytes& words, uint64_t position) const {
        if (capabilities & capLargeFiles) Put32(words, uint32_t(position >> 32));
        else if (position > UINT32_MAX) throw std::runtime_error("The Zaurus PC Link connection cannot copy files this large.");
    }

    uint16_t TransferSize() const { return std::min<uint16_t>(bufferSize - 128, ioSize); }

    void WriteFile(uint16_t fileId, FILE* source) {
        std::array<uint8_t, ioSize> buffer{};
        uint64_t position = 0;
        while (const auto count = std::fread(buffer.data(), 1, TransferSize(), source)) {
            size_t sent = 0;
            while (sent < count) {
                const uint16_t remaining = uint16_t(count - sent);
                Bytes words = AndX();
                Put16(words, fileId);
                Put32(words, uint32_t(position));
                Put32(words, 0);  // Timeout.
                Put16(words, 0);  // Write mode.
                Put16(words, 0);  // Remaining field, unused for disk files.
                Put16(words, 0);  // No high length.
                Put16(words, remaining);
                const bool highOffset = (capabilities & capLargeFiles) != 0;
                Put16(words, headerSize + 1 + (highOffset ? 28 : 24) + 2 + 1);
                AppendHighOffset(words, position);
                Bytes data(1, 0);  // Padding before the write data.
                data.insert(data.end(), buffer.begin() + sent, buffer.begin() + count);

                const auto reply = Exchange(smbWriteAndX, words, data);
                const auto written = reply.Words().U16(4);
                if (!written) throw std::runtime_error("The Zaurus could not store the file.");
                if (written > remaining) InvalidReply();
                sent += written;
                position += written;
            }
        }
        if (std::ferror(source)) throw std::runtime_error("Couldn't read the file to send.");
    }

    Stream& stream;
    uint16_t bufferSize = maxBuffer;
    uint16_t uid = 0, tid = 0, mid = 0;
    uint32_t capabilities = 0;
};

Client::Client(Stream& stream) : impl(std::make_unique<Impl>(stream)) {}
Client::~Client() = default;

std::vector<Entry> Client::List(const std::string& remote) { return impl->List(remote); }
void Client::Send(const std::string& remote, FILE* source) { impl->Send(remote, source); }
void Client::Receive(const std::string& remote, FILE* target) { impl->Receive(remote, target); }

}  // namespace zaurusPcLink

#endif
