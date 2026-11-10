#include "RunningBar.h"
#include "Icons.h"
#include <QCoreApplication>
#include <QKeyEvent>
#include <algorithm>
#include <QPainter>
#include <QWidgetAction>
#include <QActionGroup>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QToolButton>

using runningRules::Id;

namespace {
// A menu row in red for the destructive Hard reset. A plain QAction can't be coloured.
class RedItem : public QWidget {
 public:
    RedItem(QAction* action, QWidget* parent) : QWidget(parent), act(action) {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setEnabled(action->isEnabled());
        // The menu's arrow-key highlight moves without the mouse, so repaint when it does.
        if (auto* menu = qobject_cast<QMenu*>(parentWidget())) connect(menu, &QMenu::hovered, this, [this] { update(); });
    }
    QSize sizeHint() const override {
        return {fontMetrics().horizontalAdvance(act->text()) + 52, fontMetrics().height() + 10};
    }
 protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const auto* menu = qobject_cast<QMenu*>(parentWidget());
        const bool lit = isEnabled() && (underMouse() || hasFocus() || (menu && menu->activeAction() == act));
        if (lit) p.fillRect(rect(), palette().color(QPalette::Highlight));
        QColor colour(205, 40, 40);
        if (!isEnabled()) colour = palette().color(QPalette::Disabled, QPalette::Text);
        else if (lit) colour = Qt::white;
        p.setPen(colour);
        p.drawText(rect().adjusted(26, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, act->text());
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())) choose();
    }
    void keyPressEvent(QKeyEvent* e) override {
        static const int navigation[] = {Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End,
                                         Qt::Key_Escape};
        auto* menu = qobject_cast<QMenu*>(parentWidget());
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) choose();
        else if (menu && std::find(std::begin(navigation), std::end(navigation), e->key()) != std::end(navigation)) {
            // The menu walks its rows, this one included.
            QCoreApplication::sendEvent(menu, e);
            e->accept();
        } else QWidget::keyPressEvent(e);
    }
    void actionEvent(QActionEvent* e) override {
        if (e->type() == QEvent::ActionChanged) { setEnabled(act->isEnabled()); updateGeometry(); update(); }
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
 private:
    void choose() {
        if (!isEnabled()) return;
        if (auto* menu = qobject_cast<QMenu*>(parentWidget())) menu->close();
        act->trigger();
    }
    QAction* act;
};
class RedAction : public QWidgetAction {
 public:
    using QWidgetAction::QWidgetAction;
 protected:
    QWidget* createWidget(QWidget* parent) override { return new RedItem(this, parent); }
};
}  // namespace

RunningBar::RunningBar(QWidget* parent) : QToolBar(parent) {
    setObjectName("runningBar");
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setIconSize({20, 20});
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
            if (row.id == Id::HardReset) {
                action = new RedAction(machineMenu);
                action->setText(row.label);
                machineMenu->addAction(action);
            } else action = machineMenu->addAction(row.label);
            const auto id = row.id;
            connect(action, &QAction::triggered, this, [this, id] { emit chosen(id); });
        }
        for (const auto& pair : names) if (pair.first == row.id) action->setObjectName(pair.second);
        // The keys only show beside the row, as text after a tab. The window's own shortcuts do the
        // work, so no row owns a key and none can fire twice or clash.
        for (const auto& key : runningRules::shortcutKeys())
            if (runningRules::rowFor(key.what) == row.id)
                keyText[row.id] = QKeySequence(QString::fromLatin1(key.keys)).toString(QKeySequence::NativeText);
        if (!keyText[row.id].isEmpty() && row.id != Id::Network) action->setText(row.label + '\t' + keyText[row.id]);
        actions[row.id] = action;
    }
    for (auto id : {Id::Hold, Id::Keyboard, Id::Microphone}) actions[id]->setCheckable(true);

    auto button = [this](const QString& icon, const QString& name, const QString& tip, Button which) {
        auto* action = addAction(frondIcons::icon(icon), tip);
        action->setObjectName(name);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, which] { emit pressed(which); });
        return action;
    };
    stopAction = button("back", "barStop", tr("Save and Stop"), Button::Stop);
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
    forceAction = button({}, "barForceStop", tr("Force Stop…"), Button::ForceStop);
    forceAction->setIcon(QIcon());
    forceAction->setText(tr("Force Stop…"));
    forceAction->setVisible(false);
    // The one text button; the toolbar shows it as text.
    static_cast<QToolButton*>(widgetForAction(forceAction))->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(spacer);
    keyboardAction = addAction(frondIcons::icon("keyboard"), tr("Infrared keyboard attached"));
    keyboardAction->setObjectName("barKeyboard");
    keyboardAction->setToolTip(tr("Infrared keyboard attached"));
    keyboardAction->setVisible(false);
    applicationsAction = button("apps", "barApplications", tr("Applications (Ctrl+1)"), Button::Applications);
    cardAction = button("card-out", "barCard", tr("Storage (Ctrl+2)"), Button::Card);
    soundAction = button("sound-on", "barSound", tr("Sound"), Button::Sound);
    soundAction->setCheckable(true);
    powerAction = button("power", "barPower", tr("Power"), Button::Power);
    auto* more = new QToolButton(this);
    more->setObjectName("barMore");
    more->setIcon(frondIcons::icon("more"));
    more->setIconSize({20, 20});
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
    // The bar has no room for a long message; the tooltip has all of it.
    statusLabel->setText(statusLabel->fontMetrics().elidedText(text, Qt::ElideRight, 170));
    statusLabel->setToolTip(text);
    if (ms > 0 && !text.isEmpty()) statusTimer->start(ms);
    else statusTimer->stop();
}

void RunningBar::setOpenPanel(int index) {
    applicationsAction->setCheckable(true);
    cardAction->setCheckable(true);
    applicationsAction->setChecked(index == 0);
    cardAction->setChecked(index == 1);
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
        action->setText(keyText[row.id].isEmpty() ? row.label : row.label + '\t' + keyText[row.id]);
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
    // Without the embedded Palm there is no strip to bring the bar back, so the row isn't offered.
    actions[Id::HideBar]->setVisible(state.embedded);
    const auto buttons = runningRules::buttons(traits, state);
    cardAction->setVisible(buttons.card);
    cardAction->setEnabled(buttons.cardEnabled);
    static const char* icons[] = {"card-none", "card-out", "card-in", "card-ejected"};
    static const char* tips[] = {"", "No card", "Card inserted", "Card ejected"};
    cardAction->setIcon(frondIcons::icon(icons[int(card)]));
    cardAction->setToolTip(card == runningRules::CardIcon::None ? tr("Storage (Ctrl+2)")
                                                                : tr("Storage: %1 (Ctrl+2)").arg(tr(tips[int(card)])));
    powerAction->setVisible(buttons.power);
    powerAction->setEnabled(buttons.powerEnabled);
    keyboardAction->setVisible(buttons.keyboard);
    soundAction->setEnabled(buttons.soundEnabled);
    soundAction->setChecked(soundOn);
    soundAction->setIcon(frondIcons::icon(soundOn ? "sound-on" : "sound-off"));
    soundAction->setToolTip(soundOn ? tr("Sound on") : tr("Sound off"));
    applicationsAction->setEnabled(true);
    stopAction->setEnabled(actions[Id::SaveStop]->isEnabled());
}
