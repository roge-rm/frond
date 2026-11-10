#pragma once
#include <QPointer>
#include <QSize>
#include <QWidget>
#include <vector>
class QTimer;
class QWindow;

// Holds the core's Palm window inside Frond's window. The core's X11 window is wrapped and
// reparented here, and the core's `--window-fit fit` makes it follow this widget's size.
class PalmHolder : public QWidget {
    Q_OBJECT
 public:
    explicit PalmHolder(QWidget* parent = nullptr);
    // Embeds the X11 window `id`. False when Qt isn't on X11 or the window can't be wrapped.
    bool embed(quint64 id);
    // Lets go of the window; the core's own window is gone with the core.
    void release();
    bool embedded() const { return !container.isNull(); }
    // The Palm's size at the first embedding, and the size the window should give it.
    QSize palmSize() const { return wanted; }
    void setPalmSize(QSize size);
    QSize sizeHint() const override { return wanted; }
    QSize minimumSizeHint() const override { return wanted.isValid() ? wanted / 2 : QSize(); }
    // Gives the keyboard to the Palm's window, or to Frond's key window, which forwards keys to
    // Qt's focus widget. The X focus is read back every poll and moved off any window that holds
    // the Palm (see runningRules::keyTarget).
    void focusPalm();
    void focusFrond();
    bool palmWantsKeys() const { return wantPalm; }
    // Puts the Palm's window above the other native windows of Frond's window.
    void raisePalm();
    // The Palm's rectangle in global coordinates, empty when not embedded.
    QRect palmGlobalRect() const;
 protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
 private:
    void setInputFocus(quint32 window);
    void poll();
    // Moves the X focus where runningRules::keyTarget says.
    void applyFocus();
    QPointer<QWidget> container;
    QPointer<QWindow> palm;
    QWidget* keySink{nullptr};
    QTimer* pollTimer{nullptr};
    QSize wanted;
    // The Palm's X window and its parents, refreshed about once a second and on each press.
    std::vector<quint32> holders;
    int holdersAge{0};
    bool buttonWasDown{false}, wantPalm{true};
};

// Frond's key window while the Palm is embedded: a 1x1 native window under the Palm's, so the
// keys can go to Frond with the pointer over the Palm. Qt hands its keys to the top-level's focus
// widget; any key or input method event that still reaches it goes on there too.
class KeySink : public QWidget {
    Q_OBJECT
 public:
    explicit KeySink(QWidget* parent);
 protected:
    bool event(QEvent* event) override;
};

// The thin strip left over the Palm when the bar is hidden, with a tab in the middle that brings
// the bar back.
class BarHandle : public QWidget {
    Q_OBJECT
 public:
    explicit BarHandle(QWidget* parent = nullptr);
 signals:
    void clicked();
 protected:
    void paintEvent(QPaintEvent*) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
};
