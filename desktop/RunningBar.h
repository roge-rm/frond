#pragma once
#include <QToolBar>
#include <map>
#include "RunningRules.h"
class QLabel;
class QMenu;
class QTimer;

// The slim bar over the running Palm and the Machine menu. The menu's actions are made once and
// show in the menu bar and under the three dots.
class RunningBar : public QToolBar {
    Q_OBJECT
 public:
    enum class Button { Stop, Applications, Card, Sound, Power, ForceStop };
    explicit RunningBar(QWidget* parent = nullptr);
    QMenu* menu() const { return machineMenu; }
    void setMachine(const QString& name, const QString& subtitle);
    // Shows a message beside the name for `ms`, or until the next one with 0.
    void say(const QString& text, int ms = 5000);
    void setForceStop(bool shown);
    // Updates every row and button; `networks` are the modes (mode, label) the core offers.
    void apply(const runningRules::Traits& traits, const runningRules::State& state, runningRules::CardIcon card,
               bool soundOn, const QList<QPair<QString, QString>>& networks);
    // The panel open at the right of the Palm (0 Applications, 1 Card, 2 Display, 3 Connections), or -1.
    void setOpenPanel(int index);
    bool barShown() const { return shown; }
    void setBarShown(bool on);
 signals:
    void chosen(runningRules::Id id);
    void pressed(RunningBar::Button button);
    void networkChosen(const QString& mode);
 private:
    void update();
    QMenu* machineMenu;
    QMenu* networkMenu{nullptr};
    QLabel *nameLabel, *subtitleLabel, *statusLabel;
    QTimer* statusTimer;
    std::map<runningRules::Id, QAction*> actions;
    std::map<runningRules::Id, QString> keyText;  // the keys shown beside a row
    QAction *stopAction, *applicationsAction, *cardAction, *soundAction, *powerAction, *keyboardAction, *forceAction;
    bool shown{true}, wantVisible{false};
};
