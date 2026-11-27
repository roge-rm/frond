#pragma once

#include <QDialog>
#include <QTimer>

class DesktopClient;
class QLabel;
class QListWidget;
class QPushButton;

class ZaurusPcLinkDialog : public QDialog {
    Q_OBJECT
 public:
    ZaurusPcLinkDialog(DesktopClient& client, QWidget* parent);
    void browse();
    void machineStopped();
    void pump();
    void reply(const QString& command, bool ok, const QString& code, const QString& message);
 protected:
    void done(int result) override;
 private:
    enum class Connection { disconnected, opening, ready, closing };
    enum class Job { none, list, send, receive };
    void connectCable();
    void list(const QString& folder);
    void sendFile();
    void receiveFile();
    void start(Job job, const QString& command, const QString& argument, const QString& status);
    void showListing(const QString& listing);
    void updateButtons();
    QString pathFor(const QString& name) const;

    DesktopClient& client;
    QTimer timer;
    Connection connection{Connection::disconnected};
    Job job{Job::none};
    bool haveListing{false};
    QString folder, requestedFolder;
    QLabel *path, *status;
    QListWidget* entries;
    QPushButton *up, *refresh, *send, *receive;
};
