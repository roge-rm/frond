#include "CardFilesWindow.h"
#include "CardsWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

using frondCardFile::Entry;
using frondCardFile::FatImage;
namespace cf = frondCardFiles;

namespace {
const QStringList shortcutFolders{QStringLiteral("/PALM/Launcher"), QStringLiteral("/DCIM")};
enum Column { NameColumn, SizeColumn, DateColumn };

// Folders first in either order, then by the column; size and date compare by value.
class FileItem : public QTreeWidgetItem {
 public:
    using QTreeWidgetItem::QTreeWidgetItem;
    Entry entry;
    bool operator<(const QTreeWidgetItem& other) const override {
        const auto& o = static_cast<const FileItem&>(other);
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const bool descending = treeWidget() && treeWidget()->header()->sortIndicatorOrder() == Qt::DescendingOrder;
        if (entry.directory != o.entry.directory) return descending ? !entry.directory : entry.directory;
        const auto byName = [&] { return entry.name.toLower() < o.entry.name.toLower(); };
        if (column == NameColumn) return byName();
        if (column == SizeColumn && entry.size != o.entry.size) return entry.size < o.entry.size;
        if (column == DateColumn && entry.modified != o.entry.modified) return entry.modified < o.entry.modified;
        // Ties by name, A to Z in either order.
        return descending ? !byName() : byName();
    }
};
}  // namespace

// The list: drags out a copy of the selection (exported to a private folder first).
class CardFilesList : public QTreeWidget {
 public:
    explicit CardFilesList(CardFilesWindow* owner) : QTreeWidget(owner), owner(owner) {}
 protected:
    void startDrag(Qt::DropActions) override {
        const auto files = owner->dragOut();
        if (files.isEmpty()) return;
        auto* data = new QMimeData;
        QList<QUrl> urls;
        for (const auto& f : files) urls << QUrl::fromLocalFile(f);
        data->setUrls(urls);
        auto* drag = new QDrag(this);
        drag->setMimeData(data);
        drag->exec(Qt::CopyAction, Qt::CopyAction);
    }
    void dragEnterEvent(QDragEnterEvent* e) override { owner->dragEnterEvent(e); }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->source() != this && e->mimeData()->hasUrls()) e->acceptProposedAction();
        else e->ignore();
    }
    void dropEvent(QDropEvent* e) override { owner->dropEvent(e); }
 private:
    CardFilesWindow* owner;
};

CardFilesWindow::CardFilesWindow(std::unique_ptr<frondCardFile::CardFile> opened, const QString& title, QWidget* parent)
    : QDialog(parent), card(std::move(opened)) {
    writable = card && card->writable;
    build(title);
    open(QStringLiteral("/"));
    // The shortcuts the card has.
    try {
        FatImage fat(*card);
        for (const auto& s : shortcutFolders) {
            const auto e = fat.find(s);
            if (!e || !e->directory) continue;
            auto* b = new QPushButton(s, this);
            b->setObjectName("filesShortcut");
            b->setAutoDefault(false);
            connect(b, &QPushButton::clicked, this, [this, s] { open(s); });
            shortcuts->addWidget(b);
        }
    } catch (const frondCardFile::Error&) {}
}

CardFilesWindow::CardFilesWindow(Databases databases, const QString& title, QWidget* parent) : QDialog(parent), source(std::move(databases)) {
    build(title);
    // One flat list of databases: no folders, and nothing to move or rename.
    list->setHeaderLabels({tr("Name"), tr("Type"), tr("Creator")});
    list->setDragEnabled(false);
    for (QWidget* w : std::initializer_list<QWidget*>{upButton, pathBar, cutButton, copyButton, pasteButton, renameButton, newFolderButton,
                                                      addFolderButton})
        w->hide();
    header->setText(tr("Reading the databases…"));
    waiting = true;
    updateButtons();
    source.list();
}

