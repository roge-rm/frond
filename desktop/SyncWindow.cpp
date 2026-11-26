#include "SyncWindow.h"

#include <set>
#include <algorithm>
#include <array>
#include <functional>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include "SyncFiles.h"
#include "InstallerRunner.h"
#include "sync/InstallerFiles.h"
#include "sync/PalmConduitStorage.h"
#include "SessionStore.h"

using namespace frondSync;

namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString when(int64_t seconds) {
    return seconds ? QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds).toLocalTime(), QLocale::ShortFormat) : QString();
}

void showError(QWidget* parent, const QString& error) {
    QMessageBox::warning(parent, QObject::tr("Sync Manager"), error);
}

const std::array<const char*, 4> kindNames{
    QT_TRANSLATE_NOOP("SyncWindow", "Contacts"), QT_TRANSLATE_NOOP("SyncWindow", "Calendar"),
    QT_TRANSLATE_NOOP("SyncWindow", "Tasks"), QT_TRANSLATE_NOOP("SyncWindow", "Memos")};
const std::array<const char*, 4> actionNames{
    QT_TRANSLATE_NOOP("SyncWindow", "Synchronize"), QT_TRANSLATE_NOOP("SyncWindow", "Computer overwrites handheld"),
    QT_TRANSLATE_NOOP("SyncWindow", "Handheld overwrites computer"), QT_TRANSLATE_NOOP("SyncWindow", "Do nothing")};

QString kindName(PimKind kind) { return QCoreApplication::translate("SyncWindow", kindNames[size_t(kind)]); }

QString field(const PimRecord& record, const char* key) {
    const auto item = record.fields.find(key);
    return item == record.fields.end() ? QString() : text(item->second);
}

QString recordName(const PimRecord& record) {
    switch (record.kind) {
        case PimKind::contacts: {
            const auto name = (field(record, "firstName") + ' ' + field(record, "lastName")).trimmed();
            return name.isEmpty() ? field(record, "company") : name;
        }
        case PimKind::calendar:
        case PimKind::tasks: return field(record, "summary");
        case PimKind::memos: return field(record, "text").section('\n', 0, 0).left(160);
    }
    return {};
}

QTableWidget* table(QWidget* parent, const QStringList& headers) {
    auto* out = new QTableWidget(0, headers.size(), parent);
    out->setHorizontalHeaderLabels(headers);
    out->setSelectionBehavior(QAbstractItemView::SelectRows);
    out->setSelectionMode(QAbstractItemView::ExtendedSelection);
    out->setEditTriggers(QAbstractItemView::NoEditTriggers);
    out->verticalHeader()->hide();
    out->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    out->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    return out;
}

void addRow(QTableWidget* table, const QStringList& columns, const QVariant& id, const QVariant& extra = {}) {
    const auto row = table->rowCount();
    table->insertRow(row);
    for (int i = 0; i < columns.size(); ++i) {
        auto* cell = new QTableWidgetItem(columns[i]);
        cell->setToolTip(columns[i]);
        if (i == 0) {
            cell->setData(Qt::UserRole, id);
            cell->setData(Qt::UserRole + 1, extra);
        }
        table->setItem(row, i, cell);
    }
}

QList<QTableWidgetItem*> selectedRows(QTableWidget* table) {
    QList<QTableWidgetItem*> rows;
    for (const auto& index : table->selectionModel()->selectedRows()) rows << table->item(index.row(), 0);
    return rows;
}

QPushButton* addButton(QWidget* parent, QHBoxLayout* layout, const QString& label, const char* name,
                       const std::function<void()>& callback) {
    auto* button = new QPushButton(label, parent);
    button->setObjectName(name);
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, parent, callback);
    return button;
}

bool openProfile(SyncLibrary& library, SyncProfile& profile, int64_t id, QString& error) {
    const auto info = library.FindProfile(id);
    if (!info) { error = QObject::tr("Choose a profile."); return false; }
    std::string problem;
    if (profile.Open(library.ProfilePath(*info), problem)) return true;
    error = text(problem);
    return false;
}

QString deviceName(const QHash<QString, QString>& machineNames, const std::vector<Assignment>& assignments, const std::string& key) {
    if (key.empty()) return QObject::tr("Next device to sync");
    const auto known = machineNames.constFind(text(key));
    if (known != machineNames.constEnd()) return known.value();
    for (const auto& item : assignments)
        if (item.deviceKey == key && !item.userName.empty()) return text(item.userName);
    return text(key);
}

QString backupFilename(const QString& base, bool resource) {
    auto name = base;
    for (auto forbidden : QStringLiteral("\\/:*?\"<>|")) name.replace(forbidden, '_');
    return name + (resource ? ".prc" : ".pdb");
}

struct EditField {
    QByteArray key;
    QString label;
    bool multiline{false};
};

std::vector<EditField> editFields(PimKind kind) {
    const auto tr = [](const char* label) { return QCoreApplication::translate("SyncWindow", label); };
    switch (kind) {
        case PimKind::contacts: {
            std::vector<EditField> fields{{"firstName", tr("First name")}, {"lastName", tr("Last name")},
                                          {"company", tr("Company")}, {"title", tr("Job title")}};
            for (int i = 1; i <= 7; ++i) fields.push_back({"phone" + QByteArray::number(i), tr("Phone or email %1").arg(i)});
            for (int i = 1; i <= 3; ++i) {
                const auto suffix = i == 1 ? QByteArray{} : QByteArray::number(i);
                const auto label = [&](const char* name) { return i == 1 ? tr(name) : tr("%1 (%2)").arg(tr(name)).arg(i); };
                for (const auto& [key, name] : {std::pair{"street", QT_TRANSLATE_NOOP("SyncWindow", "Street")},
                     std::pair{"city", QT_TRANSLATE_NOOP("SyncWindow", "City")},
                     std::pair{"state", QT_TRANSLATE_NOOP("SyncWindow", "Province or state")},
                     std::pair{"postalCode", QT_TRANSLATE_NOOP("SyncWindow", "Postal code")},
                     std::pair{"country", QT_TRANSLATE_NOOP("SyncWindow", "Country")}})
                    fields.push_back({key + suffix, label(name)});
            }
            fields.push_back({"birthday", tr("Birthday (YYYY-MM-DD)")});
            fields.push_back({"webpage", tr("Website")});
            fields.push_back({"chat1", tr("Chat 1")});
            fields.push_back({"chat2", tr("Chat 2")});
            for (int i = 1; i <= 9; ++i) fields.push_back({"custom" + QByteArray::number(i), tr("Custom %1").arg(i)});
            fields.push_back({"note", tr("Note"), true});
            return fields;
        }
        case PimKind::calendar:
            return {{"summary", tr("Summary")}, {"date", tr("Date (YYYY-MM-DD)")}, {"startTime", tr("Start (HH:MM)")},
                    {"endTime", tr("End (HH:MM)")}, {"location", tr("Location")}, {"note", tr("Note"), true}};
        case PimKind::tasks:
            return {{"summary", tr("Summary")}, {"dueDate", tr("Due (YYYY-MM-DD)")}, {"priority", tr("Priority")},
                    {"completionDate", tr("Completed on (YYYY-MM-DD)")}, {"note", tr("Note"), true}};
        case PimKind::memos: return {{"text", tr("Text"), true}};
    }
    return {};
}

