#include "ControlsEditor.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString& controllerCore() {
    static QString core;
    return core;
}
}

void setControllerCore(const QString& core) { controllerCore() = core; }

ControlsDialog::ControlsDialog(const DeviceProfile5& profile, const ControllerMap& map, QWidget* parent)
    : QDialog(parent), profile(profile), current(map) {
    setWindowTitle(tr("Controls"));
    setObjectName("controlsDialog");
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* rows = new QWidget(scroll);
    grid = new QGridLayout(rows);
    grid->setColumnStretch(1, 1);
    scroll->setWidget(rows);
    layout->addWidget(scroll, 1);
    status = new QLabel(this);
    status->setObjectName("controlsStatus");
    status->hide();
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* reset = buttons->addButton(tr("Reset to defaults"), QDialogButtonBox::ResetRole);
    reset->setObjectName("controlsReset");
    connect(reset, &QPushButton::clicked, this, [this] {
        stopCapture();
        current = controllerDefaults(this->profile);
        defaulted = true;
        fill();
        emit changed(current, true);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);
    fill();
    resize(460, 520);
}

ControlsDialog::~ControlsDialog() { stopCapture(); }

// A row per Palm button: its name, a button per controller key that presses it (clicking one takes
// it off), and Add. The Zodiac starts with the controller stick that moves its analogue stick.
void ControlsDialog::fill() {
    // Later, as the button that asked for this may be one of them.
    while (auto* item = grid->takeAt(0)) {
        if (auto* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
    int row = 0;
    if (zodiacController(profile)) {
        grid->addWidget(new QLabel(tr("Analogue stick")), row, 0);
        auto* stick = new QComboBox;
        stick->setObjectName("controlsStick");
        stick->addItem(tr("Left stick"), int(PadStick::left));
        stick->addItem(tr("Right stick"), int(PadStick::right));
        stick->addItem(tr("None"), int(PadStick::none));
        stick->setCurrentIndex(stick->findData(int(current.stick)));
        connect(stick, &QComboBox::activated, this, [this, stick](int index) {
            const auto chosen = PadStick(stick->itemData(index).toInt());
            if (chosen == current.stick) return;
            stopCapture();
            current.setStick(chosen);
            defaulted = false;
            fill();
            emit changed(current, false);
        });
        grid->addWidget(stick, row, 1, 1, 2);
        ++row;
    }
    for (const auto button : controllerPalmKeys(profile)) {
        auto* name = new QLabel(QString::fromStdString(palmKeyLabel(button, profile)));
        grid->addWidget(name, row, 0);
        auto* keys = new QWidget;
        auto* keysLayout = new QHBoxLayout(keys);
        keysLayout->setContentsMargins(0, 0, 0, 0);
        const auto assigned = current.keysFor(button);
        if (assigned.empty()) keysLayout->addWidget(new QLabel(tr("None")));
        for (const auto key : assigned) {
            const auto keyName = QString::fromUtf8(padKeyName(key));
            auto* chip = new QToolButton;
            chip->setText(keyName + QStringLiteral("  ×"));
            chip->setToolTip(tr("Remove %1").arg(keyName));
            chip->setObjectName(QStringLiteral("controlsKey-") + palmKeyId(button) + '-' + padKeyId(key));
            connect(chip, &QToolButton::clicked, this, [this, key] {
                stopCapture();
                current.remove(key);
                defaulted = false;
                fill();
                emit changed(current, false);
            });
            keysLayout->addWidget(chip);
        }
        keysLayout->addStretch();
        grid->addWidget(keys, row, 1);
        auto* add = new QPushButton(waiting == button ? tr("Press a button…") : tr("Add"));
        add->setObjectName(QStringLiteral("controlsAdd-") + palmKeyId(button));
        add->setCheckable(true);
        add->setChecked(waiting == button);
        connect(add, &QPushButton::clicked, this, [this, button] {
            if (waiting == button) { stopCapture(); fill(); }
            else capture(button);
        });
        grid->addWidget(add, row, 2);
        ++row;
    }
    grid->setRowStretch(row, 1);
}

// Runs the core's --controller-listen and takes the first button it prints.
void ControlsDialog::capture(PalmKey button) {
    stopCapture();
    status->hide();
    if (controllerCore().isEmpty()) {
        status->setText(tr("Can't read game controllers here."));
        status->show();
        return;
    }
    waiting = button;
    listener = new QProcess(this);
    listener->setProcessChannelMode(QProcess::SeparateChannels);
    connect(listener, &QProcess::readyReadStandardOutput, this, &ControlsDialog::keyReceived);
    connect(listener, &QProcess::finished, this, [this] {
        if (!listener) return;
        listener->deleteLater();
        listener = nullptr;
        waiting = PalmKey::count;
        fill();
    });
    connect(listener, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !listener) return;
        listener->deleteLater();
        listener = nullptr;
        waiting = PalmKey::count;
        status->setText(tr("Can't read game controllers here."));
        status->show();
        fill();
    });
    pending.clear();
    listener->start(controllerCore(), {"--controller-listen"});
    fill();
}

void ControlsDialog::stopCapture() {
    waiting = PalmKey::count;
    if (!listener) return;
    auto* process = listener;
    listener = nullptr;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(1000);
    process->deleteLater();
}

void ControlsDialog::keyReceived() {
    if (!listener) return;
    pending += listener->readAllStandardOutput();
    int end;
    while ((end = pending.indexOf('\n')) >= 0) {
        const auto line = QString::fromUtf8(pending.left(end)).trimmed().toStdString();
        pending.remove(0, end + 1);
        PadKey key;
        if (padKeyFromId(line, key)) return take(key);
    }
}

void ControlsDialog::take(PadKey key) {
    const auto button = waiting;
    stopCapture();
    if (button == PalmKey::count) return;
    current.assign(button, key);
    defaulted = false;
    fill();
    emit changed(current, false);
}
