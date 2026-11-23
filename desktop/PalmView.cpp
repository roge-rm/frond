#include "PalmView.h"
#include "RunningRules.h"
#include <QApplication>
#include <QLabel>
#include <QGuiApplication>
#include <QMenuBar>
#include <QMouseEvent>
#include <QTimer>
#include <QToolBar>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <memory>
#include <vector>
#ifdef FROND_XCB
#include <xcb/xcb.h>
#include <QtGui/qguiapplication_platform.h>
#include <cstdlib>
namespace {
xcb_connection_t* xcbConnection() {
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->connection() : nullptr;
}
template <typename T> using XcbReply = std::unique_ptr<T, decltype(&free)>;
// `window` and its parents up to, not including, the root. The last one is the root's child:
// the window manager's frame, or Frond's top-level without one.
std::vector<quint32> withParents(xcb_connection_t* connection, xcb_window_t window) {
    std::vector<quint32> out;
    for (int depth = 0; window && depth < 16; ++depth) {
        XcbReply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr), &free);
        if (!tree) break;
        out.push_back(window);
        if (tree->parent == tree->root || !tree->parent) break;
        window = tree->parent;
    }
    return out;
}
}  // namespace
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
    connect(pollTimer, &QTimer::timeout, this, &PalmHolder::applyFocus);
    keySink = new KeySink(this);
    keySink->hide();
    // A native window of its own, so it stays above the Palm's X window; the pointer goes through it.
    mips = new QLabel(this);
    mips->setObjectName("mipsReadout");
    mips->setAttribute(Qt::WA_NativeWindow);
    mips->setAttribute(Qt::WA_TransparentForMouseEvents);
    mips->setAlignment(Qt::AlignCenter);
    mips->setStyleSheet("QLabel { color: white; background: rgba(0, 0, 0, 150); padding: 0px 4px; font-size: 10px; }");
    mips->hide();
    rumble = new QLabel(tr("Rumble"), this);
    rumble->setObjectName("rumbleCue");
    rumble->setAttribute(Qt::WA_NativeWindow);
    rumble->setAttribute(Qt::WA_TransparentForMouseEvents);
    rumble->setAlignment(Qt::AlignCenter);
    rumble->setStyleSheet("QLabel { color: white; background: rgba(0, 0, 0, 150); padding: 0px 4px; font-size: 10px; }");
    rumble->hide();
    qApp->installEventFilter(this);
}

KeySink::KeySink(QWidget* parent) : QWidget(parent) {
    setObjectName("keySink");
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::NoFocus);
}

bool KeySink::event(QEvent* event) {
    switch (event->type()) {
        case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride:
        case QEvent::InputMethod: case QEvent::InputMethodQuery: {
            QWidget* to = QApplication::focusWidget();
            if (!to || to == this || to->window() != window()) to = window();
            return QApplication::sendEvent(to, event);
        }
        default: return QWidget::event(event);
    }
}

bool PalmHolder::embed(quint64 id) {
    release();
    if (QGuiApplication::platformName() != QLatin1String("xcb") || !id) return false;
    auto* child = QWindow::fromWinId(WId(id));
    if (!child) return false;
#ifdef FROND_XCB
    // In Frond's save set the X server gives the Palm's window back to the root if Frond dies, so
    // the core keeps running and can save.
    if (auto* connection = xcbConnection()) {
        xcb_change_save_set(connection, XCB_SET_MODE_INSERT, xcb_window_t(id));
        xcb_flush(connection);
    }
#endif
    wanted = child->size();
#ifdef FROND_XCB
    // Qt before 6.7 doesn't read a foreign window's size, so ask X. X sizes are device pixels.
    if (wanted.isEmpty())
        if (auto* connection = xcbConnection()) {
            XcbReply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(connection, xcb_get_geometry(connection, xcb_window_t(id)), nullptr), &free);
            if (geometry && geometry->width > 1 && geometry->height > 1)
                wanted = (QSizeF(geometry->width, geometry->height) / devicePixelRatioF()).toSize();
        }
#endif
    keySink->setGeometry(0, 0, 1, 1);
    keySink->show();
    container = QWidget::createWindowContainer(child, this);
    container->setObjectName("palmContainer");
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(1, 1);
    layout()->addWidget(container);
    keySink->lower();
    palm = child;
    wantPalm = true;
    updateGeometry();
    pollTimer->start();
    return true;
}

void PalmHolder::release() {
    mips->hide();
    rumble->hide();
    rumbleOn = false;
    pollTimer->stop();
    if (container) delete container.data();
    container.clear();
    palm.clear();
    keySink->hide();
    holders.clear();
    holdersAge = 0;
    lastFocusPress.reset();
    wantPalm = true;
    wanted = {};
}

void PalmHolder::setMips(const QString& reply) {
    const auto figure = runningRules::speedFigure(reply);
    if (!container || figure.isEmpty()) { mips->hide(); return; }
    mips->setText(figure);
    mips->adjustSize();
    placeMips();
    mips->show();
    if (auto* handle = mips->windowHandle()) {
        handle->setFlag(Qt::WindowTransparentForInput, true);
        handle->raise();
    }
}

