#include "DanaViewController.h"
#include "DesktopClient.h"
#include <QTimer>

DanaViewController::DanaViewController(DesktopClient& client, QObject* parent) : QObject(parent), client(client) {
    connect(&client, &DesktopClient::started, this, &DanaViewController::schedule);
    connect(&client, &DesktopClient::finished, this, [this] { suspend(); });
    connect(&client, &DesktopClient::danaViewState, this, [this](const QString& text) {
        // A setter's reply contains the same state as its notification.
        if (active && !pending && !rollbackQueued) state(text, false);
    });
    connect(&client, &DesktopClient::result, this, [this](const QString& command, bool ok, const QString&, const QString& message) {
        if (active && (command == "dana-view" || command == "dana-view-state")) {
            pending = false;
            if (!ok) reject(message, false);
            else state(message, true);
        }
        schedule();
    });
}
void DanaViewController::configure(bool enabled, const frondDana::Preferences& prefs, double hostDensity) {
    active = enabled; persisted = desired = prefs; density = hostDensity;
    queued = pending = restoring = rollbackQueued = densityQueued = false; query = enabled;
    emit changed(); schedule();
}
bool DanaViewController::finishChanges() {
    query = false;
    flush();
    return active && (pending || queued || rollbackQueued);
}
void DanaViewController::suspend() { active = queued = pending = restoring = rollbackQueued = densityQueued = query = false; }
void DanaViewController::choose(const frondDana::Preferences& prefs) {
    if (!active || !frondDana::valid(prefs)) return;
    desired = prefs; queued = true; emit changed(); schedule();
}
void DanaViewController::setDensity(double value) {
    if (!std::isfinite(value) || value <= 0 || value > 16 || value == density) return;
    density = value;
    if (active) { queued = densityQueued = true; schedule(); }
}
void DanaViewController::schedule() {
    if (scheduled) return;
    scheduled = true;
    QTimer::singleShot(0, this, [this] { scheduled = false; flush(); });
}
void DanaViewController::flush() {
    if (!active || pending || !client.available()) return;
    if (!queued && !rollbackQueued && !query) return;
    const bool setting = queued || rollbackQueued;
    restoring = rollbackQueued;
    pending = true;
    if (!client.request(setting ? "dana-view" : "dana-view-state", setting ? frondDana::argument(restoring ? persisted : desired, density) : QString())) {
        pending = false;
        reject(tr("Couldn't change the Dana view."), false);
        return;
    }
    if (restoring) rollbackQueued = false;
    else queued = false;
    query = densityQueued = false;
    emit changed();
}
void DanaViewController::reject(const QString& message, bool restore) {
    desired = persisted; query = queued = restoring = false; rollbackQueued = restore;
    emit changed(); emit error(message);
    schedule();
}
void DanaViewController::state(const QString& text, bool reply) {
    frondDana::Preferences prefs;
    double nativeDensity = density;
    if (!frondDana::parseState(text, &prefs, &nativeDensity)) {
        // A malformed successful reply may already have changed the native view.
        reject(tr("The emulator sent an invalid Dana view."), reply && !restoring);
        return;
    }
    if (!densityQueued) density = nativeDensity;
    if (restoring) { restoring = false; if (!queued) desired = persisted; emit changed(); return; }
    if (prefs != persisted) {
        const auto failure = save ? save(prefs) : QString();
        if (!failure.isEmpty()) { reject(failure, true); return; }
        persisted = prefs;
    }
    if (!queued) desired = prefs;
    emit changed();
}