class RecordEditor : public QDialog {
 public:
    RecordEditor(PimRecord value, QWidget* parent) : QDialog(parent), record(std::move(value)) {
        setWindowTitle(record.id ? tr("Edit record") : tr("New record"));
        auto* layout = new QVBoxLayout(this);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll);
        auto* form = new QFormLayout(content);
        category = new QLineEdit(text(record.category), content);
        form->addRow(tr("Category"), category);
        secret = new QCheckBox(tr("Private"), content);
        secret->setChecked(record.secret);
        form->addRow(QString(), secret);
        archived = new QCheckBox(tr("Archived"), content);
        archived->setChecked(record.archived);
        form->addRow(QString(), archived);
        if (record.kind == PimKind::tasks) {
            complete = new QCheckBox(tr("Completed"), content);
            complete->setChecked(field(record, "complete") == "1");
            form->addRow(QString(), complete);
        }
        for (const auto& f : editFields(record.kind)) {
            const auto value = field(record, f.key);
            if (f.multiline) {
                auto* area = new QPlainTextEdit(value, content);
                area->setObjectName(QString::fromLatin1(f.key));
                form->addRow(f.label, area);
                fields.push_back({f.key, [area] { return area->toPlainText(); }});
            } else {
                auto* input = new QLineEdit(value, content);
                input->setObjectName(QString::fromLatin1(f.key));
                if (record.kind == PimKind::contacts && f.key.startsWith("phone")) {
                    auto* row = new QHBoxLayout;
                    row->addWidget(input, 1);
                    auto* type = new QComboBox(content);
                    type->addItems({tr("Work"), tr("Home"), tr("Fax"), tr("Other"), tr("Email"),
                                    tr("Main"), tr("Pager"), tr("Mobile")});
                    const auto key = "phoneLabel" + f.key.mid(5);
                    const auto old = field(record, key).toInt();
                    if (old >= type->count()) type->addItem(tr("Custom"));
                    type->setCurrentIndex(std::min(old, type->count() - 1));
                    row->addWidget(type);
                    form->addRow(f.label, row);
                    fields.push_back({key, [input, type, old] {
                        if (input->text().isEmpty()) return QString();
                        return QString::number(type->currentIndex() == 8 ? old : type->currentIndex());
                    }});
                } else form->addRow(f.label, input);
                fields.push_back({f.key, [input] { return input->text(); }});
            }
        }
        scroll->setWidget(content);
        layout->addWidget(scroll, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &RecordEditor::save);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        resize(485, record.kind == PimKind::contacts ? 550 : 390);
    }

    PimRecord record;

 private:
    void save() {
        auto fresh = record;
        fresh.category = category->text().trimmed().toStdString();
        fresh.secret = secret->isChecked();
        fresh.archived = archived->isChecked();
        if (complete) fresh.fields["complete"] = complete->isChecked() ? "1" : "0";
        for (const auto& f : fields) {
            const auto value = f.value();
            const auto key = f.key.toStdString();
            if (!value.isEmpty()) fresh.fields[key] = value.toStdString();
            else if (!field(record, f.key).isEmpty()) fresh.fields.erase(key);
        }
        if (fresh.kind == PimKind::calendar && field(fresh, "date").isEmpty()) {
            showError(this, tr("Enter a date for the event."));
            return;
        }
        if (fresh.kind == PimKind::calendar) {
            const auto start = field(fresh, "startTime"), end = field(fresh, "endTime");
            if (start.isEmpty() != end.isEmpty() || (!start.isEmpty() && end < start)) {
                showError(this, tr("Enter both times in order, or leave both empty for an all-day event."));
                return;
            }
        }
        // Check the same date, time and value rules used on the wire. Unicode text is allowed in the profile.
        for (const auto& [key, value] : fresh.fields) {
            auto view = text(value);
            if (view.isEmpty()) continue;
            const auto name = text(key);
            if (name == "date" || name == "birthday" || name == "dueDate" || name == "completionDate") {
                const auto date = QDate::fromString(view, "yyyy-MM-dd");
                if (!date.isValid() || date.year() < 1904 || date.year() > 2031) {
                    showError(this, tr("Enter a date between 1904 and 2031 as YYYY-MM-DD."));
                    return;
                }
            } else if ((name == "startTime" || name == "endTime") && !QTime::fromString(view, "HH:mm").isValid()) {
                showError(this, tr("Enter a time as HH:MM."));
                return;
            } else if (name == "priority") {
                bool ok;
                const auto value = view.toUInt(&ok);
                if (!ok || value > 127) { showError(this, tr("Enter a priority from 0 to 127.")); return; }
            }
        }
        record = std::move(fresh);
        accept();
    }

    struct FieldInput { QByteArray key; std::function<QString()> value; };
    std::vector<FieldInput> fields;
    QLineEdit* category;
    QCheckBox *secret, *archived, *complete{nullptr};
};
}  // namespace

