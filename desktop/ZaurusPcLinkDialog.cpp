#include "ZaurusPcLinkDialog.h"
#include "DesktopClient.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>

namespace {
bool isFileName(const QString& name) {
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/') &&
           !name.contains('\\') && !name.contains(QChar::Null);
}

QString transferArgument(const QString& remote, const QString& local) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"remote", remote}, {"local", local}})
                                 .toJson(QJsonDocument::Compact));
}
}

ZaurusPcLinkDialog::ZaurusPcLinkDialog(DesktopClient& client, QWidget* parent)
    : QDialog(parent), client(client) {
    setWindowTitle(tr("PC Link"));
    setObjectName("zaurusPcLinkDialog");
    resize(460, 380);

    auto* layout = new QVBoxLayout(this);
    path = new QLabel(this);
    path->setTextFormat(Qt::PlainText);
    path->setWordWrap(true);
    auto* navigation = new QHBoxLayout;
    up = new QPushButton(tr("Up"), this);
    up->setObjectName("pcLinkUp");
    refresh = new QPushButton(this);
    refresh->setObjectName("pcLinkRefresh");
    navigation->addWidget(path, 1);
    navigation->addWidget(up);
    navigation->addWidget(refresh);
    layout->addLayout(navigation);

    entries = new QListWidget(this);
    entries->setAccessibleName(tr("Internal Disk files"));
    entries->setObjectName("pcLinkFiles");
    layout->addWidget(entries, 1);
    status = new QLabel(this);
    status->setObjectName("pcLinkStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    send = buttons->addButton(tr("Send file…"), QDialogButtonBox::ActionRole);
    send->setObjectName("pcLinkSend");
    receive = buttons->addButton(tr("Save file…"), QDialogButtonBox::ActionRole);
    receive->setObjectName("pcLinkReceive");
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(send, &QPushButton::clicked, this, &ZaurusPcLinkDialog::sendFile);
    connect(receive, &QPushButton::clicked, this, &ZaurusPcLinkDialog::receiveFile);
    connect(up, &QPushButton::clicked, this, [this] {
        const auto slash = folder.lastIndexOf('/');
        list(slash < 0 ? QString() : folder.left(slash));
    });
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (connection == Connection::disconnected) connectCable();
        else list(folder);
    });
    connect(entries, &QListWidget::itemActivated, this, [this](QListWidgetItem* row) {
        if (row->data(Qt::UserRole).toBool()) list(pathFor(row->text()));
    });
    connect(entries, &QListWidget::currentItemChanged, this, &ZaurusPcLinkDialog::updateButtons);
    timer.setInterval(200);
    connect(&timer, &QTimer::timeout, this, &ZaurusPcLinkDialog::pump);
    updateButtons();
}

void ZaurusPcLinkDialog::browse() {
    if (connection != Connection::disconnected) return;
    status->clear();
    folder.clear();
    haveListing = false;
    entries->clear();
    open();
    connectCable();
}

void ZaurusPcLinkDialog::connectCable() {
    if (client.request("zaurus-pc-link-open")) {
        connection = Connection::opening;
        status->setText(tr("Connecting…"));
    } else {
        status->setText(tr("The emulator is busy. Try again."));
    }
    updateButtons();
}

void ZaurusPcLinkDialog::done(int result) {
    if (connection == Connection::opening || connection == Connection::ready) {
        connection = Connection::closing;
        job = Job::none;
        timer.start();
        pump();
    }
    QDialog::done(result);
}

void ZaurusPcLinkDialog::machineStopped() {
    timer.stop();
    connection = Connection::disconnected;
    job = Job::none;
    reject();
}

void ZaurusPcLinkDialog::pump() {
    if (connection == Connection::closing) {
        if (client.request("zaurus-pc-link-close")) timer.stop();
    } else if (job != Job::none) {
        client.request("zaurus-pc-link-result");
    }
}

void ZaurusPcLinkDialog::start(Job next, const QString& command, const QString& argument, const QString& text) {
    if (!isVisible() || connection != Connection::ready || job != Job::none) return;
    if (!client.request(command, argument)) {
        status->setText(tr("The emulator is busy. Try again."));
        return;
    }
    job = next;
    status->setText(text);
    updateButtons();
}

