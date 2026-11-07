#include "RunningBar.h"
#include <QActionGroup>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QToolButton>

using runningRules::Id;

RunningBar::RunningBar(QWidget* parent) : QToolBar(parent) {
    setObjectName("runningBar");
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setIconSize({16, 16});
    setFixedHeight(36);
    machineMenu = new QMenu(tr("&Machine"), this);
    machineMenu->setObjectName("machineMenu");
    static const std::pair<Id, const char*> names[] = {
        {Id::Hold, "machineHold"}, {Id::Keyboard, "machineKeyboard"}, {Id::Microphone, "machineMicrophone"},
        {Id::Network, "machineNetwork"}, {Id::Display, "machineDisplay"}, {Id::Connections, "machineConnections"},
        {Id::Screenshot, "machineScreenshot"}, {Id::HideBar, "machineHideBar"}, {Id::FullScreen, "machineFullScreen"},
        {Id::Save, "machineSave"}, {Id::SaveCopy, "machineSaveCopy"}, {Id::Log, "machineLog"},
        {Id::Reset, "machineReset"}, {Id::HardReset, "machineHardReset"}, {Id::SaveStop, "machineSaveStop"}};
    int group = 0;
    // Every row is made here; apply() shows the ones a device has.
    runningRules::Traits all;
    all.holdPresent = all.holdSupported = all.keyboard = all.microphone = all.network = true;
    for (const auto& row : runningRules::rows(all, {})) {
        if (row.group != group) { machineMenu->addSeparator(); group = row.group; }
        QAction* action;
        if (row.id == Id::Network) {
            networkMenu = machineMenu->addMenu(row.label);
            action = networkMenu->menuAction();
        } else {
            action = machineMenu->addAction(row.label);
            const auto id = row.id;
            connect(action, &QAction::triggered, this, [this, id] { emit chosen(id); });
        }
        for (const auto& pair : names) if (pair.first == row.id) action->setObjectName(pair.second);
        actions[row.id] = action;
    }
    for (auto id : {Id::Hold, Id::Keyboard, Id::Microphone}) actions[id]->setCheckable(true);

    auto button = [this](const QString& text, const QString& name, const QString& tip, Button which) {
        auto* action = addAction(text);
        action->setObjectName(name);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, which] { emit pressed(which); });
        return action;
    };
    stopAction = button(QString::fromUtf8("←"), "barStop", tr("Save and Stop"), Button::Stop);
    nameLabel = new QLabel(this);
    nameLabel->setObjectName("barName");
    auto font = nameLabel->font(); font.setBold(true); nameLabel->setFont(font);
    subtitleLabel = new QLabel(this);
    subtitleLabel->setObjectName("barSubtitle");
    subtitleLabel->setEnabled(false);
    statusLabel = new QLabel(this);
    statusLabel->setObjectName("barStatus");
    for (auto* l : {nameLabel, subtitleLabel, statusLabel}) l->setContentsMargins(8, 0, 4, 0);
    addWidget(nameLabel); addWidget(subtitleLabel); addWidget(statusLabel);
    forceAction = button(tr("Force Stop…"), "barForceStop", tr("Force Stop…"), Button::ForceStop);
    forceAction->setVisible(false);
    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(spacer);
    keyboardAction = addAction(QString::fromUtf8("⌨"));
    keyboardAction->setObjectName("barKeyboard");
    keyboardAction->setToolTip(tr("Infrared keyboard attached"));
    keyboardAction->setVisible(false);
    applicationsAction = button(tr("Applications"), "barApplications", tr("Applications"), Button::Applications);
    cardAction = button(tr("Card"), "barCard", tr("Card"), Button::Card);
    soundAction = button(tr("Sound"), "barSound", tr("Sound"), Button::Sound);
    soundAction->setCheckable(true);
    powerAction = button(tr("Power"), "barPower", tr("Power"), Button::Power);
    auto* more = new QToolButton(this);
    more->setObjectName("barMore");
    more->setText(QString::fromUtf8("⋮"));
    more->setToolTip(tr("Machine"));
    more->setPopupMode(QToolButton::InstantPopup);
    more->setMenu(machineMenu);
    more->setStyleSheet("QToolButton::menu-indicator { image: none; }");
    addWidget(more);
    statusTimer = new QTimer(this);
    statusTimer->setSingleShot(true);
    connect(statusTimer, &QTimer::timeout, statusLabel, &QLabel::clear);
    hide();
}

void RunningBar::setMachine(const QString& name, const QString& subtitle) {
    nameLabel->setText(name);
    subtitleLabel->setText(subtitle);
}

void RunningBar::say(const QString& text, int ms) {
    statusLabel->setText(text);
    statusLabel->setToolTip(text);
    if (ms > 0 && !text.isEmpty()) statusTimer->start(ms);
    else statusTimer->stop();
}

void RunningBar::setForceStop(bool on) { forceAction->setVisible(on); }

void RunningBar::setBarShown(bool on) {
    shown = on;
    setVisible(wantVisible && shown);
}

void RunningBar::apply(const runningRules::Traits& traits, const runningRules::State& state, runningRules::CardIcon card,
                       bool soundOn, const QList<QPair<QString, QString>>& networks) {
    wantVisible = state.running;
    shown = state.barShown;
    setVisible(wantVisible && shown);
    for (const auto& row : runningRules::rows(traits, state)) {
        auto* action = actions[row.id];
        action->setVisible(true);
        action->setText(row.label);
        action->setEnabled(row.enabled);
        if (row.on) action->setChecked(*row.on);
        action->setToolTip(row.value);
        if (row.id == Id::Network) {
            // Built once, so a menu that is open isn't replaced under the pointer.
            if (networkMenu->actions().size() != networks.size()) {
                networkMenu->clear();
                auto* group = new QActionGroup(networkMenu);
                for (const auto& [mode, label] : networks) {
                    auto* item = networkMenu->addAction(label);
                    item->setObjectName("machineNetwork_" + mode);
                    item->setData(mode);
                    item->setCheckable(true);
                    group->addAction(item);
                    connect(item, &QAction::triggered, this, [this, mode = mode] { emit networkChosen(mode); });
                }
            }
            for (auto* item : networkMenu->actions()) item->setChecked(item->data().toString() == row.value);
        }
    }
    // A 68k device leaves out the rows for hardware it lacks.
    const auto present = runningRules::rows(traits, state);
    for (auto& [id, action] : actions) {
        bool listed = false;
        for (const auto& row : present) listed = listed || row.id == id;
        action->setVisible(listed);
    }
    const auto buttons = runningRules::buttons(traits, state);
    cardAction->setVisible(buttons.card);
    cardAction->setEnabled(buttons.cardEnabled);
    static const QString glyph[] = {QString(), QString::fromUtf8("▫"), QString::fromUtf8("▣"), QString::fromUtf8("▢")};
    static const char* tips[] = {"", "No card", "Card inserted", "Card ejected"};
    cardAction->setText(tr("Card") + " " + glyph[int(card)]);
    cardAction->setToolTip(tr(tips[int(card)]));
    powerAction->setVisible(buttons.power);
    powerAction->setEnabled(buttons.powerEnabled);
    keyboardAction->setVisible(buttons.keyboard);
    soundAction->setEnabled(buttons.soundEnabled);
    soundAction->setChecked(soundOn);
    applicationsAction->setEnabled(true);
    stopAction->setEnabled(actions[Id::SaveStop]->isEnabled());
}
