#include "PalmView.h"
#include "RunningRules.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWindow>
#include <memory>
#ifdef FROND_XCB
#include <xcb/xcb.h>
#include <QtGui/qguiapplication_platform.h>
#include <cstdlib>
#endif

PalmHolder::PalmHolder(QWidget* parent) : QWidget(parent) {
    setObjectName("palmHolder");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::ClickFocus);
    setAutoFillBackground(true);
    QPalette dark = palette();
    dark.setColor(QPalette::Window, Qt::black);
    setPalette(dark);
    pollTimer = new QTimer(this);
    pollTimer->setInterval(15);
    connect(pollTimer, &QTimer::timeout, this, &PalmHolder::poll);
    qApp->installEventFilter(this);
}

bool PalmHolder::embed(quint64 id) {
    release();
    if (QGuiApplication::platformName() != QLatin1String("xcb") || !id) return false;
    auto* child = QWindow::fromWinId(WId(id));
    if (!child) return false;
    wanted = child->size();
    container = QWidget::createWindowContainer(child, this);
    container->setObjectName("palmContainer");
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(1, 1);
    layout()->addWidget(container);
    palm = child;
    updateGeometry();
    pollTimer->start();
    QTimer::singleShot(0, this, [this] { if (embedded() && window()->isActiveWindow()) focusPalm(); });
    return true;
}

void PalmHolder::release() {
    pollTimer->stop();
    if (container) delete container.data();
    container.clear();
    palm.clear();
    palmHasFocus = buttonWasDown = false;
    wanted = {};
}

void PalmHolder::setPalmSize(QSize size) {
    wanted = size;
    updateGeometry();
}

BarHandle::BarHandle(QWidget* parent) : QWidget(parent) {
    setObjectName("barHandle");
    setFixedHeight(runningRules::handleHeight);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Show bar (Ctrl+B)"));
}

void BarHandle::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Mid));
    const QRect tab(width() / 2 - 20, 0, 40, height());
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Highlight));
    p.drawRoundedRect(tab.adjusted(0, -4, 0, 0), 4, 4);
}

void BarHandle::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit clicked();
}

QRect PalmHolder::palmGlobalRect() const {
    return container ? QRect(container->mapToGlobal(QPoint(0, 0)), container->size()) : QRect();
}

// The X server sends keys to the focus window; setting it by hand is what puts them on the
// Palm's window or on Frond's.
void PalmHolder::setInputFocus(quint32 target) {
#ifdef FROND_XCB
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    auto* connection = x11 ? x11->connection() : nullptr;
    if (!connection || !target) return;
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_PARENT, target, XCB_CURRENT_TIME);
    xcb_flush(connection);
#else
    Q_UNUSED(target);
#endif
}

void PalmHolder::focusPalm() {
    if (!palm) return;
    setInputFocus(quint32(palm->winId()));
    palmHasFocus = true;
}

void PalmHolder::focusFrond() {
    if (!window()->windowHandle()) return;
    setInputFocus(quint32(window()->windowHandle()->winId()));
    palmHasFocus = false;
}

// A press on the Palm never reaches Qt, so the pointer is watched: a button going down over the
// Palm gives it the keys.
void PalmHolder::poll() {
#ifdef FROND_XCB
    if (!container || !window()->isActiveWindow()) return;
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    auto* connection = x11 ? x11->connection() : nullptr;
    if (!connection) return;
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&free)> reply(
        xcb_query_pointer_reply(connection, xcb_query_pointer(connection, root), nullptr), &free);
    if (!reply) return;
    const bool down = reply->mask & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3);
    const bool pressed = down && !buttonWasDown;
    buttonWasDown = down;
    if (!pressed || palmHasFocus || QApplication::activePopupWidget() || !palmGlobalRect().contains(QCursor::pos())) return;
    // In full screen the bar and a panel float over the Palm; a press on one of them isn't on the Palm.
    const auto* over = QApplication::widgetAt(QCursor::pos());
    if (over && over != container && over != this && !container->isAncestorOf(over) && over->window() == window()) return;
    focusPalm();
#endif
}

bool PalmHolder::eventFilter(QObject* watched, QEvent* event) {
    if (!container) return false;
    if (event->type() == QEvent::MouseButtonPress) {
        // A click on one of Frond's own widgets takes the keys back, so fields can be typed in.
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget != this && widget != container && widget->window() == window() && !container->isAncestorOf(widget)
            && runningRules::keyTarget(window()->isActiveWindow(), false, true, false) == runningRules::KeyTarget::Frond)
            focusFrond();
    } else if (event->type() == QEvent::WindowActivate && watched == window()) {
        // The window manager sets the focus after this, so wait. A field being typed in keeps it.
        QTimer::singleShot(0, this, [this] {
            const auto* focus = QApplication::focusWidget();
            const bool typing = qobject_cast<const QLineEdit*>(focus) || qobject_cast<const QAbstractSpinBox*>(focus) ||
                                qobject_cast<const QTextEdit*>(focus) || qobject_cast<const QPlainTextEdit*>(focus);
            if (container && !typing &&
                runningRules::keyTarget(window()->isActiveWindow(), false, false, true) == runningRules::KeyTarget::Palm)
                focusPalm();
        });
    }
    return false;
}

void PalmHolder::raisePalm() {
    if (palm) palm->raise();
}
