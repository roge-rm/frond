#pragma once
#include <QPointer>
#include <QSize>
#include <QWidget>
class QWindow;

// Holds the core's Palm window inside Frond's window. The core's X11 window is wrapped and
// reparented here, and the core's `--window-fit whole` makes it follow this widget's size.
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
 private:
    QPointer<QWidget> container;
    QSize wanted;
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
