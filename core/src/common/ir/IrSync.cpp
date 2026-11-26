#include "IrSync.h"

#include <algorithm>

namespace frondIr {

IrLap::Options IrSync::DefaultLap() {
    IrLap::Options options;
    options.hints = {{0x84, 0x04}};
    options.nickname = "Frond";
    return options;
}

IrSync::IrSync(IrLap::Options options)
    : lmp(lap), lap(lmp, options), ias(lmp), comm(lmp, ias, *this, options.nickname) {
    endpoint.Start(frondSync::Transport::infrared);
}

std::string IrSync::Describe() const {
    const char* link = comm.GetMode() != IrComm::Mode::Closed ? "open" : lap.Connected() ? "linked" : "idle";
    return std::string("sync ") + link + "; " + frondSync::SyncManager::Get().Status().Describe();
}

void IrSync::Service(uint64_t now) {
    Flush();
    lap.Service(now);
}

// Bytes left from an earlier HotSync aren't the new one's. After a close the old session has ended
// its input, so the endpoint starts again with a fresh pipe and the next HotSync can't reach it.
void IrSync::Opened(uint64_t now) {
    pending.clear();
    auto* pipe = endpoint.Pipe();
    if (!pipe || pipe->Ended()) endpoint.Start(frondSync::Transport::infrared);
    else pipe->Clear();
}

// The guest closed IrCOMM or the link went: the Sync Manager reads what came, then sees the end, so
// a HotSync cut off midway fails instead of waiting for the guest.
void IrSync::Closed(uint64_t now) {
    Flush();
    pending.clear();
    if (auto* pipe = endpoint.Pipe()) pipe->FinishInput();
}

// Past pendingMax the excess is dropped, as a serial port overruns.
void IrSync::FromGuest(const uint8_t* data, size_t size, uint64_t now) {
    Flush();
    const size_t room = pendingMax - std::min(pendingMax, pending.size());
    pending.insert(pending.end(), data, data + std::min(size, room));
    Flush();
}

size_t IrSync::ToGuest(uint8_t* data, size_t max, uint64_t now) {
    Flush();
    auto* pipe = endpoint.Pipe();
    return pipe ? pipe->HostToDevice(data, max) : 0;
}

void IrSync::Flush() {
    auto* pipe = endpoint.Pipe();
    if (!pipe) return pending.clear();
    uint8_t buffer[1024];
    while (!pending.empty()) {
        const size_t count = std::min(pending.size(), sizeof(buffer));
        std::copy_n(pending.begin(), count, buffer);
        const size_t done = pipe->DeviceToHost(buffer, count);
        if (!done) break;
        pending.erase(pending.begin(), pending.begin() + long(done));
    }
}

}  // namespace frondIr
