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
    bool running() const { return process.state() != QProcess::NotRunning; }
    bool available() const { return ready && pending.isEmpty(); }
    void forceStop();
 signals:
    void started();
    void finished();
    void result(const QString& command, bool ok, const QString& code, const QString& message);
    void error(const QString& message);
    void closeRequested();
    void serialState(const QString& state);  // pushed by the core on a change
    void usbState(const QString& state);
    void infraredState(const QString& state);
    void bluetoothState(const QString& state);
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
    QByteArray input;
    QString pending;
    quint64 serial{0};
    bool ready{false}, expectingExit{false};
};