namespace frondDesktopSync {

Protocol protocol(frondFirmware::Device device) {
    if (device == frondFirmware::Device::automatic) return Protocol::none;
    if (int(device) == deviceTypeRx1950) return Protocol::activeSync;
    const auto caps = frondFirmware::capabilities(device);
    return frondFirmware::hasHotSyncButton(device) && (caps.serial || caps.usb) ? Protocol::hotSync : Protocol::none;
}

bool available(Protocol protocol) {
    return protocol != Protocol::none;
}

QString readMachineId(const QString& session) {
    QFile file(frondSessions::syncIdPath(session));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QUuid id(QString::fromLatin1(file.read(80)).trimmed());
    return !id.isNull() && file.atEnd() ? id.toString(QUuid::WithoutBraces) : QString();
}

QString machineId(const QString& session, QString& error) {
    const auto path = frondSessions::syncIdPath(session);
    if (QFileInfo::exists(path)) {
        const auto id = readMachineId(session);
        if (id.isEmpty()) error = QObject::tr("Couldn't read this device's sync ID.");
        return id;
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto bytes = id.toLatin1() + '\n';
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        error = QObject::tr("Couldn't save this device's sync ID. %1").arg(output.errorString());
        return {};
    }
    return id;
}

bool openLibrary(SyncLibrary& library, QString& error) {
    std::string problem;
    if (library.Open(DefaultSyncRoot(), problem)) return true;
    error = text(problem);
    return false;
}

bool editMachine(QWidget* parent, const QString& id, Protocol protocol, bool networkHotSync) {
    if (id.isEmpty()) return false;
    SyncLibrary library;
    QString error;
    if (!openLibrary(library, error)) { showError(parent, error); return false; }
    const auto device = MachineDeviceKey(id.toStdString());
    const auto assignment = library.FindAssignment(device);
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Sync settings"));
    dialog.setObjectName("syncSettings");
    auto* layout = new QFormLayout(&dialog);
    auto* profiles = new QComboBox(&dialog);
    profiles->setObjectName("syncAssignment");
    auto* profileRow = new QHBoxLayout;
    profileRow->addWidget(profiles, 1);
    auto* create = new QPushButton(QObject::tr("New profile…"), &dialog);
    profileRow->addWidget(create);
    layout->addRow(QObject::tr("Profile"), profileRow);
    auto load = [&](int64_t wanted) {
        const QSignalBlocker blocked(profiles);
        profiles->clear();
        profiles->addItem(QObject::tr("None"), QVariant::fromValue<qlonglong>(0));
        for (const auto& p : library.Profiles()) profiles->addItem(text(p.name), QVariant::fromValue<qlonglong>(p.id));
        profiles->setCurrentIndex(std::max(0, profiles->findData(QVariant::fromValue<qlonglong>(wanted))));
    };
    load(assignment ? assignment->profileId : 0);
    const bool palm = protocol != Protocol::activeSync;
    auto* transport = !palm || networkHotSync ? new QComboBox(&dialog) : nullptr;
    if (transport) {
        transport->setObjectName("syncConnection");
        if (palm) transport->addItems({QObject::tr("USB or serial"), QObject::tr("Network")});
        else transport->addItems({QObject::tr("USB"), QObject::tr("Serial")});
        layout->addRow(QObject::tr("Connection"), transport);
    }
    std::array<QComboBox*, 4> actions{};
    for (int i = 0; i < int(actions.size()); ++i) {
        if (!palm) break;
        actions[i] = new QComboBox(&dialog);
        for (const auto* label : actionNames) actions[i]->addItem(QCoreApplication::translate("SyncWindow", label));
        layout->addRow(kindName(PimKind(i)), actions[i]);
    }
    auto* pairStatus = !palm ? new QLabel(&dialog) : nullptr;
    auto* pairButton = !palm ? new QPushButton(&dialog) : nullptr;
    auto* enableFiles = !palm ? new QCheckBox(QObject::tr("Enable file exchange"), &dialog) : nullptr;
    auto* openFiles = !palm ? new QPushButton(QObject::tr("Open folder…"), &dialog) : nullptr;
    std::string originalPairing;
    std::string pairing;
    std::string profileFiles;
    if (!palm) {
        auto* partnershipRow = new QHBoxLayout;
        partnershipRow->addWidget(pairStatus, 1);
        partnershipRow->addWidget(pairButton);
        layout->addRow(QObject::tr("Partnership"), partnershipRow);
        auto* filesRow = new QHBoxLayout;
        filesRow->addWidget(enableFiles);
        filesRow->addWidget(openFiles);
        layout->addRow(QObject::tr("Profile files"), filesRow);
        pairButton->setObjectName("syncPairDevice");
        enableFiles->setObjectName("syncEnableFiles");
    }
    auto showPairing = [&] {
        if (pairing == "confirmed") {
            pairStatus->setText(QObject::tr("Paired"));
        } else if (pairing == "requested") {
            pairStatus->setText(QObject::tr("Pending next sync"));
        } else {
            pairStatus->setText(QObject::tr("No Frond partnership"));
        }
        pairButton->setText(pairing == "requested" ? QObject::tr("Cancel request") : QObject::tr("Pair on next sync"));
        pairButton->setVisible(pairing != "confirmed");
    };
    if (!palm) {
        QObject::connect(pairButton, &QPushButton::clicked, &dialog, [&] {
            pairing = pairing == "requested" ? "" : "requested";
            showPairing();
        });
        QObject::connect(openFiles, &QPushButton::clicked, &dialog, [&] {
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(text(profileFiles))))
                showError(&dialog, QObject::tr("Couldn't open the profile Files folder."));
        });
    }
    auto showActions = [&] {
        SyncProfile profile;
        const auto id = profiles->currentData().toLongLong();
        const bool opened = id && openProfile(library, profile, id, error);
        if (id && !opened) showError(&dialog, error);
        if (transport) {
            transport->setEnabled(opened);
            if (!palm) {
                transport->setCurrentIndex(opened && profile.ActiveSyncUsesSerial(device) ? 1 : 0);
                originalPairing = opened ? profile.ActiveSyncPairing(device) : "";
                pairing = originalPairing;
                showPairing();
                pairStatus->setEnabled(opened);
                pairButton->setEnabled(opened);
                try {
                    const auto info = opened ? library.FindProfile(id) : std::nullopt;
                    profileFiles = info ? ActiveSyncFilesFolder(library.ProfilePath(*info)) : "";
                } catch (const std::exception& error) {
                    profileFiles.clear();
                    showError(&dialog, text(error.what()));
                }
                enableFiles->setEnabled(opened);
                enableFiles->setChecked(false);
                enableFiles->setVisible(profileFiles.empty());
                openFiles->setVisible(!profileFiles.empty());
                return;
            }
            transport->setCurrentIndex(opened && profile.HotSyncUsesNetwork(device) ? 1 : 0);
        }
        for (int i = 0; i < int(actions.size()); ++i) {
            actions[i]->setEnabled(opened);
            actions[i]->setCurrentIndex(opened ? int(profile.GetPimAction(device, PimKind(i))) : 0);
        }
    };
    showActions();
    QObject::connect(profiles, &QComboBox::currentIndexChanged, &dialog, showActions);
    QObject::connect(create, &QPushButton::clicked, &dialog, [&] {
        bool ok;
        const auto name = QInputDialog::getText(&dialog, QObject::tr("New profile"), QObject::tr("Name"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        ProfileInfo created;
        std::string problem;
        if (!library.CreateProfile(name.toStdString(), created, problem)) { showError(&dialog, text(problem)); return; }
        load(created.id);
        showActions();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto id = profiles->currentData().toLongLong();
        std::string problem;
        if (id) {
            ProfileSyncLock lock;
            if (!lock.Acquire(library.Root(), id, problem)) { showError(&dialog, text(problem)); return; }
            SyncProfile profile;
            if (!openProfile(library, profile, id, error)) { showError(&dialog, error); return; }
            if (palm && !profile.BeginPimChange(problem)) { showError(&dialog, text(problem)); return; }
            for (int i = 0; palm && i < int(actions.size()); ++i)
                if (!profile.SetPimAction(device, PimKind(i), PimAction(actions[i]->currentIndex()), problem)) {
                    profile.RollBackPimChange();
                    showError(&dialog, text(problem));
                    return;
                }
            if (palm && transport && !profile.SetHotSyncNetwork(device, transport->currentIndex() == 1, problem)) {
                profile.RollBackPimChange();
                showError(&dialog, text(problem));
                return;
            }
            if (palm && !profile.CommitPimChange(problem)) {
                profile.RollBackPimChange();
                showError(&dialog, text(problem));
                return;
            }
            if (!palm) {
                if (enableFiles->isChecked()) {
                    const auto info = library.FindProfile(id);
                    if (!info) { showError(&dialog, QObject::tr("This profile is unavailable.")); return; }
                    try {
                        ActiveSyncFilesFolder(library.ProfilePath(*info), true);
                    } catch (const std::exception& error) {
                        showError(&dialog, text(error.what()));
                        return;
                    }
                }
                if (!profile.BeginPimChange(problem)) { showError(&dialog, text(problem)); return; }
                if (!profile.SetActiveSyncSerial(device, transport->currentIndex() == 1, problem) ||
                    (pairing != originalPairing && !profile.RequestActiveSyncPairing(device, pairing == "requested", problem)) ||
                    !profile.CommitPimChange(problem)) {
                    profile.RollBackPimChange();
                    showError(&dialog, text(problem));
                    return;
                }
            }
            if (!library.Assign(device, id, problem, palm ? "palm" : "winmo")) { showError(&dialog, text(problem)); return; }
        } else if (!library.Unassign(device, problem)) { showError(&dialog, text(problem)); return; }
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}
}  // namespace frondDesktopSync

SyncWindow::SyncWindow(QWidget* parent) : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Sync Manager"));
    setObjectName("syncManager");
    auto* layout = new QVBoxLayout(this);
    auto* profileRow = new QHBoxLayout;
    layout->addLayout(profileRow);
    auto* label = new QLabel(tr("&Profile"), this);
    profileRow->addWidget(label);
    profiles = new QComboBox(this);
    profiles->setObjectName("syncProfiles");
    label->setBuddy(profiles);
    profileRow->addWidget(profiles, 1);
    addButton(this, profileRow, tr("New…"), "profileNew", [this] { nameProfile(false); });
    rename = addButton(this, profileRow, tr("Rename…"), "profileRename", [this] { nameProfile(true); });
    remove = addButton(this, profileRow, tr("Delete…"), "profileDelete", [this] { deleteProfile(); });
    addButton(this, profileRow, tr("Refresh"), "syncRefresh", [this] { refresh(); });
    tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);

    auto* data = new QWidget(tabs);
    auto* dataLayout = new QVBoxLayout(data);
    auto* kinds = new QHBoxLayout;
    dataLayout->addLayout(kinds);
    recordKind = new QComboBox(data);
    recordKind->setObjectName("syncRecordKind");
    for (int i = 0; i < int(kindNames.size()); ++i) recordKind->addItem(kindName(PimKind(i)));
    kinds->addWidget(recordKind);
    kinds->addStretch();
    showPrivate = new QCheckBox(tr("Show private"), data);
    showArchived = new QCheckBox(tr("Show archived"), data);
    kinds->addWidget(showPrivate);
    kinds->addWidget(showArchived);
    records = table(data, {tr("Name"), tr("Category"), tr("Date"), tr("State")});
    records->setObjectName("syncRecords");
    dataLayout->addWidget(records, 1);
    auto* recordButtons = new QHBoxLayout;
    dataLayout->addLayout(recordButtons);
    addButton(data, recordButtons, tr("New…"), "recordNew", [this] { editRecord(false); });
    edit = addButton(data, recordButtons, tr("Edit…"), "recordEdit", [this] { editRecord(true); });
    deleteRecord = addButton(data, recordButtons, tr("Delete…"), "recordDelete", [this] { deleteRecords(); });
    recordButtons->addStretch();
    addButton(data, recordButtons, tr("Import…"), "recordImport", [this] { importRecords(); });
    exportFile = addButton(data, recordButtons, tr("Export…"), "recordExport", [this] { exportRecords(); });
    tabs->addTab(data, tr("Data"));

    auto* backup = new QWidget(tabs);
    auto* backupLayout = new QVBoxLayout(backup);
    auto* source = new QFormLayout;
    backupLayout->addLayout(source);
    backupDevice = new QComboBox(backup);
    backupDevice->setObjectName("syncBackupDevice");
    source->addRow(tr("Device"), backupDevice);
    recoveryCopy = new QComboBox(backup);
    recoveryCopy->setObjectName("syncRecoveryCopy");
    source->addRow(tr("Recovery copies"), recoveryCopy);
    backups = table(backup, {tr("Database"), tr("Size"), tr("Backed up")});
    backups->setObjectName("syncBackups");
    backupLayout->addWidget(backups, 1);
    auto* backupButtons = new QHBoxLayout;
    backupLayout->addLayout(backupButtons);
    backupButtons->addStretch();
    exportDb = addButton(backup, backupButtons, tr("Export database…"), "backupExport", [this] { exportBackup(); });
    restore = addButton(backup, backupButtons, tr("Queue restore…"), "backupRestore", [this] { restoreBackups(); });
    tabs->addTab(backup, tr("Backups"));

    auto* installs = new QWidget(tabs);
    auto* installLayout = new QVBoxLayout(installs);
    queue = table(installs, {tr("File"), tr("Device"), tr("State"), tr("Details")});
    queue->setObjectName("syncInstallQueue");
    installLayout->addWidget(queue, 1);
    auto* installButtons = new QHBoxLayout;
    installLayout->addLayout(installButtons);
    palmConduits = new QCheckBox(tr("Run Palm conduits"), installs);
    palmConduits->setObjectName("profilePalmConduits");
    palmConduits->hide();
    installButtons->addWidget(palmConduits);
    installButtons->addStretch();
    addButton(installs, installButtons, tr("Queue files…"), "installQueue", [this] { queueFiles(); });
    auto* runInstaller = addButton(installs, installButtons, tr("Run Windows installer…"), "runInstaller",
                                   [this] { runWindowsInstaller(); });
    runInstaller->setEnabled(frondInstallerRunner::available());
    removeFiles = addButton(installs, installButtons, tr("Remove"), "installRemove", [this] { removeQueued(); });
    tabs->addTab(installs, tr("Install queue"));

    log = table(tabs, {tr("Message"), tr("Device"), tr("Date")});
    log->setObjectName("syncLog");
    tabs->addTab(log, tr("Log"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(profiles, &QComboBox::currentIndexChanged, this, &SyncWindow::changeProfile);
    connect(recordKind, &QComboBox::currentIndexChanged, this, &SyncWindow::showRecords);
    connect(showPrivate, &QCheckBox::toggled, this, &SyncWindow::showRecords);
    connect(showArchived, &QCheckBox::toggled, this, &SyncWindow::showRecords);
    connect(palmConduits, &QCheckBox::toggled, this, &SyncWindow::setPalmConduits);
    connect(backupDevice, &QComboBox::currentIndexChanged, this, &SyncWindow::showRecoveryCopies);
    connect(recoveryCopy, &QComboBox::currentIndexChanged, this, &SyncWindow::showBackups);
    connect(records, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto count = selectedRows(records).size();
        edit->setEnabled(count == 1);
        deleteRecord->setEnabled(count != 0);
    });
    connect(records, &QTableWidget::cellDoubleClicked, this, [this] { editRecord(true); });
    connect(backups, &QTableWidget::itemSelectionChanged, this, [this] { exportDb->setEnabled(selectedRows(backups).size() == 1); });
    connect(queue, &QTableWidget::itemSelectionChanged, this, [this] { removeFiles->setEnabled(!selectedRows(queue).isEmpty()); });
    resize(760, 510);
}

void SyncWindow::setDeviceKind(const std::string& newKind) {
    deviceKind = newKind;
    tabs->setTabVisible(1, deviceKind != "winmo");
    if (!tabs->isTabVisible(tabs->currentIndex())) tabs->setCurrentIndex(0);
    if (library.Root().empty()) return;
    showPalmConduits();
    showQueue();
}

PimKind SyncWindow::kind() const { return PimKind(recordKind->currentIndex()); }

void SyncWindow::refresh() {
    QString problem;
    if (library.Root().empty() && !frondDesktopSync::openLibrary(library, problem)) {
        tabs->setEnabled(false);
        showError(this, problem);
        return;
    }
    {
        const QSignalBlocker blocked(profiles);
        profiles->clear();
        for (const auto& p : library.Profiles()) profiles->addItem(text(p.name), QVariant::fromValue<qlonglong>(p.id));
        const auto previous = profiles->findData(QVariant::fromValue<qlonglong>(profileId));
        if (previous >= 0) profiles->setCurrentIndex(previous);
    }
    changeProfile();
}

void SyncWindow::changeProfile() {
    profile.Close();
    profileId = profiles->currentData().toLongLong();
    rename->setEnabled(profileId != 0);
    remove->setEnabled(profileId != 0);
    if (profileId) {
        QString problem;
        if (!openProfile(library, profile, profileId, problem)) showError(this, problem);
    }
    tabs->setEnabled(profile.IsOpen());
    showPalmConduits();
    {
        const QSignalBlocker blocked(backupDevice);
        const auto previous = backupDevice->currentData();
        backupDevice->clear();
        if (profile.IsOpen()) {
            const auto assignments = library.Assignments();
            // A device that only has recovery copies is listed under its own name, with the copies under it.
            std::set<std::string> listed;
            for (const auto& source : profile.BackupDevices(true)) {
                std::string key = source;
                IsRecoverySource(source, &key);
                if (listed.insert(key).second) backupDevice->addItem(deviceName(machineNames, assignments, key), text(key));
            }
        }
        const int at = backupDevice->findData(previous);
        if (at >= 0) backupDevice->setCurrentIndex(at);
    }
    showRecords();
    showRecoveryCopies();
    showQueue();
    showLog();
}

void SyncWindow::nameProfile(bool editing) {
    if (editing && !profileId) return;
    QString problem;
    if (library.Root().empty() && !frondDesktopSync::openLibrary(library, problem)) return showError(this, problem);
    bool ok;
    const auto name = QInputDialog::getText(this, editing ? tr("Rename profile") : tr("New profile"),
                                           tr("Name"), QLineEdit::Normal, editing ? profiles->currentText() : QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    std::string error;
    if (editing) {
        if (!library.RenameProfile(profileId, name.toStdString(), error)) return showError(this, text(error));
    } else {
        ProfileInfo info;
        if (!library.CreateProfile(name.toStdString(), info, error)) return showError(this, text(error));
        profileId = info.id;
    }
    refresh();
}

void SyncWindow::deleteProfile() {
    if (!profileId) return;
    if (QMessageBox::warning(this, tr("Delete profile"),
                             tr("Delete %1 and all its data, backups and queued installs?").arg(profiles->currentText()),
                             QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    profile.Close();
    std::string error;
    if (!library.DeleteProfile(profileId, error)) showError(this, text(error));
    refresh();
}

void SyncWindow::showRecords() {
    records->setRowCount(0);
    edit->setEnabled(false);
    deleteRecord->setEnabled(false);
    exportFile->setEnabled(false);
    if (!profile.IsOpen()) return;
    std::string error;
    std::vector<PimRecord> items;
    if (!profile.ReadPimRecords(kind(), false, items, error)) return showError(this, text(error));
    for (const auto& record : items) {
        if ((record.secret && !showPrivate->isChecked()) || (record.archived && !showArchived->isChecked())) continue;
        auto name = recordName(record);
        if (name.isEmpty()) name = tr("Untitled");
        QString state;
        if (record.archived) state = tr("Archived");
        else if (record.secret) state = tr("Private");
        else if (record.kind == PimKind::tasks && field(record, "complete") == "1") state = tr("Completed");
        addRow(records, {name, text(record.category), field(record, record.kind == PimKind::tasks ? "dueDate" : "date"), state},
               QVariant::fromValue<qlonglong>(record.id), QVariant::fromValue<qlonglong>(record.revision));
    }
    exportFile->setEnabled(records->rowCount() != 0);
}

void SyncWindow::editRecord(bool existing) {
    if (!profile.IsOpen()) return;
    PimRecord record;
    record.kind = kind();
    if (existing) {
        const auto rows = selectedRows(records);
        if (rows.size() != 1) return;
        const auto found = profile.FindPimRecord(rows.first()->data(Qt::UserRole).toLongLong());
        if (!found || found->deleted) { showRecords(); return showError(this, tr("This record was deleted.")); }
        record = *found;
    }
    RecordEditor dialog(std::move(record), this);
    while (dialog.exec() == QDialog::Accepted) {
        std::string error;
        if (profile.SavePimRecord(dialog.record, error)) break;
        showError(this, text(error));
        // A conflict needs a fresh read. Keep the stored revision intact.
        if (existing) break;
    }
    showRecords();
}

void SyncWindow::deleteRecords() {
    const auto selected = selectedRows(records);
    if (!profile.IsOpen() || selected.isEmpty()) return;
    QList<QPair<qlonglong, qlonglong>> rows;
    for (const auto* row : selected)
        rows.append({row->data(Qt::UserRole).toLongLong(), row->data(Qt::UserRole + 1).toLongLong()});
    if (QMessageBox::question(this, tr("Delete records"),
                              tr("Delete %n selected record(s)?", nullptr, rows.size()),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    std::string error;
    if (!profile.BeginPimChange(error)) return showError(this, text(error));
    for (const auto& [id, revision] : rows)
        if (!profile.DeletePimRecord(id, revision, error)) {
            profile.RollBackPimChange();
            showError(this, text(error));
            showRecords();
            return;
        }
    if (!profile.CommitPimChange(error)) {
        profile.RollBackPimChange();
        showError(this, text(error));
    }
    showRecords();
}

void SyncWindow::importRecords() {
    if (!profile.IsOpen()) return;
    const auto type = kind();
    const auto path = QFileDialog::getOpenFileName(this, tr("Import records"), {}, frondSyncFiles::filters(type));
    if (path.isEmpty()) return;
    QString message;
    std::vector<PimRecord> items;
    if (!frondSyncFiles::read(path, type, items, message)) return showError(this, message);
    std::string error;
    if (!profile.BeginPimChange(error)) return showError(this, text(error));
    for (auto& record : items)
        if (!profile.SavePimRecord(record, error)) {
            profile.RollBackPimChange();
            return showError(this, text(error));
        }
    if (!profile.CommitPimChange(error)) {
        profile.RollBackPimChange();
        return showError(this, text(error));
    }
    showRecords();
}

void SyncWindow::exportRecords() {
    if (!profile.IsOpen()) return;
    auto rows = selectedRows(records);
    if (rows.isEmpty())
        for (int i = 0; i < records->rowCount(); ++i) rows << records->item(i, 0);
    if (rows.isEmpty()) return;
    std::vector<PimRecord> items;
    for (const auto* row : rows) {
        auto item = profile.FindPimRecord(row->data(Qt::UserRole).toLongLong());
        if (item && !item->deleted && (!item->secret || showPrivate->isChecked()) && (!item->archived || showArchived->isChecked()))
            items.push_back(std::move(*item));
    }
    if (items.empty()) return;
    QFileDialog dialog(this, tr("Export records"), QString(), frondSyncFiles::filters(kind()));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("csv");
    dialog.selectFile(kindName(kind()));
    connect(&dialog, &QFileDialog::filterSelected, &dialog, [this, &dialog](const QString& filter) {
        dialog.setDefaultSuffix(filter.contains("*.csv") ? "csv" : frondSyncFiles::extension(kind()).mid(1));
    });
    if (dialog.exec() != QDialog::Accepted) return;
    QString error;
    if (!frondSyncFiles::write(dialog.selectedFiles().first(), kind(), items, error)) showError(this, error);
}

// Recovery copies kept for the chosen device. The first entry is its normal backups.
void SyncWindow::showRecoveryCopies() {
    {
        const QSignalBlocker blocked(recoveryCopy);
        recoveryCopy->clear();
        recoveryCopy->addItem(tr("None, show the latest backups"), QString());
        if (profile.IsOpen() && backupDevice->currentIndex() != -1)
            for (const auto& copy : profile.RecoveryCopies(backupDevice->currentData().toString().toStdString()))
                recoveryCopy->addItem(when(copy.storedAt), text(copy.source));
        recoveryCopy->setEnabled(recoveryCopy->count() > 1);
    }
    showBackups();
}

std::string SyncWindow::backupSource() const {
    const auto copy = recoveryCopy->currentData().toString();
    return (copy.isEmpty() ? backupDevice->currentData().toString() : copy).toStdString();
}

void SyncWindow::showBackups() {
    backups->setRowCount(0);
    exportDb->setEnabled(false);
    restore->setEnabled(false);
    if (!profile.IsOpen() || backupDevice->currentIndex() == -1) return;
    for (const auto& item : profile.Backups(backupSource()))
        addRow(backups, {text(PalmPimText(Bytes(item.name.begin(), item.name.end()))),
                         QLocale().formattedDataSize(item.size), when(item.storedAt)},
               QByteArray::fromStdString(item.name), bool(item.attributes & dbResource));
    restore->setEnabled(backups->rowCount() != 0);
}

void SyncWindow::exportBackup() {
    const auto rows = selectedRows(backups);
    if (!profile.IsOpen() || rows.size() != 1) return;
    const auto db = rows.first()->data(Qt::UserRole).toByteArray().toStdString();
    Bytes bytes;
    if (!profile.LoadBackup(backupSource(), db, bytes))
        return showError(this, tr("Couldn't read this backup."));
    const auto name = backupFilename(rows.first()->text(), rows.first()->data(Qt::UserRole + 1).toBool());
    const auto path = QFileDialog::getSaveFileName(this, tr("Export database"), name, tr("Palm databases (*.prc *.pdb)"));
    QString error;
    if (!path.isEmpty() && !frondSyncFiles::writeDatabase(path, bytes, error)) showError(this, error);
}

void SyncWindow::restoreBackups() {
    if (!profile.IsOpen() || backupDevice->currentIndex() == -1) return;
    const auto from = backupDevice->currentData().toString().toStdString();
    const auto source = backupSource();
    const auto assignments = library.Assignments();
    QStringList devices;
    std::vector<std::string> keys;
    for (const auto& assignment : assignments) {
        if (assignment.profileId != profileId || assignment.kind != "palm") continue;
        keys.push_back(assignment.deviceKey);
        devices << deviceName(machineNames, assignments, assignment.deviceKey);
    }
    if (keys.empty()) return showError(this, tr("Assign a device to this profile first."));
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Queue restore"));
    auto* layout = new QFormLayout(&dialog);
    auto* target = new QComboBox(&dialog);
    target->addItems(devices);
    if (auto at = std::find(keys.begin(), keys.end(), from); at != keys.end()) target->setCurrentIndex(at - keys.begin());
    layout->addRow(tr("Restore to"), target);
    auto* warning = new QLabel(tr("The next HotSync replaces databases on this device with the backups."), &dialog);
    warning->setWordWrap(true);
    layout->addRow(warning);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Queue restore"));
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    std::string error;
    ProfileSyncLock lock;
    if (!lock.Acquire(library.Root(), profileId, error)) return showError(this, text(error));
    const auto count = profile.QueueRestore(source, keys[target->currentIndex()], error);
    if (count <= 0) return showError(this, error.empty() ? tr("No backups to restore.") : text(error));
    showQueue();
    tabs->setCurrentIndex(2);
}

void SyncWindow::showQueue() {
    queue->setRowCount(0);
    removeFiles->setEnabled(false);
    if (!profile.IsOpen()) return;
    const auto assignments = library.Assignments();
    for (const auto& item : profile.InstallQueue()) {
        if (!frondSync::InstallItemFitsKind(item, deviceKind)) continue;
        const auto state = item.state == "queued" ? tr("Queued") : item.state == "done" ? tr("Installed") : tr("Failed");
        addRow(queue, {text(item.fileName), deviceName(machineNames, assignments, item.deviceKey), state, text(item.message)},
               QVariant::fromValue<qlonglong>(item.id));
    }
}

void SyncWindow::queueFiles() {
    if (!profile.IsOpen()) return;
    const auto paths = QFileDialog::getOpenFileNames(
        this, tr("Queue files"), {}, tr("Applications and installers (*.prc *.pdb *.pqa *.cab *.zip *.exe *.msi *.7z *.rar)"));
    if (paths.isEmpty()) return;
    std::vector<InstallerFile> files;
    QStringList failed;
    for (const auto& path : paths) {
        InstallerFile file{QFileInfo(path).fileName().toStdString(), {}};
        QString error;
        if (IsInstallFileName(file.name)) {
            if (frondSyncFiles::readDatabase(path, file.bytes, error)) files.push_back(std::move(file));
            else failed << tr("%1: %2").arg(text(file.name), error);
            continue;
        }
        std::vector<InstallerFile> unpacked;
        if (!IsInstallerFileName(file.name)) failed << tr("%1: Unsupported installer.").arg(text(file.name));
        else if (frondInstallerRunner::unpack(this, path, text(library.Root()), unpacked, error)) choosePayloads(unpacked, files);
        else failed << tr("%1: %2").arg(text(file.name), error);
    }
    queuePayloads(files, std::move(failed));
}

void SyncWindow::showPalmConduits() {
    const QSignalBlocker blocked(palmConduits);
    const auto info = profile.IsOpen() ? library.FindProfile(profileId) : std::nullopt;
    if (!info || deviceKind == "winmo") {
        palmConduits->hide();
        return;
    }
    const QDir root(text(library.ProfilePath(*info)) + "/conduits");
    const QDir current(root.filePath("registered"));
    const QDir registered(current.exists() ? current.path() : root.filePath("registered.previous"));
    palmConduits->setVisible(!registered.entryList({"*.conf"}, QDir::Files | QDir::NoSymLinks).isEmpty());
    palmConduits->setChecked(!root.exists("disabled"));
}

void SyncWindow::setPalmConduits(bool enabled) {
    const auto info = profile.IsOpen() ? library.FindProfile(profileId) : std::nullopt;
    if (!info) return;
    ProfileSyncLock lock;
    std::string problem;
    if (!lock.Acquire(library.Root(), info->id, problem)) showError(this, text(problem));
    else {
        const auto confirmed = library.FindProfile(info->id);
        if (!confirmed || confirmed->folder != info->folder || confirmed->created != info->created)
            showError(this, tr("The sync profile was changed."));
        else {
            QFile disabled(text(library.ProfilePath(*confirmed)) + "/conduits/disabled");
            const bool changed = enabled ? (!disabled.exists() || disabled.remove()) :
                                           (disabled.exists() || disabled.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            if (!changed) showError(this, tr("Couldn't change the Palm conduit setting."));
        }
    }
    showPalmConduits();
}

void SyncWindow::runWindowsInstaller() {
    if (!profile.IsOpen()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Run Windows installer"), {},
                                                   tr("Windows installers (*.exe *.msi)"));
    if (path.isEmpty()) return;
    const QStringList kinds{tr("Palm Desktop"), tr("ActiveSync")};
    bool chosen = false;
    const auto kind = QInputDialog::getItem(this, tr("Run Windows installer"), tr("Installer expects"), kinds, 0, false, &chosen);
    if (!chosen) return;
    const bool windowsMobile = kind == kinds.back();
    const auto syncRoot = text(library.Root());
    bool extract = false;
    frondInstallerRunner::RunResult launch;
#ifdef Q_OS_WIN
    QMessageBox method(QMessageBox::Question, tr("Run Windows installer"),
                       tr("Run the installer or extract its files?"), QMessageBox::Cancel, this);
    auto* run = method.addButton(tr("Run installer"), QMessageBox::AcceptRole);
    auto* unpack = method.addButton(tr("Extract files"), QMessageBox::ActionRole);
    method.setDefaultButton(QMessageBox::Cancel);
    method.exec();
    extract = method.clickedButton() == unpack;
    if (!extract && method.clickedButton() != run) return;
#endif
    if (!extract) {
        const auto info = library.FindProfile(profileId);
        if (!info) return showError(this, tr("The sync profile was changed."));
        ProfileSyncLock installerLock;
        std::string problem;
        if (!installerLock.Acquire(library.Root(), info->id, problem)) return showError(this, text(problem));
        const auto confirmed = library.FindProfile(info->id);
        if (!confirmed || confirmed->folder != info->folder || confirmed->created != info->created)
            return showError(this, tr("The sync profile was changed."));
        auto user = profiles->currentText();
        QString userKey;
        uint32_t userId = 0;
        if (!windowsMobile) {
            const auto assignments = library.Assignments();
            std::vector<const Assignment*> palmUsers;
            QStringList labels;
            for (const auto& item : assignments) {
                if (item.profileId != info->id || item.kind != "palm" || !item.userId || item.userName.empty()) continue;
                const auto name = text(item.userName);
                const auto key = text(item.deviceKey);
                auto label = tr("%1 (%2)").arg(name, machineNames.value(key, key));
                if (labels.contains(label)) label = tr("%1 (%2)").arg(name, key);
                labels << label;
                palmUsers.push_back(&item);
            }
            if (!palmUsers.empty()) {
                int selected = 0;
                if (palmUsers.size() > 1) {
                    bool chosen = false;
                    const auto label = QInputDialog::getItem(this, tr("Run Windows installer"), tr("Palm user"),
                                                             labels, 0, false, &chosen);
                    if (!chosen) return;
                    selected = labels.indexOf(label);
                }
                const auto& target = *palmUsers.at(selected);
                user = text(target.userName);
                userId = target.userId;
                userKey = text(PalmConduitDataKey(target.deviceKey, userId));
            }
        }
        launch = frondInstallerRunner::run(this, path, user, windowsMobile, syncRoot,
                                          text(library.ProfilePath(*confirmed)), userKey, userId);
        showPalmConduits();
        if (!launch.error.isEmpty()) {
            QMessageBox fallback(QMessageBox::Warning, tr("Run Windows installer"), launch.error, QMessageBox::Cancel, this);
            auto* staged = launch.files.empty() ? nullptr : fallback.addButton(tr("Choose staged files"), QMessageBox::AcceptRole);
            auto* unpack = fallback.addButton(tr("Extract files"), QMessageBox::ActionRole);
            fallback.setDefaultButton(QMessageBox::Cancel);
            fallback.exec();
            extract = fallback.clickedButton() == unpack;
            if (!extract && (!staged || fallback.clickedButton() != staged)) return;
        }
    }
    if (extract) {
        QString error;
        std::vector<InstallerFile> unpacked;
        if (!frondInstallerRunner::unpack(this, path, syncRoot, unpacked, error)) return showError(this, error);
        unpacked.erase(std::remove_if(unpacked.begin(), unpacked.end(), [windowsMobile](const InstallerFile& file) {
            return IsCabFileName(file.name) != windowsMobile;
        }), unpacked.end());
        if (unpacked.empty()) return showError(this, tr("The extracted files are for a different device type."));
        frondInstallerRunner::removeDuplicatePayloads(unpacked);
        launch.files = std::move(unpacked);
    }
    if (launch.files.empty()) {
        if (launch.palmDesktopSaved) QMessageBox::information(this, tr("Run Windows installer"), tr("Palm desktop files saved."));
        else if (launch.conduitsChanged)
            QMessageBox::information(this, tr("Run Windows installer"), tr("Palm conduit registrations updated."));
        return;
    }
    std::vector<InstallerFile> files;
    choosePayloads(launch.files, files);
    queuePayloads(files, {});
}

void SyncWindow::choosePayloads(std::vector<InstallerFile>& candidates, std::vector<InstallerFile>& selected) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Choose install files"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog);
    auto* pane = new QWidget(scroll);
    auto* choices = new QVBoxLayout(pane);
    std::vector<QCheckBox*> checks;
    for (const auto& member : candidates) {
        auto* check = new QCheckBox(text(member.name), pane);
        choices->addWidget(check);
        checks.push_back(check);
    }
    scroll->setWidget(pane);
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        for (size_t i = 0; i < candidates.size(); ++i)
            if (checks[i]->isChecked()) selected.push_back(std::move(candidates[i]));
}

void SyncWindow::queuePayloads(const std::vector<InstallerFile>& files, QStringList failed) {
    if (files.empty()) {
        if (!failed.isEmpty()) showError(this, failed.join('\n'));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Queue files"));
    auto* layout = new QFormLayout(&dialog);
    const auto assignments = library.Assignments();
    const auto targetFor = [&](bool cab, const QString& label) -> QComboBox* {
        const auto matches = [cab](const InstallerFile& file) { return IsCabFileName(file.name) == cab; };
        if (std::none_of(files.begin(), files.end(), matches)) return nullptr;
        auto* target = new QComboBox(&dialog);
        target->addItem(tr("Next device to sync"));
        for (const auto& assignment : assignments)
            if (assignment.profileId == profileId && assignment.kind == (cab ? "winmo" : "palm"))
                target->addItem(deviceName(machineNames, assignments, assignment.deviceKey), text(assignment.deviceKey));
        layout->addRow(label, target);
        return target;
    };
    auto* palmTarget = targetFor(false, tr("Install Palm files on"));
    auto* cabTarget = targetFor(true, tr("Install CAB files on"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Queue files"));
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    ProfileSyncLock lock;
    std::string lockError;
    if (!lock.Acquire(library.Root(), profileId, lockError)) return showError(this, text(lockError));
    for (const auto& file : files) {
        std::string problem;
        InstallItem item;
        const auto cab = IsCabFileName(file.name);
        auto* target = cab ? cabTarget : palmTarget;
        if (!profile.QueueInstall(target->currentData().toString().toStdString(), file.name, file.bytes, item, problem, cab ? "cab" : "ram"))
            failed << tr("%1: %2").arg(text(file.name), text(problem));
    }
    showQueue();
    if (!failed.isEmpty()) showError(this, failed.join('\n'));
}

void SyncWindow::removeQueued() {
    if (!profile.IsOpen()) return;
    ProfileSyncLock lock;
    std::string error;
    if (!lock.Acquire(library.Root(), profileId, error)) return showError(this, text(error));
    for (const auto* row : selectedRows(queue)) {
        if (!profile.RemoveInstall(row->data(Qt::UserRole).toLongLong(), error)) {
            showError(this, text(error));
            break;
        }
    }
    showQueue();
}

void SyncWindow::showLog() {
    log->setRowCount(0);
    if (!profile.IsOpen()) return;
    const auto assignments = library.Assignments();
    for (const auto& item : profile.LogEntries({}))
        addRow(log, {text(item.text), deviceName(machineNames, assignments, item.deviceKey), when(item.time)}, {});
}