void CardFilesWindow::setDatabases(const std::vector<runningRules::Database>& all) {
    if (!internal()) return;
    const auto keep = selectedNames();
    list->clear();
    databaseOf.clear();
    for (const auto& d : all) {
        auto file = runningRules::databaseFile(d);
        if (databaseOf.contains(file)) file = cf::keepBothName(file, [&](const QString& n) { return databaseOf.contains(n); });
        databaseOf.insert(file, d.name);
        auto* item = new FileItem(list);
        item->entry.name = file;
        item->setText(NameColumn, file);
        item->setText(SizeColumn, d.type);
        item->setText(DateColumn, d.creator);
        item->setIcon(NameColumn, style()->standardIcon(QStyle::SP_FileIcon));
        if (keep.contains(file)) item->setSelected(true);
    }
    header->setText(all.size() == 1 ? tr("1 database") : tr("%1 databases").arg(all.size()));
    list->resizeColumnToContents(SizeColumn);
    list->resizeColumnToContents(DateColumn);
    waiting = false;
    listed = true;
    updateButtons();
}

void CardFilesWindow::databasesDone(const QString& message) {
    if (!internal()) return;
    note->setText(message);
    waiting = true;
    updateButtons();
    source.list();
}

void CardFilesWindow::build(const QString& title) {
    setObjectName("cardFilesWindow");
    openLevel = QThread::currentThread()->loopLevel();
    setWindowTitle(tr("%1 files").arg(title));
    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    upButton = new QPushButton(tr("Up"), this);
    upButton->setObjectName("filesUp");
    pathBar = new QLineEdit(this);
    pathBar->setObjectName("filesPath");
    top->addWidget(upButton);
    top->addWidget(pathBar, 1);
    layout->addLayout(top);

    header = new QLabel(this);
    header->setObjectName("filesHeader");
    header->setForegroundRole(QPalette::PlaceholderText);
    shortcuts = new QHBoxLayout;
    auto* headerRow = new QHBoxLayout;
    headerRow->addWidget(header, 1);
    headerRow->addLayout(shortcuts);
    layout->addLayout(headerRow);

    list = new CardFilesList(this);
    list->setObjectName("filesList");
    list->setColumnCount(3);
    list->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    list->setRootIsDecorated(false);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setSortingEnabled(true);
    list->sortByColumn(NameColumn, Qt::AscendingOrder);
    list->setDragEnabled(true);
    list->setAcceptDrops(true);
    list->setDragDropMode(QAbstractItemView::DragDrop);
    list->setDefaultDropAction(Qt::CopyAction);
    list->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    list->header()->setStretchLastSection(false);
    layout->addWidget(list, 1);
    setAcceptDrops(true);

    note = new QLabel(this);
    note->setObjectName("filesNote");
    layout->addWidget(note);

    auto button = [&](const QString& text, const char* name) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(name);
        b->setAutoDefault(false);
        return b;
    };
    auto* edits = new QHBoxLayout;
    deleteButton = button(tr("Delete"), "filesDelete");
    cutButton = button(tr("Cut"), "filesCut");
    copyButton = button(tr("Copy"), "filesCopy");
    pasteButton = button(tr("Paste"), "filesPaste");
    renameButton = button(tr("Rename…"), "filesRename");
    exportButton = button(tr("Export…"), "filesExport");
    selectAllButton = button(tr("Select all"), "filesSelectAll");
    for (auto* b : {deleteButton, cutButton, copyButton, pasteButton, renameButton, exportButton, selectAllButton}) edits->addWidget(b);
    edits->addStretch(1);
    layout->addLayout(edits);
    auto* adds = new QHBoxLayout;
    newFolderButton = button(tr("New folder…"), "filesNewFolder");
    addFilesButton = button(tr("Add files…"), "filesAddFiles");
    addFolderButton = button(tr("Add folder…"), "filesAddFolder");
    doneButton = button(tr("Done"), "filesDone");
    for (auto* b : {newFolderButton, addFilesButton, addFolderButton}) adds->addWidget(b);
    adds->addStretch(1);
    adds->addWidget(doneButton);
    layout->addLayout(adds);

    connect(upButton, &QPushButton::clicked, this, &CardFilesWindow::up);
    connect(pathBar, &QLineEdit::returnPressed, this, [this] { open(pathBar->text().trimmed().isEmpty() ? QStringLiteral("/") : pathBar->text().trimmed()); });
    connect(list, &QTreeWidget::itemActivated, this, &CardFilesWindow::activated);
    connect(list, &QTreeWidget::itemSelectionChanged, this, &CardFilesWindow::updateButtons);
    connect(deleteButton, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(cutButton, &QPushButton::clicked, this, &CardFilesWindow::cut);
    connect(copyButton, &QPushButton::clicked, this, &CardFilesWindow::copy);
    connect(pasteButton, &QPushButton::clicked, this, &CardFilesWindow::paste);
    connect(selectAllButton, &QPushButton::clicked, list, &QTreeWidget::selectAll);
    connect(renameButton, &QPushButton::clicked, this, [this] {
        const auto names = selectedNames();
        if (names.size() != 1) return;
        bool ok = false;
        const auto name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, names.first(), &ok);
        if (ok && !name.trimmed().isEmpty()) renameSelected(name);
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto folder = QFileDialog::getExistingDirectory(this, tr("Export to folder"));
        if (!folder.isEmpty()) exportSelected(folder);
    });
    connect(newFolderButton, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const auto name = QInputDialog::getText(this, tr("New folder"), tr("Name"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) makeFolder(name);
    });
    connect(addFilesButton, &QPushButton::clicked, this, [this] {
        const auto files = QFileDialog::getOpenFileNames(this, tr("Add files"));
        if (!files.isEmpty()) addPaths(files);
    });
    connect(addFolderButton, &QPushButton::clicked, this, [this] {
        const auto folder = QFileDialog::getExistingDirectory(this, tr("Add folder"));
        if (!folder.isEmpty()) addPaths({folder});
    });
    connect(doneButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(this, &QDialog::finished, this, &CardFilesWindow::closeCard);

    auto key = [&](const QKeySequence& sequence, std::function<void()> f) {
        auto* s = new QShortcut(sequence, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, std::move(f));
    };
    key(QKeySequence::Delete, [this] { if (deleteButton->isEnabled()) deleteSelected(); });
    key(QKeySequence::Cut, [this] { if (cutButton->isEnabled()) cut(); });
    key(QKeySequence::Copy, [this] { if (copyButton->isEnabled()) copy(); });
    key(QKeySequence::Paste, [this] { if (pasteButton->isEnabled()) paste(); });
    key(QKeySequence(Qt::Key_F2), [this] { if (renameButton->isEnabled()) renameButton->click(); });
    key(QKeySequence(Qt::ALT | Qt::Key_Up), [this] { up(); });
    key(QKeySequence(Qt::Key_Backspace), [this] { if (!pathBar->hasFocus()) up(); });

    askClash = [this](const QString& name, bool several) {
        QMessageBox box(QMessageBox::Question, tr("Name in use"), tr("%1 is already here.").arg(name), QMessageBox::NoButton, this);
        auto* replace = box.addButton(tr("Replace"), QMessageBox::AcceptRole);
        auto* skip = box.addButton(tr("Skip"), QMessageBox::RejectRole);
        auto* both = box.addButton(tr("Keep both"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        auto* all = several ? new QCheckBox(tr("For all"), &box) : nullptr;
        if (all) box.setCheckBox(all);
        box.setDefaultButton(both);
        clashBox = &box;
        box.exec();
        const bool forAll = all && all->isChecked();
        if (box.clickedButton() == replace) return cf::Decision{cf::Clash::Replace, forAll};
        if (box.clickedButton() == skip) return cf::Decision{cf::Clash::Skip, forAll};
        if (box.clickedButton() == both) return cf::Decision{cf::Clash::KeepBoth, forAll};
        return cf::Decision{cf::Clash::Cancel, false};
    };
    resize(720, 560);
}

CardFilesWindow::~CardFilesWindow() {
    card.reset();
    hold.reset();
}

void CardFilesWindow::done(int result) {
    if (busy && !internal()) {
        // The worker is still writing the card: stop it, close when it has.
        stopRequested = true;
        closeWanted = true;
        closeResult = result;
        if (clashBox) clashBox->reject();
        return;
    }
    QDialog::done(result);
    if (deleteOnClose) deleteWhenSafe();
}

void CardFilesWindow::deleteWhenSafe() {
    if (!busy && QThread::currentThread()->loopLevel() <= openLevel) {
        deleteLater();
        return;
    }
    QTimer::singleShot(20, this, &CardFilesWindow::deleteWhenSafe);
}

void CardFilesWindow::closeCard() {
    if (internal()) {
        source = {};
        waiting = false;
        updateButtons();
        return;
    }
    if (busy) {
        done(QDialog::Rejected);
        return;
    }
    card.reset();
    hold.reset();
    dragFolder.reset();
    updateButtons();
}

void CardFilesWindow::open(const QString& path) {
    if (!card) return;
    const auto normal = FatImage::parts(path).isEmpty() ? QStringLiteral("/") : '/' + FatImage::parts(path).join('/');
    try {
        FatImage fat(*card);
        fat.list(normal);
        current = normal;
    } catch (const frondCardFile::Error& e) {
        QMessageBox::warning(this, tr("Couldn't open the folder"), e.message);
    }
    relist();
}

void CardFilesWindow::up() {
    auto p = FatImage::parts(current);
    if (p.isEmpty()) return;
    p.removeLast();
    open('/' + p.join('/'));
}

void CardFilesWindow::relist() {
    const auto keep = selectedNames();
    list->clear();
    if (!card) { updateButtons(); return; }
    try {
        FatImage fat(*card);
        // The real names of each folder on the way, as the card has them.
        QString shown = QStringLiteral("/");
        for (const auto& part : FatImage::parts(current)) {
            const auto e = fat.find(FatImage::joined(shown, part));
            shown = FatImage::joined(shown, e ? e->name : part);
        }
        current = shown;
        const QLocale locale;
        for (const auto& e : fat.list(current)) {
            auto* item = new FileItem(list);
            item->entry = e;
            item->setText(NameColumn, e.name);
            item->setText(SizeColumn, e.directory ? tr("Folder") : locale.formattedDataSize(e.size));
            item->setText(DateColumn, e.modified.isValid() ? locale.toString(e.modified, QLocale::ShortFormat) : QString());
            item->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
            item->setIcon(NameColumn, style()->standardIcon(e.directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
            if (keep.contains(e.name)) item->setSelected(true);
        }
        auto text = tr("%1 free of %2").arg(locale.formattedDataSize(fat.freeBytes()), locale.formattedDataSize(fat.totalBytes()));
        if (wasChanged) text += tr(" · Changed");
        if (!writable) text += tr(" · Write protected");
        header->setText(text);
    } catch (const frondCardFile::Error& e) {
        header->setText(e.message);
    }
    pathBar->setText(current);
    list->resizeColumnToContents(SizeColumn);
    list->resizeColumnToContents(DateColumn);
    updateButtons();
}

void CardFilesWindow::updateButtons() {
    const int picked = int(list->selectedItems().size());
    if (internal()) {
        const bool ready = internal() && !waiting;
        deleteButton->setEnabled(ready && picked > 0);
        exportButton->setEnabled(ready && picked > 0);
        addFilesButton->setEnabled(ready);
        selectAllButton->setEnabled(ready && list->topLevelItemCount() > 0);
        list->setEnabled(internal());
        return;
    }
    const bool open = card != nullptr;
    const bool write = open && writable;
    upButton->setEnabled(open && current != "/");
    deleteButton->setEnabled(write && picked > 0);
    cutButton->setEnabled(write && picked > 0);
    copyButton->setEnabled(open && picked > 0);
    renameButton->setEnabled(write && picked == 1);
    exportButton->setEnabled(open && picked > 0);
    pasteButton->setEnabled(write && !clip.isEmpty());
    pasteButton->setText(clip.isEmpty() ? tr("Paste") : tr("Paste %1").arg(clip.size()));
    for (auto* b : {newFolderButton, addFilesButton, addFolderButton}) b->setEnabled(write);
    selectAllButton->setEnabled(open && list->topLevelItemCount() > 0);
    pathBar->setEnabled(open);
    list->setEnabled(open);
}

QStringList CardFilesWindow::selectedNames() const {
    QStringList names;
    for (auto* item : list->selectedItems()) names << static_cast<FileItem*>(item)->entry.name;
    return names;
}
QStringList CardFilesWindow::selectedPaths() const {
    QStringList paths;
    for (const auto& n : selectedNames()) paths << FatImage::joined(current, n);
    return paths;
}
void CardFilesWindow::selectNames(const QStringList& names) {
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = static_cast<FileItem*>(list->topLevelItem(i));
        item->setSelected(names.contains(item->entry.name, Qt::CaseInsensitive));
    }
}

void CardFilesWindow::activated(QTreeWidgetItem* item) {
    const auto& e = static_cast<FileItem*>(item)->entry;
    if (internal()) {
        if (waiting) return;
        const auto folder = QFileDialog::getExistingDirectory(this, tr("Export to folder"));
        if (!folder.isEmpty() && internal()) {
            waiting = true;
            updateButtons();
            source.exportTo({databaseOf.value(e.name)}, folder);
        }
        return;
    }
    if (e.directory) return open(FatImage::joined(current, e.name));
    const auto to = QFileDialog::getSaveFileName(this, tr("Export file"), QDir::homePath() + '/' + e.name);
    if (!to.isEmpty()) exportFile(e.name, to);
}

cf::Decision CardFilesWindow::askOnUi(const QString& name, bool several) {
    if (stopRequested) return cf::Decision{cf::Clash::Cancel, false};
    if (QThread::currentThread() == thread()) return askClash(name, several);
    cf::Decision d;
    QMetaObject::invokeMethod(this, [&] { d = stopRequested ? cf::Decision{cf::Clash::Cancel, false} : askClash(name, several); },
                              Qt::BlockingQueuedConnection);
    return d;
}

bool CardFilesWindow::job(const QString& label, const QString& failTitle, const Work& work) {
    if (!card || closeWanted || busy) return false;
    QString message;
    bool cancelled = false, committed = false;
    auto* opened = card.get();
    busy = true;
    stopRequested = false;
    const auto error = runCardJob(this, label, [&](const frondSessions::CopyProgress& progress) -> QString {
        qint64 total = 0, done = 0;
        const cf::Cancelled stop = [&] { return !progress(done, std::max<qint64>(total, 1)) || stopRequested.load(); };
        const cf::Bytes bytes = [&](qint64 n) { done = n; };
        if (workHook) workHook();
        try {
            FatImage fat(*opened);
            try {
                message = work(fat, stop, bytes, total);
            } catch (...) {
                committed = fat.committed;
                throw;
            }
            committed = fat.committed;
        } catch (const frondCardFile::Cancelled&) {
            cancelled = true;
        } catch (const frondCardFile::Error& e) {
            return e.message;
        } catch (const std::exception&) {
            return tr("Something went wrong with the card.");
        }
        return {};
    });
    busy = false;
    wasChanged = wasChanged || committed;
    relist();
    if (cancelled) message = tr("Cancelled.");
    note->setText(message);
    if (closeWanted) {
        // Closed while it ran: close now the worker has stopped, after this call returns.
        QTimer::singleShot(0, this, [this] { done(closeResult); });
        return false;
    }
    if (!error.isEmpty()) QMessageBox::warning(this, failTitle, error);
    return error.isEmpty() && !cancelled;
}

static QString counted(const cf::Report& r, const QString& what) {
    return QStringLiteral("%1 %2").arg(r.done).arg(what) + (r.skipped ? QObject::tr(", %1 skipped").arg(r.skipped) : QString());
}

void CardFilesWindow::addPaths(const QStringList& hostPaths) {
    if (hostPaths.isEmpty()) return;
    if (internal()) {
        // Databases install one file at a time; a folder adds the files in it.
        QStringList files;
        for (const auto& p : hostPaths) {
            if (QFileInfo(p).isDir()) {
                for (const auto& f : QDir(p).entryInfoList(QDir::Files, QDir::Name)) files << f.absoluteFilePath();
            } else files << QFileInfo(p).absoluteFilePath();
        }
        if (files.isEmpty() || waiting) return;
        waiting = true;
        updateButtons();
        source.install(files);
        return;
    }
    const auto at = current;
    job(tr("Copying…"), tr("Couldn't add the files"), [&](FatImage& fat, const cf::Cancelled& stop, const cf::Bytes& bytes, qint64& total) {
        QString problem;
        total = cf::hostBytes(hostPaths, &problem);
        if (!problem.isEmpty()) throw frondCardFile::Error(problem);
        if (total > fat.freeBytes()) throw frondCardFile::Error(tr("There isn't room on the card for that."));
        bool several = hostPaths.size() > 1;
        for (const auto& p : hostPaths) several = several || QFileInfo(p).isDir();
        cf::Resolver resolver([&](const QString& name) { return askOnUi(name, several); });
        return counted(cf::addHost(fat, hostPaths, at, resolver, stop, bytes), tr("added"));
    });
}

void CardFilesWindow::exportSelected(const QString& hostFolder) {
    if (internal()) {
        QStringList names;
        for (const auto& n : selectedNames()) names << databaseOf.value(n);
        if (names.isEmpty() || waiting) return;
        waiting = true;
        updateButtons();
        source.exportTo(names, hostFolder);
        return;
    }
    const auto paths = selectedPaths();
    if (paths.isEmpty()) return;
    job(tr("Copying…"), tr("Couldn't copy to the folder"), [&](FatImage& fat, const cf::Cancelled& stop, const cf::Bytes& bytes, qint64& total) {
        total = cf::bytesIn(fat, paths);
        const int n = cf::exportTo(fat, paths, hostFolder, stop, bytes);
        return n == 1 ? tr("1 file copied") : tr("%1 files copied").arg(n);
    });
}

void CardFilesWindow::exportFile(const QString& name, const QString& hostFile) {
    const auto path = FatImage::joined(current, name);
    job(tr("Copying…"), tr("Couldn't export the file"), [&](FatImage& fat, const cf::Cancelled& stop, const cf::Bytes& bytes, qint64& total) {
        const auto e = fat.find(path);
        if (!e || e->directory) throw frondCardFile::Error(tr("%1 isn't on the card.").arg(name));
        total = e->size;
        QSaveFile out(hostFile);
        if (!out.open(QIODevice::WriteOnly)) throw frondCardFile::Error(tr("Couldn't write there."));
        qint64 done = 0;
        fat.read(*e, [&](const char* b, qint64 n) {
            if (stop()) throw frondCardFile::Cancelled();
            if (out.write(b, n) != n) throw frondCardFile::Error(tr("Couldn't write there."));
            bytes(done += n);
        });
        if (!out.commit()) throw frondCardFile::Error(tr("Couldn't write there."));
        return tr("%1 exported").arg(e->name);
    });
}

void CardFilesWindow::makeFolder(const QString& name) {
    const auto at = current;
    job(tr("Copying…"), tr("Couldn't make the folder"), [&](FatImage& fat, const cf::Cancelled&, const cf::Bytes&, qint64&) {
        fat.makeFolder(at, name.trimmed());
        return QString();
    });
}

void CardFilesWindow::renameSelected(const QString& name) {
    const auto paths = selectedPaths();
    if (paths.size() != 1) return;
    job(tr("Renaming…"), tr("Couldn't rename"), [&](FatImage& fat, const cf::Cancelled&, const cf::Bytes&, qint64&) {
        cf::rename(fat, paths.first(), name);
        return QString();
    });
    selectNames({name.trimmed()});
}

void CardFilesWindow::deleteSelected(bool ask) {
    const auto names = selectedNames();
    if (names.isEmpty()) return;
    if (ask) {
        bool folders = false;
        for (auto* item : list->selectedItems()) folders = folders || static_cast<FileItem*>(item)->entry.directory;
        QMessageBox box(QMessageBox::Question, tr("Delete"),
                        names.size() == 1 ? tr("Delete %1?").arg(names.first()) : tr("Delete %1 items?").arg(names.size()),
                        QMessageBox::Cancel, this);
        if (folders) box.setInformativeText(tr("Folders go with everything in them."));
        auto* yes = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != yes) return;
    }
    if (internal()) {
        QStringList databases;
        for (const auto& n : names) databases << databaseOf.value(n);
        if (waiting || !internal()) return;
        waiting = true;
        updateButtons();
        source.remove(databases);
        return;
    }
    const auto paths = selectedPaths();
    job(tr("Deleting…"), tr("Couldn't delete"), [&](FatImage& fat, const cf::Cancelled& stop, const cf::Bytes&, qint64&) {
        cf::remove(fat, paths, stop);
        return tr("Deleted");
    });
    // Cut entries that went are dropped from the clip.
    if (!clip.isEmpty()) {
        QStringList left;
        for (const auto& c : clip)
            if (std::none_of(paths.begin(), paths.end(), [&](const QString& p) { return FatImage::within(p, c); })) left << c;
        clip = left;
    }
    updateButtons();
}

void CardFilesWindow::cut() {
    clip = selectedPaths();
    clipMove = true;
    note->setText(clip.size() == 1 ? tr("1 item cut") : tr("%1 items cut").arg(clip.size()));
    updateButtons();
}
void CardFilesWindow::copy() {
    clip = selectedPaths();
    clipMove = false;
    note->setText(clip.size() == 1 ? tr("1 item copied") : tr("%1 items copied").arg(clip.size()));
    updateButtons();
}
void CardFilesWindow::paste() {
    if (clip.isEmpty()) return;
    const auto sources = clip;
    const bool move = clipMove;
    const auto at = current;
    const bool ok = job(move ? tr("Moving…") : tr("Copying…"), tr("Couldn't paste"),
                        [&](FatImage& fat, const cf::Cancelled& stop, const cf::Bytes& bytes, qint64& total) {
        total = move ? 1 : cf::bytesIn(fat, sources);
        cf::Resolver resolver([&](const QString& name) { return askOnUi(name, sources.size() > 1); });
        return counted(cf::paste(fat, sources, at, move, resolver, stop, bytes), move ? tr("moved") : tr("copied"));
    });
    // A move empties the clip; a copy can be pasted again.
    if (move && ok) clip.clear();
    updateButtons();
}

// Copies the selection into a private folder for a drag to another program. The paths, or none.
QStringList CardFilesWindow::dragOut() {
    const auto paths = selectedPaths();
    if (paths.isEmpty() || !card || internal()) return {};
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cache);
    dragFolder = std::make_unique<QTemporaryDir>(cache + "/card-drag-XXXXXX");
    if (!dragFolder->isValid()) return {};
    const auto folder = dragFolder->path();
    if (!job(tr("Copying…"), tr("Couldn't copy the files"), [&](FatImage& fat, const cf::Cancelled& stop, const cf::Bytes& bytes, qint64& total) {
            total = cf::bytesIn(fat, paths);
            cf::exportTo(fat, paths, folder, stop, bytes);
            return QString();
        }))
        return {};
    QStringList out;
    for (const auto& e : QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) out << e.absoluteFilePath();
    return out;
}

void CardFilesWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->source() != list && ((writable && card) || (internal() && !waiting)) && event->mimeData()->hasUrls()) event->acceptProposedAction();
    else event->ignore();
}
void CardFilesWindow::dropEvent(QDropEvent* event) {
    if (event->source() == list || !((writable && card) || internal())) return event->ignore();
    QStringList paths;
    for (const auto& url : event->mimeData()->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    if (paths.isEmpty()) return event->ignore();
    event->acceptProposedAction();
    // After the drop returns, so the dragging program isn't held up.
    QMetaObject::invokeMethod(this, [this, paths] { addPaths(paths); }, Qt::QueuedConnection);
}
