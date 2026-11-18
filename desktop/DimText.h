#pragma once
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QObject>
#include <QPalette>
#include <QWidget>

// Second-line text (sizes, states, empty-list notes) drawn as the window text blended
// towards the window colour. PlaceholderText isn't used because some platform themes
// leave it a dark grey on dark backgrounds.
namespace frondDimText {

// Colours come from the parent, which follows the application palette, so setting the
// label's own palette doesn't feed back into the next change.
inline void apply(QWidget* w) {
    auto palette = w->palette();
    const auto base = w->parentWidget() ? w->parentWidget()->palette() : QApplication::palette(w);
    for (auto group : {QPalette::Active, QPalette::Inactive}) {
        const auto text = base.color(group, QPalette::WindowText);
        const auto back = base.color(group, QPalette::Window);
        const QColor dim((text.red() * 3 + back.red() * 2) / 5, (text.green() * 3 + back.green() * 2) / 5,
                         (text.blue() * 3 + back.blue() * 2) / 5);
        palette.setColor(group, QPalette::WindowText, dim);
        palette.setColor(group, QPalette::Text, dim);
    }
    w->setPalette(palette);
}

// Redoes the colour when the parent's palette changes, such as a light/dark switch.
class Watcher : public QObject {
public:
    explicit Watcher(QWidget* label) : QObject(label), label(label) {}
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::PaletteChange) apply(label);
        return false;
    }

private:
    QWidget* label;
};

inline void dim(QWidget* w) {
    apply(w);
    if (auto* parent = w->parentWidget()) parent->installEventFilter(new Watcher(w));
}

}
