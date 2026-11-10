#pragma once
#include <QObject>
#include <functional>
#include "DanaView.h"
class DesktopClient;

// Serialises view changes with the core's other commands and saves only accepted choices.
class DanaViewController : public QObject {
    Q_OBJECT
 public:
    explicit DanaViewController(DesktopClient& client, QObject* parent = nullptr);
    void configure(bool active, const frondDana::Preferences& prefs, double density);
    void choose(const frondDana::Preferences& prefs);
    void setDensity(double density);
    void suspend();
    bool finishChanges();
    frondDana::Preferences values() const { return desired; }
    std::function<QString(const frondDana::Preferences&)> save;
 signals:
    void changed();
    void error(const QString& message);
 private:
    void schedule();
    void flush();
    void state(const QString& text, bool reply);
    void reject(const QString& message, bool restore);
    DesktopClient& client;
    frondDana::Preferences persisted, desired;
    double density{1};
    bool active{false}, queued{false}, scheduled{false}, pending{false}, restoring{false}, rollbackQueued{false}, densityQueued{false}, query{false};
};
