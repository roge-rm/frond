#include "ActiveSync.h"
#include "ActiveSyncFiles.h"
#include "ActiveSyncPartnership.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "SyncStore.h"
#include "SynceRapi.h"

namespace frondSync {
namespace {
using Clock = std::chrono::steady_clock;

bool Read(SyncStream& stream, void* data, size_t size, int timeoutMs) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    auto* out = static_cast<uint8_t*>(data);
    while (size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
        if (remaining <= 0) return false;
        const auto count = stream.Read(out, size, int(remaining));
        if (count <= 0) return false;
        out += count;
        size -= size_t(count);
    }
    return true;
}

uint32_t Get32(const uint8_t* data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
uint32_t Read32(SyncStream& stream, const char* error, int timeoutMs = 15000) {
    uint8_t bytes[4];
    if (!Read(stream, bytes, sizeof(bytes), timeoutMs)) throw std::runtime_error(error);
    return Get32(bytes);
}
void Send(SyncStream& stream, std::initializer_list<uint32_t> values) {
    Bytes bytes;
    for (const auto value : values)
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(value >> shift));
    if (!stream.Write(bytes.data(), bytes.size())) throw std::runtime_error("ActiveSync connection closed.");
}

struct DeviceInfo {
    std::string guid;
    uint32_t partner{0};
};

DeviceInfo Connect(SyncStream& control, SyncStream& rapi) {
    // SynCE dccm RNDIS handshake. The information record sent by WM5 follows
    // the command 4 and size. Read the current partnership without changing it.
    uint32_t command = Read32(control, "Windows Mobile did not open its ActiveSync connection.", 90000);
    for (unsigned attempt = 0; command != 4 && attempt < 8; ++attempt) {
        if (command == 0) Send(control, {3});
        else if (command == 6) Send(control, {7, 8, 4, 5});
        else throw std::runtime_error("Unexpected ActiveSync control message.");
        command = Read32(control, "Windows Mobile did not send its ActiveSync information.");
    }
    if (command != 4) throw std::runtime_error("Incomplete ActiveSync handshake.");
    const uint32_t size = Read32(control, "Missing ActiveSync device information.");
    if (size < 70 || size > 4096) throw std::runtime_error("Invalid ActiveSync device information.");
    Bytes data(size);
    if (!Read(control, data.data(), data.size(), 15000)) throw std::runtime_error("Incomplete ActiveSync device information.");
    const size_t nameLength = Get32(data.data() + 24);
    if (nameLength > 31) throw std::runtime_error("Invalid ActiveSync device name.");
    size_t pos = 28 + 2 * (nameLength + 1);
    if (pos > size || size - pos < 20) throw std::runtime_error("Invalid ActiveSync device information.");
    DeviceInfo info;
    for (size_t i = 0; i < 16; ++i) {
        constexpr char digits[] = "0123456789abcdef";
        info.guid.push_back(digits[data[i] >> 4]);
        info.guid.push_back(digits[data[i] & 15]);
    }
    info.partner = Get32(data.data() + pos + 12);
    pos += 20;
    for (unsigned i = 0; i < 2; ++i) {
        if (size - pos < 4) throw std::runtime_error("Missing ActiveSync platform information.");
        const size_t length = Get32(data.data() + pos);
        if (length > 255 || size - pos - 4 < length) throw std::runtime_error("Invalid ActiveSync platform information.");
        pos += 4 + length;
    }
    if (size - pos < 9) throw std::runtime_error("Missing ActiveSync components.");
    ++pos;
    const uint32_t count = Get32(data.data() + pos);
    if (count > 6 || size - pos < 8 + count * 8) throw std::runtime_error("Invalid ActiveSync components.");
    if (Get32(data.data() + pos + 4 + count * 8))
        throw std::runtime_error("The Windows Mobile device needs to be unlocked before syncing.");

    constexpr uint32_t requestId = 1;
    Send(control, {5, 4, requestId});
    if (Read32(rapi, "Windows Mobile did not open its RAPI connection.") != requestId)
        throw std::runtime_error("Unexpected ActiveSync RAPI connection.");
    return info;
}

std::unique_ptr<FrondRapi, decltype(&FrondRapiClose)> OpenRapi(SyncStream& rapi) {
    FrondRapiStream stream{
        &rapi,
        [](void* self, void* data, size_t size, int timeoutMs) {
            return Read(*static_cast<SyncStream*>(self), data, size, timeoutMs);
        },
        [](void* self, const void* data, size_t size) {
            return static_cast<SyncStream*>(self)->Write(static_cast<const uint8_t*>(data), size);
        },
        [](void* self) { static_cast<SyncStream*>(self)->Abort(); },
    };
    std::unique_ptr<FrondRapi, decltype(&FrondRapiClose)> client(FrondRapiOpen(stream), FrondRapiClose);
    if (!client || !FrondRapiReady(client.get())) throw std::runtime_error("Windows Mobile did not answer RAPI.");
    return client;
}

std::vector<uint16_t> WideAscii(const std::string& ascii) {
    std::vector<uint16_t> out(ascii.begin(), ascii.end());
    out.push_back(0);
    return out;
}

std::string Utf8(const uint16_t* text) {
    std::string out;
    while (*text) {
        uint32_t c = *text++;
        if (c >= 0xd800 && c <= 0xdbff && *text >= 0xdc00 && *text <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (*text++ - 0xdc00);
        else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) {
            out.push_back(char(0xc0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3f)));
        } else {
            if (c >= 0x10000) {
                out.push_back(char(0xf0 | (c >> 18)));
                out.push_back(char(0x80 | ((c >> 12) & 0x3f)));
            } else out.push_back(char(0xe0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(char(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

bool StageCab(FrondRapi* client, const Bytes& cab, const InstallItem& item, std::string& error) {
    const auto name = "\\Temp\\Frond-" + item.sha256.substr(0, 24) + ".cab";
    const auto path = WideAscii(name);
    const auto installer = WideAscii("\\Windows\\wceload.exe");
    auto handle = FrondRapiCreate(client, path.data());
    if (handle == UINT32_MAX && (FrondRapiError(client) == 80 || FrondRapiError(client) == 183)) {
        // Retry a staged copy left by this file's earlier attempt.
        if (FrondRapiRemove(client, path.data())) handle = FrondRapiCreate(client, path.data());
    }
    if (handle == UINT32_MAX) {
        error = "Couldn't create a CAB file on the device (RAPI " + std::to_string(FrondRapiError(client)) + ").";
        return false;
    }
    constexpr size_t chunk = 16384;
    size_t offset = 0;
    for (; offset < cab.size();) {
        const auto size = uint32_t(std::min(chunk, cab.size() - offset));
        if (!FrondRapiWrite(client, handle, cab.data() + offset, size)) break;
        offset += size;
    }
    const auto writeError = FrondRapiError(client);
    const bool closed = FrondRapiCloseFile(client, handle);
    const auto args = WideAscii("\"" + name + "\"");
    if (offset == cab.size() && closed && FrondRapiRun(client, installer.data(), args.data())) return true;
    if (offset != cab.size()) {
        error = "The CAB transfer stopped after " + std::to_string(offset) + " of " + std::to_string(cab.size()) + " bytes";
        if (writeError) error += " (RAPI " + std::to_string(writeError) + ")";
        error += ".";
    } else {
        const auto action = closed ? "Couldn't open the CAB on the device" : "Couldn't finish writing the CAB on the device";
        error = action + std::string(" (RAPI ") + std::to_string(FrondRapiError(client)) + ").";
    }
    FrondRapiRemove(client, path.data());
    return false;
}
}  // namespace

bool ConfiguredActiveSyncUsesSerial() {
    auto& manager = SyncManager::Get();
    const auto options = manager.Options();
    if (options.machineKey.empty()) return false;
    SyncLibrary library;
    std::string error;
    if (!library.Open(manager.Root(), error)) return false;
    const auto key = MachineDeviceKey(options.machineKey);
    const auto assigned = library.FindAssignment(key);
    const auto selected = !options.profile.empty() ? library.FindProfile(options.profile)
                                                    : assigned ? library.FindProfile(assigned->profileId) : std::nullopt;
    SyncProfile profile;
    return selected && profile.Open(library.ProfilePath(*selected), error) && profile.ActiveSyncUsesSerial(key);
}

std::shared_ptr<SyncPipe> ActiveSyncSession::AcceptAirSync() {
    std::lock_guard lock(airMutex);
    if (!air) return nullptr;
    auto pipe = std::make_shared<SyncPipe>();
    airPipes.push_back(pipe);
    AirSyncServer* server = air;
    airThreads.emplace_back([server, pipe] {
        SyncPipe::End end(pipe);
        server->Serve(end);
        pipe->Close();
    });
    return pipe;
}

std::string ActiveSyncSession::SyncPocketOutlook(FrondRapi* client, SyncProfile& profile, const std::string& key,
                                                 const std::string& guid, const std::vector<PimKind>& kinds) {
    AirSyncServer server(profile, key, kinds);
    struct Close {
        void operator()(FILE* file) const { std::fclose(file); }
    };
    std::unique_ptr<FILE, Close> trace;
    // Qualification runs set FROND_AIRSYNC_TRACE to a file that gets every request and answer.
    if (const char* path = std::getenv("FROND_AIRSYNC_TRACE")) trace.reset(std::fopen(path, "a"));
    server.SetTrace(trace.get());
    std::string zoneSource;
    const auto zone = ReadActiveSyncTimeZone(client, &zoneSource);
    if (!FrondRapiConnected(client)) throw std::runtime_error("Windows Mobile disconnected.");
    server.SetDeviceTimeZone(zone);
    if (trace) std::fprintf(trace.get(), "time zone %s %s\n", zone.empty() ? "unknown" : zone.c_str(), zoneSource.c_str());
    {
        std::lock_guard lock(airMutex);
        air = &server;
    }
    std::string failure;
    try {
        StartActiveSyncDesktopSync(client, profile.PcId(), guid, kinds);
        // The guest connects about a second after the start and syncs each folder in turn.
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto idle = server.Idle();
            if (pipes[1]->Closed()) {
                failure = "Windows Mobile disconnected during Pocket Outlook sync.";
                break;
            }
            if (!server.Requests() && idle > std::chrono::seconds(60)) {
                failure = "Pocket Outlook didn't start syncing.";
                break;
            }
            if (server.Finished() && idle > std::chrono::seconds(3)) break;
            if (server.Requests() && idle > std::chrono::seconds(30)) {
                failure = "Pocket Outlook stopped before every folder synced.";
                break;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(20)) {
                failure = "Pocket Outlook sync took too long.";
                break;
            }
        }
    } catch (const std::exception& error) {
        failure = error.what();
    }
    std::vector<std::shared_ptr<SyncPipe>> open;
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(airMutex);
        air = nullptr;
        open.swap(airPipes);
        threads.swap(airThreads);
    }
    server.Stop();
    for (auto& pipe : open) pipe->Close();
    for (auto& thread : threads) thread.join();
    if (!server.LastError().empty()) profile.Log(key, "ActiveSync: Pocket Outlook: " + server.LastError());
    if (!failure.empty()) throw std::runtime_error(failure);
    std::string error;
    if (!server.FinishActions(error)) throw std::runtime_error(error);
    const auto counts = server.Counts();
    auto summary = "Pocket Outlook synced: " + std::to_string(counts.toDevice) + " to the device, " +
                   std::to_string(counts.fromDevice) + " from it, " + std::to_string(counts.deleted) + " deleted";
    if (counts.conflicts) summary += ", " + std::to_string(counts.conflicts) + " conflicts kept as copies";
    return summary + ".";
}

ActiveSyncSession::~ActiveSyncSession() { Stop(); }

void ActiveSyncSession::Start() {
    if (!listApplications) SyncManager::Get().Update([this](SyncStatus& status) {
        const auto count = status.syncs;
        status = {};
        status.syncs = count;
        status.state = SyncStatus::State::syncing;
        status.transport = serial ? "serial" : "usb";
        status.stage = "connecting";
    });
    running = true;
    worker = std::thread(&ActiveSyncSession::Run, this);
}

void ActiveSyncSession::Stop() {
    for (auto& pipe : pipes) pipe->Close();
    if (worker.joinable()) worker.join();
}

void ActiveSyncSession::ReadApplications(SyncStream& control, SyncStream& rapi) {
    try {
        Connect(control, rapi);
        auto client = OpenRapi(rapi);
        // Use the same read-only query as Remove Programs.
        const auto query = WideAscii(
            R"(<wap-provisioningdoc><characteristic-query type="UnInstall" recursive="false"/></wap-provisioningdoc>)");
        std::vector<uint16_t> reply(32768);
        if (!FrondRapiProvision(client.get(), query.data(), reply.data(), reply.size()))
            throw std::runtime_error("Couldn't read the installed programs from Windows Mobile.");
        applications = Utf8(reply.data());
    } catch (const std::exception& error) {
        applicationError = error.what();
    }
    control.Abort();
    rapi.Abort();
    running = false;
}

void ActiveSyncSession::Run() {
    auto& manager = SyncManager::Get();
    SyncPipe::End control(pipes[0]), rapi(pipes[1]);
    if (listApplications) {
        ReadApplications(control, rapi);
        return;
    }
    std::string message;
    bool ok = false;
    const auto progress = [&](const std::string& stage, int done, int total, const std::string& name) {
        manager.Update([&](SyncStatus& status) {
            status.stage = stage;
            status.done = done;
            status.total = total;
            status.item = name;
        });
    };
    try {
        const auto options = manager.Options();
        SyncLibrary library;
        std::string error;
        if (!library.Open(manager.Root(), error)) throw std::runtime_error(error);
        // Determine the requested profile before we connect so an unassigned
        // machine cannot send files into an arbitrary profile.
        const auto machine = options.machineKey.empty() ? std::string() : MachineDeviceKey(options.machineKey);
        const auto assigned = library.FindAssignment(machine);
        const auto chosen = !options.profile.empty() ? library.FindProfile(options.profile)
                                                    : assigned ? library.FindProfile(assigned->profileId) : std::nullopt;
        if (!chosen) throw std::runtime_error("Assign a sync profile to this device first.");
        ProfileSyncLock lock;
        if (!lock.Acquire(library.Root(), chosen->id, error)) throw std::runtime_error(error);
        const auto confirmed = library.FindProfile(chosen->id);
        if (!confirmed || confirmed->folder != chosen->folder)
            throw std::runtime_error("The sync profile changed.");
        SyncProfile profile;
        if (!profile.Open(library.ProfilePath(*confirmed), error)) throw std::runtime_error(error);
        const auto info = Connect(control, rapi);
        const auto key = machine.empty() ? "winmo:" + info.guid : machine;

        auto client = OpenRapi(rapi);

        const auto current = library.FindAssignment(key);
        if (assigned && (!current || current->profileId != assigned->profileId))
            throw std::runtime_error("The device's profile assignment changed.");
        Assignment connected = current.value_or(Assignment{});
        connected.deviceKey = key;
        connected.profileId = confirmed->id;
        connected.kind = "winmo";
        connected.userId = info.partner;
        connected.lastSync = NowSeconds();
        if (current && current->profileId == confirmed->id) {
            if (!library.FinishAssignment(connected, error)) throw std::runtime_error(error);
        } else if (!library.UpdateAssignment(connected, error)) {
            throw std::runtime_error(error);
        }
        manager.Update([&](SyncStatus& status) { status.profile = confirmed->name; });

        const auto pairing = profile.ActiveSyncPairing(key);
        bool partnershipMissing = false;
        if (pairing == "requested") {
            progress("partnership", 0, 0, {});
            connected.userId = EnsureActiveSyncPartnership(client.get(), profile.PcId(), info.guid);
            if (!library.FinishAssignment(connected, error) ||
                !profile.ConfirmActiveSyncPairing(key, error))
                throw std::runtime_error(error);
        } else if (pairing == "confirmed") {
            progress("partnership", 0, 0, {});
            partnershipMissing = !HasActiveSyncPartnership(client.get(), profile.PcId(), info.guid);
            if (partnershipMissing && !profile.ClearConfirmedActiveSyncPairing(key, error))
                throw std::runtime_error(error);
        }

        progress("files", 0, 0, {});
        std::string files;
        try {
            files = ExchangeActiveSyncFiles(client.get(), profile, key + ":" + info.guid, library.ProfilePath(*confirmed),
                                            profile.GetPimAction(key, PimKind::memos) != PimAction::skip);
        } catch (const std::exception& failure) {
            files = "File exchange failed: " + std::string(failure.what());
            profile.Log(key, "ActiveSync: " + files);
            if (!FrondRapiConnected(client.get())) throw std::runtime_error(files);
        }

        std::string outlook;
        if (pairing == "requested" || (pairing == "confirmed" && !partnershipMissing)) {
            std::vector<PimKind> kinds;
            for (const auto kind : {PimKind::contacts, PimKind::calendar, PimKind::tasks})
                if (profile.GetPimAction(key, kind) != PimAction::skip) kinds.push_back(kind);
            if (!kinds.empty()) {
                progress("pim", 0, 0, {});
                try {
                    outlook = SyncPocketOutlook(client.get(), profile, key, info.guid, kinds);
                } catch (const std::exception& error) {
                    outlook = "Pocket Outlook sync failed: " + std::string(error.what());
                    profile.Log(key, "ActiveSync: " + outlook);
                    if (!FrondRapiConnected(client.get())) throw std::runtime_error(outlook);
                }
            }
        }

        auto queue = profile.PendingInstalls(key);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [](const auto& item) { return item.target != "cab"; }), queue.end());
        int sent = 0;
        // wceload presents its result on the emulated screen. Don't start several
        // overlapping installs. The next queued CAB can be sent by the next sync.
        if (!queue.empty()) {
            const auto& item = queue.front();
            progress("install", 0, 1, item.fileName);
            Bytes cab;
            if (!profile.LoadInstall(item.id, cab)) throw std::runtime_error("Couldn't read the queued CAB.");
            if (!StageCab(client.get(), cab, item, error)) {
                profile.Log(key, "ActiveSync: " + item.fileName + ": " + error);
                throw std::runtime_error(error);
            }
            if (!profile.FinishInstall(item.id, true, "Sent to Windows Mobile installer"))
                throw std::runtime_error("The CAB was sent, but its queue entry couldn't be updated.");
            sent = 1;
        }
        message = sent ? "CAB sent. Finish installation on the device." : "Windows Mobile connected. No CABs queued.";
        if (partnershipMissing) message += " Desktop partnership missing. Pair again in sync settings.";
        else if (pairing == "requested" || pairing == "confirmed") message += " Desktop partnership verified.";
        if (!files.empty()) message += " " + files;
        if (!outlook.empty()) message += " " + outlook;
        profile.Log(key, "ActiveSync: " + message);
        ok = true;
    } catch (const std::exception& error) {
        message = error.what();
    }
    control.Abort();
    rapi.Abort();
    manager.Update([&](SyncStatus& status) {
        status.state = ok ? SyncStatus::State::done : SyncStatus::State::failed;
        status.message = message;
        ++status.syncs;
    });
    running = false;
}

}  // namespace frondSync
