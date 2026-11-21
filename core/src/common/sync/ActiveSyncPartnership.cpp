#include "ActiveSyncPartnership.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "Sha256.h"

namespace frondSync {
namespace {

constexpr char partners[] = R"(HKLM\Software\Microsoft\Windows CE Services\Partners)";

std::string Characteristic(std::string_view type, const std::string& children) {
    return "<characteristic type=\"" + std::string(type) + "\">" + children + "</characteristic>";
}

std::string Query(std::string_view type) {
    return "<characteristic-query type=\"" + std::string(type) + "\"/>";
}

std::string Parm(std::string_view name, std::string_view value, const char* dataType = nullptr) {
    std::string parm = "<parm name=\"" + std::string(name) + "\" value=\"" + std::string(value) + "\"";
    if (dataType) parm += std::string(" datatype=\"") + dataType + "\"";
    return parm + "/>";
}

std::string Provision(FrondRapi* client, const std::string& xml) {
    const auto document = "<wap-provisioningdoc>" + xml + "</wap-provisioningdoc>";
    std::vector<uint16_t> request(document.begin(), document.end());
    request.push_back(0);
    std::vector<uint16_t> reply(32768);
    if (!FrondRapiProvision(client, request.data(), reply.data(), reply.size()))
        throw std::runtime_error("Windows Mobile refused the partnership configuration request.");
    std::string text;
    for (const auto character : reply) {
        if (!character) break;
        text.push_back(character < 128 ? char(character) : '\x7f');
    }
    if (text.find("<wap-provisioningdoc>") == std::string::npos ||
        text.find("</wap-provisioningdoc>") == std::string::npos)
        throw std::runtime_error("Windows Mobile returned invalid partnership configuration.");
    return text;
}

bool HasCharacteristic(const std::string& reply, const char* tag, const std::string& type) {
    const auto prefix = std::string("<") + tag + " type=\"" + type + "\"";
    return reply.find(prefix + ">") != std::string::npos || reply.find(prefix + "/>") != std::string::npos;
}

std::string_view Value(const std::string& reply, const char* name) {
    const auto prefix = std::string("<parm name=\"") + name + "\" value=\"";
    const auto start = reply.find(prefix);
    if (start == std::string::npos) return {};
    const auto end = reply.find('\"', start + prefix.size());
    if (end == std::string::npos) return {};
    return std::string_view(reply).substr(start + prefix.size(), end - start - prefix.size());
}

struct Identity {
    std::string source, host, id;
};

Identity PartnershipIdentity(uint32_t profileId, const std::string& deviceGuid) {
    const auto name = "Frond ActiveSync:" + std::to_string(profileId) + ":" + deviceGuid;
    auto hash = Sha256Hex(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    std::transform(hash.begin(), hash.end(), hash.begin(), [](char digit) {
        return digit >= 'a' && digit <= 'f' ? char(digit - 'a' + 'A') : digit;
    });
    uint32_t id = 0;
    std::from_chars(hash.data() + 32, hash.data() + 40, id, 16);
    // The Registry CSP takes signed decimal integers.
    id = (id & 0x3fffffffu) + 1;
    return {
        "{" + hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-8" + hash.substr(13, 3) + "-8" +
            hash.substr(17, 3) + "-" + hash.substr(20, 12) + "}",
        "FROND-" + hash.substr(40, 8), std::to_string(id)};
}

std::string SyncSource(const std::string& body) {
    return Characteristic("Sync", Characteristic("Sources", body));
}

std::string ReadSlot(FrondRapi* client, const std::string& path) {
    return Provision(client, Characteristic("Registry", Query(path)));
}

bool MatchesSlot(const Identity& identity, const std::string& reply) {
    return Value(reply, "DataSourceID") == identity.source && Value(reply, "PName") == identity.host &&
           Value(reply, "PId") == identity.id;
}

bool HasSource(FrondRapi* client, const Identity& identity) {
    const auto sources = Provision(client, Characteristic("Sync", R"XML(<characteristic-query recursive="false" type="Sources"/>)XML"));
    if (HasCharacteristic(sources, "characteristic", identity.source)) return true;
    if (sources.find(R"XML(<characteristic recursive="false" type="Sources")XML") == std::string::npos &&
        sources.find(R"XML(<nocharacteristic recursive="false" type="Sources")XML") == std::string::npos)
        throw std::runtime_error("Couldn't read the Windows Mobile desktop sync sources.");
    return false;
}

void VerifySource(FrondRapi* client, const Identity& identity) {
    const auto config = Provision(client, SyncSource(Query(identity.source)));
    if (!HasCharacteristic(config, "characteristic", identity.source) ||
        Value(config, "Server") != identity.host || Value(config, "StoreType") != "2")
        throw std::runtime_error("Couldn't verify the Windows Mobile desktop sync source.");
}

std::string SourceConfig(const Identity& identity) {
    // Leave providers disabled until Frond can serve their Desktop AirSync requests.
    struct Provider { const char* id; const char* name; bool readOnly; };
    constexpr Provider providers[] = {
        {"{4A5D9FE0-F139-4A63-A5A4-4F31CEEA02AD}", "Calendar", false},
        {"{0DD8685C-E272-4FCB-9ECF-2EAD7EA2497B}", "Contacts", false},
        {"{C6D47067-6E92-480E-B0FC-4BA82182FAC7}", "E-mail", false},
        {"{7E29B5F7-C686-4B0C-9892-FD8BAD8E0D08}", "Favorites", true},
        {"{B7B6ACB2-AF1D-43F5-BF9A-586111B263EF}", "Files", true},
        {"{A38DEEBF-C535-D8E2-AE24-8AE61845CF82}", "Media", true},
        {"{8E98CB51-85A4-4777-8DEB-A0298DF8899F}", "Notes", true},
        {"{783AE4F6-4C12-4423-8270-66361260D4F1}", "Tasks", false},
    };
    std::string config;
    for (const auto& provider : providers) {
        config += Characteristic(provider.id, Parm("Name", provider.name) + Parm("Enabled", "0") +
                                               Parm("ReadOnly", provider.readOnly ? "1" : "0"));
    }
    const auto settings = Parm("User", "DEFAULT") + Parm("Domain", "DEFAULT") + Parm("Password", "DEFAULT") +
                          Parm("SavePassword", "1") + Parm("UseSSL", "0") + Parm("ConflictResolution", "1") +
                          Parm("URI", "Microsoft-Server-ActiveSync");
    const auto engine = Characteristic("{176F4FFD-F20C-4BD4-BDD7-01D0726C567B}",
                                       Characteristic("Settings", settings) + Characteristic("Providers", config));
    return Characteristic(identity.source, Parm("Name", "Frond Sync") + Parm("Server", identity.host) +
                                               Parm("StoreType", "2") + Characteristic("Engines", engine));
}

}  // namespace

bool HasActiveSyncPartnership(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid) {
    const auto identity = PartnershipIdentity(profileId, deviceGuid);
    bool found = false;
    for (unsigned slot = 1; slot <= 2; ++slot) {
        const auto path = std::string(partners) + "\\P" + std::to_string(slot);
        const auto value = ReadSlot(client, path);
        if (HasCharacteristic(value, "nocharacteristic", path)) continue;
        if (!HasCharacteristic(value, "characteristic", path))
            throw std::runtime_error("Couldn't read the Windows Mobile partnership slots.");
        if (Value(value, "DataSourceID") != identity.source) continue;
        if (found || !MatchesSlot(identity, value))
            throw std::runtime_error("Windows Mobile reports a conflicting Frond partnership.");
        found = true;
    }
    if (!found || !HasSource(client, identity)) return false;
    VerifySource(client, identity);
    return true;
}

uint32_t EnsureActiveSyncPartnership(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid) {
    const auto identity = PartnershipIdentity(profileId, deviceGuid);
    std::string ours, available;
    for (unsigned slot = 1; slot <= 2; ++slot) {
        const auto path = std::string(partners) + "\\P" + std::to_string(slot);
        const auto value = ReadSlot(client, path);
        if (HasCharacteristic(value, "nocharacteristic", path)) {
            if (available.empty()) available = path;
        } else if (!HasCharacteristic(value, "characteristic", path)) {
            throw std::runtime_error("Couldn't read the Windows Mobile partnership slots.");
        } else if (Value(value, "DataSourceID") == identity.source) {
            if (!ours.empty() || (!Value(value, "PName").empty() && Value(value, "PName") != identity.host) ||
                (!Value(value, "PId").empty() && Value(value, "PId") != identity.id))
                throw std::runtime_error("Windows Mobile reports a conflicting Frond partnership.");
            ours = path;
        }
    }
    const auto& slot = ours.empty() ? available : ours;
    if (slot.empty()) throw std::runtime_error("Windows Mobile has no free desktop partnership slot.");

    if (!HasSource(client, identity)) Provision(client, SyncSource(SourceConfig(identity)));
    VerifySource(client, identity);

    if (ours.empty() || !MatchesSlot(identity, ReadSlot(client, slot))) {
        const auto fields = Parm("DataSourceID", identity.source, "string") + Parm("PName", identity.host, "string") +
                            Parm("PId", identity.id, "integer");
        const auto registry = ReadSlot(client, partners);
        if (!HasCharacteristic(registry, "characteristic", partners))
            throw std::runtime_error("Couldn't read the Windows Mobile partnership registry.");
        auto write = Characteristic(slot, fields);
        if (Value(registry, "PCur") == "0") {
            const auto slotNumber = std::string_view(slot).substr(slot.size() - 1);
            write += Characteristic(partners, Parm("PCur", slotNumber, "integer"));
        }
        Provision(client, Characteristic("Registry", write));
        if (!MatchesSlot(identity, ReadSlot(client, slot))) throw std::runtime_error("Couldn't verify the Windows Mobile partnership slot.");
    }
    uint32_t id = 0;
    std::from_chars(identity.id.data(), identity.id.data() + identity.id.size(), id);
    return id;
}

}  // namespace frondSync
