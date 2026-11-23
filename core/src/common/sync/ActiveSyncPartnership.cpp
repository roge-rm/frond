#include "ActiveSyncPartnership.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "AirSync.h"
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
    if (!FrondRapiProvision(client, request.data(), reply.data(), reply.size())) {
        if (!FrondRapiConnected(client)) throw std::runtime_error("The ActiveSync connection closed during partnership setup.");
        throw std::runtime_error("Windows Mobile refused the partnership configuration request.");
    }
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

std::vector<uint16_t> Wide(const std::string& ascii) {
    std::vector<uint16_t> out(ascii.begin(), ascii.end());
    out.push_back(0);
    return out;
}

}  // namespace

void StartActiveSyncDesktopSync(FrondRapi* client, uint32_t profileId, const std::string& deviceGuid,
                                const std::vector<PimKind>& kinds) {
    const auto identity = PartnershipIdentity(profileId, deviceGuid);
    std::string slot;
    for (unsigned number = 1; number <= 2 && slot.empty(); ++number) {
        const auto path = std::string(partners) + "\\P" + std::to_string(number);
        if (Value(ReadSlot(client, path), "DataSourceID") == identity.source) slot = std::to_string(number);
    }
    if (slot.empty()) throw std::runtime_error("Windows Mobile has no Frond partnership slot.");

    struct Provider { const char* id; const char* name; PimKind kind; };
    constexpr Provider served[] = {
        {"{0DD8685C-E272-4FCB-9ECF-2EAD7EA2497B}", "Contacts", PimKind::contacts},
        {"{4A5D9FE0-F139-4A63-A5A4-4F31CEEA02AD}", "Calendar", PimKind::calendar},
        {"{783AE4F6-4C12-4423-8270-66361260D4F1}", "Tasks", PimKind::tasks},
    };
    std::string providers;
    for (const auto& provider : served) {
        const bool on = std::find(kinds.begin(), kinds.end(), provider.kind) != kinds.end();
        providers += Characteristic(provider.id, Parm("Name", provider.name) + Parm("Enabled", on ? "1" : "0") +
                                                     Parm("ReadOnly", "0"));
    }
    Provision(client, SyncSource(Characteristic(identity.source, Characteristic("Engines",
        Characteristic("{176F4FFD-F20C-4BD4-BDD7-01D0726C567B}", Characteristic("Providers", providers))))));
    // The device sets the value but reports error 6 for this key.
    Provision(client, Characteristic("Registry", Characteristic(partners, Parm("PCur", slot, "integer"))));
    if (Value(ReadSlot(client, partners), "PCur") != slot)
        throw std::runtime_error("Windows Mobile didn't take Frond as its current partnership.");

    uint32_t result = 0;
    if (!FrondRapiSyncResume(client, &result))
        throw std::runtime_error("Windows Mobile didn't resume desktop sync.");
    const auto request = Wide("<sync xmlns=\"http://schemas.microsoft.com/as/2004/core\" type=\"Interactive\">"
                              "<partner id=\"" + identity.source + "\"/></sync>");
    if (!FrondRapiSyncStart(client, request.data(), &result) || int32_t(result) < 0)
        throw std::runtime_error("Windows Mobile didn't start Pocket Outlook sync.");
}

namespace {

// A registry value from the device, or nothing when the key or value is missing.
// misses, if given, gets "path\name error" for each value that couldn't be read.
std::optional<Bytes> RegistryValue(FrondRapi* client, const std::string& path, const std::string& name,
                                   std::string* misses = nullptr) {
    constexpr uint32_t localMachine = 0x80000002;
    uint32_t key = 0;
    const auto miss = [&](uint32_t error) {
        if (misses) *misses += (misses->empty() ? "" : "; ") + path + "\\" + name + " " + std::to_string(error);
        return std::nullopt;
    };
    if (const auto error = FrondRapiRegOpenKey(client, localMachine, Wide(path).data(), &key)) return miss(error);
    Bytes data(1024);
    uint32_t type = 0, size = uint32_t(data.size());
    const auto error = FrondRapiRegQueryValue(client, key, Wide(name).data(), &type, data.data(), &size);
    FrondRapiRegCloseKey(client, key);
    if (error || size > data.size()) return miss(error ? error : 234);
    data.resize(size);
    return data;
}

// A REG_SZ value as UTF-16 code units without the terminator.
std::vector<uint16_t> Units(const Bytes& data) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        const uint16_t unit = uint16_t(data[i] | data[i + 1] << 8);
        if (!unit) break;
        out.push_back(unit);
    }
    return out;
}

}  // namespace

std::string ReadActiveSyncTimeZone(FrondRapi* client, std::string* source) {
    // HKLM\Time\TimeZoneInformation is the TIME_ZONE_INFORMATION itself, but it only exists once
    // the zone has been changed on the device. Windows CE then looks for the zone named by
    // HKLM\Time Zones\Default, whose TZI value is a REG_TZI_FORMAT (bias, standard bias,
    // daylight bias, standard date, daylight date). WM5 has neither until the zone is set and
    // falls back to Pacific time built into the ROM. Names are left out, as the device's own
    // calendar items send them.
    std::string misses;
    if (const auto info = RegistryValue(client, "Time", "TimeZoneInformation", &misses); info && info->size() == 172) {
        if (source) *source = "HKLM\\Time";
        return AirSyncBase64(*info);
    }
    if (!FrondRapiConnected(client)) return {};
    const auto name = RegistryValue(client, "Time Zones", "Default", &misses);
    if (!name) {
        if (source) *source = misses;
        constexpr const char* bothMissing = "Time\\TimeZoneInformation 2; Time Zones\\Default 2";
        if (!FrondRapiConnected(client) || misses != bothMissing) return {};
        // Both missing (error 2): the ROM's Pacific zone, US rules before 2007 (first Sunday in
        // April to the last Sunday in October, 2:00), as the rx1950 sends it in calendar items.
        Bytes info(172, 0);
        const auto put = [&](size_t at, uint32_t value) {
            for (int i = 0; i < 4; ++i) info[at + i] = uint8_t(value >> (8 * i));
        };
        const auto rule = [&](size_t at, int month, int week) {
            info[at + 2] = uint8_t(month);
            info[at + 6] = uint8_t(week);
            info[at + 8] = 2;
        };
        put(0, 480);
        rule(68, 10, 5);
        put(168, uint32_t(-60));
        rule(152, 4, 1);
        if (source) *source += "; built-in Pacific";
        return AirSyncBase64(info);
    }
    const auto units = Units(*name);
    std::string path = "Time Zones\\";
    for (const auto unit : units) {
        if (unit > 0x7f) return {};  // zone key names are ASCII
        path += char(unit);
    }
    const auto tzi = RegistryValue(client, path, "TZI", &misses);
    if (!tzi || tzi->size() != 44) {
        if (source) *source = misses;
        return {};
    }
    Bytes info(172, 0);
    const auto copy = [&](size_t to, size_t from, size_t count) { std::copy_n(tzi->begin() + long(from), count, info.begin() + long(to)); };
    copy(0, 0, 4);      // Bias
    copy(84, 4, 4);     // StandardBias
    copy(168, 8, 4);    // DaylightBias
    copy(68, 12, 16);   // StandardDate
    copy(152, 28, 16);  // DaylightDate
    if (source) *source = "HKLM\\" + path;
    return AirSyncBase64(info);
}

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
