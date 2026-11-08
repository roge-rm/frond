#include "PalmView.h"
#include "RunningRules.h"
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWindow>

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
}

bool PalmHolder::embed(quint64 id) {
    release();
    if (QGuiApplication::platformName() != QLatin1String("xcb") || !id) return false;
    auto* window = QWindow::fromWinId(WId(id));
    if (!window) return false;
    wanted = window->size();
    container = QWidget::createWindowContainer(window, this);
    container->setObjectName("palmContainer");
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(1, 1);
    layout()->addWidget(container);
    updateGeometry();
    return true;
}

void PalmHolder::release() {
    if (container) delete container.data();
    container.clear();
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
