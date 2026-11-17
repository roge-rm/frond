#pragma once
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include "DanaCardsDialog.h"

namespace frondSecondCard {
// What sd2-state says: none, inserted or ejected.
struct State {
    bool known{}, present{}, inserted{};
};
inline State state(const QString& text) {
    if (text == "none") return {true};
    if (text == "inserted" || text == "ejected") return {true, true, text == "inserted"};
    return {};
}
// New card sizes in MB the slot takes.
inline QList<int> sizes(const DeviceProfile5& profile) {
    QList<int> result;
    for (int size : {32, 64, 128, 256, 512, 1024, 2048})
        if (deviceProfileCardSizeValid5(profile.deviceType, DeviceCardSlot5::sd2, DeviceCardFamily5::sdsc, quint64(size) << 20))
            result.append(size);
    return result;
}
}

// The Zodiac's second SD slot, through the core's sd2- commands. The card lives in the machine
// folder's sd2.frondsd.
class SecondCardDialog : public QDialog {
 public:
    SecondCardDialog(DesktopClient& client, frondFirmware::Device device, QWidget* parent = nullptr)
        : QDialog(parent), client(client), profile(frondFirmware::profile(device)) {
        setWindowTitle(tr("SD card 2"));
        auto* layout = new QVBoxLayout(this);
        status = new QLabel(this);
        layout->addWidget(status);
        auto* make = new QHBoxLayout;
        layout->addLayout(make);
        size = new QComboBox(this);
        for (int mb : frondSecondCard::sizes(profile)) size->addItem(QString::number(mb) + " MB", mb);
        newCard = new QPushButton(tr("New card"), this);
        make->addWidget(size);
        make->addWidget(newCard);
        insertImage = new QPushButton(tr("Insert image"), this);
        reinsert = new QPushButton(tr("Insert"), this);
        eject = new QPushButton(tr("Eject"), this);
        exportImage = new QPushButton(tr("Export image"), this);
        for (auto* b : {insertImage, reinsert, eject, exportImage}) layout->addWidget(b);
        connect(newCard, &QPushButton::clicked, this, [this] {
            if (perform("create", size->currentData().toString())) refresh();
        });
        connect(insertImage, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, tr("Insert image"), QString(), tr("SD images (*.img);;All files (*)"));
            if (!path.isEmpty() && perform("insert", path)) refresh();
        });
        connect(reinsert, &QPushButton::clicked, this, [this] { if (perform("reinsert")) refresh(); });
        connect(eject, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(this, tr("Eject the card?"), tr("Close any files open on the Palm first.")) == QMessageBox::Yes &&
                perform("eject"))
                refresh();
        });
        connect(exportImage, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getSaveFileName(this, tr("Export image"), "SD card 2.img", tr("SD images (*.img)"));
            if (path.isEmpty()) return;
            if (const auto problem = frondDanaCards::exportProblem(path); !problem.isEmpty()) QMessageBox::warning(this, tr("SD card 2"), problem);
            else perform("export", path);
        });
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
        layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
        QTimer::singleShot(0, this, [this] { refresh(); });
    }
    // The buttons the card's state allows, as the slot 1 row shows them.
    void apply(const frondSecondCard::State& s) {
        status->setText(!s.known ? tr("Not known") : !s.present ? tr("No card") : s.inserted ? tr("Inserted") : tr("Ejected"));
        size->setVisible(s.known && !s.present);
        newCard->setVisible(s.known && !s.present);
        insertImage->setVisible(s.known && !s.present);
        reinsert->setVisible(s.known && s.present && !s.inserted);
        eject->setVisible(s.known && s.inserted);
        exportImage->setVisible(s.known && s.present && !s.inserted);
    }
 private:
    DesktopClient& client;
    const DeviceProfile5& profile;
    QLabel* status{};
    QComboBox* size{};
    QPushButton *newCard{}, *insertImage{}, *reinsert{}, *eject{}, *exportImage{};
    bool perform(const QString& operation, const QString& argument = {}) {
        const auto result = DanaCardsDialog::requestWithClient(client, "sd2-" + operation, argument);
        if (!result.ok) QMessageBox::warning(this, tr("SD card 2"), result.message);
        return result.ok;
    }
    void refresh() {
        const auto reply = DanaCardsDialog::requestWithClient(client, "sd2-state");
        apply(reply.ok ? frondSecondCard::state(reply.message) : frondSecondCard::State{});
    }
};
