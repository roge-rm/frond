#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

#include "FirmwareInput.h"
#include "sync/SyncStore.h"
#include "sync/InstallerFiles.h"

class QCheckBox;
class QComboBox;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace frondDesktopSync {
enum class Protocol { none, hotSync, activeSync };
Protocol protocol(frondFirmware::Device device);
bool available(Protocol protocol);
QString readMachineId(const QString& session);
QString machineId(const QString& session, QString& error);  // the caller owns the session lock
bool openLibrary(frondSync::SyncLibrary& library, QString& error);
bool editMachine(QWidget* parent, const QString& machineId, Protocol protocol, bool networkHotSync);
}

class SyncWindow : public QDialog {
    Q_OBJECT

 public:
    explicit SyncWindow(QWidget* parent = nullptr);
    void refresh();
    QHash<QString, QString> machineNames;

 private:
    void changeProfile();
    void nameProfile(bool rename);
    void deleteProfile();
    void showRecords();
    void editRecord(bool existing);
    void deleteRecords();
    void importRecords();
    void exportRecords();
    void showBackups();
    void showRecoveryCopies();
    std::string backupSource() const;
    void exportBackup();
    void restoreBackups();
    void showQueue();
    void queueFiles();
    void runWindowsInstaller();
    void showPalmConduits();
    void setPalmConduits(bool enabled);
    void choosePayloads(std::vector<frondSync::InstallerFile>& candidates, std::vector<frondSync::InstallerFile>& selected);
    void queuePayloads(const std::vector<frondSync::InstallerFile>& files, QStringList failed);
    void removeQueued();
    void showLog();
    frondSync::PimKind kind() const;
    frondSync::SyncLibrary library;
    frondSync::SyncProfile profile;
    int64_t profileId{0};
    QComboBox *profiles, *recordKind, *backupDevice, *recoveryCopy;
    QTabWidget* tabs;
    QCheckBox *showPrivate, *showArchived, *palmConduits;
    QTableWidget *records, *backups, *queue, *log;
    QPushButton *rename, *remove, *edit, *deleteRecord, *exportFile, *exportDb, *restore, *removeFiles;
};
