#pragma once
#include <QDialog>
#include <functional>
#include "FolderCard.h"
#include "SessionStore.h"
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QRadioButton;
class QVBoxLayout;
class CardFilesWindow;

// Runs `work` on a worker thread behind a progress dialog with Cancel; the progress it is given
// returns false once Cancel is pressed. Returns work's error, empty on success.
QString runCardJob(QWidget* parent, const QString& text, std::function<QString(const frondSessions::CopyProgress&)> work);

// Name, Type and Size for a new card. With a device, only the types its slot takes and the sizes
// it can't take greyed; with automatic, every type and size some device takes.
class NewCardDialog : public QDialog {
    Q_OBJECT
 public:
    NewCardDialog(const QString& sessions, frondFirmware::Device device, QWidget* parent = nullptr);
    QString kind() const;
    int size() const;  // as in frondCards::sizesFor
    QLineEdit* name;
    QComboBox *type, *sizes;
 private:
    void fillSizes();
    QString sessions;
    frondFirmware::Device device;
    bool nameEdited{false};
};

// Name, Type and Size for a card built from a host folder. Types are the ones a folder can
// become (SD, Memory Stick Pro); sizes the folder doesn't fit are greyed "Too small" and the
// default is the smallest it fits.
class FolderCardDialog : public QDialog {
    Q_OBJECT
 public:
    FolderCardDialog(const QString& sessions, const QString& folder, const frondFolderCard::Plan& plan, QWidget* parent = nullptr);
    QString kind() const;
    int size() const;
    QLineEdit* name;
    QComboBox *type, *sizes;
 private:
    void fillSizes();
    QString sessions;
    frondFolderCard::Plan plan;
    bool nameEdited{false};
};

// "Link Games to machines": a checklist of machines that take the card.
class LinkDialog : public QDialog {
    Q_OBJECT
 public:
    // `held` is this window's running machine; `tick` is ticked to start with.
    LinkDialog(const QString& sessions, const QString& cardId, const QString& held, const QString& tick, QWidget* parent = nullptr);
    QStringList linked() const;  // machine folders ticked
    QListWidget* machines;
};

// "SD card for Palm TX": No card or a card from the drawer for the running machine's slot.
class CardPickerDialog : public QDialog {
    Q_OBJECT
 public:
    CardPickerDialog(const QString& sessions, const QString& machineDir, frondFirmware::Device device, const QString& current,
                     QWidget* parent = nullptr);
    QString chosen() const;  // card id, empty for No card
    void fill(const QString& select);
    QPushButton *newCard, *linkCards;
 private:
    QString sessions, machineDir, current;
    frondFirmware::Device device;
    QVBoxLayout* choices;
    QList<QRadioButton*> radios;
};

class CardsWindow : public QDialog {
    Q_OBJECT
 public:
    CardsWindow(const QString& sessions, QWidget* parent = nullptr);
    void refresh();
    QMenu* menuFor(const QString& id, QWidget* parent);
    // The window's running machine (empty when none), whether its card is in an exchange, and
    // Take out through its running controls.
    std::function<QString()> runningMachine;
    std::function<bool()> exchangeOpen;
    std::function<void(const QString& id)> takeOutRunning;
    std::function<void()> changed;
    QListWidget *sdList, *msList;
    QLabel *sdHeading, *msHeading, *sdEmpty, *msEmpty;
    // Files… on a card: a free card or one in a stopped machine, edited in place.
    void openFiles(const QString& id);
    bool discardChange(const QString& path);
    CardFilesWindow* files{nullptr};
 private:
    void newCard();
    void importImage();
    void importFolder();
    void renameCard(const QString& id);
    void duplicateCard(const QString& id);
    void exportImage(const QString& id);
    void linkCard(const QString& id);
    void takeOut(const QString& id);
    void deleteCard(const QString& id);
    void failed(const QString& message);
    void fitRows();
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    QString held() const { return runningMachine ? runningMachine() : QString(); }
    QString sessions;
};
