#include "Ias.h"

#include <algorithm>

namespace frondIr {

namespace {

// IAP control byte: last frame, acknowledge, then the opcode.
constexpr uint8_t last = 0x80, ack = 0x40, opcodeMask = 0x3f;
constexpr uint8_t getValueByClass = 0x04;
constexpr uint8_t typeInteger = 1, typeOctets = 2, typeString = 3;

}  // namespace

IasServer::IasServer(IrLmp& lmp) : lmp(lmp) { lmp.Bind(lsap, *this); }

void IasServer::Add(const std::string& className, const std::string& attribute, Bytes value) {
    auto found = std::find(classes.begin(), classes.end(), className);
    if (found == classes.end()) found = classes.insert(classes.end(), className);
    attributes.push_back({className, attribute, uint16_t(found - classes.begin()), std::move(value)});
}

void IasServer::AddInteger(const std::string& className, const std::string& attribute, uint32_t value) {
    Add(className, attribute,
        {typeInteger, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)});
}

void IasServer::AddOctets(const std::string& className, const std::string& attribute, const Bytes& value) {
    Bytes encoded{typeOctets, uint8_t(value.size() >> 8), uint8_t(value.size())};
    encoded.insert(encoded.end(), value.begin(), value.end());
    Add(className, attribute, std::move(encoded));
}

void IasServer::AddString(const std::string& className, const std::string& attribute, const std::string& value) {
    Bytes encoded{typeString, 0x00, uint8_t(std::min<size_t>(value.size(), 255))};
    encoded.insert(encoded.end(), value.begin(), value.begin() + encoded[2]);
    Add(className, attribute, std::move(encoded));
}

bool IasServer::Connect(const Bytes& data, Bytes& reply, uint64_t now) {
    pending.clear();
    overflow = false;
    return true;
}

void IasServer::Disconnect(uint64_t now) {
    pending.clear();
    overflow = false;
}

void IasServer::Data(const Bytes& data, uint64_t now) {
    if (data.empty()) return;
    const uint8_t control = data[0];
    // An acknowledgement of one of our frames; ours always fit in one.
    if ((control & ack) && data.size() == 1) return;
    // A request in several frames comes with the opcode in each and the last one marked.
    if (!overflow && pending.size() + data.size() - 1 <= maxRequest) {
        pending.insert(pending.end(), data.begin() + 1, data.end());
    } else {
        overflow = true;
        pending.clear();
    }
    if (!(control & last)) return;
    if (overflow) {
        overflow = false;
        return;
    }
    Bytes request;
    request.push_back(control & opcodeMask);
    request.insert(request.end(), pending.begin(), pending.end());
    pending.clear();
    Request(request);
}

void IasServer::Request(const Bytes& request) {
    const uint8_t opcode = request[0];
    if (opcode != getValueByClass) {
        // Unsupported operations answer with the opcode and 0xff.
        const Bytes reply{uint8_t(last | opcode), 0xff};
        lmp.Send(lsap, reply.data(), reply.size());
        return;
    }
    size_t i = 1;
    auto text = [&](std::string& out) {
        if (i >= request.size()) return false;
        const size_t length = request[i++];
        if (i + length > request.size()) return false;
        out.assign(request.begin() + i, request.begin() + i + length);
        i += length;
        return true;
    };
    std::string className, attribute;
    if (!text(className) || !text(attribute)) return;
    Bytes reply{uint8_t(last | getValueByClass)};
    const bool known = std::find(classes.begin(), classes.end(), className) != classes.end();
    std::vector<const Attribute*> found;
    for (const Attribute& a : attributes)
        if (a.className == className && a.name == attribute) found.push_back(&a);
    const uint8_t result = !known ? noSuchClass : found.empty() ? noSuchAttribute : success;
    reply.push_back(result);
    if (result == success) {
        reply.push_back(uint8_t(found.size() >> 8));
        reply.push_back(uint8_t(found.size()));
        for (const Attribute* a : found) {
            reply.push_back(uint8_t(a->object >> 8));
            reply.push_back(uint8_t(a->object));
            reply.insert(reply.end(), a->value.begin(), a->value.end());
        }
    }
    queries.push_back({className, attribute, result});
    if (queries.size() > keptQueries) queries.pop_front();
    lmp.Send(lsap, reply.data(), reply.size());
}

}  // namespace frondIr
