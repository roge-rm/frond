#pragma once
#include <memory>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

class DesktopClient : public QObject {
    Q_OBJECT
 public:
    explicit DesktopClient(QObject* parent = nullptr);
    ~DesktopClient();
    bool start(const QString& core, const QStringList& arguments);
    bool request(const QString& command, const QString& argument = {});
    // A read-only query that doesn't take the request slot: available() and the pending command are
    // untouched, and the answer comes as backgroundResult. Sent only when the slot is free and no other
    // background query is out.
    bool requestBackground(const QString& command);
    bool backgroundOut() const { return backgroundId != 0; }
    bool cancelFiles();
    bool running() const { return process.state() != QProcess::NotRunning; }
    bool available() const { return ready && pending.isEmpty(); }
    void forceStop();
 signals:
    void started();
    void finished();
    void result(const QString& command, bool ok, const QString& code, const QString& message);
    void backgroundResult(const QString& command, bool ok, const QString& message);
    void error(const QString& message);
    void closeRequested();
    void shortcut(const QString& name);  // a key pressed over the Palm that the app owns
    void microphoneState(const QString& state);
    void serialState(const QString& state);  // pushed by the core on a change
    void usbState(const QString& state);
    void infraredState(const QString& state);
    void bluetoothState(const QString& state);
    void sdProgress(qint64 done, qint64 total);  // SD create, import and export
    void danaViewState(const QString& state);
    void cardState(const QString& state);  // a card put in while the machine was off went in
    void stalled();
    void diagnostic(const QString& text);
 private:
    void receive();
    void fail(const QString& message);
    QProcess process;
    QLocalServer server;
    QLocalSocket* socket{nullptr};
    std::unique_ptr<QTemporaryDir> directory;
    QTimer deadline;
    QTimer lostConnection;  // the socket closed while the core still runs
    QByteArray input;
    QString pending;
    quint64 serial{0}, backgroundId{0};
    QString backgroundCommand;
    bool ready{false}, expectingExit{false};
};
