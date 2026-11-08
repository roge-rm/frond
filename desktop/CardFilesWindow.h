#pragma once
// The file manager for every medium: a card file (sd.frondsd, ms.frondms or ms.frondmsp) opened
// with CardFile and edited in place, each change committed as it's made, the same for a card
// opened from Cards and for the running machine's card while it is taken out; and Internal, a
// running machine's RAM databases through the core's database commands, which has no folders.
// Closing the window ends the visit.
#include <QDialog>
#include <QHash>
#include <QLockFile>
#include <QPointer>
#include <QTemporaryDir>
#include <atomic>
#include <functional>
#include <memory>
#include "CardFiles.h"
#include "RunningRules.h"

class QLabel;
class QMessageBox;
class QLineEdit;
class QPushButton;
class QHBoxLayout;
class QTreeWidget;
class QTreeWidgetItem;

class CardFilesWindow : public QDialog {
    Q_OBJECT
 public:
    // `card` is open for this visit; `writable` false for a write-protected card.
    CardFilesWindow(std::unique_ptr<frondCardFile::CardFile> card, const QString& title, QWidget* parent = nullptr);
    // Internal: what the window asks of the running machine. Each runs one job through the core and
    // answers with databasesDone(); `list` answers with setDatabases().
    struct Databases {
        std::function<void()> list;
        std::function<void(const QStringList& hostFiles)> install;
        std::function<void(const QStringList& names, const QString& hostFolder)> exportTo;
        std::function<void(const QStringList& names)> remove;
    };
    CardFilesWindow(Databases databases, const QString& title, QWidget* parent = nullptr);
    bool internal() const { return source.list != nullptr; }
    // The databases db-list found; the window lists them.
    void setDatabases(const std::vector<runningRules::Database>& all);
    // A database job ended with `message` for the note; the list is asked for again.
    void databasesDone(const QString& message);
    ~CardFilesWindow() override;

    // Held until the card is closed: the machine's lock for a card in a stopped machine.
    std::unique_ptr<QLockFile> hold;
    // Closes the card file and lets go of `hold`. Called on close; safe to call again. While a
    // job runs it cancels the job and closes once its worker has stopped.
    void closeCard();
    // Set by the owner instead of WA_DeleteOnClose: deleted after closing, once no job and no
    // event loop started since it opened (a prompt, a job's progress) is still on the stack.
    bool deleteOnClose{false};
    // A closing that waits for a job is held back until the worker stops.
    void done(int result) override;
    bool working() const { return busy || waiting; }
    // Run on the job's worker thread before the work, for the tests.
    std::function<void()> workHook;
    bool changed() const { return wasChanged; }
    QString path() const { return current; }

    // What the buttons do once their dialogs are answered; the tests call these.
    void open(const QString& path);
    void up();
    void addPaths(const QStringList& hostPaths);
    void exportSelected(const QString& hostFolder);
    void exportFile(const QString& name, const QString& hostFile);
    void makeFolder(const QString& name);
    void renameSelected(const QString& name);
    void deleteSelected(bool ask = true);
    void cut();
    void copy();
    void paste();
    void selectNames(const QStringList& names);
    QStringList selectedNames() const;
    // Asks about a name clash; `several` offers For all. The tests replace it.
    std::function<frondCardFiles::Decision(const QString& name, bool several)> askClash;

    QTreeWidget* list;
    QLineEdit* pathBar;
    QLabel *header, *note;
    QPushButton *upButton, *deleteButton, *cutButton, *copyButton, *pasteButton, *renameButton, *exportButton, *addFilesButton,
        *addFolderButton, *newFolderButton, *selectAllButton, *doneButton;

 protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

 private:
    void build(const QString& title);
    using Work = std::function<QString(frondCardFile::FatImage&, const frondCardFiles::Cancelled&, const frondCardFiles::Bytes&, qint64& total)>;
    // Runs a job on the card behind a progress dialog with Cancel, then lists the folder again.
    // Returns false when it failed (the message is shown).
    bool job(const QString& label, const QString& failTitle, const Work& work);
    void relist();
    void updateButtons();
    void activated(QTreeWidgetItem* item);
    frondCardFiles::Decision askOnUi(const QString& name, bool several);
    QStringList selectedPaths() const;
    QStringList dragOut();
    friend class CardFilesList;

    std::unique_ptr<frondCardFile::CardFile> card;
    QString current{QStringLiteral("/")};
    QStringList clip;
    bool clipMove{false}, wasChanged{false}, writable{true};
    QHBoxLayout* shortcuts;
    std::unique_ptr<QTemporaryDir> dragFolder;
    // A job is running; a close asked for meanwhile (and its result) waits for it.
    bool busy{false}, closeWanted{false};
    int closeResult{0};
    std::atomic<bool> stopRequested{false};
    QPointer<QMessageBox> clashBox;
    int openLevel{0};
    // Internal: the commands, the database each listed file is, and a job in the core.
    Databases source;
    QHash<QString, QString> databaseOf;
    bool waiting{false}, listed{false};
    void deleteWhenSafe();
};