void ZaurusPcLinkDialog::list(const QString& nextFolder) {
    if (job != Job::none) return;
    requestedFolder = nextFolder;
    start(Job::list, "zaurus-pc-link-list", nextFolder, tr("Reading files…"));
}

QString ZaurusPcLinkDialog::pathFor(const QString& name) const {
    return folder.isEmpty() ? name : folder + '/' + name;
}

void ZaurusPcLinkDialog::sendFile() {
    const auto file = QFileDialog::getOpenFileName(this, tr("Send a file to the Zaurus"));
    if (file.isEmpty()) return;
    const QFileInfo info(file);
    start(Job::send, "zaurus-pc-link-send", transferArgument(pathFor(info.fileName()), info.absoluteFilePath()),
          tr("Sending file…"));
}

void ZaurusPcLinkDialog::receiveFile() {
    const auto* row = entries->currentItem();
    if (!row || row->data(Qt::UserRole).toBool()) return;
    const auto name = row->text();
    const auto remote = pathFor(name);
    const auto defaultPath = QDir::homePath() + '/' + name;
    const auto file = QFileDialog::getSaveFileName(this, tr("Save a file from the Zaurus"), defaultPath);
    if (file.isEmpty()) return;
    start(Job::receive, "zaurus-pc-link-receive", transferArgument(remote, QFileInfo(file).absoluteFilePath()),
          tr("Receiving file…"));
}

void ZaurusPcLinkDialog::showListing(const QString& listing) {
    const auto document = QJsonDocument::fromJson(listing.toUtf8());
    if (!document.isArray()) {
        status->setText(tr("Couldn't read the folder listing."));
        return;
    }
    struct Entry { QString name; bool directory; };
    QList<Entry> files;
    for (const auto& value : document.array()) {
        const auto row = value.toObject();
        const auto name = row.value("name").toString();
        if (isFileName(name) && row.value("directory").isBool())
            files.append({name, row.value("directory").toBool()});
    }
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
        return a.directory != b.directory ? a.directory : a.name.localeAwareCompare(b.name) < 0;
    });
    entries->clear();
    for (const auto& file : files) {
        auto* row = new QListWidgetItem(style()->standardIcon(file.directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon),
                                        file.name, entries);
        row->setData(Qt::UserRole, file.directory);
    }
    folder = requestedFolder;
    haveListing = true;
    status->setText(files.isEmpty() ? tr("This folder is empty.") : QString());
}

void ZaurusPcLinkDialog::reply(const QString& command, bool ok, const QString& code, const QString& message) {
    if (command == "zaurus-pc-link-close") {
        connection = Connection::disconnected;
        timer.stop();
        return;
    }
    if (connection == Connection::closing) {
        if (command == "zaurus-pc-link-open" && !ok) {
            connection = Connection::disconnected;
            timer.stop();
        } else pump();
        return;
    }
    if (command == "zaurus-pc-link-open") {
        connection = ok ? Connection::ready : Connection::disconnected;
        if (ok) list({});
        else status->setText(message);
        updateButtons();
        return;
    }
    if (job == Job::none) return;
    if (ok && (code == "started" || code == "busy")) {
        if (!timer.isActive()) timer.start();
        return;
    }
    const auto completed = job;
    job = Job::none;
    timer.stop();
    if (!ok) status->setText(message);
    else if (code != "done") status->setText(tr("Couldn't read the transfer result."));
    else if (completed == Job::list) showListing(message);
    else if (completed == Job::send) list(folder);
    else status->setText(tr("File saved."));
    updateButtons();
}

void ZaurusPcLinkDialog::updateButtons() {
    path->setText(folder.isEmpty() ? tr("Internal Disk") : tr("Internal Disk / %1").arg(folder));
    const bool idle = connection == Connection::ready && job == Job::none;
    refresh->setText(connection == Connection::disconnected ? tr("Connect") : tr("Refresh"));
    refresh->setEnabled(idle || connection == Connection::disconnected);
    up->setEnabled(idle && !folder.isEmpty());
    entries->setEnabled(idle && haveListing);
    send->setEnabled(idle && haveListing);
    const auto* row = entries->currentItem();
    receive->setEnabled(idle && row && !row->data(Qt::UserRole).toBool());
}