// The Zodiac's rumble with no game controller to play it: a small tag in the Palm's bottom left corner.
void PalmHolder::setRumble(bool on) {
    rumbleOn = on;
    if (!container || !on) { rumble->hide(); return; }
    rumble->adjustSize();
    placeMips();
    rumble->show();
    if (auto* handle = rumble->windowHandle()) {
        handle->setFlag(Qt::WindowTransparentForInput, true);
        handle->raise();
    }
}

void PalmHolder::placeMips() {
    if (!container) return;
    // The core draws the device fitted and centred in its window; the box sits flush in that rectangle's bottom right corner.
    QRect area = container->geometry();
    if (wanted.isValid() && area.width() > 0 && area.height() > 0) {
        const double fit = std::min(double(area.width()) / wanted.width(), double(area.height()) / wanted.height());
        const QSize drawn(qRound(wanted.width() * fit), qRound(wanted.height() * fit));
        area = QRect(area.center() - QPoint(drawn.width() / 2, drawn.height() / 2), drawn);
    }
    const auto corner = area.bottomRight();
    mips->move(corner.x() + 1 - mips->width(), corner.y() + 1 - mips->height());
    rumble->move(area.left(), corner.y() + 1 - rumble->height());
}

void PalmHolder::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (mips->isVisible() || rumble->isVisible()) placeMips();
    if (mips->isVisible()) if (auto* handle = mips->windowHandle()) handle->raise();
    if (rumble->isVisible()) if (auto* handle = rumble->windowHandle()) handle->raise();
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

void PalmHolder::setInputFocus(quint32 target) {
#ifdef FROND_XCB
    auto* connection = xcbConnection();
    if (!connection || !target) return;
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_PARENT, target, XCB_CURRENT_TIME);
    xcb_flush(connection);
#else
    Q_UNUSED(target);
#endif
}

void PalmHolder::focusPalm(quint32 time) {
    if (!container || !runningRules::acceptFocusPress(lastFocusPress, time, runningRules::Press::OnPalm)) return;
    wantPalm = true;
    applyFocus();
}

void PalmHolder::focusFrond() {
    wantPalm = false;
    applyFocus();
}

// Reads the X focus back and moves it to the Palm or the key window, so it never stays on a
// window that holds the Palm, where keys would follow the pointer.
void PalmHolder::applyFocus() {
#ifdef FROND_XCB
    using runningRules::FocusAt;
    auto* connection = xcbConnection();
    if (!connection || !palm || !window()->windowHandle()) return;
    XcbReply<xcb_get_input_focus_reply_t> reply(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr), &free);
    if (!reply) return;
    const xcb_window_t focus = reply->focus, palmId = xcb_window_t(palm->winId());
    FocusAt at = FocusAt::Elsewhere;
    if (focus == palmId) at = FocusAt::Palm;
    else if (focus != XCB_NONE && focus != XCB_INPUT_FOCUS_POINTER_ROOT) {
        if (holders.empty() || ++holdersAge > 64) { holders = withParents(connection, palmId); holdersAge = 0; }
        if (!holders.empty() && std::find(holders.begin() + 1, holders.end(), focus) != holders.end()) at = FocusAt::HoldsPalm;
        else
            for (QWindow* w : QGuiApplication::allWindows()) {
                if (!w->handle() || xcb_window_t(w->winId()) != focus) continue;
                while (w->parent()) w = w->parent();
                if (w == window()->windowHandle()) at = FocusAt::Frond;
                break;
            }
    }
    switch (runningRules::keyTarget(at, wantPalm, QApplication::activePopupWidget() != nullptr)) {
        case runningRules::KeyTarget::Palm: setInputFocus(palmId); break;
        case runningRules::KeyTarget::Frond: setInputFocus(quint32(keySink->winId())); break;
        case runningRules::KeyTarget::Keep: break;
    }
#endif
}

bool PalmHolder::eventFilter(QObject* watched, QEvent* event) {
    if (!container || (event->type() != QEvent::MouseButtonPress &&
                       event->type() != QEvent::MouseButtonDblClick)) return false;
    // A press on one of Frond's own widgets takes the keys, so fields can be typed in. The bar,
    // the menu bar and the handle strip leave them where they are.
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget == container || widget->window() != window() || container->isAncestorOf(widget)) return false;
    auto press = runningRules::Press::OnFrond;
    for (const QWidget* w = widget; w; w = w->parentWidget())
        if (qobject_cast<const QToolBar*>(w) || qobject_cast<const QMenuBar*>(w) || qobject_cast<const BarHandle*>(w) || w == this) {
            press = runningRules::Press::None;
            break;
        }
    if (runningRules::acceptFocusPress(lastFocusPress, quint32(static_cast<QMouseEvent*>(event)->timestamp()), press))
        wantPalm = runningRules::palmWantsKeys(wantPalm, press);
    applyFocus();
    return false;
}

void PalmHolder::raisePalm() {
    if (palm) palm->raise();
    if (mips->isVisible()) if (auto* handle = mips->windowHandle()) handle->raise();
    if (rumble->isVisible()) if (auto* handle = rumble->windowHandle()) handle->raise();
}
