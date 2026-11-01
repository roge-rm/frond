#include "MemoryStickFilesDialog.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
QString hex(const QString& value) { return QString::fromLatin1(value.toUtf8().toHex()); }
QString pair(const QString& first, const QString& second) { return hex(first) + '\n' + hex(second); }
}
MemoryStickFilesDialog::MemoryStickFilesDialog(DesktopClient& client,
        std::function<bool(const QString&, const QString&)> send, bool writeProtected, QWidget* parent)
    : QDialog(parent), client(client), send(std::move(send)), writeProtected(writeProtected) {
    setObjectName("memoryStickFilesDialog"); setWindowTitle(tr("Memory Stick files")); resize(680, 520);
    auto* layout = new QVBoxLayout(this);
    auto* warning = new QLabel(tr("Changes are saved on the card immediately."), this);
    warning->setWordWrap(true); layout->addWidget(warning);
    auto* form = new QFormLayout;
    path = new QLineEdit("/", this); path->setObjectName("cardPath");
    form->addRow(tr("Card folder"), path); layout->addLayout(form);
    auto* browse = new QPushButton(tr("List folder"), this); browse->setObjectName("listCardFolder");
    buttons << browse; layout->addWidget(browse);
    files = new QTreeWidget(this); files->setObjectName("cardFiles"); files->setColumnCount(2);
    files->setHeaderLabels({tr("Name"), tr("Size")}); files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    files->setRootIsDecorated(false); layout->addWidget(files);
    status = new QLabel(this); status->setObjectName("cardFilesStatus"); status->setWordWrap(true); layout->addWidget(status);
    auto* operations = new QDialogButtonBox(this);
    auto* put = operations->addButton(tr("Add file…"), QDialogButtonBox::ActionRole); put->setObjectName("putCardFile");
    auto* get = operations->addButton(tr("Export file…"), QDialogButtonBox::ActionRole); get->setObjectName("getCardFile");
    auto* mkdir = operations->addButton(tr("New folder…"), QDialogButtonBox::ActionRole); mkdir->setObjectName("newCardFolder");
    auto* rename = operations->addButton(tr("Rename…"), QDialogButtonBox::ActionRole); rename->setObjectName("renameCardFile");
    auto* remove = operations->addButton(tr("Delete…"), QDialogButtonBox::ActionRole); remove->setObjectName("deleteCardFile");
    buttons << put << get << mkdir << rename << remove; writes << put << mkdir << rename << remove;
    layout->addWidget(operations);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this); layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::rejected, this, [this] { if (!pending.isEmpty()) this->client.cancelFiles(); });
    for (auto* button : findChildren<QPushButton*>()) button->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, &MemoryStickFilesDialog::list);
    connect(path, &QLineEdit::returnPressed, this, &MemoryStickFilesDialog::list);
    connect(path, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(files, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (pending.isEmpty() && item->data(0, Qt::UserRole).toBool()) { path->setText(joined(item->text(0))); list(); }
    });
    connect(put, &QPushButton::clicked, this, [this] {
        const auto source = QFileDialog::getOpenFileName(this, tr("Add file to Memory Stick"));
        if (source.isEmpty()) return;
        bool ok;
        const auto name = QInputDialog::getText(this, tr("New card filename"), tr("Filename"), QLineEdit::Normal,
                                              QFileInfo(source).fileName(), &ok);
        if (ok) request("ms-put", pair(QFileInfo(source).absoluteFilePath(), joined(name)));
    });
    connect(get, &QPushButton::clicked, this, [this] {
        const auto source = selectedPath();
        if (source.isEmpty()) return;
        if (files->currentItem()->data(0, Qt::UserRole).toBool()) {
            QMessageBox::warning(this, tr("Memory Stick files"), tr("Choose a regular file.")); return;
        }
        const auto target = QFileDialog::getSaveFileName(this, tr("Export card file"), files->currentItem()->text(0));
        if (!target.isEmpty()) request("ms-get", pair(QFileInfo(target).absoluteFilePath(), source));
    });
    connect(mkdir, &QPushButton::clicked, this, [this] {
        bool ok;
        const auto name = QInputDialog::getText(this, tr("New card folder"), tr("Folder name"), QLineEdit::Normal, {}, &ok);
        if (ok) request("ms-mkdir", joined(name));
    });
    connect(rename, &QPushButton::clicked, this, [this] {
        const auto source = selectedPath(); if (source.isEmpty()) return;
        bool ok;
        const auto name = QInputDialog::getText(this, tr("Rename card entry"), tr("New name"), QLineEdit::Normal,
                                              files->currentItem()->text(0), &ok);
        if (ok) request("ms-rename", pair(source, name));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto source = selectedPath(); if (source.isEmpty()) return;
        if (QMessageBox::question(this, tr("Delete card entry?"), source, QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) == QMessageBox::Yes) request("ms-delete", source);
    });
    connect(&client, &DesktopClient::result, this, &MemoryStickFilesDialog::result);
    connect(&client, &DesktopClient::finished, this, &QDialog::reject);
    refresh(); QTimer::singleShot(0, this, &MemoryStickFilesDialog::list);
}
QString MemoryStickFilesDialog::joined(const QString& name) const { return (listedPath == "/" ? QString("/") : listedPath + '/') + name; }
QString MemoryStickFilesDialog::selectedPath() {
    if (const auto* item = files->currentItem()) return joined(item->text(0));
    QMessageBox::warning(this, tr("Memory Stick files"), tr("Choose a card entry."));
    return {};
}
void MemoryStickFilesDialog::refresh() {
    const bool available = pending.isEmpty() && client.available();
    const bool listed = !listedPath.isEmpty() && path->text() == listedPath;
    path->setEnabled(available); files->setEnabled(available && listed);
    for (auto* button : buttons)
        button->setEnabled(available && (button == buttons.first() || (listed && (!writeProtected || !writes.contains(button)))));
}
void MemoryStickFilesDialog::request(const QString& command, const QString& argument) {
    if (!isVisible() || !pending.isEmpty() || !client.available()) return;
    pending = command; refresh();
    if (!send(command, argument)) {
        pending.clear(); refresh(); status->setText(tr("Could not send the emulator command."));
    }
}
void MemoryStickFilesDialog::list() {
    if (!pending.isEmpty() || !client.available()) return;
    if (path->text().toUtf8().size() > 511) {
        files->clear(); listedPath.clear(); refresh(); status->setText(tr("Use a card path of at most 511 bytes.")); return;
    }
    listingPath = path->text(); request("ms-files", listingPath);
}
void MemoryStickFilesDialog::result(const QString& command, bool ok, const QString& code, const QString& message) {
    if (command != pending) return;
    pending.clear(); refresh();
    if (!isVisible()) return;
    if (!ok) {
        const auto error = code == "not-ready" ? tr("Open Date Book on the Palm, then try again.") : message;
        if (command == "ms-files") { files->clear(); listedPath.clear(); refresh(); }
        status->setText(error); return;
    }
    if (command != "ms-files") { status->setText(message); list(); return; }
    const auto lines = message.split('\n', Qt::SkipEmptyParts);
    const auto space = lines.value(0).split('\t'); bool usedOk = false, totalOk = false;
    const auto used = space.value(1).toULongLong(&usedOk), total = space.value(2).toULongLong(&totalOk);
    QList<QTreeWidgetItem*> replacement;
    QSet<QByteArray> names;
    bool valid = space.size() == 3 && space[0] == "space" && usedOk && totalOk && used <= total && total <= 128ull * 1024 * 1024 && lines.size() <= 513;
    for (const auto& line : lines.mid(1)) {
        const auto fields = line.split('\t'); bool sizeOk = false;
        const auto size = fields.value(1).toULongLong(&sizeOk);
        const auto encoded = fields.value(2).toLatin1(), bytes = QByteArray::fromHex(encoded);
        bool nameOk = !bytes.isEmpty() && bytes.size() <= 255 && bytes.toHex() == encoded;
        for (const unsigned char c : bytes) if (c < 32 || c >= 127 || QByteArray("/\\:*?\"<>|").contains(c)) nameOk = false;
        if (fields.size() != 3 || (fields[0] != "d" && fields[0] != "f") || !sizeOk || size > total || !nameOk || names.contains(bytes.toLower())) { valid = false; break; }
        names.insert(bytes.toLower());
        auto* item = new QTreeWidgetItem({QString::fromLatin1(bytes), fields[0] == "d" ? tr("Folder") : tr("%1 bytes").arg(size)});
        item->setData(0, Qt::UserRole, fields[0] == "d"); replacement << item;
    }
    if (!valid) {
        qDeleteAll(replacement); files->clear(); listedPath.clear(); refresh();
        status->setText(tr("The Palm returned an invalid folder listing.")); return;
    }
    listedPath = listingPath;
    files->clear(); files->addTopLevelItems(replacement); refresh();
    status->setText(tr("%1 MB free").arg(double(total - used) / (1024 * 1024), 0, 'f', 1));
}
